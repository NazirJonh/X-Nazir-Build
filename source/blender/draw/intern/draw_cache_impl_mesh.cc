/* SPDX-FileCopyrightText: 2017 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 *
 * \brief Mesh API for render engines
 */

#include <array>
#include <cstdio>
#include <optional>

#ifdef _WIN32
#  include <windows.h>
#endif

#include "MEM_guardedalloc.h"

#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_map.hh"
#include "BLI_span.hh"
#include "BLI_string_ref.hh"

#include "DNA_ID.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_userdef_types.h"

#include "BKE_attribute.hh"
#include "BKE_customdata.hh"
#include "BKE_editmesh.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_object.hh"
#include "BKE_object_deform.h"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_subdiv_modifier.hh"

#include "GPU_batch.hh"
#include "GPU_material.hh"

#include "DRW_render.hh"

#include "draw_cache_extract.hh"
#include "draw_cache_inline.hh"
#include "draw_subdivision.hh"

#include "draw_cache_impl.hh" /* own include */
#include "draw_context_private.hh"

#include "DEG_depsgraph_query.hh"

#include "mesh_extractors/extract_mesh.hh"

namespace blender::draw {

/* Static map to preserve sculpt custom batch flags across cache recreations and
 * different mesh instances (evaluated vs original). Uses pointer to original Object
 * as key (preferred) or original mesh ID as fallback. */
static Map<const void *, DRWBatchFlag> sculpt_custom_flags_preserved;

/* Get stable key for mesh. Uses original Object as key if available (preferred),
 * otherwise falls back to original mesh ID. */
static const void *get_mesh_key(const Mesh &mesh, const Object *ob = nullptr)
{
  if (ob) {
    /* We have Object - use original Object as key (preferred) */
    const Object *orig_ob = DEG_get_original(ob);
    const void *key = orig_ob ? static_cast<const void *>(orig_ob) : static_cast<const void *>(ob);
    // fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] get_mesh_key: ob=%p, orig_ob=%p, mesh.id=%p, key=%p (Object)\n",
    //         ob, orig_ob, &mesh.id, key);
    return key;
  }
  /* Fallback: use original mesh ID (for Python API where Object is not available) */
  const ID *orig_id = DEG_get_original_id(&mesh.id);
  const void *key = orig_id ? static_cast<const void *>(orig_id) : static_cast<const void *>(&mesh.id);
  // fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] get_mesh_key: fallback, mesh.id=%p, orig_id=%p, key=%p (ID)\n",
  //         &mesh.id, orig_id, key);
  return key;
}

/* ---------------------------------------------------------------------- */
/** \name Dependencies between buffer and batch
 * \{ */

#define TRIS_PER_MAT_INDEX BUFFER_LEN


static void mesh_batch_cache_clear(MeshBatchCache &cache);

static void discard_buffers(MeshBatchCache &cache,
                            const Span<VBOType> vbos,
                            const Span<IBOType> ibos)
{
  Set<const void *, 16> buffer_ptrs;
  buffer_ptrs.reserve(vbos.size() + ibos.size());
  FOREACH_MESH_BUFFER_CACHE (cache, mbc) {
    for (const VBOType vbo : vbos) {
      if (const auto *buffer = mbc->buff.vbos.lookup_ptr(vbo)) {
        buffer_ptrs.add(buffer->get());
      }
    }
  }
  FOREACH_MESH_BUFFER_CACHE (cache, mbc) {
    for (const IBOType ibo : ibos) {
      if (const auto *buffer = mbc->buff.ibos.lookup_ptr(ibo)) {
        buffer_ptrs.add(buffer->get());
      }
    }
  }

  const auto batch_contains_data = [&](gpu::Batch &batch) {
    if (buffer_ptrs.contains(batch.elem)) {
      return true;
    }
    if (std::any_of(batch.verts, batch.verts + ARRAY_SIZE(batch.verts), [&](gpu::VertBuf *vbo) {
          return vbo && buffer_ptrs.contains(vbo);
        }))
    {
      return true;
    }
    return false;
  };

  for (const int i : IndexRange(MBC_BATCH_LEN)) {
    gpu::Batch *batch = (reinterpret_cast<gpu::Batch **>(&cache.batch))[i];
    if (batch && batch_contains_data(*batch)) {
      GPU_BATCH_DISCARD_SAFE(((gpu::Batch **)&cache.batch)[i]);
      cache.batch_ready &= ~DRWBatchFlag(uint64_t(1u) << i);
    }
  }

  if (!cache.surface_per_mat.is_empty()) {
    if (cache.surface_per_mat.first() && batch_contains_data(*cache.surface_per_mat.first())) {
      /* The format for all `surface_per_mat` batches is the same, discard them all. */
      for (const int i : cache.surface_per_mat.index_range()) {
        GPU_BATCH_DISCARD_SAFE(cache.surface_per_mat[i]);
      }
      cache.batch_ready &= ~(MBC_SURFACE | MBC_SURFACE_PER_MAT);
    }
  }

  for (const VBOType vbo : vbos) {
    cache.final.buff.vbos.remove(vbo);
    cache.cage.buff.vbos.remove(vbo);
    cache.uv_cage.buff.vbos.remove(vbo);
  }
  for (const IBOType ibo : ibos) {
    cache.final.buff.ibos.remove(ibo);
    cache.cage.buff.ibos.remove(ibo);
    cache.uv_cage.buff.ibos.remove(ibo);
  }
}

BLI_INLINE void mesh_cd_layers_type_merge(DRW_MeshCDMask *a, const DRW_MeshCDMask &b)
{
  drw_attributes_merge(&a->uv, &b.uv);
  drw_attributes_merge(&a->tan, &b.tan);
  a->orco |= b.orco;
  a->tan_orco |= b.tan_orco;
  a->sculpt_overlays |= b.sculpt_overlays;
  a->edit_uv |= b.edit_uv;
}

static void mesh_cd_calc_edit_uv_layer(const Mesh & /*mesh*/, DRW_MeshCDMask *cd_used)
{
  cd_used->edit_uv = true;
}

static void mesh_cd_calc_active_uv_layer(const Object &object,
                                         const Mesh &mesh,
                                         DRW_MeshCDMask &cd_used)
{
  const Mesh &me_final = editmesh_final_or_this(object, mesh);
  const StringRef active_uv_map = me_final.active_or_default_uv_map_name();
  if (!active_uv_map.is_empty()) {
    cd_used.uv.add_as(active_uv_map);
  }
}

static void mesh_cd_calc_active_mask_uv_layer(const Object &object,
                                              const Mesh &mesh,
                                              DRW_MeshCDMask &cd_used)
{
  const Mesh &me_final = editmesh_final_or_this(object, mesh);
  StringRef name = me_final.stencil_uv_map_attribute;
  if (name.is_empty()) {
    name = mesh.active_uv_map_name();
  }
  if (!name.is_empty()) {
    cd_used.uv.add_as(name);
  }
}

static bool attribute_exists(const Mesh &mesh, const StringRef name)
{
  if (BMEditMesh *em = mesh.runtime->edit_mesh.get()) {
    return bool(BM_data_layer_lookup(*em->bm, name));
  }
  return mesh.attributes().contains(name);
};

static std::optional<bke::AttributeMetaData> lookup_meta_data(const Mesh &mesh,
                                                              const StringRef name)
{
  if (mesh.runtime->wrapper_type == ME_WRAPPER_TYPE_BMESH) {
    if (BMEditMesh *em = mesh.runtime->edit_mesh.get()) {
      if (const BMDataLayerLookup attr = BM_data_layer_lookup(*em->bm, name)) {
        return bke::AttributeMetaData{attr.domain, attr.type};
      }
      return std::nullopt;
    }
  }
  return mesh.attributes().lookup_meta_data(name);
}

static void mesh_cd_calc_used_gpu_layers(const Object &object,
                                         const Mesh &mesh,
                                         const Span<const GPUMaterial *> materials,
                                         VectorSet<std::string> *r_attributes,
                                         DRW_MeshCDMask *r_cd_used)
{
  constexpr bke::AttributeMetaData UV_METADATA{bke::AttrDomain::Corner, bke::AttrType::Float2};
  const Mesh &me_final = editmesh_final_or_this(object, mesh);

  for (const GPUMaterial *gpumat : materials) {
    if (gpumat == nullptr) {
      continue;
    }
    ListBaseT<GPUMaterialAttribute> gpu_attrs = GPU_material_attributes(gpumat);
    for (GPUMaterialAttribute &gpu_attr : gpu_attrs) {

      if (gpu_attr.is_default_color) {
        const StringRef default_color_name = me_final.default_color_attribute;
        if (attribute_exists(me_final, default_color_name)) {
          drw_attributes_add_request(r_attributes, default_color_name);
        }
        continue;
      }

      if (gpu_attr.type == CD_ORCO) {
        r_cd_used->orco = true;
        continue;
      }

      StringRef name = gpu_attr.name;

      if (gpu_attr.type == CD_TANGENT) {
        if (name.is_empty()) {
          const StringRef default_name = me_final.default_uv_map_name();
          if (!default_name.is_empty()) {
            name = default_name;
          }
        }
        if (lookup_meta_data(mesh, name) == UV_METADATA) {
          r_cd_used->tan.add(name);
        }
        else {
          r_cd_used->tan_orco = true;
          r_cd_used->orco = true;
        }

        continue;
      }

      if (name.is_empty()) {
        const StringRef default_name = me_final.default_uv_map_name();
        if (!default_name.is_empty()) {
          if (lookup_meta_data(mesh, default_name) == UV_METADATA) {
            r_cd_used->uv.add(default_name);
          }
        }
        continue;
      }

      const std::optional<bke::AttributeMetaData> meta_data = lookup_meta_data(mesh, name);
      if (!meta_data) {
        continue;
      }
      if (meta_data == UV_METADATA) {
        r_cd_used->uv.add(name);
        continue;
      }
      drw_attributes_add_request(r_attributes, name);
    }
  }
}

/** \} */

/* ---------------------------------------------------------------------- */
/** \name Vertex Group Selection
 * \{ */

/** Reset the selection structure, deallocating heap memory as appropriate. */
static void drw_mesh_weight_state_clear(DRW_MeshWeightState *wstate)
{
  MEM_SAFE_DELETE(wstate->defgroup_sel);
  MEM_SAFE_DELETE(wstate->defgroup_locked);
  MEM_SAFE_DELETE(wstate->defgroup_unlocked);

  memset(wstate, 0, sizeof(*wstate));

  wstate->defgroup_active = -1;
}

/** Copy selection data from one structure to another, including heap memory. */
static void drw_mesh_weight_state_copy(DRW_MeshWeightState *wstate_dst,
                                       const DRW_MeshWeightState *wstate_src)
{
  MEM_SAFE_DELETE(wstate_dst->defgroup_sel);
  MEM_SAFE_DELETE(wstate_dst->defgroup_locked);
  MEM_SAFE_DELETE(wstate_dst->defgroup_unlocked);

  memcpy(wstate_dst, wstate_src, sizeof(*wstate_dst));

  if (wstate_src->defgroup_sel) {
    wstate_dst->defgroup_sel = MEM_dupalloc(wstate_src->defgroup_sel);
  }
  if (wstate_src->defgroup_locked) {
    wstate_dst->defgroup_locked = MEM_dupalloc(wstate_src->defgroup_locked);
  }
  if (wstate_src->defgroup_unlocked) {
    wstate_dst->defgroup_unlocked = static_cast<bool *>(
        MEM_dupalloc(wstate_src->defgroup_unlocked));
  }
}

static bool drw_mesh_flags_equal(const bool *array1, const bool *array2, int size)
{
  return ((!array1 && !array2) ||
          (array1 && array2 && memcmp(array1, array2, size * sizeof(bool)) == 0));
}

/** Compare two selection structures. */
static bool drw_mesh_weight_state_compare(const DRW_MeshWeightState *a,
                                          const DRW_MeshWeightState *b)
{
  return a->defgroup_active == b->defgroup_active && a->defgroup_len == b->defgroup_len &&
         a->flags == b->flags && a->alert_mode == b->alert_mode &&
         a->defgroup_sel_count == b->defgroup_sel_count &&
         drw_mesh_flags_equal(a->defgroup_sel, b->defgroup_sel, a->defgroup_len) &&
         drw_mesh_flags_equal(a->defgroup_locked, b->defgroup_locked, a->defgroup_len) &&
         drw_mesh_flags_equal(a->defgroup_unlocked, b->defgroup_unlocked, a->defgroup_len);
}

static void drw_mesh_weight_state_extract(
    Object &ob, Mesh &mesh, const ToolSettings &ts, bool paint_mode, DRW_MeshWeightState *wstate)
{
  /* Extract complete vertex weight group selection state and mode flags. */
  memset(wstate, 0, sizeof(*wstate));

  wstate->defgroup_active = mesh.vertex_group_active_index - 1;
  wstate->defgroup_len = BLI_listbase_count(&mesh.vertex_group_names);

  wstate->alert_mode = ts.weightuser;

  if (paint_mode && ts.multipaint) {
    /* Multi-paint needs to know all selected bones, not just the active group.
     * This is actually a relatively expensive operation, but caching would be difficult. */
    wstate->defgroup_sel = BKE_object_defgroup_selected_get(
        &ob, wstate->defgroup_len, &wstate->defgroup_sel_count);

    if (wstate->defgroup_sel_count > 1) {
      wstate->flags |= DRW_MESH_WEIGHT_STATE_MULTIPAINT |
                       (ts.auto_normalize ? DRW_MESH_WEIGHT_STATE_AUTO_NORMALIZE : 0);

      if (ME_USING_MIRROR_X_VERTEX_GROUPS(&mesh)) {
        BKE_object_defgroup_mirror_selection(&ob,
                                             wstate->defgroup_len,
                                             wstate->defgroup_sel,
                                             wstate->defgroup_sel,
                                             &wstate->defgroup_sel_count);
      }
    }
    /* With only one selected bone Multi-paint reverts to regular mode. */
    else {
      wstate->defgroup_sel_count = 0;
      MEM_SAFE_DELETE(wstate->defgroup_sel);
    }
  }

  if (paint_mode && ts.wpaint_lock_relative) {
    /* Set of locked vertex groups for the lock relative mode. */
    wstate->defgroup_locked = BKE_object_defgroup_lock_flags_get(&ob, wstate->defgroup_len);
    wstate->defgroup_unlocked = BKE_object_defgroup_validmap_get(&ob, wstate->defgroup_len);

    /* Check that a deform group is active, and none of selected groups are locked. */
    if (BKE_object_defgroup_check_lock_relative(
            wstate->defgroup_locked, wstate->defgroup_unlocked, wstate->defgroup_active) &&
        BKE_object_defgroup_check_lock_relative_multi(wstate->defgroup_len,
                                                      wstate->defgroup_locked,
                                                      wstate->defgroup_sel,
                                                      wstate->defgroup_sel_count))
    {
      wstate->flags |= DRW_MESH_WEIGHT_STATE_LOCK_RELATIVE;

      /* Compute the set of locked and unlocked deform vertex groups. */
      BKE_object_defgroup_split_locked_validmap(wstate->defgroup_len,
                                                wstate->defgroup_locked,
                                                wstate->defgroup_unlocked,
                                                wstate->defgroup_locked, /* out */
                                                wstate->defgroup_unlocked);
    }
    else {
      MEM_SAFE_DELETE(wstate->defgroup_unlocked);
      MEM_SAFE_DELETE(wstate->defgroup_locked);
    }
  }
}

/** \} */

/* ---------------------------------------------------------------------- */
/** \name Mesh gpu::Batch Cache
 * \{ */

/* gpu::Batch cache management. */

static bool mesh_batch_cache_valid(Mesh &mesh)
{
  MeshBatchCache *cache = mesh.runtime->batch_cache;

  if (cache == nullptr) {
    fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] mesh_batch_cache_valid: cache is nullptr\n");
    return false;
  }

  /* NOTE: bke::pbvh::Tree draw data should not be checked here. */

  if (cache->is_editmode != (mesh.runtime->edit_mesh != nullptr)) {
    fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] mesh_batch_cache_valid: editmode mismatch "
                    "(cache=%d, mesh=%d)\n",
            cache->is_editmode, (mesh.runtime->edit_mesh != nullptr));
    return false;
  }

  if (cache->is_dirty) {
    fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] mesh_batch_cache_valid: cache is dirty\n");
    return false;
  }

  const int current_mat_len = BKE_id_material_used_with_fallback_eval(mesh.id);
  if (cache->mat_len != current_mat_len) {
    fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] mesh_batch_cache_valid: mat_len mismatch "
                    "(cache=%d, mesh=%d)\n",
            cache->mat_len, current_mat_len);
    return false;
  }

  return true;
}

static void mesh_batch_cache_init(Mesh &mesh)
{
  const DRWBatchFlag sculpt_custom_flags = MBC_SCULPT_CUSTOM_TRIANGLES | MBC_SCULPT_CUSTOM_EDGES |
                                           MBC_SCULPT_CUSTOM_VERTICES;
  DRWBatchFlag preserved_sculpt_custom = (DRWBatchFlag)0;
  const void *mesh_key = get_mesh_key(mesh);
  
  /* Check static map for flags that were set by Python before cache was created */
  if (sculpt_custom_flags_preserved.contains(mesh_key)) {
    preserved_sculpt_custom = sculpt_custom_flags_preserved.lookup(mesh_key);
    fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] mesh_batch_cache_init: restored flags from static map: 0x%llx\n",
            uint64_t(preserved_sculpt_custom));
  }
  
  if (!mesh.runtime->batch_cache) {
    fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] mesh_batch_cache_init: creating new cache, mesh_key=%p\n",
            mesh_key);
    mesh.runtime->batch_cache = MEM_new<MeshBatchCache>(__func__);
  }
  else {
    MeshBatchCache *old_cache = static_cast<MeshBatchCache *>(mesh.runtime->batch_cache);
    /* Preserve sculpt custom flags from old cache - they may have been set by Python
     * after the last batch creation and need to persist across cache resets. */
    DRWBatchFlag old_sculpt_custom = old_cache->batch_requested & sculpt_custom_flags;
    if (old_sculpt_custom != 0) {
      preserved_sculpt_custom |= old_sculpt_custom;
      /* Also save to static map */
      sculpt_custom_flags_preserved.add_or_modify(
          mesh_key,
          [&](DRWBatchFlag *value) { *value = old_sculpt_custom; },
          [&](DRWBatchFlag *value) { *value |= old_sculpt_custom; });
    }

    /* Clear old batch pointers before resetting cache to prevent access violations.
     * Old batches will be freed when cache is reset, but we need to clear pointers
     * to prevent stale references. */
    old_cache->batch.sculpt_custom_triangles = nullptr;
    old_cache->batch.sculpt_custom_edges = nullptr;
    old_cache->batch.sculpt_custom_vertices = nullptr;

    /* Reset cache - this will free old batches */
    *old_cache = {};
  }
  MeshBatchCache *cache = mesh.runtime->batch_cache;

  cache->is_editmode = mesh.runtime->edit_mesh != nullptr;

  if (cache->is_editmode == false) {
    // cache->edge_len = mesh_render_edges_len_get(mesh);
    // cache->tri_len = mesh_render_corner_tris_len_get(mesh);
    // cache->face_len = mesh_render_faces_len_get(mesh);
    // cache->vert_len = mesh_render_verts_len_get(mesh);
  }

  cache->mat_len = BKE_id_material_used_with_fallback_eval(mesh.id);
  cache->surface_per_mat = Array<gpu::Batch *>(cache->mat_len, nullptr);
  cache->tris_per_mat.reinitialize(cache->mat_len);

  cache->is_dirty = false;
  cache->batch_ready = DRWBatchFlag(0);
  /* Restore preserved sculpt custom batch requests. These flags are set by Python
   * and must persist until the batches are created and ready. */
  cache->batch_requested = preserved_sculpt_custom;

  drw_mesh_weight_state_clear(&cache->weight_state);
}

void DRW_mesh_batch_cache_validate(Mesh &mesh)
{
  /* If mesh has no runtime, we cannot create batch cache */
  if (!mesh.runtime) {
    fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] validate: mesh.runtime is nullptr!\n");
    return;
  }
  
  bool is_valid = mesh_batch_cache_valid(mesh);
  fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] validate: mesh=%p runtime=%p batch_cache=%p valid=%d\n",
          &mesh, mesh.runtime, mesh.runtime ? mesh.runtime->batch_cache : nullptr, is_valid);
  if (!is_valid) {
    if (mesh.runtime->batch_cache) {
      mesh_batch_cache_clear(*mesh.runtime->batch_cache);
    }
    /* mesh_batch_cache_init will automatically preserve sculpt custom flags */
    mesh_batch_cache_init(mesh);
    fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] validate: after init, batch_cache=%p\n",
            mesh.runtime->batch_cache);
  }
}

static MeshBatchCache *mesh_batch_cache_get(Mesh &mesh)
{
  return mesh.runtime->batch_cache;
}

static void mesh_batch_cache_check_vertex_group(MeshBatchCache &cache,
                                                const DRW_MeshWeightState *wstate)
{
  if (!drw_mesh_weight_state_compare(&cache.weight_state, wstate)) {
    FOREACH_MESH_BUFFER_CACHE (cache, mbc) {
      mbc->buff.vbos.remove(VBOType::VertexGroupWeight);
    }
    GPU_BATCH_CLEAR_SAFE(cache.batch.surface_weights);

    cache.batch_ready &= ~MBC_SURFACE_WEIGHTS;

    drw_mesh_weight_state_clear(&cache.weight_state);
  }
}

static void mesh_batch_cache_request_surface_batches(Mesh &mesh, MeshBatchCache &cache)
{
  cache.batch_requested |= (MBC_SURFACE | MBC_SURFACE_PER_MAT);
  DRW_batch_request(&cache.batch.surface);

  /* If there are only a few materials at most, just request batches for everything. However, if
   * the maximum material index is large, detect the actually used material indices first and only
   * request those. This reduces the overhead of dealing with all these batches down the line. */
  if (cache.mat_len < 16) {
    for (int i = 0; i < cache.mat_len; i++) {
      DRW_batch_request(&cache.surface_per_mat[i]);
    }
  }
  else {
    const VectorSet<int> &used_material_indices = mesh.material_indices_used();
    for (const int i : used_material_indices) {
      DRW_batch_request(&cache.surface_per_mat[i]);
    }
  }
}

static void mesh_batch_cache_discard_shaded_tri(MeshBatchCache &cache)
{
  discard_buffers(cache, {VBOType::UVs, VBOType::Tangents, VBOType::Orco}, {});
}

static void mesh_batch_cache_discard_uvedit(MeshBatchCache &cache)
{
  discard_buffers(cache,
                  {VBOType::EditUVStretchAngle,
                   VBOType::EditUVStretchArea,
                   VBOType::UVs,
                   VBOType::EditUVData,
                   VBOType::FaceDotUV,
                   VBOType::FaceDotEditUVData},
                  {IBOType::EditUVTris,
                   IBOType::EditUVLines,
                   IBOType::EditUVPoints,
                   IBOType::EditUVFaceDots,
                   IBOType::UVLines,
                   IBOType::UVTris});

  cache.tot_area = 0.0f;
  cache.tot_uv_area = 0.0f;

  /* We discarded the vbo.uv so we need to reset the cd_used flag. */
  cache.cd_used.uv.clear();
  cache.cd_used.edit_uv = false;
}

static void mesh_batch_cache_discard_uvedit_select(MeshBatchCache &cache)
{
  discard_buffers(cache,
                  {VBOType::EditUVData, VBOType::FaceDotEditUVData},
                  {IBOType::EditUVTris,
                   IBOType::EditUVLines,
                   IBOType::EditUVPoints,
                   IBOType::EditUVFaceDots,
                   IBOType::UVLines,
                   IBOType::UVTris});
}

void DRW_mesh_batch_cache_dirty_tag(Mesh *mesh, eMeshBatchDirtyMode mode)
{
  if (!mesh->runtime->batch_cache) {
    return;
  }
  MeshBatchCache &cache = *mesh->runtime->batch_cache;
  switch (mode) {
    case BKE_MESH_BATCH_DIRTY_SELECT:
      discard_buffers(cache, {VBOType::EditData, VBOType::FaceDotNormal}, {});

      /* Because visible UVs depends on edit mode selection, discard topology. */
      mesh_batch_cache_discard_uvedit_select(cache);
      break;
    case BKE_MESH_BATCH_DIRTY_SELECT_PAINT:
      discard_buffers(cache, {VBOType::PaintOverlayFlag}, {IBOType::LinesPaintMask});
      break;
    case BKE_MESH_BATCH_DIRTY_ALL:
      cache.is_dirty = true;
      /* Also invalidate custom sculpt batches */
      {
        const DRWBatchFlag sculpt_custom_flags = MBC_SCULPT_CUSTOM_TRIANGLES |
                                                 MBC_SCULPT_CUSTOM_EDGES |
                                                 MBC_SCULPT_CUSTOM_VERTICES;
        cache.batch_ready &= ~sculpt_custom_flags;
        cache.batch.sculpt_custom_triangles = nullptr;
        cache.batch.sculpt_custom_edges = nullptr;
        cache.batch.sculpt_custom_vertices = nullptr;
        /* Re-request batches so they will be recreated on next frame */
        cache.batch_requested |= sculpt_custom_flags;
        /* Also ensure flags are in preserved map so they persist across cache recreation */
        if (mesh->runtime) {
          const void *mesh_key = get_mesh_key(*mesh, nullptr);
          sculpt_custom_flags_preserved.add_or_modify(
              mesh_key,
              [&](DRWBatchFlag *value) { *value = sculpt_custom_flags; },
              [&](DRWBatchFlag *value) { *value |= sculpt_custom_flags; });
        }
      }
      break;
    case BKE_MESH_BATCH_DIRTY_SHADING:
      mesh_batch_cache_discard_shaded_tri(cache);
      mesh_batch_cache_discard_uvedit(cache);
      break;
    case BKE_MESH_BATCH_DIRTY_UVEDIT_ALL:
      mesh_batch_cache_discard_uvedit(cache);
      break;
    case BKE_MESH_BATCH_DIRTY_UVEDIT_SELECT:
      discard_buffers(cache, {VBOType::EditUVData, VBOType::FaceDotEditUVData}, {});
      break;
    case BKE_MESH_BATCH_DIRTY_SCULPT_CUSTOM: {
      /* Invalidate only custom sculpt batches */
      const DRWBatchFlag sculpt_custom_flags = MBC_SCULPT_CUSTOM_TRIANGLES |
                                               MBC_SCULPT_CUSTOM_EDGES |
                                               MBC_SCULPT_CUSTOM_VERTICES;
      // #region agent log
      {
        static unsigned long long counter = 0;
        FILE *f = fopen("i:\\Blender_DAD\\blender\\.cursor\\debug.log", "a");
        if (f) {
          fprintf(f, "{\"id\":\"log_%llu\",\"location\":\"draw_cache_impl_mesh.cc:674\",\"message\":\"INVALIDATION: BKE_MESH_BATCH_DIRTY_SCULPT_CUSTOM\",\"data\":{\"mesh_id\":\"%p\",\"batch_ready_before\":\"0x%llx\",\"batch_requested_before\":\"0x%llx\"},\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"A\"}\n",
                  counter++, mesh, (unsigned long long)cache.batch_ready, (unsigned long long)cache.batch_requested);
          fclose(f);
        }
      }
      // #endregion
      cache.batch_ready &= ~sculpt_custom_flags;
      /* Clear batch pointers */
      cache.batch.sculpt_custom_triangles = nullptr;
      cache.batch.sculpt_custom_edges = nullptr;
      cache.batch.sculpt_custom_vertices = nullptr;
      /* CRITICAL: Discard VBO/IBO buffers used by sculpt custom batches so they will be
       * recreated with new geometry data. Sculpt custom batches use:
       * - VBOType::Position (for vertex positions)
       * - VBOType::CornerNormal (for normals)
       * - IBOType::Tris (for triangles)
       * - IBOType::Lines (for edges)
       * - IBOType::Points (for vertices)
       * 
       * IMPORTANT: We only discard buffers in the Final buffer cache, as sculpt custom
       * batches use BufferList::Final. We don't discard buffers used by other batches
       * (e.g., MBC_SURFACE) to avoid unnecessary recreation. */
      FOREACH_MESH_BUFFER_CACHE (cache, mbc) {
        /* Remove VBO Position and CornerNormal - these are used by sculpt custom batches */
        mbc->buff.vbos.remove(VBOType::Position);
        mbc->buff.vbos.remove(VBOType::CornerNormal);
        /* Remove IBO Tris, Lines, and Points - these are used by sculpt custom batches */
        mbc->buff.ibos.remove(IBOType::Tris);
        mbc->buff.ibos.remove(IBOType::Lines);
        mbc->buff.ibos.remove(IBOType::Points);
      }
      /* Re-request batches so they will be recreated on next frame.
       * Don't remove from preserved map - Python may still need these batches. */
      cache.batch_requested |= sculpt_custom_flags;
      /* Also ensure flags are in preserved map so they persist across cache recreation.
       * IMPORTANT: We need to check both mesh.id key (used here) and Object key (used in create_requested).
       * If flags exist with mesh.id key but Object key is available, migrate to Object key.
       * This ensures consistency with DRW_mesh_batch_cache_create_requested which prefers Object key. */
      if (mesh->runtime) {
        const void *mesh_key_id = get_mesh_key(*mesh, nullptr);
        /* Try to find Object key by checking if this mesh is used by any Object.
         * We can't get Object directly here, but we can check if flags exist with Object key
         * by looking for original mesh ID and finding associated Object. */
        /* For now, add flags using mesh.id key, but also check if we need to migrate.
         * The migration will happen in DRW_mesh_batch_cache_create_requested when Object is available. */
        // #region agent log
        {
          static unsigned long long counter = 0;
          FILE *f = fopen("i:\\Blender_DAD\\blender\\.cursor\\debug.log", "a");
          if (f) {
            fprintf(f, "{\"id\":\"log_%llu\",\"location\":\"draw_cache_impl_mesh.cc:700\",\"message\":\"INVALIDATION: Adding flags to map (mesh.id key)\",\"data\":{\"mesh_key_id\":\"%p\",\"flags\":\"0x%llx\",\"map_size_before\":%zu},\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"B\"}\n",
                    counter++, mesh_key_id, (unsigned long long)sculpt_custom_flags, sculpt_custom_flags_preserved.size());
            fclose(f);
          }
        }
        // #endregion
        sculpt_custom_flags_preserved.add_or_modify(
            mesh_key_id,
            [&](DRWBatchFlag *value) { *value = sculpt_custom_flags; },
            [&](DRWBatchFlag *value) { *value |= sculpt_custom_flags; });
        // #region agent log
        {
          static unsigned long long counter = 0;
          FILE *f = fopen("i:\\Blender_DAD\\blender\\.cursor\\debug.log", "a");
          if (f) {
            fprintf(f, "{\"id\":\"log_%llu\",\"location\":\"draw_cache_impl_mesh.cc:710\",\"message\":\"INVALIDATION: Flags added to map (mesh.id key)\",\"data\":{\"mesh_key_id\":\"%p\",\"map_size_after\":%zu,\"batch_ready_after\":\"0x%llx\",\"batch_requested_after\":\"0x%llx\"},\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"B\"}\n",
                    counter++, mesh_key_id, sculpt_custom_flags_preserved.size(), (unsigned long long)cache.batch_ready, (unsigned long long)cache.batch_requested);
            fclose(f);
          }
        }
        // #endregion
      }
      break;
    }
    default:
      BLI_assert(0);
  }
}

static void mesh_buffer_cache_clear(MeshBufferCache *mbc)
{
  mbc->buff.ibos.clear();
  mbc->buff.vbos.clear();

  mbc->loose_geom = {};
  mbc->face_sorted = {};
}

static void mesh_batch_cache_free_subdiv_cache(MeshBatchCache &cache)
{
  if (cache.subdiv_cache) {
    draw_subdiv_cache_free(*cache.subdiv_cache);
    MEM_delete(cache.subdiv_cache);
    cache.subdiv_cache = nullptr;
  }
}

static void mesh_batch_cache_clear(MeshBatchCache &cache)
{
  FOREACH_MESH_BUFFER_CACHE (cache, mbc) {
    mesh_buffer_cache_clear(mbc);
  }

  cache.tris_per_mat = {};

  /* Clear sculpt custom batch pointers first to prevent access violations.
   * These batches may be shared with evaluated mesh cache and already freed.
   * We need to save pointers before clearing to skip them in the loop below. */
  gpu::Batch *sculpt_custom_triangles = cache.batch.sculpt_custom_triangles;
  gpu::Batch *sculpt_custom_edges = cache.batch.sculpt_custom_edges;
  gpu::Batch *sculpt_custom_vertices = cache.batch.sculpt_custom_vertices;
  cache.batch.sculpt_custom_triangles = nullptr;
  cache.batch.sculpt_custom_edges = nullptr;
  cache.batch.sculpt_custom_vertices = nullptr;

  for (int i = 0; i < sizeof(cache.batch) / sizeof(void *); i++) {
    gpu::Batch **batch = reinterpret_cast<gpu::Batch **>(&cache.batch);
    /* Skip sculpt custom batches - they may be shared with evaluated mesh cache
     * and already freed. We clear pointers but don't try to free them here. */
    if (batch[i] == sculpt_custom_triangles ||
        batch[i] == sculpt_custom_edges ||
        batch[i] == sculpt_custom_vertices) {
      continue;
    }
    /* GPU_BATCH_DISCARD_SAFE already checks for nullptr, so it's safe to call */
    GPU_BATCH_DISCARD_SAFE(batch[i]);
  }
  for (const int i : cache.surface_per_mat.index_range()) {
    GPU_BATCH_DISCARD_SAFE(cache.surface_per_mat[i]);
  }

  mesh_batch_cache_discard_shaded_tri(cache);
  mesh_batch_cache_discard_uvedit(cache);
  cache.surface_per_mat = {};
  cache.mat_len = 0;

  cache.batch_ready = DRWBatchFlag(0);
  drw_mesh_weight_state_clear(&cache.weight_state);

  mesh_batch_cache_free_subdiv_cache(cache);
}

void DRW_mesh_batch_cache_free(draw::MeshBatchCache *batch_cache)
{
  mesh_batch_cache_clear(*batch_cache);
  MEM_delete(batch_cache);
}

/* Clear sculpt custom flags from static map for a specific mesh/object.
 * Called when exiting sculpt mode to prevent stale entries.
 * Implementation of public API in DRW_engine.hh. */
void DRW_mesh_batch_cache_clear_sculpt_custom_flags(const Mesh &mesh, const Object *ob)
{
  const void *mesh_key = get_mesh_key(mesh, ob);
  if (sculpt_custom_flags_preserved.contains(mesh_key)) {
    fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] DRW_mesh_batch_cache_clear_sculpt_custom_flags: "
                    "clearing flags for mesh_key=%p\n", mesh_key);
    sculpt_custom_flags_preserved.remove(mesh_key);
  }
}

/** \} */

/* ---------------------------------------------------------------------- */
/** \name Public API
 * \{ */

static void texpaint_request_active_uv(MeshBatchCache &cache, Object &object, Mesh &mesh)
{
  mesh_cd_calc_active_uv_layer(object, mesh, cache.cd_needed);

  BLI_assert(!cache.cd_needed.uv.is_empty() &&
             "No uv layer available in texpaint, but batches requested anyway!");

  mesh_cd_calc_active_mask_uv_layer(object, mesh, cache.cd_needed);
}

static void request_active_and_default_color_attributes(const Object &object,
                                                        const Mesh &mesh,
                                                        VectorSet<std::string> &attributes)
{
  const Mesh &me_final = editmesh_final_or_this(object, mesh);

  auto request_color_attribute = [&](const StringRef name) {
    if (!name.is_empty()) {
      if (attribute_exists(me_final, name)) {
        drw_attributes_add_request(&attributes, name);
      }
    }
  };

  request_color_attribute(me_final.active_color_attribute);
  request_color_attribute(me_final.default_color_attribute);
}

gpu::Batch *DRW_mesh_batch_cache_get_all_verts(Mesh &mesh)
{
  MeshBatchCache &cache = *mesh_batch_cache_get(mesh);
  cache.batch_requested |= MBC_ALL_VERTS;
  return DRW_batch_request(&cache.batch.all_verts);
}

gpu::Batch *DRW_mesh_batch_cache_get_paint_overlay_verts(Mesh &mesh)
{
  MeshBatchCache &cache = *mesh_batch_cache_get(mesh);
  cache.batch_requested |= MBC_PAINT_OVERLAY_VERTS;
  return DRW_batch_request(&cache.batch.paint_overlay_verts);
}

gpu::Batch *DRW_mesh_batch_cache_get_all_edges(Mesh &mesh)
{
  MeshBatchCache &cache = *mesh_batch_cache_get(mesh);
  cache.batch_requested |= MBC_ALL_EDGES;
  return DRW_batch_request(&cache.batch.all_edges);
}

gpu::Batch *DRW_mesh_batch_cache_get_surface(Mesh &mesh)
{
  MeshBatchCache &cache = *mesh_batch_cache_get(mesh);
  mesh_batch_cache_request_surface_batches(mesh, cache);

  return cache.batch.surface;
}

gpu::Batch *DRW_mesh_batch_cache_get_paint_overlay_surface(Mesh &mesh)
{
  MeshBatchCache &cache = *mesh_batch_cache_get(mesh);
  cache.batch_requested |= MBC_PAINT_OVERLAY_SURFACE;
  return DRW_batch_request(&cache.batch.paint_overlay_surface);
}

gpu::Batch *DRW_mesh_batch_cache_get_loose_edges(Mesh &mesh)
{
  MeshBatchCache &cache = *mesh_batch_cache_get(mesh);
  if (cache.no_loose_wire) {
    return nullptr;
  }
  cache.batch_requested |= MBC_LOOSE_EDGES;
  return DRW_batch_request(&cache.batch.loose_edges);
}

gpu::Batch *DRW_mesh_batch_cache_get_surface_weights(Mesh &mesh)
{
  MeshBatchCache &cache = *mesh_batch_cache_get(mesh);
  cache.batch_requested |= MBC_SURFACE_WEIGHTS;
  return DRW_batch_request(&cache.batch.surface_weights);
}

gpu::Batch *DRW_mesh_batch_cache_get_edge_detection(Mesh &mesh, bool *r_is_manifold)
{
  MeshBatchCache &cache = *mesh_batch_cache_get(mesh);
  cache.batch_requested |= MBC_EDGE_DETECTION;
  /* Even if is_manifold is not correct (not updated),
   * the default (not manifold) is just the worst case. */
  if (r_is_manifold) {
    *r_is_manifold = cache.is_manifold;
  }
  return DRW_batch_request(&cache.batch.edge_detection);
}

gpu::Batch *DRW_mesh_batch_cache_get_wireframes_face(Mesh &mesh)
{
  MeshBatchCache &cache = *mesh_batch_cache_get(mesh);
  cache.batch_requested |= (MBC_WIRE_EDGES);
  return DRW_batch_request(&cache.batch.wire_edges);
}

gpu::Batch *DRW_mesh_batch_cache_get_edit_mesh_analysis(Mesh &mesh)
{
  MeshBatchCache &cache = *mesh_batch_cache_get(mesh);
  cache.batch_requested |= MBC_EDIT_MESH_ANALYSIS;
  return DRW_batch_request(&cache.batch.edit_mesh_analysis);
}

void DRW_mesh_get_attributes(const Object &object,
                             const Mesh &mesh,
                             const Span<const GPUMaterial *> materials,
                             VectorSet<std::string> *r_attrs,
                             DRW_MeshCDMask *r_cd_needed)
{
  mesh_cd_calc_used_gpu_layers(object, mesh, materials, r_attrs, r_cd_needed);
}

Span<gpu::Batch *> DRW_mesh_batch_cache_get_surface_shaded(
    Object &object, Mesh &mesh, const Span<const GPUMaterial *> materials)
{
  MeshBatchCache &cache = *mesh_batch_cache_get(mesh);
  mesh_cd_calc_used_gpu_layers(object, mesh, materials, &cache.attr_needed, &cache.cd_needed);

  BLI_assert(materials.size() == cache.mat_len);

  mesh_batch_cache_request_surface_batches(mesh, cache);
  return cache.surface_per_mat;
}

Span<gpu::Batch *> DRW_mesh_batch_cache_get_surface_texpaint(Object &object, Mesh &mesh)
{
  MeshBatchCache &cache = *mesh_batch_cache_get(mesh);
  texpaint_request_active_uv(cache, object, mesh);
  mesh_batch_cache_request_surface_batches(mesh, cache);
  return cache.surface_per_mat;
}

gpu::Batch *DRW_mesh_batch_cache_get_surface_texpaint_single(Object &object, Mesh &mesh)
{
  MeshBatchCache &cache = *mesh_batch_cache_get(mesh);
  texpaint_request_active_uv(cache, object, mesh);
  mesh_batch_cache_request_surface_batches(mesh, cache);
  return cache.batch.surface;
}

gpu::Batch *DRW_mesh_batch_cache_get_surface_vertpaint(Object &object, Mesh &mesh)
{
  MeshBatchCache &cache = *mesh_batch_cache_get(mesh);

  VectorSet<std::string> attrs_needed{};
  request_active_and_default_color_attributes(object, mesh, attrs_needed);

  drw_attributes_merge(&cache.attr_needed, &attrs_needed);

  mesh_batch_cache_request_surface_batches(mesh, cache);
  return cache.batch.surface;
}

gpu::Batch *DRW_mesh_batch_cache_get_surface_sculpt(Object &object, Mesh &mesh)
{
  MeshBatchCache &cache = *mesh_batch_cache_get(mesh);

  VectorSet<std::string> attrs_needed{};
  request_active_and_default_color_attributes(object, mesh, attrs_needed);

  drw_attributes_merge(&cache.attr_needed, &attrs_needed);

  mesh_batch_cache_request_surface_batches(mesh, cache);
  return cache.batch.surface;
}

gpu::Batch *DRW_mesh_batch_cache_get_sculpt_overlays(Mesh &mesh)
{
  MeshBatchCache &cache = *mesh_batch_cache_get(mesh);

  cache.cd_needed.sculpt_overlays = true;
  cache.batch_requested |= (MBC_SCULPT_OVERLAYS);
  DRW_batch_request(&cache.batch.sculpt_overlays);

  return cache.batch.sculpt_overlays;
}

gpu::Batch *DRW_mesh_batch_cache_get_surface_viewer_attribute(Mesh &mesh)
{
  MeshBatchCache &cache = *mesh_batch_cache_get(mesh);

  cache.batch_requested |= (MBC_VIEWER_ATTRIBUTE_OVERLAY);
  DRW_batch_request(&cache.batch.surface_viewer_attribute);

  return cache.batch.surface_viewer_attribute;
}

/** \} */

/* ---------------------------------------------------------------------- */
/** \name Sculpt Mode Custom Overlay API (for Python addons)
 * \{ */

/**
 * Check if a batch is valid and safe to draw.
 * Returns true if the batch has valid data (non-zero vertex or index count).
 * 
 * Safely handles cases where batch may have been freed (e.g., after cache recreation)
 * by catching access violations on Windows using SEH.
 */
static bool is_batch_valid_for_drawing(gpu::Batch *batch)
{
  if (!batch) {
    fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] is_batch_valid_for_drawing: batch is null\n");
    return false;
  }

#ifdef _WIN32
  /* Use SEH to catch access violations on Windows. */
  __try {
#endif
    /* Check procedural batch. */
    if (batch->procedural_vertices >= 0) {
      const bool valid = batch->procedural_vertices > 0;
      fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] is_batch_valid_for_drawing: procedural batch, "
                      "procedural_vertices=%d, valid=%d\n",
              batch->procedural_vertices, valid);
      return valid;
    }

    /* Check indexed batch. */
    if (batch->elem) {
      gpu::IndexBuf *elem = batch->elem_();
      if (!elem) {
        fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] is_batch_valid_for_drawing: elem_() returned null\n");
        return false;
      }
      const uint32_t index_len = elem->index_len_get();
      const bool is_init = elem->is_init();
      const bool valid = index_len > 0 && is_init;
      fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] is_batch_valid_for_drawing: indexed batch, "
                      "index_len=%u, is_init=%d, valid=%d\n",
              index_len, is_init, valid);
      return valid;
    }

    /* Check vertex-only batch. */
    if (batch->verts_(0)) {
      const uint vertex_len = GPU_vertbuf_get_vertex_len(batch->verts_(0));
      const bool valid = vertex_len > 0;
      fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] is_batch_valid_for_drawing: vertex-only batch, "
                      "vertex_len=%u, valid=%d\n",
              vertex_len, valid);
      return valid;
    }

    /* Batch has no valid data. */
    fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] is_batch_valid_for_drawing: batch has no valid data "
                    "(no elem, no verts[0])\n");
    return false;
#ifdef _WIN32
  }
  __except (EXCEPTION_EXECUTE_HANDLER) {
    /* Batch was likely freed - return false to indicate invalid batch. */
    fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] is_batch_valid_for_drawing: access violation "
                    "accessing batch (likely freed), returning false\n");
    return false;
  }
#endif
}

/* ---------------------------------------------------------------------- */
/** \name Custom Overlay API Implementation
 * \{ */

/** Determine overlay mode based on object mode. */
CustomOverlayMode get_custom_overlay_mode(const Object *ob)
{
  if (!ob) {
    return CustomOverlayMode::ModeNone;
  }

  switch (ob->mode) {
    case OB_MODE_EDIT:
      return CustomOverlayMode::ModeEdit;
    case OB_MODE_SCULPT:
    case OB_MODE_WEIGHT_PAINT:
    case OB_MODE_VERTEX_PAINT:
    case OB_MODE_TEXTURE_PAINT:
      return CustomOverlayMode::ModeSculpt;
    default:
      return CustomOverlayMode::ModeNone;
  }
}

gpu::Batch *DRW_mesh_batch_cache_get_custom_overlay(
    Mesh &mesh,
    Object *ob,
    CustomOverlayType type)
{
  /* If mesh has no runtime, we cannot access batch cache. This can happen
   * when mesh is being created or destroyed. */
  if (!mesh.runtime) {
    fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] get_custom_overlay: mesh.runtime is nullptr!\n");
    return nullptr;
  }

  CustomOverlayMode mode = get_custom_overlay_mode(ob);
  
  DRW_mesh_batch_cache_validate(mesh);

  fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] get_custom_overlay: post-validate mesh=%p runtime=%p batch_cache=%p\n",
          &mesh, mesh.runtime, mesh.runtime ? mesh.runtime->batch_cache : nullptr);
  
  if (mode == CustomOverlayMode::ModeNone) {
    return nullptr;
  }
  
  /* Now get the cache (guaranteed to proceed if validate succeeded, or null if failed) */
  MeshBatchCache *cache = mesh_batch_cache_get(mesh);
  if (!cache) {
    fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] DRW_mesh_batch_cache_get_custom_overlay: failed to get cache after validation! mesh=%p\n", &mesh);
    return nullptr;
  }
  
  gpu::Batch **batch_ptr = nullptr;
  DRWBatchFlag flag = DRWBatchFlag(0);
  
  switch (mode) {
    case CustomOverlayMode::ModeEdit: {
      switch (type) {
        case CustomOverlayType::TypeTriangles:
          batch_ptr = &cache->batch.edit_triangles;
          flag = MBC_EDIT_TRIANGLES;
          break;
        case CustomOverlayType::TypeEdges:
          batch_ptr = &cache->batch.edit_edges;
          flag = MBC_EDIT_EDGES;
          break;
        case CustomOverlayType::TypeVertices:
          batch_ptr = &cache->batch.edit_vertices;
          flag = MBC_EDIT_VERTICES;
          break;
      }
      break;
    }
    
    case CustomOverlayMode::ModeSculpt: {
      switch (type) {
        case CustomOverlayType::TypeTriangles:
          batch_ptr = &cache->batch.sculpt_custom_triangles;
          flag = MBC_SCULPT_CUSTOM_TRIANGLES;
          break;
        case CustomOverlayType::TypeEdges:
          batch_ptr = &cache->batch.sculpt_custom_edges;
          flag = MBC_SCULPT_CUSTOM_EDGES;
          break;
        case CustomOverlayType::TypeVertices:
          batch_ptr = &cache->batch.sculpt_custom_vertices;
          flag = MBC_SCULPT_CUSTOM_VERTICES;
          break;
      }
      break;
    }
    
    case CustomOverlayMode::ModeNone:
    default:
      return nullptr;
  }
  
  if (!batch_ptr) {
    fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] DRW_mesh_batch_cache_get_custom_overlay: batch_ptr is null (invalid type/mode mix?)\n");
    return nullptr;
  }
  
  cache->batch_requested |= flag;
  
  /* Mark cache as dirty to ensure batches are created on next draw call.
   * This is necessary because the batch creation happens asynchronously in
   * mesh_buffer_cache_create_requested which is called by the render engine.
   * By marking dirty, we ensure the batches will be created on the next frame. */
  cache->is_dirty = true;
  
  /* For Sculpt Custom batches, we also need to ensure flags are preserved in static map */
  if (mode == CustomOverlayMode::ModeSculpt) {
    const void *mesh_key = get_mesh_key(mesh, ob);
    fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] DRW_mesh_batch_cache_get_custom_overlay: adding flag 0x%llx for key %p, marked dirty\n",
            uint64_t(flag), mesh_key);
    sculpt_custom_flags_preserved.add_or_modify(
        mesh_key,
        [&](DRWBatchFlag *value) { *value = flag; },
        [&](DRWBatchFlag *value) { *value |= flag; });
  }

  return DRW_batch_request(batch_ptr);
}

/* Wrapper functions implementation */
gpu::Batch *DRW_mesh_batch_cache_get_custom_overlay_triangles(Mesh &mesh, Object *ob)
{
  return DRW_mesh_batch_cache_get_custom_overlay(mesh, ob, CustomOverlayType::TypeTriangles);
}

gpu::Batch *DRW_mesh_batch_cache_get_custom_overlay_edges(Mesh &mesh, Object *ob)
{
  return DRW_mesh_batch_cache_get_custom_overlay(mesh, ob, CustomOverlayType::TypeEdges);
}

gpu::Batch *DRW_mesh_batch_cache_get_custom_overlay_vertices(Mesh &mesh, Object *ob)
{
  return DRW_mesh_batch_cache_get_custom_overlay(mesh, ob, CustomOverlayType::TypeVertices);
}

/** \} */

/* ---------------------------------------------------------------------- */
/** \name Legacy Sculpt Mode API (redirects to universal API)
 * \{ */

gpu::Batch *DRW_mesh_batch_cache_get_sculpt_custom_triangles(Mesh &mesh, Object *ob)
{
  return DRW_mesh_batch_cache_get_custom_overlay(mesh, ob, CustomOverlayType::TypeTriangles);
}


gpu::Batch *DRW_mesh_batch_cache_get_sculpt_custom_edges(Mesh &mesh, Object *ob)
{
  return DRW_mesh_batch_cache_get_custom_overlay(mesh, ob, CustomOverlayType::TypeEdges);
}
  

  

  


gpu::Batch *DRW_mesh_batch_cache_get_sculpt_custom_vertices(Mesh &mesh, Object *ob)
{
  return DRW_mesh_batch_cache_get_custom_overlay(mesh, ob, CustomOverlayType::TypeVertices);
}

/** \} */
  


/* ---------------------------------------------------------------------- */
/** \name Edit Mode API
 * \{ */

gpu::Batch *DRW_mesh_batch_cache_get_edit_triangles(Mesh &mesh)
{
  MeshBatchCache &cache = *mesh_batch_cache_get(mesh);
  cache.batch_requested |= MBC_EDIT_TRIANGLES;
  return DRW_batch_request(&cache.batch.edit_triangles);
}

gpu::Batch *DRW_mesh_batch_cache_get_edit_edges(Mesh &mesh)
{
  MeshBatchCache &cache = *mesh_batch_cache_get(mesh);
  cache.batch_requested |= MBC_EDIT_EDGES;
  return DRW_batch_request(&cache.batch.edit_edges);
}

gpu::Batch *DRW_mesh_batch_cache_get_edit_vertices(Mesh &mesh)
{
  MeshBatchCache &cache = *mesh_batch_cache_get(mesh);
  cache.batch_requested |= MBC_EDIT_VERTICES;
  return DRW_batch_request(&cache.batch.edit_vertices);
}

gpu::Batch *DRW_mesh_batch_cache_get_edit_vert_normals(Mesh &mesh)
{
  MeshBatchCache &cache = *mesh_batch_cache_get(mesh);
  cache.batch_requested |= MBC_EDIT_VNOR;
  return DRW_batch_request(&cache.batch.edit_vnor);
}

gpu::Batch *DRW_mesh_batch_cache_get_edit_loop_normals(Mesh &mesh)
{
  MeshBatchCache &cache = *mesh_batch_cache_get(mesh);
  cache.batch_requested |= MBC_EDIT_LNOR;
  return DRW_batch_request(&cache.batch.edit_lnor);
}

gpu::Batch *DRW_mesh_batch_cache_get_edit_facedots(Mesh &mesh)
{
  MeshBatchCache &cache = *mesh_batch_cache_get(mesh);
  cache.batch_requested |= MBC_EDIT_FACEDOTS;
  return DRW_batch_request(&cache.batch.edit_fdots);
}

gpu::Batch *DRW_mesh_batch_cache_get_edit_skin_roots(Mesh &mesh)
{
  MeshBatchCache &cache = *mesh_batch_cache_get(mesh);
  cache.batch_requested |= MBC_SKIN_ROOTS;
  return DRW_batch_request(&cache.batch.edit_skin_roots);
}

/** \} */

/* ---------------------------------------------------------------------- */
/** \name Edit Mode selection API
 * \{ */

gpu::Batch *DRW_mesh_batch_cache_get_triangles_with_select_id(Mesh &mesh)
{
  MeshBatchCache &cache = *mesh_batch_cache_get(mesh);
  cache.batch_requested |= MBC_EDIT_SELECTION_FACES;
  return DRW_batch_request(&cache.batch.edit_selection_faces);
}

gpu::Batch *DRW_mesh_batch_cache_get_facedots_with_select_id(Mesh &mesh)
{
  MeshBatchCache &cache = *mesh_batch_cache_get(mesh);
  cache.batch_requested |= MBC_EDIT_SELECTION_FACEDOTS;
  return DRW_batch_request(&cache.batch.edit_selection_fdots);
}

gpu::Batch *DRW_mesh_batch_cache_get_edges_with_select_id(Mesh &mesh)
{
  MeshBatchCache &cache = *mesh_batch_cache_get(mesh);
  cache.batch_requested |= MBC_EDIT_SELECTION_EDGES;
  return DRW_batch_request(&cache.batch.edit_selection_edges);
}

gpu::Batch *DRW_mesh_batch_cache_get_verts_with_select_id(Mesh &mesh)
{
  MeshBatchCache &cache = *mesh_batch_cache_get(mesh);
  cache.batch_requested |= MBC_EDIT_SELECTION_VERTS;
  return DRW_batch_request(&cache.batch.edit_selection_verts);
}

/** \} */

/* ---------------------------------------------------------------------- */
/** \name UV Image editor API
 * \{ */

static void edituv_request_active_uv(MeshBatchCache &cache, Object &object, Mesh &mesh)
{
  mesh_cd_calc_active_uv_layer(object, mesh, cache.cd_needed);
  mesh_cd_calc_edit_uv_layer(mesh, &cache.cd_needed);

  mesh_cd_calc_active_mask_uv_layer(object, mesh, cache.cd_needed);
}

gpu::Batch *DRW_mesh_batch_cache_get_edituv_faces_stretch_area(Object &object,
                                                               Mesh &mesh,
                                                               float **tot_area,
                                                               float **tot_uv_area)
{
  MeshBatchCache &cache = *mesh_batch_cache_get(mesh);
  edituv_request_active_uv(cache, object, mesh);
  cache.batch_requested |= MBC_EDITUV_FACES_STRETCH_AREA;

  if (tot_area != nullptr) {
    *tot_area = &cache.tot_area;
  }
  if (tot_uv_area != nullptr) {
    *tot_uv_area = &cache.tot_uv_area;
  }
  return DRW_batch_request(&cache.batch.edituv_faces_stretch_area);
}

gpu::Batch *DRW_mesh_batch_cache_get_edituv_faces_stretch_angle(Object &object, Mesh &mesh)
{
  MeshBatchCache &cache = *mesh_batch_cache_get(mesh);
  edituv_request_active_uv(cache, object, mesh);
  cache.batch_requested |= MBC_EDITUV_FACES_STRETCH_ANGLE;
  return DRW_batch_request(&cache.batch.edituv_faces_stretch_angle);
}

gpu::Batch *DRW_mesh_batch_cache_get_edituv_faces(Object &object, Mesh &mesh)
{
  MeshBatchCache &cache = *mesh_batch_cache_get(mesh);
  edituv_request_active_uv(cache, object, mesh);
  cache.batch_requested |= MBC_EDITUV_FACES;
  return DRW_batch_request(&cache.batch.edituv_faces);
}

gpu::Batch *DRW_mesh_batch_cache_get_edituv_edges(Object &object, Mesh &mesh)
{
  MeshBatchCache &cache = *mesh_batch_cache_get(mesh);
  edituv_request_active_uv(cache, object, mesh);
  cache.batch_requested |= MBC_EDITUV_EDGES;
  return DRW_batch_request(&cache.batch.edituv_edges);
}

gpu::Batch *DRW_mesh_batch_cache_get_edituv_verts(Object &object, Mesh &mesh)
{
  MeshBatchCache &cache = *mesh_batch_cache_get(mesh);
  edituv_request_active_uv(cache, object, mesh);
  cache.batch_requested |= MBC_EDITUV_VERTS;
  return DRW_batch_request(&cache.batch.edituv_verts);
}

gpu::Batch *DRW_mesh_batch_cache_get_edituv_facedots(Object &object, Mesh &mesh)
{
  MeshBatchCache &cache = *mesh_batch_cache_get(mesh);
  edituv_request_active_uv(cache, object, mesh);
  cache.batch_requested |= MBC_EDITUV_FACEDOTS;
  return DRW_batch_request(&cache.batch.edituv_fdots);
}

gpu::Batch *DRW_mesh_batch_cache_get_uv_faces(Object &object, Mesh &mesh)
{
  MeshBatchCache &cache = *mesh_batch_cache_get(mesh);
  edituv_request_active_uv(cache, object, mesh);
  cache.batch_requested |= MBC_UV_FACES;
  return DRW_batch_request(&cache.batch.uv_faces);
}

gpu::Batch *DRW_mesh_batch_cache_get_all_uv_wireframe(Object &object, Mesh &mesh)
{
  MeshBatchCache &cache = *mesh_batch_cache_get(mesh);
  edituv_request_active_uv(cache, object, mesh);
  cache.batch_requested |= MBC_WIRE_LOOPS_ALL_UVS;
  return DRW_batch_request(&cache.batch.wire_loops_all_uvs);
}

gpu::Batch *DRW_mesh_batch_cache_get_uv_wireframe(Object &object, Mesh &mesh)
{
  MeshBatchCache &cache = *mesh_batch_cache_get(mesh);
  edituv_request_active_uv(cache, object, mesh);
  cache.batch_requested |= MBC_WIRE_LOOPS_UVS;
  return DRW_batch_request(&cache.batch.wire_loops_uvs);
}

gpu::Batch *DRW_mesh_batch_cache_get_edituv_wireframe(Object &object, Mesh &mesh)
{
  MeshBatchCache &cache = *mesh_batch_cache_get(mesh);
  edituv_request_active_uv(cache, object, mesh);
  cache.batch_requested |= MBC_WIRE_LOOPS_EDITUVS;
  return DRW_batch_request(&cache.batch.wire_loops_edituvs);
}

gpu::Batch *DRW_mesh_batch_cache_get_paint_overlay_edges(Mesh &mesh)
{
  MeshBatchCache &cache = *mesh_batch_cache_get(mesh);
  cache.batch_requested |= MBC_PAINT_OVERLAY_WIRE_LOOPS;
  return DRW_batch_request(&cache.batch.paint_overlay_wire_loops);
}

/** \} */

/* ---------------------------------------------------------------------- */
/** \name Grouped batch generation
 * \{ */

void DRW_mesh_batch_cache_free_old(Mesh *mesh, int ctime)
{
  MeshBatchCache *cache = mesh->runtime->batch_cache;

  if (cache == nullptr) {
    return;
  }

  if (cache->cd_used_over_time == cache->cd_used) {
    cache->lastmatch = ctime;
  }

  if (drw_attributes_overlap(&cache->attr_used_over_time, &cache->attr_used)) {
    cache->lastmatch = ctime;
  }

  if (ctime - cache->lastmatch > U.vbotimeout) {
    mesh_batch_cache_discard_shaded_tri(*cache);
  }

  cache->cd_used_over_time.clear();
  cache->attr_used_over_time.clear();
}

static void init_empty_dummy_batch(gpu::Batch &batch)
{
  /* The dummy batch is only used in cases with invalid edit mode mapping, so the overhead of
   * creating a vertex buffer shouldn't matter. */
  GPUVertFormat format{};
  GPU_vertformat_attr_add(&format, "dummy", gpu::VertAttrType::SFLOAT_32);
  gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
  GPU_vertbuf_data_alloc(*vbo, 1);
  /* Avoid the batch being rendered at all. */
  GPU_vertbuf_data_len_set(*vbo, 0);

  GPU_batch_vertbuf_add(&batch, vbo, true);
}

void DRW_mesh_batch_cache_create_requested(TaskGraph &task_graph,
                                           Object &ob,
                                           Mesh &mesh,
                                           const Scene &scene,
                                           const bool is_paint_mode,
                                           const bool use_hide)
{
  const ToolSettings *ts = scene.toolsettings;

  /* Validate cache before accessing it - this may recreate the cache! */
  DRW_mesh_batch_cache_validate(mesh);
  
  MeshBatchCache &cache = *mesh_batch_cache_get(mesh);
  bool cd_uv_update = false;
  
  /* Restore sculpt custom flags from static map if they were set by Python for a different mesh instance */
  const DRWBatchFlag sculpt_custom_flags = MBC_SCULPT_CUSTOM_TRIANGLES | MBC_SCULPT_CUSTOM_EDGES |
                                           MBC_SCULPT_CUSTOM_VERTICES;
  /* Try to find preserved flags using both Object key (preferred) and mesh.id key (fallback).
   * This is needed because:
   * - Python API uses Object key (preferred)
   * - DRW_mesh_batch_cache_dirty_tag uses mesh.id key (when Object is not available)
   * - mesh_batch_cache_init uses mesh.id key (when Object is not available) */
  const void *mesh_key_object = get_mesh_key(mesh, &ob);
  const void *mesh_key_id = get_mesh_key(mesh, nullptr);
  DRWBatchFlag preserved = (DRWBatchFlag)0;
  const void *found_key = nullptr;
  
  // #region agent log
  {
    static unsigned long long counter = 0;
    FILE *f = fopen("i:\\Blender_DAD\\blender\\.cursor\\debug.log", "a");
    if (f) {
      fprintf(f, "{\"id\":\"log_%llu\",\"location\":\"draw_cache_impl_mesh.cc:1536\",\"message\":\"LOOKUP: Checking map for flags\",\"data\":{\"mesh_key_object\":\"%p\",\"mesh_key_id\":\"%p\",\"map_size\":%zu,\"contains_object\":%s,\"contains_id\":%s},\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"B\"}\n",
              counter++, mesh_key_object, mesh_key_id, sculpt_custom_flags_preserved.size(),
              sculpt_custom_flags_preserved.contains(mesh_key_object) ? "true" : "false",
              sculpt_custom_flags_preserved.contains(mesh_key_id) ? "true" : "false");
      fclose(f);
    }
  }
  // #endregion
  /* First try Object key (preferred) */
  if (sculpt_custom_flags_preserved.contains(mesh_key_object)) {
    preserved = sculpt_custom_flags_preserved.lookup(mesh_key_object);
    found_key = mesh_key_object;
    fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] DRW_mesh_batch_cache_create_requested: found in map using Object key=%p, preserved=0x%llx\n",
            found_key, uint64_t(preserved));
  }
  /* Fallback to mesh.id key if Object key not found */
  else if (mesh_key_id != mesh_key_object && sculpt_custom_flags_preserved.contains(mesh_key_id)) {
    preserved = sculpt_custom_flags_preserved.lookup(mesh_key_id);
    found_key = mesh_key_id;
    fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] DRW_mesh_batch_cache_create_requested: found in map using mesh.id key=%p, preserved=0x%llx\n",
            found_key, uint64_t(preserved));
    /* Migrate to Object key for future lookups (preferred) */
    sculpt_custom_flags_preserved.add_or_modify(
        mesh_key_object,
        [&](DRWBatchFlag *value) { *value = preserved; },
        [&](DRWBatchFlag *value) { *value |= preserved; });
    sculpt_custom_flags_preserved.remove(mesh_key_id);
    // #region agent log
    {
      static unsigned long long counter = 0;
      FILE *f = fopen("i:\\Blender_DAD\\blender\\.cursor\\debug.log", "a");
      if (f) {
        fprintf(f, "{\"id\":\"log_%llu\",\"location\":\"draw_cache_impl_mesh.cc:1554\",\"message\":\"MIGRATION: Migrated flags from mesh.id to Object key\",\"data\":{\"mesh_key_id\":\"%p\",\"mesh_key_object\":\"%p\",\"preserved\":\"0x%llx\",\"map_size_after\":%zu},\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"B\"}\n",
                counter++, mesh_key_id, mesh_key_object, (unsigned long long)preserved, sculpt_custom_flags_preserved.size());
        fclose(f);
      }
    }
    // #endregion
    fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] DRW_mesh_batch_cache_create_requested: migrated flags from mesh.id key to Object key\n");
  }
  
  fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] DRW_mesh_batch_cache_create_requested: checking map, "
                  "mesh_key_object=%p, mesh_key_id=%p, map_size=%zu, found_key=%p\n",
          mesh_key_object, mesh_key_id, sculpt_custom_flags_preserved.size(), found_key);
  
  if (found_key != nullptr) {
    
    /* Check which batch_ready flags actually have valid batches.
     * IMPORTANT: Python works with original mesh, so we need to check original mesh cache,
     * not evaluated mesh cache. After cache recreation, batch_ready may be set but batches may be freed. */
    DRWBatchFlag valid_batch_ready = (DRWBatchFlag)0;
    Mesh *orig_mesh = BKE_object_get_original_mesh(&ob);
    MeshBatchCache *orig_cache = nullptr;
    if (orig_mesh && orig_mesh != &mesh && orig_mesh->runtime) {
      DRW_mesh_batch_cache_validate(*orig_mesh);
      orig_cache = mesh_batch_cache_get(*orig_mesh);
    }
    
    /* Check original mesh cache if it exists, otherwise check evaluated mesh cache.
     * IMPORTANT: We need to check BOTH caches because:
     * - Original mesh cache is used by Python API
     * - Evaluated mesh cache is used for batch creation (batches_to_create calculation)
     * - If either cache has invalid batches, we need to clear batch_ready in BOTH */
    
    /* IMPORTANT: Check evaluated mesh cache FIRST, because if batch_ready is cleared there
     * (e.g., after invalidation), batches should NOT be considered valid, even if they are
     * technically valid in original cache. This ensures that after invalidation, batches
     * are recreated even if they still exist in original cache. */
    
    /* First check original mesh cache */
    // #region agent log
    {
      static unsigned long long counter = 0;
      FILE *f = fopen("i:\\Blender_DAD\\blender\\.cursor\\debug.log", "a");
      if (f) {
        fprintf(f, "{\"id\":\"log_%llu\",\"location\":\"draw_cache_impl_mesh.cc:1540\",\"message\":\"VALIDATION: Starting batch validation\",\"data\":{\"orig_cache\":%s,\"orig_batch_ready\":\"0x%llx\",\"eval_batch_ready\":\"0x%llx\",\"preserved\":\"0x%llx\"},\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"C\"}\n",
                counter++, orig_cache ? "true" : "false",
                orig_cache ? (unsigned long long)orig_cache->batch_ready : 0ULL,
                (unsigned long long)cache.batch_ready, (unsigned long long)preserved);
        fclose(f);
      }
    }
    // #endregion
    
    /* CRITICAL: If original cache has batch_ready cleared (indicating invalidation),
     * we MUST also clear batch_ready in evaluated cache, even if batches still exist.
     * This ensures that after invalidation, batches are recreated even if they are
     * technically valid in evaluated cache.
     * IMPORTANT: We must also clear batch pointers to prevent re-initialization of
     * already initialized batches, which would cause GPU_batch_init_ex() assert. */
    if (orig_cache && (orig_cache->batch_ready & sculpt_custom_flags) == 0) {
      /* Original cache has been invalidated - clear batch_ready in evaluated cache too */
      DRWBatchFlag eval_batch_ready_before = cache.batch_ready;
      /* Clear batch pointers BEFORE clearing batch_ready to prevent re-initialization */
      if ((eval_batch_ready_before & MBC_SCULPT_CUSTOM_TRIANGLES) != 0) {
        cache.batch.sculpt_custom_triangles = nullptr;
      }
      if ((eval_batch_ready_before & MBC_SCULPT_CUSTOM_EDGES) != 0) {
        cache.batch.sculpt_custom_edges = nullptr;
      }
      if ((eval_batch_ready_before & MBC_SCULPT_CUSTOM_VERTICES) != 0) {
        cache.batch.sculpt_custom_vertices = nullptr;
      }
      cache.batch_ready &= ~sculpt_custom_flags;
      
      /* CRITICAL: Also discard VBO/IBO buffers in evaluated cache so they will be
       * recreated with new geometry data. This ensures that when batches are recreated
       * in evaluated cache, they use fresh VBO/IBO with updated geometry, not stale buffers. */
      FOREACH_MESH_BUFFER_CACHE (cache, mbc) {
        mbc->buff.vbos.remove(VBOType::Position);
        mbc->buff.vbos.remove(VBOType::CornerNormal);
        mbc->buff.ibos.remove(IBOType::Tris);
        mbc->buff.ibos.remove(IBOType::Lines);
        mbc->buff.ibos.remove(IBOType::Points);
      }
      // #region agent log
      {
        static unsigned long long counter = 0;
        FILE *f = fopen("i:\\Blender_DAD\\blender\\.cursor\\debug.log", "a");
        if (f) {
          fprintf(f, "{\"id\":\"log_%llu\",\"location\":\"draw_cache_impl_mesh.cc:1630\",\"message\":\"INVALIDATION_SYNC: Clearing eval batch_ready and VBO/IBO after orig invalidation\",\"data\":{\"orig_batch_ready\":\"0x%llx\",\"eval_batch_ready_before\":\"0x%llx\",\"eval_batch_ready_after\":\"0x%llx\"},\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"I\"}\n",
                  counter++, (unsigned long long)orig_cache->batch_ready, (unsigned long long)eval_batch_ready_before, (unsigned long long)cache.batch_ready);
          fclose(f);
        }
      }
      // #endregion
    }
    
    /* Check evaluated mesh cache FIRST - if batch_ready is not set here, batches are invalid
     * even if they exist in original cache. This handles the case where invalidation cleared
     * batch_ready in evaluated cache but batches still exist in original cache. */
    if (cache.batch_ready & MBC_SCULPT_CUSTOM_TRIANGLES) {
      bool is_valid = cache.batch.sculpt_custom_triangles && is_batch_valid_for_drawing(cache.batch.sculpt_custom_triangles);
      // #region agent log
      {
        static unsigned long long counter = 0;
        FILE *f = fopen("i:\\Blender_DAD\\blender\\.cursor\\debug.log", "a");
        if (f) {
          fprintf(f, "{\"id\":\"log_%llu\",\"location\":\"draw_cache_impl_mesh.cc:1571\",\"message\":\"VALIDATION: Checking eval TRIANGLES\",\"data\":{\"batch_ptr\":\"%p\",\"is_valid\":%s,\"batch_ready\":\"0x%llx\"},\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"C\"}\n",
                  counter++, cache.batch.sculpt_custom_triangles, is_valid ? "true" : "false",
                  (unsigned long long)cache.batch_ready);
          fclose(f);
        }
      }
      // #endregion
      if (is_valid) {
        valid_batch_ready |= MBC_SCULPT_CUSTOM_TRIANGLES;
      }
      else {
        /* Batch marked as ready but invalid - clear the flag in evaluated cache */
        cache.batch_ready &= ~MBC_SCULPT_CUSTOM_TRIANGLES;
        /* Also clear in original cache if it exists */
        if (orig_cache) {
          orig_cache->batch_ready &= ~MBC_SCULPT_CUSTOM_TRIANGLES;
        }
      }
    }
    if (cache.batch_ready & MBC_SCULPT_CUSTOM_EDGES) {
      bool is_valid = cache.batch.sculpt_custom_edges && is_batch_valid_for_drawing(cache.batch.sculpt_custom_edges);
      // #region agent log
      {
        static unsigned long long counter = 0;
        FILE *f = fopen("i:\\Blender_DAD\\blender\\.cursor\\debug.log", "a");
        if (f) {
          fprintf(f, "{\"id\":\"log_%llu\",\"location\":\"draw_cache_impl_mesh.cc:1584\",\"message\":\"VALIDATION: Checking eval EDGES\",\"data\":{\"batch_ptr\":\"%p\",\"is_valid\":%s},\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"C\"}\n",
                  counter++, cache.batch.sculpt_custom_edges, is_valid ? "true" : "false");
          fclose(f);
        }
      }
      // #endregion
      if (is_valid) {
        valid_batch_ready |= MBC_SCULPT_CUSTOM_EDGES;
      }
      else {
        cache.batch_ready &= ~MBC_SCULPT_CUSTOM_EDGES;
        if (orig_cache) {
          orig_cache->batch_ready &= ~MBC_SCULPT_CUSTOM_EDGES;
        }
      }
    }
    if (cache.batch_ready & MBC_SCULPT_CUSTOM_VERTICES) {
      bool is_valid = cache.batch.sculpt_custom_vertices && is_batch_valid_for_drawing(cache.batch.sculpt_custom_vertices);
      // #region agent log
      {
        static unsigned long long counter = 0;
        FILE *f = fopen("i:\\Blender_DAD\\blender\\.cursor\\debug.log", "a");
        if (f) {
          fprintf(f, "{\"id\":\"log_%llu\",\"location\":\"draw_cache_impl_mesh.cc:1595\",\"message\":\"VALIDATION: Checking eval VERTICES\",\"data\":{\"batch_ptr\":\"%p\",\"is_valid\":%s},\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"C\"}\n",
                  counter++, cache.batch.sculpt_custom_vertices, is_valid ? "true" : "false");
          fclose(f);
        }
      }
      // #endregion
      if (is_valid) {
        valid_batch_ready |= MBC_SCULPT_CUSTOM_VERTICES;
      }
      else {
        cache.batch_ready &= ~MBC_SCULPT_CUSTOM_VERTICES;
        if (orig_cache) {
          orig_cache->batch_ready &= ~MBC_SCULPT_CUSTOM_VERTICES;
        }
      }
    }
    
    /* Also check original mesh cache - but only if evaluated cache has batch_ready set.
     * This ensures that if evaluated cache was invalidated (batch_ready cleared), batches
     * are not considered valid even if they exist in original cache. */
    if (orig_cache) {
      /* Only check original cache if evaluated cache also has batch_ready set.
       * If evaluated cache doesn't have batch_ready, batches are invalid regardless. */
      if (orig_cache->batch_ready & MBC_SCULPT_CUSTOM_TRIANGLES) {
        /* Only consider valid if evaluated cache also has batch_ready set */
        if (cache.batch_ready & MBC_SCULPT_CUSTOM_TRIANGLES) {
          bool is_valid = orig_cache->batch.sculpt_custom_triangles && is_batch_valid_for_drawing(orig_cache->batch.sculpt_custom_triangles);
          // #region agent log
          {
            static unsigned long long counter = 0;
            FILE *f = fopen("i:\\Blender_DAD\\blender\\.cursor\\debug.log", "a");
            if (f) {
              fprintf(f, "{\"id\":\"log_%llu\",\"location\":\"draw_cache_impl_mesh.cc:1543\",\"message\":\"VALIDATION: Checking orig TRIANGLES\",\"data\":{\"batch_ptr\":\"%p\",\"is_valid\":%s,\"batch_ready\":\"0x%llx\"},\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"C\"}\n",
                      counter++, orig_cache->batch.sculpt_custom_triangles, is_valid ? "true" : "false",
                      (unsigned long long)orig_cache->batch_ready);
              fclose(f);
            }
          }
          // #endregion
          if (is_valid) {
            valid_batch_ready |= MBC_SCULPT_CUSTOM_TRIANGLES;
          }
          else {
            orig_cache->batch_ready &= ~MBC_SCULPT_CUSTOM_TRIANGLES;
          }
        }
        else {
          /* Evaluated cache doesn't have batch_ready - clear in original cache too */
          orig_cache->batch_ready &= ~MBC_SCULPT_CUSTOM_TRIANGLES;
        }
      }
      if (orig_cache->batch_ready & MBC_SCULPT_CUSTOM_EDGES) {
        if (cache.batch_ready & MBC_SCULPT_CUSTOM_EDGES) {
          bool is_valid = orig_cache->batch.sculpt_custom_edges && is_batch_valid_for_drawing(orig_cache->batch.sculpt_custom_edges);
          // #region agent log
          {
            static unsigned long long counter = 0;
            FILE *f = fopen("i:\\Blender_DAD\\blender\\.cursor\\debug.log", "a");
            if (f) {
              fprintf(f, "{\"id\":\"log_%llu\",\"location\":\"draw_cache_impl_mesh.cc:1551\",\"message\":\"VALIDATION: Checking orig EDGES\",\"data\":{\"batch_ptr\":\"%p\",\"is_valid\":%s},\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"C\"}\n",
                      counter++, orig_cache->batch.sculpt_custom_edges, is_valid ? "true" : "false");
              fclose(f);
            }
          }
          // #endregion
          if (is_valid) {
            valid_batch_ready |= MBC_SCULPT_CUSTOM_EDGES;
          }
          else {
            orig_cache->batch_ready &= ~MBC_SCULPT_CUSTOM_EDGES;
          }
        }
        else {
          orig_cache->batch_ready &= ~MBC_SCULPT_CUSTOM_EDGES;
        }
      }
      if (orig_cache->batch_ready & MBC_SCULPT_CUSTOM_VERTICES) {
        if (cache.batch_ready & MBC_SCULPT_CUSTOM_VERTICES) {
          bool is_valid = orig_cache->batch.sculpt_custom_vertices && is_batch_valid_for_drawing(orig_cache->batch.sculpt_custom_vertices);
          // #region agent log
          {
            static unsigned long long counter = 0;
            FILE *f = fopen("i:\\Blender_DAD\\blender\\.cursor\\debug.log", "a");
            if (f) {
              fprintf(f, "{\"id\":\"log_%llu\",\"location\":\"draw_cache_impl_mesh.cc:1559\",\"message\":\"VALIDATION: Checking orig VERTICES\",\"data\":{\"batch_ptr\":\"%p\",\"is_valid\":%s},\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"C\"}\n",
                      counter++, orig_cache->batch.sculpt_custom_vertices, is_valid ? "true" : "false");
              fclose(f);
            }
          }
          // #endregion
          if (is_valid) {
            valid_batch_ready |= MBC_SCULPT_CUSTOM_VERTICES;
          }
          else {
            orig_cache->batch_ready &= ~MBC_SCULPT_CUSTOM_VERTICES;
          }
        }
        else {
          orig_cache->batch_ready &= ~MBC_SCULPT_CUSTOM_VERTICES;
        }
      }
    }
    
    /* CRITICAL: If flags are in preserved map, it means invalidation occurred.
     * In this case, batches should NOT be considered valid, even if batch_ready is set,
     * because the evaluated mesh may have been recreated and batches may be stale.
     * Only consider batches valid if they were created AFTER the last invalidation.
     * 
     * IMPORTANT: The presence of flags in preserved map is the PRIMARY indicator of invalidation.
     * We check this FIRST, before checking batch_ready flags, because:
     * - After invalidation, orig_batch_ready is cleared immediately
     * - eval_batch_ready may be cleared by INVALIDATION_SYNC (line 1617)
     * - But if INVALIDATION_SYNC hasn't run yet, eval_batch_ready may still be set
     * - So we use preserved map as the definitive source of truth for invalidation state */
    // #region agent log
    {
      static unsigned long long counter = 0;
      FILE *f = fopen("i:\\Blender_DAD\\blender\\.cursor\\debug.log", "a");
      if (f) {
        fprintf(f, "{\"id\":\"log_%llu\",\"location\":\"draw_cache_impl_mesh.cc:1840\",\"message\":\"INVALIDATION_CHECK: Before invalidation check\",\"data\":{\"orig_cache\":%s,\"orig_batch_ready\":\"0x%llx\",\"eval_batch_ready\":\"0x%llx\",\"valid_batch_ready\":\"0x%llx\",\"preserved\":\"0x%llx\",\"found_key\":\"%p\"},\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"A\"}\n",
                counter++, orig_cache ? "true" : "false",
                orig_cache ? (unsigned long long)orig_cache->batch_ready : 0ULL,
                (unsigned long long)cache.batch_ready, (unsigned long long)valid_batch_ready,
                (unsigned long long)preserved, found_key);
        fclose(f);
      }
    }
    // #endregion
    /* PRIMARY CHECK: If flags are in preserved map, invalidation occurred.
     * Clear valid_batch_ready to force recreation, regardless of batch_ready state. */
    if (found_key != nullptr && (preserved & sculpt_custom_flags) != 0) {
      /* Flags are in preserved map - invalidation occurred.
       * Clear valid_batch_ready to force recreation, even if batches appear valid. */
      DRWBatchFlag valid_batch_ready_before = valid_batch_ready;
      valid_batch_ready &= ~sculpt_custom_flags;
      // #region agent log
      {
        static unsigned long long counter = 0;
        FILE *f = fopen("i:\\Blender_DAD\\blender\\.cursor\\debug.log", "a");
        if (f) {
          fprintf(f, "{\"id\":\"log_%llu\",\"location\":\"draw_cache_impl_mesh.cc:1860\",\"message\":\"INVALIDATION_CHECK: Invalidation detected (flags in preserved map) - clearing valid_batch_ready\",\"data\":{\"orig_batch_ready\":\"0x%llx\",\"eval_batch_ready\":\"0x%llx\",\"valid_batch_ready_before\":\"0x%llx\",\"valid_batch_ready_after\":\"0x%llx\",\"preserved\":\"0x%llx\"},\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"A\"}\n",
                  counter++, orig_cache ? (unsigned long long)orig_cache->batch_ready : 0ULL, (unsigned long long)cache.batch_ready,
                  (unsigned long long)valid_batch_ready_before, (unsigned long long)valid_batch_ready, (unsigned long long)preserved);
          fclose(f);
        }
      }
      // #endregion
    }
    /* SECONDARY CHECK: If original cache doesn't have batch_ready but evaluated cache does,
     * it also means invalidation occurred (original was invalidated, but evaluated wasn't synced yet). */
    else if (orig_cache && (orig_cache->batch_ready & sculpt_custom_flags) == 0 && 
             (cache.batch_ready & sculpt_custom_flags) != 0) {
      /* Original cache doesn't have batch_ready, but evaluated cache does.
       * This means invalidation occurred and batches in evaluated cache are stale.
       * Clear valid_batch_ready to force recreation. */
      DRWBatchFlag valid_batch_ready_before = valid_batch_ready;
      valid_batch_ready &= ~sculpt_custom_flags;
      // #region agent log
      {
        static unsigned long long counter = 0;
        FILE *f = fopen("i:\\Blender_DAD\\blender\\.cursor\\debug.log", "a");
        if (f) {
          fprintf(f, "{\"id\":\"log_%llu\",\"location\":\"draw_cache_impl_mesh.cc:1875\",\"message\":\"INVALIDATION_CHECK: Invalidation detected (orig cleared, eval not) - clearing valid_batch_ready\",\"data\":{\"orig_batch_ready\":\"0x%llx\",\"eval_batch_ready\":\"0x%llx\",\"valid_batch_ready_before\":\"0x%llx\",\"valid_batch_ready_after\":\"0x%llx\"},\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"A\"}\n",
                  counter++, (unsigned long long)orig_cache->batch_ready, (unsigned long long)cache.batch_ready,
                  (unsigned long long)valid_batch_ready_before, (unsigned long long)valid_batch_ready);
          fclose(f);
        }
      }
      // #endregion
    }
    else {
      // #region agent log
      {
        static unsigned long long counter = 0;
        FILE *f = fopen("i:\\Blender_DAD\\blender\\.cursor\\debug.log", "a");
        if (f) {
          fprintf(f, "{\"id\":\"log_%llu\",\"location\":\"draw_cache_impl_mesh.cc:1885\",\"message\":\"INVALIDATION_CHECK: No invalidation detected\",\"data\":{\"orig_cache\":%s,\"orig_batch_ready\":\"0x%llx\",\"eval_batch_ready\":\"0x%llx\",\"valid_batch_ready\":\"0x%llx\",\"found_key\":\"%p\"},\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"A\"}\n",
                  counter++, orig_cache ? "true" : "false",
                  orig_cache ? (unsigned long long)orig_cache->batch_ready : 0ULL,
                  (unsigned long long)cache.batch_ready, (unsigned long long)valid_batch_ready, found_key);
          fclose(f);
        }
      }
      // #endregion
    }
    
    /* Only restore flags that are not already valid */
    const DRWBatchFlag preserved_unprocessed = preserved & ~valid_batch_ready;
    // #region agent log
    {
      static unsigned long long counter = 0;
      FILE *f = fopen("i:\\Blender_DAD\\blender\\.cursor\\debug.log", "a");
      if (f) {
        fprintf(f, "{\"id\":\"log_%llu\",\"location\":\"draw_cache_impl_mesh.cc:1625\",\"message\":\"VALIDATION: Calculated preserved_unprocessed\",\"data\":{\"valid_batch_ready\":\"0x%llx\",\"preserved\":\"0x%llx\",\"preserved_unprocessed\":\"0x%llx\"},\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"D\"}\n",
                counter++, (unsigned long long)valid_batch_ready, (unsigned long long)preserved, (unsigned long long)preserved_unprocessed);
        fclose(f);
      }
    }
    // #endregion
    if (preserved_unprocessed != 0) {
      DRWBatchFlag batch_requested_before = cache.batch_requested;
      cache.batch_requested |= preserved_unprocessed;
      // #region agent log
      {
        static unsigned long long counter = 0;
        FILE *f = fopen("i:\\Blender_DAD\\blender\\.cursor\\debug.log", "a");
        if (f) {
          fprintf(f, "{\"id\":\"log_%llu\",\"location\":\"draw_cache_impl_mesh.cc:1830\",\"message\":\"RESTORE: Restoring flags to batch_requested\",\"data\":{\"preserved_unprocessed\":\"0x%llx\",\"batch_requested_before\":\"0x%llx\",\"batch_requested_after\":\"0x%llx\"},\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"D\"}\n",
                  counter++, (unsigned long long)preserved_unprocessed, (unsigned long long)batch_requested_before, (unsigned long long)cache.batch_requested);
          fclose(f);
        }
      }
      // #endregion
      fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] DRW_mesh_batch_cache_create_requested: restored flags from map: 0x%llx "
                      "(valid_batch_ready=0x%llx, preserved=0x%llx, check_cache=%s)\n",
              uint64_t(preserved_unprocessed), uint64_t(valid_batch_ready), uint64_t(preserved),
              orig_cache ? "original" : "evaluated");
    }
    else {
      // #region agent log
      {
        static unsigned long long counter = 0;
        FILE *f = fopen("i:\\Blender_DAD\\blender\\.cursor\\debug.log", "a");
        if (f) {
          fprintf(f, "{\"id\":\"log_%llu\",\"location\":\"draw_cache_impl_mesh.cc:1840\",\"message\":\"RESTORE: All flags already processed, NOT restoring\",\"data\":{\"valid_batch_ready\":\"0x%llx\",\"preserved\":\"0x%llx\",\"preserved_unprocessed\":\"0x0\"},\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"D\"}\n",
                  counter++, (unsigned long long)valid_batch_ready, (unsigned long long)preserved);
          fclose(f);
        }
      }
      // #endregion
      fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] DRW_mesh_batch_cache_create_requested: found in map but all flags already processed "
                      "(valid_batch_ready=0x%llx, preserved=0x%llx, check_cache=%s)\n",
              uint64_t(valid_batch_ready), uint64_t(preserved),
              orig_cache ? "original" : "evaluated");
    }
  }
  else {
    fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] DRW_mesh_batch_cache_create_requested: NOT found in map for "
                    "mesh_key_object=%p, mesh_key_id=%p\n",
            mesh_key_object, mesh_key_id);
  }

  fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] DRW_mesh_batch_cache_create_requested: "
                  "batch_requested=0x%llx, batch_ready=0x%llx, mesh.id=%p, mesh_key_object=%p\n",
          uint64_t(cache.batch_requested), uint64_t(cache.batch_ready), &mesh.id, mesh_key_object);

  // #region agent log
  {
    static unsigned long long counter = 0;
    FILE *f = fopen("i:\\Blender_DAD\\blender\\.cursor\\debug.log", "a");
    if (f) {
      fprintf(f, "{\"id\":\"log_%llu\",\"location\":\"draw_cache_impl_mesh.cc:1877\",\"message\":\"EARLY_OUT: Checking if should early out\",\"data\":{\"batch_requested\":\"0x%llx\",\"batch_ready\":\"0x%llx\"},\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"D\"}\n",
              counter++, (unsigned long long)cache.batch_requested, (unsigned long long)cache.batch_ready);
      fclose(f);
    }
  }
  // #endregion

  /* Early out if nothing is requested */
  if (cache.batch_requested == 0) {
    // #region agent log
    {
      static unsigned long long counter = 0;
      FILE *f = fopen("i:\\Blender_DAD\\blender\\.cursor\\debug.log", "a");
      if (f) {
        fprintf(f, "{\"id\":\"log_%llu\",\"location\":\"draw_cache_impl_mesh.cc:1942\",\"message\":\"EARLY_OUT: Early out - batch_requested is 0\",\"data\":{\"found_key\":\"%p\",\"preserved\":\"0x%llx\",\"map_size\":%zu},\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"D\"}\n",
                counter++, found_key, (unsigned long long)preserved, sculpt_custom_flags_preserved.size());
        fclose(f);
      }
    }
    // #endregion
    fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] Early out: batch_requested is 0\n");
    return;
  }

  /* Sanity check. */
  if ((mesh.runtime->edit_mesh != nullptr) && (ob.mode & OB_MODE_EDIT)) {
    BLI_assert(BKE_object_get_editmesh_eval_final(&ob) != nullptr);
  }

  const bool is_editmode = ob.mode == OB_MODE_EDIT;

  /* Sculpt custom batches use a different request/ready lifecycle:
   * - Normal batches: requested and created in the same frame
   * - Sculpt custom: requested by Python in POST_VIEW (after batch creation),
   *   so flags persist in batch_requested across frames until batches are ready.
   * We include sculpt custom flags in batch_requested for processing. */
  DRWBatchFlag batch_requested = cache.batch_requested;
  cache.batch_requested = DRWBatchFlag(0);

  if (batch_requested & MBC_SURFACE_WEIGHTS) {
    /* Check vertex weights. */
    if ((cache.batch.surface_weights != nullptr) && (ts != nullptr)) {
      DRW_MeshWeightState wstate;
      BLI_assert(ob.type == OB_MESH);
      drw_mesh_weight_state_extract(ob, mesh, *ts, is_paint_mode, &wstate);
      mesh_batch_cache_check_vertex_group(cache, &wstate);
      drw_mesh_weight_state_copy(&cache.weight_state, &wstate);
      drw_mesh_weight_state_clear(&wstate);
    }
  }

  if (batch_requested &
      (MBC_SURFACE | MBC_SURFACE_PER_MAT | MBC_WIRE_LOOPS_ALL_UVS | MBC_WIRE_LOOPS_UVS |
       MBC_WIRE_LOOPS_EDITUVS | MBC_UV_FACES | MBC_EDITUV_FACES_STRETCH_AREA |
       MBC_EDITUV_FACES_STRETCH_ANGLE | MBC_EDITUV_FACES | MBC_EDITUV_EDGES | MBC_EDITUV_VERTS))
  {
    /* Modifiers will only generate an orco layer if the mesh is deformed. */
    if (cache.cd_needed.orco != 0) {
      /* Orco is always extracted from final mesh. */
      const Mesh *me_final = (mesh.runtime->edit_mesh) ? BKE_object_get_editmesh_eval_final(&ob) :
                                                         &mesh;
      if (CustomData_get_layer(&me_final->vert_data, CD_ORCO) == nullptr) {
        /* Skip orco calculation */
        cache.cd_needed.orco = false;
      }
    }

    /* Verify that all surface batches have needed attribute layers, i.e. that none of the shaders
     * need vertex buffers that aren't currently cached. */
    /* TODO(fclem): We could be a bit smarter here and only do it per
     * material. */
    const bool uvs_overlap = drw_attributes_overlap(&cache.cd_used.uv, &cache.cd_needed.uv);
    const bool tan_overlap = drw_attributes_overlap(&cache.cd_used.tan, &cache.cd_needed.tan);
    const bool attr_overlap = drw_attributes_overlap(&cache.attr_used, &cache.attr_needed);
    if (!uvs_overlap || !tan_overlap || !attr_overlap ||
        (cache.cd_needed.orco && !cache.cd_used.orco) ||
        (cache.cd_needed.tan_orco && !cache.cd_used.tan_orco) ||
        (cache.cd_needed.sculpt_overlays && !cache.cd_used.sculpt_overlays))
    {
      FOREACH_MESH_BUFFER_CACHE (cache, mbc) {
        if (!uvs_overlap) {
          mbc->buff.vbos.remove(VBOType::UVs);
          cd_uv_update = true;
        }
        if (!tan_overlap || cache.cd_used.tan_orco != cache.cd_needed.tan_orco) {
          mbc->buff.vbos.remove(VBOType::Tangents);
        }
        if (cache.cd_used.orco != cache.cd_needed.orco) {
          mbc->buff.vbos.remove(VBOType::Orco);
        }
        if (cache.cd_used.sculpt_overlays != cache.cd_needed.sculpt_overlays) {
          mbc->buff.vbos.remove(VBOType::SculptData);
        }
        if (!attr_overlap) {
          for (int i = 0; i < GPU_MAX_ATTR; i++) {
            mbc->buff.vbos.remove(VBOType(int8_t(VBOType::Attr0) + i));
          }
        }
      }
      /* We can't discard batches at this point as they have been
       * referenced for drawing. Just clear them in place. */
      for (int i = 0; i < cache.mat_len; i++) {
        GPU_BATCH_CLEAR_SAFE(cache.surface_per_mat[i]);
      }
      GPU_BATCH_CLEAR_SAFE(cache.batch.surface);
      GPU_BATCH_CLEAR_SAFE(cache.batch.sculpt_overlays);
      cache.batch_ready &= ~(MBC_SURFACE | MBC_SURFACE_PER_MAT | MBC_SCULPT_OVERLAYS);

      mesh_cd_layers_type_merge(&cache.cd_used, cache.cd_needed);
      drw_attributes_merge(&cache.attr_used, &cache.attr_needed);
    }
    mesh_cd_layers_type_merge(&cache.cd_used_over_time, cache.cd_needed);
    cache.cd_needed.clear();

    drw_attributes_merge(&cache.attr_used_over_time, &cache.attr_needed);
    cache.attr_needed.clear();
  }

  if ((batch_requested & MBC_EDITUV) || cd_uv_update) {
    /* Discard UV batches if sync_selection changes */
    const bool is_uvsyncsel = ts && (ts->uv_flag & UV_FLAG_SELECT_SYNC);
    if (cd_uv_update || (cache.is_uvsyncsel != is_uvsyncsel)) {
      cache.is_uvsyncsel = is_uvsyncsel;
      FOREACH_MESH_BUFFER_CACHE (cache, mbc) {
        mbc->buff.vbos.remove(VBOType::EditUVData);
        mbc->buff.vbos.remove(VBOType::FaceDotUV);
        mbc->buff.vbos.remove(VBOType::FaceDotEditUVData);
        mbc->buff.ibos.remove(IBOType::EditUVTris);
        mbc->buff.ibos.remove(IBOType::EditUVLines);
        mbc->buff.ibos.remove(IBOType::EditUVPoints);
        mbc->buff.ibos.remove(IBOType::EditUVFaceDots);
      }
      /* We only clear the batches as they may already have been
       * referenced. */
      GPU_BATCH_CLEAR_SAFE(cache.batch.uv_faces);
      GPU_BATCH_CLEAR_SAFE(cache.batch.wire_loops_all_uvs);
      GPU_BATCH_CLEAR_SAFE(cache.batch.wire_loops_uvs);
      GPU_BATCH_CLEAR_SAFE(cache.batch.wire_loops_edituvs);
      GPU_BATCH_CLEAR_SAFE(cache.batch.edituv_faces_stretch_area);
      GPU_BATCH_CLEAR_SAFE(cache.batch.edituv_faces_stretch_angle);
      GPU_BATCH_CLEAR_SAFE(cache.batch.edituv_faces);
      GPU_BATCH_CLEAR_SAFE(cache.batch.edituv_edges);
      GPU_BATCH_CLEAR_SAFE(cache.batch.edituv_verts);
      GPU_BATCH_CLEAR_SAFE(cache.batch.edituv_fdots);
      cache.batch_ready &= ~MBC_EDITUV;
    }
  }

  /* Second chance to early out */
  if ((batch_requested & ~cache.batch_ready) == 0) {
    return;
  }

  /* TODO(pablodp606): This always updates the sculpt normals for regular drawing (non-pbvh::Tree).
   * This makes tools that sample the surface per step get wrong normals until a redraw happens.
   * Normal updates should be part of the brush loop and only run during the stroke when the
   * brush needs to sample the surface. The drawing code should only update the normals
   * per redraw when smooth shading is enabled. */
  if (bke::pbvh::Tree *pbvh = bke::object::pbvh_get(ob)) {
    bke::pbvh::update_normals_from_eval(ob, *pbvh);
  }

  /* This is the mesh before modifier evaluation, used to test how the mesh changed during
   * evaluation to decide which data is valid to extract. */
  const Mesh *orig_edit_mesh = is_editmode ? BKE_object_get_pre_modified_mesh(&ob) : nullptr;

  bool do_cage = false;
  const Mesh *edit_data_mesh = nullptr;
  if (is_editmode) {
    const Mesh *eval_cage = DRW_object_get_editmesh_cage_for_drawing(ob);
    if (eval_cage && eval_cage != &mesh) {
      /* Extract "cage" data separately when it exists and it's not just the same mesh as the
       * regular evaluated mesh. Otherwise edit data will be extracted from the final evaluated
       * mesh. */
      do_cage = true;
      edit_data_mesh = eval_cage;
    }
    else {
      edit_data_mesh = &mesh;
    }
  }

  bool do_uvcage = false;
  if (is_editmode) {
    /* Currently we don't extract UV data from the evaluated mesh unless it's the same mesh as the
     * original edit mesh. */
    do_uvcage = !(mesh.runtime->is_original_bmesh &&
                  mesh.runtime->wrapper_type == ME_WRAPPER_TYPE_BMESH);
  }

  /* Calculate which batches need to be created: requested but not ready yet */
  const DRWBatchFlag batches_to_create = batch_requested & ~cache.batch_ready;
  // #region agent log
  {
    static unsigned long long counter = 0;
    FILE *f = fopen("i:\\Blender_DAD\\blender\\.cursor\\debug.log", "a");
    if (f) {
      fprintf(f, "{\"id\":\"log_%llu\",\"location\":\"draw_cache_impl_mesh.cc:2126\",\"message\":\"BATCHES_TO_CREATE: Calculated batches_to_create\",\"data\":{\"batch_requested\":\"0x%llx\",\"batch_ready\":\"0x%llx\",\"batches_to_create\":\"0x%llx\",\"sculpt_custom_in_batches_to_create\":\"0x%llx\",\"sculpt_custom_flags\":\"0x%llx\"},\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"F\"}\n",
              counter++, (unsigned long long)batch_requested, (unsigned long long)cache.batch_ready, (unsigned long long)batches_to_create, (unsigned long long)(batches_to_create & sculpt_custom_flags), (unsigned long long)sculpt_custom_flags);
      fclose(f);
    }
  }
  // #endregion
  fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] batches_to_create=0x%llx (batch_requested=0x%llx, batch_ready=0x%llx)\n",
          uint64_t(batches_to_create), uint64_t(batch_requested), uint64_t(cache.batch_ready));
  fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] Sculpt custom flags: TRIANGLES=0x%llx, EDGES=0x%llx, VERTICES=0x%llx\n",
          uint64_t(MBC_SCULPT_CUSTOM_TRIANGLES), uint64_t(MBC_SCULPT_CUSTOM_EDGES),
          uint64_t(MBC_SCULPT_CUSTOM_VERTICES));

  const bool do_subdivision = BKE_subsurf_modifier_has_gpu_subdiv(&mesh);

  enum class BufferList : int8_t { Final, Cage, UVCage };

  struct BatchCreateData {
    gpu::Batch &batch;
    GPUPrimType prim_type;
    BufferList list;
    std::optional<IBOType> ibo;
    Vector<VBOType> vbos;
    DRWBatchFlag flag; /* Flag to set in batch_ready when batch is successfully created */
  };
  Vector<BatchCreateData> batch_info;

  {
    const BufferList list = BufferList::Final;
    if (batches_to_create & MBC_SURFACE) {
      BatchCreateData batch{*cache.batch.surface,
                            GPU_PRIM_TRIS,
                            list,
                            IBOType::Tris,
                            {VBOType::CornerNormal, VBOType::Position},
                            (DRWBatchFlag)0};
      if (!cache.cd_used.uv.is_empty()) {
        batch.vbos.append(VBOType::UVs);
      }
      for (const int i : cache.attr_used.index_range()) {
        batch.vbos.append(VBOType(int8_t(VBOType::Attr0) + i));
      }
      batch_info.append(std::move(batch));
    }
    if (batches_to_create & MBC_PAINT_OVERLAY_SURFACE) {
      BatchCreateData batch{*cache.batch.paint_overlay_surface,
                            GPU_PRIM_TRIS,
                            list,
                            IBOType::Tris,
                            {VBOType::Position, VBOType::PaintOverlayFlag},
                            (DRWBatchFlag)0};
      batch_info.append(std::move(batch));
    }
    if (batches_to_create & MBC_VIEWER_ATTRIBUTE_OVERLAY) {
      batch_info.append({*cache.batch.surface_viewer_attribute,
                         GPU_PRIM_TRIS,
                         list,
                         IBOType::Tris,
                         {VBOType::Position, VBOType::AttrViewer},
                         (DRWBatchFlag)0});
    }
    if (batches_to_create & MBC_ALL_VERTS) {
      batch_info.append(
          {*cache.batch.all_verts, GPU_PRIM_POINTS, list, std::nullopt, {VBOType::Position}, (DRWBatchFlag)0});
    }
    if (batches_to_create & MBC_PAINT_OVERLAY_VERTS) {
      batch_info.append({*cache.batch.paint_overlay_verts,
                         GPU_PRIM_POINTS,
                         list,
                         std::nullopt,
                         {VBOType::Position, VBOType::PaintOverlayFlag},
                         (DRWBatchFlag)0});
    }
    if (batches_to_create & MBC_SCULPT_OVERLAYS) {
      batch_info.append({*cache.batch.sculpt_overlays,
                         GPU_PRIM_TRIS,
                         list,
                         IBOType::Tris,
                         {VBOType::Position, VBOType::SculptData},
                         (DRWBatchFlag)0});
    }
    /* Sculpt Mode Custom Overlays (for Python addons) */
    fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] Checking MBC_SCULPT_CUSTOM_TRIANGLES: "
                    "flag=0x%llx, batches_to_create=0x%llx, result=%d\n",
            uint64_t(MBC_SCULPT_CUSTOM_TRIANGLES), uint64_t(batches_to_create),
            (batches_to_create & MBC_SCULPT_CUSTOM_TRIANGLES) != 0);
    if (batches_to_create & MBC_SCULPT_CUSTOM_TRIANGLES) {
      // #region agent log
      {
        static unsigned long long counter = 0;
        FILE *f = fopen("i:\\Blender_DAD\\blender\\.cursor\\debug.log", "a");
        if (f) {
          fprintf(f, "{\"id\":\"log_%llu\",\"location\":\"draw_cache_impl_mesh.cc:2186\",\"message\":\"CREATING: Starting SCULPT_CUSTOM_TRIANGLES batch creation\",\"data\":{\"batches_to_create\":\"0x%llx\"},\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"F\"}\n",
                  counter++, (unsigned long long)batches_to_create);
          fclose(f);
        }
      }
      // #endregion
      fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] Creating SCULPT_CUSTOM_TRIANGLES batch, flag=0x%llx\n",
              uint64_t(MBC_SCULPT_CUSTOM_TRIANGLES));
      DRW_batch_request(&cache.batch.sculpt_custom_triangles);
      if (!cache.batch.sculpt_custom_triangles) {
        // #region agent log
        {
          static unsigned long long counter = 0;
          FILE *f = fopen("i:\\Blender_DAD\\blender\\.cursor\\debug.log", "a");
          if (f) {
            fprintf(f, "{\"id\":\"log_%llu\",\"location\":\"draw_cache_impl_mesh.cc:2191\",\"message\":\"CREATING: ERROR - sculpt_custom_triangles batch is nullptr\",\"data\":{},\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"F\"}\n",
                    counter++);
            fclose(f);
          }
        }
        // #endregion
        fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] ERROR: sculpt_custom_triangles batch is nullptr!\n");
      }
      else {
        batch_info.append({*cache.batch.sculpt_custom_triangles,
                           GPU_PRIM_TRIS,
                           list,
                           IBOType::Tris,
                           {VBOType::Position, VBOType::CornerNormal},
                           MBC_SCULPT_CUSTOM_TRIANGLES});
        // #region agent log
        {
          static unsigned long long counter = 0;
          FILE *f = fopen("i:\\Blender_DAD\\blender\\.cursor\\debug.log", "a");
          if (f) {
            fprintf(f, "{\"id\":\"log_%llu\",\"location\":\"draw_cache_impl_mesh.cc:2194\",\"message\":\"CREATING: Added SCULPT_CUSTOM_TRIANGLES to batch_info\",\"data\":{\"batch_info_size\":%zu},\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"F\"}\n",
                    counter++, batch_info.size());
            fclose(f);
          }
        }
        // #endregion
        fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] Added batch_info entry, batch_info.size()=%zu\n",
                batch_info.size());
      }
    }
    else {
      // #region agent log
      {
        static unsigned long long counter = 0;
        FILE *f = fopen("i:\\Blender_DAD\\blender\\.cursor\\debug.log", "a");
        if (f) {
          fprintf(f, "{\"id\":\"log_%llu\",\"location\":\"draw_cache_impl_mesh.cc:2205\",\"message\":\"CREATING: MBC_SCULPT_CUSTOM_TRIANGLES NOT in batches_to_create\",\"data\":{\"batches_to_create\":\"0x%llx\"},\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"F\"}\n",
                  counter++, (unsigned long long)batches_to_create);
          fclose(f);
        }
      }
      // #endregion
      fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] MBC_SCULPT_CUSTOM_TRIANGLES not in batches_to_create "
                      "(0x%llx)\n",
              uint64_t(batches_to_create));
    }
    if (batches_to_create & MBC_SCULPT_CUSTOM_EDGES) {
      fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] Creating SCULPT_CUSTOM_EDGES batch, flag=0x%llx\n",
              uint64_t(MBC_SCULPT_CUSTOM_EDGES));
      DRW_batch_request(&cache.batch.sculpt_custom_edges);
      if (!cache.batch.sculpt_custom_edges) {
        fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] ERROR: sculpt_custom_edges batch is nullptr!\n");
      }
      else {
        batch_info.append({*cache.batch.sculpt_custom_edges,
                           GPU_PRIM_LINES,
                           list,
                           IBOType::Lines,
                           {VBOType::Position, VBOType::CornerNormal},
                           MBC_SCULPT_CUSTOM_EDGES});
      }
    }
    if (batches_to_create & MBC_SCULPT_CUSTOM_VERTICES) {
      fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] Creating SCULPT_CUSTOM_VERTICES batch, flag=0x%llx\n",
              uint64_t(MBC_SCULPT_CUSTOM_VERTICES));
      DRW_batch_request(&cache.batch.sculpt_custom_vertices);
      if (!cache.batch.sculpt_custom_vertices) {
        fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] ERROR: sculpt_custom_vertices batch is nullptr!\n");
      }
      else {
        batch_info.append({*cache.batch.sculpt_custom_vertices,
                           GPU_PRIM_POINTS,
                           list,
                           IBOType::Points,
                           {VBOType::Position, VBOType::CornerNormal},
                           MBC_SCULPT_CUSTOM_VERTICES});
      }
    }
    if (batches_to_create & MBC_ALL_EDGES) {
      batch_info.append(
          {*cache.batch.all_edges, GPU_PRIM_LINES, list, IBOType::Lines, {VBOType::Position}, (DRWBatchFlag)0});
    }
    if (batches_to_create & MBC_LOOSE_EDGES) {
      batch_info.append({*cache.batch.loose_edges,
                         GPU_PRIM_LINES,
                         list,
                         IBOType::LinesLoose,
                         {VBOType::Position},
                         (DRWBatchFlag)0});
    }
    if (batches_to_create & MBC_EDGE_DETECTION) {
      batch_info.append({*cache.batch.edge_detection,
                         GPU_PRIM_LINES_ADJ,
                         list,
                         IBOType::LinesAdjacency,
                         {VBOType::Position},
                         (DRWBatchFlag)0});
    }
    if (batches_to_create & MBC_SURFACE_WEIGHTS) {
      batch_info.append({*cache.batch.surface_weights,
                         GPU_PRIM_TRIS,
                         list,
                         IBOType::Tris,
                         {VBOType::Position, VBOType::CornerNormal, VBOType::VertexGroupWeight},
                         (DRWBatchFlag)0});
    }
    if (batches_to_create & MBC_PAINT_OVERLAY_WIRE_LOOPS) {
      batch_info.append({*cache.batch.paint_overlay_wire_loops,
                         GPU_PRIM_LINES,
                         list,
                         IBOType::LinesPaintMask,
                         {VBOType::Position, VBOType::PaintOverlayFlag},
                         (DRWBatchFlag)0});
    }
    if (batches_to_create & MBC_WIRE_EDGES) {
      batch_info.append({*cache.batch.wire_edges,
                         GPU_PRIM_LINES,
                         list,
                         IBOType::Lines,
                         {VBOType::Position, VBOType::CornerNormal, VBOType::EdgeFactor},
                         (DRWBatchFlag)0});
    }
    if (batches_to_create & MBC_WIRE_LOOPS_ALL_UVS) {
      BatchCreateData batch{
          *cache.batch.wire_loops_all_uvs, GPU_PRIM_LINES, list, IBOType::AllUVLines, {}, (DRWBatchFlag)0};
      if (!cache.cd_used.uv.is_empty()) {
        batch.vbos.append(VBOType::UVs);
      }
      batch_info.append(std::move(batch));
    }
    if (batches_to_create & MBC_WIRE_LOOPS_UVS) {
      BatchCreateData batch{
          *cache.batch.wire_loops_uvs, GPU_PRIM_LINES, list, IBOType::UVLines, {}, (DRWBatchFlag)0};
      if (!cache.cd_used.uv.is_empty()) {
        batch.vbos.append(VBOType::UVs);
      }
      batch_info.append(std::move(batch));
    }
    if (batches_to_create & MBC_WIRE_LOOPS_EDITUVS) {
      BatchCreateData batch{
          *cache.batch.wire_loops_edituvs, GPU_PRIM_LINES, list, IBOType::EditUVLines, {}, (DRWBatchFlag)0};
      if (!cache.cd_used.uv.is_empty()) {
        batch.vbos.append(VBOType::UVs);
      }
      batch_info.append(std::move(batch));
    }
    if (batches_to_create & MBC_UV_FACES) {
      const bool use_face_selection = (mesh.editflag & ME_EDIT_PAINT_FACE_SEL);
      /* Sculpt mode does not support selection, therefore the generic `is_paint_mode` check cannot
       * be used */
      const bool is_face_selectable =
          ELEM(ob.mode, OB_MODE_VERTEX_PAINT, OB_MODE_WEIGHT_PAINT, OB_MODE_TEXTURE_PAINT) &&
          use_face_selection;

      const IBOType ibo = is_face_selectable || is_editmode ? IBOType::UVTris : IBOType::Tris;
      BatchCreateData batch{*cache.batch.uv_faces, GPU_PRIM_TRIS, list, ibo, {}, (DRWBatchFlag)0};
      if (!cache.cd_used.uv.is_empty()) {
        batch.vbos.append(VBOType::UVs);
      }
      batch_info.append(std::move(batch));
    }
    if (batches_to_create & MBC_EDIT_MESH_ANALYSIS) {
      batch_info.append({*cache.batch.edit_mesh_analysis,
                         GPU_PRIM_TRIS,
                         list,
                         IBOType::Tris,
                         {VBOType::Position, VBOType::MeshAnalysis},
                         (DRWBatchFlag)0});
    }
  }

  /* When the mesh doesn't correspond to the object's original mesh (i.e. the mesh was replaced by
   * another with the object info node during evaluation), don't extract edit mode data for it.
   * That data can be invalid because any original indices (#CD_ORIGINDEX) on the evaluated mesh
   * won't correspond to the correct mesh. */
  const bool edit_mapping_valid = is_editmode && BKE_editmesh_eval_orig_map_available(
                                                     *edit_data_mesh, orig_edit_mesh);

  {
    const BufferList list = do_cage ? BufferList::Cage : BufferList::Final;
    if (batches_to_create & MBC_EDIT_TRIANGLES) {
      if (edit_mapping_valid) {
        batch_info.append({*cache.batch.edit_triangles,
                           GPU_PRIM_TRIS,
                           list,
                           IBOType::Tris,
                           {VBOType::Position, VBOType::EditData},
                           (DRWBatchFlag)0});
      }
      else {
        init_empty_dummy_batch(*cache.batch.edit_triangles);
      }
    }
    if (batches_to_create & MBC_EDIT_VERTICES) {
      if (edit_mapping_valid) {
        BatchCreateData batch{*cache.batch.edit_vertices,
                              GPU_PRIM_POINTS,
                              list,
                              IBOType::Points,
                              {VBOType::Position, VBOType::EditData},
                              (DRWBatchFlag)0};
        if (!do_subdivision || do_cage) {
          batch.vbos.append(VBOType::CornerNormal);
        }
        batch_info.append(std::move(batch));
      }
      else {
        init_empty_dummy_batch(*cache.batch.edit_vertices);
      }
    }
    if (batches_to_create & MBC_EDIT_EDGES) {
      if (edit_mapping_valid) {
        BatchCreateData batch{*cache.batch.edit_edges,
                              GPU_PRIM_LINES,
                              list,
                              IBOType::Lines,
                              {VBOType::CornerNormal, VBOType::Position, VBOType::EditData},
                              (DRWBatchFlag)0};
        batch_info.append(std::move(batch));
      }
      else {
        init_empty_dummy_batch(*cache.batch.edit_edges);
      }
    }
    if (batches_to_create & MBC_EDIT_VNOR) {
      if (edit_mapping_valid) {
        batch_info.append({*cache.batch.edit_vnor,
                           GPU_PRIM_POINTS,
                           list,
                           IBOType::Points,
                           {VBOType::Position, VBOType::VertexNormal},
                           (DRWBatchFlag)0});
      }
      else {
        init_empty_dummy_batch(*cache.batch.edit_vnor);
      }
    }
    if (batches_to_create & MBC_EDIT_LNOR) {
      if (edit_mapping_valid) {
        batch_info.append({*cache.batch.edit_lnor,
                           GPU_PRIM_POINTS,
                           list,
                           IBOType::Tris,
                           {VBOType::Position, VBOType::CornerNormal},
                           (DRWBatchFlag)0});
      }
      else {
        init_empty_dummy_batch(*cache.batch.edit_lnor);
      }
    }
    if (batches_to_create & MBC_EDIT_FACEDOTS) {
      if (edit_mapping_valid) {
        batch_info.append({*cache.batch.edit_fdots,
                           GPU_PRIM_POINTS,
                           list,
                           IBOType::FaceDots,
                           {VBOType::FaceDotPosition, VBOType::FaceDotNormal},
                           (DRWBatchFlag)0});
      }
      else {
        init_empty_dummy_batch(*cache.batch.edit_fdots);
      }
    }
    if (batches_to_create & MBC_SKIN_ROOTS) {
      if (edit_mapping_valid) {
        batch_info.append({*cache.batch.edit_skin_roots,
                           GPU_PRIM_POINTS,
                           list,
                           std::nullopt,
                           {VBOType::SkinRoots},
                           (DRWBatchFlag)0});
      }
      else {
        init_empty_dummy_batch(*cache.batch.edit_skin_roots);
      }
    }
    if (batches_to_create & MBC_EDIT_SELECTION_VERTS) {
      if (is_editmode && !edit_mapping_valid) {
        init_empty_dummy_batch(*cache.batch.edit_selection_verts);
      }
      else {
        batch_info.append({*cache.batch.edit_selection_verts,
                           GPU_PRIM_POINTS,
                           list,
                           IBOType::Points,
                           {VBOType::Position, VBOType::IndexVert},
                           (DRWBatchFlag)0});
      }
    }
    if (batches_to_create & MBC_EDIT_SELECTION_EDGES) {
      if (is_editmode && !edit_mapping_valid) {
        init_empty_dummy_batch(*cache.batch.edit_selection_edges);
      }
      else {
        batch_info.append({*cache.batch.edit_selection_edges,
                           GPU_PRIM_LINES,
                           list,
                           IBOType::Lines,
                           {VBOType::Position, VBOType::IndexEdge},
                           (DRWBatchFlag)0});
      }
    }
    if (batches_to_create & MBC_EDIT_SELECTION_FACES) {
      if (is_editmode && !edit_mapping_valid) {
        init_empty_dummy_batch(*cache.batch.edit_selection_faces);
      }
      else {
        batch_info.append({*cache.batch.edit_selection_faces,
                           GPU_PRIM_TRIS,
                           list,
                           IBOType::Tris,
                           {VBOType::Position, VBOType::IndexFace},
                           (DRWBatchFlag)0});
      }
    }
    if (batches_to_create & MBC_EDIT_SELECTION_FACEDOTS) {
      if (is_editmode && !edit_mapping_valid) {
        init_empty_dummy_batch(*cache.batch.edit_selection_fdots);
      }
      else {
        batch_info.append({*cache.batch.edit_selection_fdots,
                           GPU_PRIM_POINTS,
                           list,
                           IBOType::FaceDots,
                           {VBOType::FaceDotPosition, VBOType::IndexFaceDot},
                           (DRWBatchFlag)0});
      }
    }
  }

  {
    /**
     * TODO: The code and data structure is ready to support modified UV display
     * but the selection code for UVs needs to support it first. So for now, only
     * display the cage in all cases.
     */
    const BufferList list = do_uvcage ? BufferList::UVCage : BufferList::Final;

    if (batches_to_create & MBC_EDITUV_FACES) {
      if (edit_mapping_valid) {
        batch_info.append({*cache.batch.edituv_faces,
                           GPU_PRIM_TRIS,
                           list,
                           IBOType::EditUVTris,
                           {VBOType::UVs, VBOType::EditUVData},
                           (DRWBatchFlag)0});
      }
      else {
        init_empty_dummy_batch(*cache.batch.edituv_faces);
      }
    }
    if (batches_to_create & MBC_EDITUV_FACES_STRETCH_AREA) {
      if (edit_mapping_valid) {
        batch_info.append({*cache.batch.edituv_faces_stretch_area,
                           GPU_PRIM_TRIS,
                           list,
                           IBOType::EditUVTris,
                           {VBOType::UVs, VBOType::EditUVData, VBOType::EditUVStretchArea},
                           (DRWBatchFlag)0});
      }
      else {
        init_empty_dummy_batch(*cache.batch.edituv_faces_stretch_area);
      }
    }
    if (batches_to_create & MBC_EDITUV_FACES_STRETCH_ANGLE) {
      if (edit_mapping_valid) {
        batch_info.append({*cache.batch.edituv_faces_stretch_angle,
                           GPU_PRIM_TRIS,
                           list,
                           IBOType::EditUVTris,
                           {VBOType::UVs, VBOType::EditUVData, VBOType::EditUVStretchAngle},
                           (DRWBatchFlag)0});
      }
      else {
        init_empty_dummy_batch(*cache.batch.edituv_faces_stretch_angle);
      }
    }
    if (batches_to_create & MBC_EDITUV_EDGES) {
      if (edit_mapping_valid) {
        batch_info.append({*cache.batch.edituv_edges,
                           GPU_PRIM_LINES,
                           list,
                           IBOType::EditUVLines,
                           {VBOType::UVs, VBOType::EditUVData},
                           (DRWBatchFlag)0});
      }
      else {
        init_empty_dummy_batch(*cache.batch.edituv_edges);
      }
    }
    if (batches_to_create & MBC_EDITUV_VERTS) {
      if (edit_mapping_valid) {
        batch_info.append({*cache.batch.edituv_verts,
                           GPU_PRIM_POINTS,
                           list,
                           IBOType::EditUVPoints,
                           {VBOType::UVs, VBOType::EditUVData},
                           (DRWBatchFlag)0});
      }
      else {
        init_empty_dummy_batch(*cache.batch.edituv_verts);
      }
    }
    if (batches_to_create & MBC_EDITUV_FACEDOTS) {
      if (edit_mapping_valid) {
        batch_info.append({*cache.batch.edituv_fdots,
                           GPU_PRIM_POINTS,
                           list,
                           IBOType::EditUVFaceDots,
                           {VBOType::FaceDotUV, VBOType::FaceDotEditUVData},
                           (DRWBatchFlag)0});
      }
      else {
        init_empty_dummy_batch(*cache.batch.edituv_fdots);
      }
    }
  }

  std::array<VectorSet<IBOType>, 3> ibo_requests;
  std::array<VectorSet<VBOType>, 3> vbo_requests;
  for (const BatchCreateData &batch : batch_info) {
    if (batch.ibo) {
      ibo_requests[int(batch.list)].add(*batch.ibo);
      fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] Added IBO request: list=%d, ibo=%d\n",
              int(batch.list), int(*batch.ibo));
    }
    vbo_requests[int(batch.list)].add_multiple(batch.vbos);
    for (const VBOType vbo : batch.vbos) {
      fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] Added VBO request: list=%d, vbo=%d\n",
              int(batch.list), int(vbo));
    }
  }

  if (batches_to_create & MBC_SURFACE_PER_MAT) {
    ibo_requests[int(BufferList::Final)].add(IBOType::Tris);
    vbo_requests[int(BufferList::Final)].add(VBOType::CornerNormal);
    vbo_requests[int(BufferList::Final)].add(VBOType::Position);
    for (const int i : cache.attr_used.index_range()) {
      vbo_requests[int(BufferList::Final)].add(VBOType(int8_t(VBOType::Attr0) + i));
    }
    if (!cache.cd_used.uv.is_empty()) {
      vbo_requests[int(BufferList::Final)].add(VBOType::UVs);
    }
    if (!cache.cd_used.tan.is_empty() || cache.cd_used.tan_orco) {
      vbo_requests[int(BufferList::Final)].add(VBOType::Tangents);
    }
    if (cache.cd_used.orco) {
      vbo_requests[int(BufferList::Final)].add(VBOType::Orco);
    }
  }

  if (do_uvcage) {
    mesh_buffer_cache_create_requested(task_graph,
                                       scene,
                                       cache,
                                       cache.uv_cage,
                                       ibo_requests[int(BufferList::UVCage)],
                                       vbo_requests[int(BufferList::UVCage)],
                                       ob,
                                       mesh,
                                       is_editmode,
                                       is_paint_mode,
                                       false,
                                       true,
                                       true);
  }

  if (do_cage) {
    mesh_buffer_cache_create_requested(task_graph,
                                       scene,
                                       cache,
                                       cache.cage,
                                       ibo_requests[int(BufferList::Cage)],
                                       vbo_requests[int(BufferList::Cage)],
                                       ob,
                                       mesh,
                                       is_editmode,
                                       is_paint_mode,
                                       false,
                                       false,
                                       true);
  }

  if (do_subdivision) {
    DRW_create_subdivision(ob,
                           mesh,
                           cache,
                           cache.final,
                           ibo_requests[int(BufferList::Final)],
                           vbo_requests[int(BufferList::Final)],
                           is_editmode,
                           is_paint_mode,
                           true,
                           false,
                           do_cage,
                           ts,
                           use_hide);
  }
  else {
    /* The subsurf modifier may have been recently removed, or another modifier was added after it,
     * so free any potential subdivision cache as it is not needed anymore. */
    mesh_batch_cache_free_subdiv_cache(cache);
  }

  // #region agent log
  {
    static unsigned long long counter = 0;
    FILE *f = fopen("i:\\Blender_DAD\\blender\\.cursor\\debug.log", "a");
    if (f) {
      fprintf(f, "{\"id\":\"log_%llu\",\"location\":\"draw_cache_impl_mesh.cc:2726\",\"message\":\"VBO_IBO: Calling mesh_buffer_cache_create_requested\",\"data\":{\"ibo_count\":%zu,\"vbo_count\":%zu,\"sculpt_custom_in_batches_to_create\":\"0x%llx\"},\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"G\"}\n",
              counter++, ibo_requests[int(BufferList::Final)].size(), vbo_requests[int(BufferList::Final)].size(), (unsigned long long)(batches_to_create & sculpt_custom_flags));
      fclose(f);
    }
  }
  // #endregion
  fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] Calling mesh_buffer_cache_create_requested for Final: "
                  "ibo_count=%zu, vbo_count=%zu\n",
          ibo_requests[int(BufferList::Final)].size(),
          vbo_requests[int(BufferList::Final)].size());
  mesh_buffer_cache_create_requested(task_graph,
                                     scene,
                                     cache,
                                     cache.final,
                                     ibo_requests[int(BufferList::Final)],
                                     vbo_requests[int(BufferList::Final)],
                                     ob,
                                     mesh,
                                     is_editmode,
                                     is_paint_mode,
                                     true,
                                     false,
                                     use_hide);
  // #region agent log
  {
    static unsigned long long counter = 0;
    FILE *f = fopen("i:\\Blender_DAD\\blender\\.cursor\\debug.log", "a");
    if (f) {
      fprintf(f, "{\"id\":\"log_%llu\",\"location\":\"draw_cache_impl_mesh.cc:2739\",\"message\":\"VBO_IBO: After mesh_buffer_cache_create_requested\",\"data\":{\"ibos_size\":%zu,\"vbos_size\":%zu,\"has_position\":%s,\"has_tris\":%s},\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"G\"}\n",
              counter++, cache.final.buff.ibos.size(), cache.final.buff.vbos.size(),
              cache.final.buff.vbos.contains(VBOType::Position) ? "true" : "false",
              cache.final.buff.ibos.contains(IBOType::Tris) ? "true" : "false");
      fclose(f);
    }
  }
  // #endregion
  fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] After mesh_buffer_cache_create_requested: "
                  "final.buff.ibos.size()=%zu, final.buff.vbos.size()=%zu\n",
          cache.final.buff.ibos.size(),
          cache.final.buff.vbos.size());

  std::array<MeshBufferCache *, 3> caches{&cache.final, &cache.cage, &cache.uv_cage};
  fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] Processing %zu batches in batch_info\n", batch_info.size());
  for (const BatchCreateData &batch : batch_info) {
    fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] Processing batch, flag=0x%llx\n", uint64_t(batch.flag));
    MeshBufferCache &cache_for_batch = *caches[int(batch.list)];
    
    /* Safely lookup IBO - use dummy batch if buffer doesn't exist or is empty. */
    gpu::IndexBuf *ibo = nullptr;
    if (batch.ibo) {
      fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] Looking for IBO: list=%d, ibo=%d\n",
              int(batch.list), int(*batch.ibo));
      if (const auto *ibo_ptr = cache_for_batch.buff.ibos.lookup_ptr(*batch.ibo)) {
        ibo = ibo_ptr->get();
        const uint32_t ibo_len = ibo->index_len_get();
        // #region agent log
        {
          static unsigned long long counter = 0;
          FILE *f = fopen("i:\\Blender_DAD\\blender\\.cursor\\debug.log", "a");
          if (f) {
            fprintf(f, "{\"id\":\"log_%llu\",\"location\":\"draw_cache_impl_mesh.cc:2755\",\"message\":\"VBO_IBO: IBO found for batch\",\"data\":{\"batch_flag\":\"0x%llx\",\"ibo_ptr\":\"%p\",\"ibo_len\":%u,\"is_init\":%d},\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"G\"}\n",
                    counter++, (unsigned long long)batch.flag, ibo, ibo_len, ibo->is_init());
            fclose(f);
          }
        }
        // #endregion
        fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] IBO found: %p, index_len=%u, is_init=%d\n",
                ibo, ibo_len, ibo->is_init());
        if (ibo_len == 0 || !ibo->is_init()) {
          /* IBO is empty or not initialized - use dummy batch. */
          fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] ERROR: IBO is empty or not initialized! "
                          "list=%d, ibo=%d, index_len=%u, using dummy batch\n",
                  int(batch.list), int(*batch.ibo), ibo_len);
          init_empty_dummy_batch(batch.batch);
          continue;
        }
      }
      else {
        /* IBO not found, initialize empty dummy batch. */
        fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] ERROR: IBO not found! list=%d, ibo=%d, using dummy batch\n",
                int(batch.list), int(*batch.ibo));
        init_empty_dummy_batch(batch.batch);
        continue;
      }
    }
    
    GPU_batch_init(&batch.batch, batch.prim_type, nullptr, ibo);
    fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] Batch initialized: prim_type=%d, ibo=%p\n",
            int(batch.prim_type), ibo);
    
    /* Safely lookup and add VBOs - use dummy batch if any required VBO is missing or empty. */
    bool all_vbos_found = true;
    bool all_vbos_valid = true;
    for (const VBOType vbo_request : batch.vbos) {
      fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] Looking for VBO: list=%d, vbo=%d\n",
              int(batch.list), int(vbo_request));
      if (const auto *vbo_ptr = cache_for_batch.buff.vbos.lookup_ptr(vbo_request)) {
        gpu::VertBuf *vbo = vbo_ptr->get();
        const uint vbo_len = GPU_vertbuf_get_vertex_len(vbo);
        // #region agent log
        {
          static unsigned long long counter = 0;
          FILE *f = fopen("i:\\Blender_DAD\\blender\\.cursor\\debug.log", "a");
          if (f) {
            fprintf(f, "{\"id\":\"log_%llu\",\"location\":\"draw_cache_impl_mesh.cc:2733\",\"message\":\"VBO_IBO: VBO found for batch\",\"data\":{\"batch_flag\":\"0x%llx\",\"vbo_type\":%d,\"vbo_ptr\":\"%p\",\"vbo_len\":%u},\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"G\"}\n",
                    counter++, (unsigned long long)batch.flag, int(vbo_request), vbo, vbo_len);
            fclose(f);
          }
        }
        // #endregion
        fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] VBO found: %p, vertex_len=%u\n", vbo, vbo_len);
        if (vbo_len == 0) {
          fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] ERROR: VBO is empty! list=%d, vbo=%d, vertex_len=%u\n",
                  int(batch.list), int(vbo_request), vbo_len);
          all_vbos_valid = false;
          break;
        }
        GPU_batch_vertbuf_add(&batch.batch, vbo, false);
        fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] VBO added to batch\n");
      }
      else {
        /* Required VBO not found - use dummy batch instead. */
        fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] ERROR: VBO not found! list=%d, vbo=%d\n",
                int(batch.list), int(vbo_request));
        all_vbos_found = false;
        break;
      }
    }
    
    if (!all_vbos_found || !all_vbos_valid) {
      /* Clear the partially initialized batch and use dummy batch. */
      fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] VBO check failed (found=%d, valid=%d), using dummy batch\n",
              all_vbos_found, all_vbos_valid);
      GPU_BATCH_CLEAR_SAFE(&batch.batch);
      init_empty_dummy_batch(batch.batch);
      /* Don't set batch_ready for dummy batches */
    }
    else {
      fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] Batch fully initialized successfully, flag=0x%llx\n",
              uint64_t(batch.flag));
      /* Set batch_ready flag for successfully created batch */
      if (batch.flag != (DRWBatchFlag)0) {
        cache.batch_ready |= batch.flag;
        fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] Set batch_ready flag: 0x%llx, new batch_ready=0x%llx\n",
                uint64_t(batch.flag), uint64_t(cache.batch_ready));
      }
      else {
        fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] WARNING: batch.flag is 0, not setting batch_ready!\n");
      }
    }
  }

  if (batches_to_create & MBC_SURFACE_PER_MAT) {
    MeshBufferList &buffers = cache.final.buff;
    gpu::IndexBuf &tris_ibo = *buffers.ibos.lookup(IBOType::Tris);
    create_material_subranges(cache.final.face_sorted, tris_ibo, cache.tris_per_mat);
    for (const int material : IndexRange(cache.mat_len)) {
      gpu::Batch *batch = cache.surface_per_mat[material];
      if (!batch) {
        continue;
      }
      GPU_batch_init(batch, GPU_PRIM_TRIS, nullptr, cache.tris_per_mat[material].get());
      GPU_batch_vertbuf_add(batch, buffers.vbos.lookup(VBOType::CornerNormal).get(), false);
      GPU_batch_vertbuf_add(batch, buffers.vbos.lookup(VBOType::Position).get(), false);
      if (!cache.cd_used.uv.is_empty()) {
        GPU_batch_vertbuf_add(batch, buffers.vbos.lookup(VBOType::UVs).get(), false);
      }
      if (!cache.cd_used.tan.is_empty() || cache.cd_used.tan_orco) {
        GPU_batch_vertbuf_add(batch, buffers.vbos.lookup(VBOType::Tangents).get(), false);
      }
      if (cache.cd_used.orco) {
        GPU_batch_vertbuf_add(batch, buffers.vbos.lookup(VBOType::Orco).get(), false);
      }
      for (const int i : cache.attr_used.index_range()) {
        GPU_batch_vertbuf_add(
            batch, buffers.vbos.lookup(VBOType(int8_t(VBOType::Attr0) + i)).get(), false);
      }
    }
  }

  /* Update batch_ready for successfully created batches.
   * IMPORTANT: Sculpt custom batches set batch_ready in the loop above (line 2439),
   * so we exclude them here to avoid setting batch_ready for batches that weren't created. */
  const DRWBatchFlag non_sculpt_custom_batch_requested = batch_requested & ~sculpt_custom_flags;
  cache.batch_ready |= non_sculpt_custom_batch_requested;
  
  /* Synchronize batch_ready with original mesh cache if it exists.
   * Python works with original mesh, but batches are created in evaluated mesh cache.
   * We need to sync batch_ready so Python can see that batches are ready.
   * IMPORTANT: Only sync batches that are actually valid. */
  const DRWBatchFlag sculpt_custom_ready = cache.batch_ready & sculpt_custom_flags;
  if (sculpt_custom_ready != 0) {
    Mesh *orig_mesh = BKE_object_get_original_mesh(&ob);
    if (orig_mesh && orig_mesh != &mesh && orig_mesh->runtime) {
      DRW_mesh_batch_cache_validate(*orig_mesh);
      MeshBatchCache *orig_cache = mesh_batch_cache_get(*orig_mesh);
      if (orig_cache) {
        /* Verify which batches are actually valid before syncing */
        DRWBatchFlag valid_sculpt_custom_ready = (DRWBatchFlag)0;
        
        if (sculpt_custom_ready & MBC_SCULPT_CUSTOM_TRIANGLES) {
          if (cache.batch.sculpt_custom_triangles && is_batch_valid_for_drawing(cache.batch.sculpt_custom_triangles)) {
            valid_sculpt_custom_ready |= MBC_SCULPT_CUSTOM_TRIANGLES;
            // #region agent log
            {
              static unsigned long long counter = 0;
              FILE *f = fopen("i:\\Blender_DAD\\blender\\.cursor\\debug.log", "a");
              if (f) {
                fprintf(f, "{\"id\":\"log_%llu\",\"location\":\"draw_cache_impl_mesh.cc:3012\",\"message\":\"SYNC: Syncing TRIANGLES batch to orig cache\",\"data\":{\"orig_batch_ptr\":\"%p\",\"eval_batch_ptr\":\"%p\",\"orig_batch_valid\":%s},\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"H\"}\n",
                        counter++, orig_cache->batch.sculpt_custom_triangles, cache.batch.sculpt_custom_triangles,
                        orig_cache->batch.sculpt_custom_triangles && is_batch_valid_for_drawing(orig_cache->batch.sculpt_custom_triangles) ? "true" : "false");
                fclose(f);
              }
            }
            // #endregion
            /* CRITICAL: Always update batch pointer in original cache, even if it already exists.
             * After invalidation, batches are recreated in evaluated cache with new geometry data.
             * Original cache must use the new batch pointer, not the old one.
             * 
             * IMPORTANT: We check if orig batch is different from eval batch to avoid unnecessary updates,
             * but we always update if they differ, even if orig batch appears valid. */
            if (orig_cache->batch.sculpt_custom_triangles != cache.batch.sculpt_custom_triangles) {
              /* Old batch pointer exists but is different - update to new batch */
              orig_cache->batch.sculpt_custom_triangles = cache.batch.sculpt_custom_triangles;
              // #region agent log
              {
                static unsigned long long counter = 0;
                FILE *f = fopen("i:\\Blender_DAD\\blender\\.cursor\\debug.log", "a");
                if (f) {
                  fprintf(f, "{\"id\":\"log_%llu\",\"location\":\"draw_cache_impl_mesh.cc:3020\",\"message\":\"SYNC: Updated orig TRIANGLES batch pointer\",\"data\":{\"old_ptr\":\"%p\",\"new_ptr\":\"%p\"},\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"H\"}\n",
                          counter++, orig_cache->batch.sculpt_custom_triangles, cache.batch.sculpt_custom_triangles);
                  fclose(f);
                }
              }
              // #endregion
            }
            else if (orig_cache->batch.sculpt_custom_triangles && 
                     !is_batch_valid_for_drawing(orig_cache->batch.sculpt_custom_triangles)) {
              /* Same pointer but invalid - clear it */
              orig_cache->batch.sculpt_custom_triangles = nullptr;
              orig_cache->batch_ready &= ~MBC_SCULPT_CUSTOM_TRIANGLES;
              // #region agent log
              {
                static unsigned long long counter = 0;
                FILE *f = fopen("i:\\Blender_DAD\\blender\\.cursor\\debug.log", "a");
                if (f) {
                  fprintf(f, "{\"id\":\"log_%llu\",\"location\":\"draw_cache_impl_mesh.cc:3030\",\"message\":\"SYNC: Cleared invalid orig TRIANGLES batch pointer\",\"data\":{},\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"H\"}\n",
                          counter++);
                  fclose(f);
                }
              }
              // #endregion
            }
            else if (!orig_cache->batch.sculpt_custom_triangles) {
              /* No batch pointer - copy from evaluated cache */
              orig_cache->batch.sculpt_custom_triangles = cache.batch.sculpt_custom_triangles;
              // #region agent log
              {
                static unsigned long long counter = 0;
                FILE *f = fopen("i:\\Blender_DAD\\blender\\.cursor\\debug.log", "a");
                if (f) {
                  fprintf(f, "{\"id\":\"log_%llu\",\"location\":\"draw_cache_impl_mesh.cc:3040\",\"message\":\"SYNC: Copied TRIANGLES batch pointer to orig cache\",\"data\":{\"batch_ptr\":\"%p\"},\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"H\"}\n",
                          counter++, cache.batch.sculpt_custom_triangles);
                  fclose(f);
                }
              }
              // #endregion
            }
          }
        }
        
        if (sculpt_custom_ready & MBC_SCULPT_CUSTOM_EDGES) {
          if (cache.batch.sculpt_custom_edges && is_batch_valid_for_drawing(cache.batch.sculpt_custom_edges)) {
            valid_sculpt_custom_ready |= MBC_SCULPT_CUSTOM_EDGES;
            // #region agent log
            {
              static unsigned long long counter = 0;
              FILE *f = fopen("i:\\Blender_DAD\\blender\\.cursor\\debug.log", "a");
              if (f) {
                fprintf(f, "{\"id\":\"log_%llu\",\"location\":\"draw_cache_impl_mesh.cc:3048\",\"message\":\"SYNC: Syncing EDGES batch to orig cache\",\"data\":{\"orig_batch_ptr\":\"%p\",\"eval_batch_ptr\":\"%p\"},\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"H\"}\n",
                        counter++, orig_cache->batch.sculpt_custom_edges, cache.batch.sculpt_custom_edges);
                fclose(f);
              }
            }
            // #endregion
            /* CRITICAL: Always update batch pointer in original cache if it differs from evaluated cache */
            if (orig_cache->batch.sculpt_custom_edges != cache.batch.sculpt_custom_edges) {
              orig_cache->batch.sculpt_custom_edges = cache.batch.sculpt_custom_edges;
              // #region agent log
              {
                static unsigned long long counter = 0;
                FILE *f = fopen("i:\\Blender_DAD\\blender\\.cursor\\debug.log", "a");
                if (f) {
                  fprintf(f, "{\"id\":\"log_%llu\",\"location\":\"draw_cache_impl_mesh.cc:3055\",\"message\":\"SYNC: Updated orig EDGES batch pointer\",\"data\":{\"old_ptr\":\"%p\",\"new_ptr\":\"%p\"},\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"H\"}\n",
                          counter++, orig_cache->batch.sculpt_custom_edges, cache.batch.sculpt_custom_edges);
                  fclose(f);
                }
              }
              // #endregion
            }
            else if (orig_cache->batch.sculpt_custom_edges && 
                     !is_batch_valid_for_drawing(orig_cache->batch.sculpt_custom_edges)) {
              orig_cache->batch.sculpt_custom_edges = nullptr;
              orig_cache->batch_ready &= ~MBC_SCULPT_CUSTOM_EDGES;
            }
            else if (!orig_cache->batch.sculpt_custom_edges) {
              orig_cache->batch.sculpt_custom_edges = cache.batch.sculpt_custom_edges;
            }
          }
        }
        
        if (sculpt_custom_ready & MBC_SCULPT_CUSTOM_VERTICES) {
          if (cache.batch.sculpt_custom_vertices && is_batch_valid_for_drawing(cache.batch.sculpt_custom_vertices)) {
            valid_sculpt_custom_ready |= MBC_SCULPT_CUSTOM_VERTICES;
            // #region agent log
            {
              static unsigned long long counter = 0;
              FILE *f = fopen("i:\\Blender_DAD\\blender\\.cursor\\debug.log", "a");
              if (f) {
                fprintf(f, "{\"id\":\"log_%llu\",\"location\":\"draw_cache_impl_mesh.cc:3074\",\"message\":\"SYNC: Syncing VERTICES batch to orig cache\",\"data\":{\"orig_batch_ptr\":\"%p\",\"eval_batch_ptr\":\"%p\"},\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"H\"}\n",
                        counter++, orig_cache->batch.sculpt_custom_vertices, cache.batch.sculpt_custom_vertices);
                fclose(f);
              }
            }
            // #endregion
            /* CRITICAL: Always update batch pointer in original cache if it differs from evaluated cache */
            if (orig_cache->batch.sculpt_custom_vertices != cache.batch.sculpt_custom_vertices) {
              orig_cache->batch.sculpt_custom_vertices = cache.batch.sculpt_custom_vertices;
              // #region agent log
              {
                static unsigned long long counter = 0;
                FILE *f = fopen("i:\\Blender_DAD\\blender\\.cursor\\debug.log", "a");
                if (f) {
                  fprintf(f, "{\"id\":\"log_%llu\",\"location\":\"draw_cache_impl_mesh.cc:3081\",\"message\":\"SYNC: Updated orig VERTICES batch pointer\",\"data\":{\"old_ptr\":\"%p\",\"new_ptr\":\"%p\"},\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"H\"}\n",
                          counter++, orig_cache->batch.sculpt_custom_vertices, cache.batch.sculpt_custom_vertices);
                  fclose(f);
                }
              }
              // #endregion
            }
            else if (orig_cache->batch.sculpt_custom_vertices && 
                     !is_batch_valid_for_drawing(orig_cache->batch.sculpt_custom_vertices)) {
              orig_cache->batch.sculpt_custom_vertices = nullptr;
              orig_cache->batch_ready &= ~MBC_SCULPT_CUSTOM_VERTICES;
            }
            else if (!orig_cache->batch.sculpt_custom_vertices) {
              orig_cache->batch.sculpt_custom_vertices = cache.batch.sculpt_custom_vertices;
            }
          }
        }
        
        /* Only sync batch_ready for valid batches */
        if (valid_sculpt_custom_ready != 0) {
          orig_cache->batch_ready |= valid_sculpt_custom_ready;
          fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] Synced batch_ready to original mesh cache: 0x%llx "
                          "(sculpt_custom_ready=0x%llx, valid=0x%llx)\n",
                  uint64_t(valid_sculpt_custom_ready), uint64_t(sculpt_custom_ready), uint64_t(valid_sculpt_custom_ready));
          
          /* CRITICAL: After syncing batches, we need to ensure viewport redraw happens.
           * We use DEG_id_tag_update to mark the mesh as changed, which will trigger
           * viewport redraw through the dependency graph system. */
          if (orig_mesh) {
            DEG_id_tag_update(&orig_mesh->id, ID_RECALC_GEOMETRY);
          }
          // #region agent log
          {
            static unsigned long long counter = 0;
            FILE *f = fopen("i:\\Blender_DAD\\blender\\.cursor\\debug.log", "a");
            if (f) {
              fprintf(f, "{\"id\":\"log_%llu\",\"location\":\"draw_cache_impl_mesh.cc:3120\",\"message\":\"SYNC_COMPLETE: Batches synced to orig cache - tagged mesh for redraw\",\"data\":{\"valid_sculpt_custom_ready\":\"0x%llx\",\"orig_batch_ready\":\"0x%llx\",\"mesh_id\":\"%p\"},\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"K\"}\n",
                      counter++, (unsigned long long)valid_sculpt_custom_ready, (unsigned long long)orig_cache->batch_ready, orig_mesh ? &orig_mesh->id : nullptr);
              fclose(f);
            }
          }
          // #endregion
        }
        else {
          fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] NOT syncing batch_ready: no valid batches "
                          "(sculpt_custom_ready=0x%llx)\n",
                  uint64_t(sculpt_custom_ready));
        }
      }
    }
  }
  
  /* Clear only the flags we processed in THIS call.
   * CRITICAL: We must clear only batch_requested (what we processed),
   * NOT cache.batch_ready (which might include flags from previous frames).
   * Python may add new flags to cache.batch_requested during this function,
   * and we must preserve them for the next frame. */
  cache.batch_requested &= ~batch_requested;
  
  /* Clear sculpt custom flags from static map if they are now ready AND valid.
   * IMPORTANT: Only clear flags if batches are actually valid, not just marked as ready.
   * After cache recreation, batch_ready may be set but batches may be freed.
   * Use Object key (preferred) - by this point any mesh.id keys should have been migrated. */
  // #region agent log
  {
    static unsigned long long counter = 0;
    FILE *f = fopen("i:\\Blender_DAD\\blender\\.cursor\\debug.log", "a");
    if (f) {
      fprintf(f, "{\"id\":\"log_%llu\",\"location\":\"draw_cache_impl_mesh.cc:2601\",\"message\":\"CLEAR_MAP: Checking if should clear flags\",\"data\":{\"mesh_key_object\":\"%p\",\"in_map\":%s,\"sculpt_custom_ready\":\"0x%llx\",\"map_size\":%zu},\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"E\"}\n",
              counter++, mesh_key_object, sculpt_custom_flags_preserved.contains(mesh_key_object) ? "true" : "false",
              (unsigned long long)sculpt_custom_ready, sculpt_custom_flags_preserved.size());
      fclose(f);
    }
  }
  // #endregion
  /* IMPORTANT: Do NOT clear flags from map here, even if batches are valid.
   * Flags should persist across cache recreations and geometry changes.
   * They will be cleared only when explicitly requested (e.g., when overlay is disabled).
   * This ensures that flags added during invalidation are preserved and can be found
   * even if evaluated mesh is recreated with a different ID. */
  if (sculpt_custom_flags_preserved.contains(mesh_key_object) && sculpt_custom_ready != 0) {
    /* Verify that batches are actually valid - but don't clear flags from map.
     * Flags should persist to handle cache recreation and evaluated mesh ID changes. */
    DRWBatchFlag valid_sculpt_custom_ready = (DRWBatchFlag)0;
    if (sculpt_custom_ready & MBC_SCULPT_CUSTOM_TRIANGLES) {
      if (cache.batch.sculpt_custom_triangles && is_batch_valid_for_drawing(cache.batch.sculpt_custom_triangles)) {
        valid_sculpt_custom_ready |= MBC_SCULPT_CUSTOM_TRIANGLES;
      }
    }
    if (sculpt_custom_ready & MBC_SCULPT_CUSTOM_EDGES) {
      if (cache.batch.sculpt_custom_edges && is_batch_valid_for_drawing(cache.batch.sculpt_custom_edges)) {
        valid_sculpt_custom_ready |= MBC_SCULPT_CUSTOM_EDGES;
      }
    }
    if (sculpt_custom_ready & MBC_SCULPT_CUSTOM_VERTICES) {
      if (cache.batch.sculpt_custom_vertices && is_batch_valid_for_drawing(cache.batch.sculpt_custom_vertices)) {
        valid_sculpt_custom_ready |= MBC_SCULPT_CUSTOM_VERTICES;
      }
    }
    
    // #region agent log
    {
      static unsigned long long counter = 0;
      FILE *f = fopen("i:\\Blender_DAD\\blender\\.cursor\\debug.log", "a");
      if (f) {
        fprintf(f, "{\"id\":\"log_%llu\",\"location\":\"draw_cache_impl_mesh.cc:2795\",\"message\":\"CLEAR_MAP: Batches valid but NOT clearing flags (flags should persist)\",\"data\":{\"mesh_key_object\":\"%p\",\"sculpt_custom_ready\":\"0x%llx\",\"valid_sculpt_custom_ready\":\"0x%llx\",\"map_size\":%zu},\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\"E\"}\n",
                counter++, mesh_key_object, (unsigned long long)sculpt_custom_ready, (unsigned long long)valid_sculpt_custom_ready, sculpt_custom_flags_preserved.size());
        fclose(f);
      }
    }
    // #endregion
    fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] Batches valid but NOT clearing flags from map (flags should persist): "
                    "sculpt_custom_ready=0x%llx, valid=0x%llx\n",
            uint64_t(sculpt_custom_ready), uint64_t(valid_sculpt_custom_ready));
  }
  
  fprintf(stderr, "[SCULPT_CUSTOM_DEBUG] After batch creation: batch_ready=0x%llx, batch_requested=0x%llx (cleared 0x%llx)\n",
          uint64_t(cache.batch_ready), uint64_t(cache.batch_requested), uint64_t(batch_requested));
}

/** \} */

}  // namespace blender::draw
