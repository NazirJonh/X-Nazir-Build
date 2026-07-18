/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>

#include "MEM_guardedalloc.h"

#include "BLI_array.hh"
#include "BLI_listbase_wrapper.hh"
#include "BLI_map.hh"
#include "BLI_math_vector.h"
#include "BLI_path_utils.hh"
#include "BLI_string.h"
#include "BLI_math_vector_types.hh"
#include "BLI_rect.h"
#include "BLI_span.hh"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_space_types.h"

#include "BKE_context.hh"
#include "BKE_blender.hh"
#include "BKE_customdata.hh"
#include "BKE_editmesh.hh"
#include "BKE_image.hh"
#include "BKE_layer.hh"
#include "BKE_library.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_object.hh"
#include "BKE_paint.hh"
#include "BKE_paint_types.hh"
#include "BKE_screen.hh"

#include "DEG_depsgraph_query.hh"

#include "bmesh.hh"

#include "ED_uvedit.hh"

#include "DEG_depsgraph.hh"

#include "ED_image.hh"
#include "ED_paint.hh"
#include "ED_screen.hh"
#include "ED_select_utils.hh"
#include "ED_space_api.hh"
#include "ED_undo.hh"

#include "BKE_undo_system.hh"

#include "BIF_glutil.hh"

#include "GPU_immediate.hh"
#include "GPU_state.hh"
#include "GPU_texture.hh"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "BLI_math_matrix.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_geom.h"

#include "BLF_api.hh"
#include "UI_interface.hh"
#include "UI_view2d.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "../paint_intern.hh"
#include "../../space_image/image_runtime.hh"
#include "paint_image_select_intern.hh"
#include "paint_image_select_gradient.hh"


namespace blender {

/* -------------------------------------------------------------------- */
/** \name Internal helpers
 * \{ */

static void image_paint_selection_reset(bContext *C)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  if (sima && sima->image) {
    BKE_image_paint_selection_mask_free(sima->image);
  }

  Scene *scene = CTX_data_scene(C);
  scene->toolsettings->imapaint.use_selection_mask = 0;

  DEG_id_tag_update(&scene->id, ID_RECALC_EDITORS);
  DEG_id_tag_update(&scene->id, ID_RECALC_SYNC_TO_EVAL);
  WM_event_add_notifier(C, NC_WINDOW, nullptr);

  if (sima && sima->image) {
    WM_event_add_notifier(C, NC_IMAGE | NA_EDITED, sima->image);
  }
}


/**
 * Poll for all image paint selection operators.
 * Does not require an active brush -- selection tools are independent of the brush.
 * Blocked while a free-transform is in progress so that stray LMB clicks outside the
 * transform widget cannot accidentally start a new selection gesture.
 */
bool image_paint_selection_poll(bContext *C)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima) {
    return false;
  }
  if (sima->mode != SI_MODE_PAINT) {
    return false;
  }
  if (sima->image != nullptr &&
      (!ID_IS_EDITABLE(sima->image) || ID_IS_OVERRIDE_LIBRARY(sima->image)))
  {
    return false;
  }
  if (image_select_transform_is_floating(C)) {
    return false;
  }
  if (image_select_gradient_is_floating(C)) {
    return false;
  }
  if (image_select_warp_is_floating(C)) {
    return false;
  }
  const ARegion *region = CTX_wm_region(C);
  if (!region || region->regiontype != RGN_TYPE_WINDOW) {
    return false;
  }
  return true;
}

/**
 * Visit every pixel center covered by a triangle given in tile-local pixel space.
 * \a fn is invoked as `fn(x, y)` and returns `true` to continue or `false` to stop early.
 * Returns `false` if \a fn requested an early stop, otherwise `true`.
 */
template<typename Fn>
static bool foreach_triangle_pixel(const float2 &p0,
                                   const float2 &p1,
                                   const float2 &p2,
                                   const int width,
                                   const int height,
                                   Fn &&fn)
{
  int min_x = int(floorf(std::min({p0.x, p1.x, p2.x})));
  int max_x = int(ceilf(std::max({p0.x, p1.x, p2.x})));
  int min_y = int(floorf(std::min({p0.y, p1.y, p2.y})));
  int max_y = int(ceilf(std::max({p0.y, p1.y, p2.y})));

  min_x = max_ii(min_x, 0);
  min_y = max_ii(min_y, 0);
  max_x = min_ii(max_x, width - 1);
  max_y = min_ii(max_y, height - 1);

  const auto edge_fn = [](const float2 &a, const float2 &b, const float fx, const float fy) {
    return (fx - a.x) * (b.y - a.y) - (fy - a.y) * (b.x - a.x);
  };

  for (int y = min_y; y <= max_y; y++) {
    for (int x = min_x; x <= max_x; x++) {
      const float fx = float(x) + 0.5f;
      const float fy = float(y) + 0.5f;
      const float w0 = edge_fn(p1, p2, fx, fy);
      const float w1 = edge_fn(p2, p0, fx, fy);
      const float w2 = edge_fn(p0, p1, fx, fy);
      const bool has_neg = (w0 < 0.0f) || (w1 < 0.0f) || (w2 < 0.0f);
      const bool has_pos = (w0 > 0.0f) || (w1 > 0.0f) || (w2 > 0.0f);
      if (has_neg == has_pos) {
        continue;
      }
      if (!fn(x, y)) {
        return false;
      }
    }
  }
  return true;
}

/**
 * Triangle-fan a face's UV polygon into tile-local pixel space and visit every covered pixel.
 * Coordinates are mapped to pixels via `(uv - uv_origin) * size`; the point-in-triangle test is
 * orientation-preserving under this map, so the same primitive serves both reads and writes.
 * See #foreach_triangle_pixel for the \a fn contract; an early stop propagates across triangles.
 */
template<typename Fn>
static void foreach_face_pixel(const BMFace *efa,
                               const BMUVOffsets &offsets,
                               const float2 &uv_origin,
                               const int width,
                               const int height,
                               Fn &&fn)
{
  Vector<float2, 8> px_verts;
  BMIter liter;
  BMLoop *l;
  BM_ITER_ELEM (l, &liter, const_cast<BMFace *>(efa), BM_LOOPS_OF_FACE) {
    const float *uv = BM_ELEM_CD_GET_FLOAT_P(l, offsets.uv);
    px_verts.append(float2((uv[0] - uv_origin.x) * width, (uv[1] - uv_origin.y) * height));
  }

  if (px_verts.size() < 3) {
    return;
  }

  for (const int i : IndexRange(1, px_verts.size() - 2)) {
    if (!foreach_triangle_pixel(px_verts[0], px_verts[i], px_verts[i + 1], width, height, fn)) {
      return;
    }
  }
}

/** Return true if any mask pixel covered by \a efa exceeds \a threshold. */
static bool face_uv_tri_intersects_mask(const BMFace *efa,
                                        const BMUVOffsets &offsets,
                                        const float2 &uv_origin,
                                        const ImBuf *mask,
                                        const float threshold)
{
  const float *data = mask->float_data();
  const int width = mask->x;

  bool found = false;
  foreach_face_pixel(efa, offsets, uv_origin, width, mask->y, [&](const int x, const int y) {
    if (data[y * width + x] > threshold) {
      found = true;
      return false;
    }
    return true;
  });
  return found;
}

/** Write \a fill_value to every mask pixel covered by \a efa. */
static void rasterize_face_to_mask(const BMFace *efa,
                                   const BMUVOffsets &offsets,
                                   const float2 &uv_origin,
                                   ImBuf *mask,
                                   const float fill_value)
{
  float *data = mask->float_data_for_write();
  const int width = mask->x;

  foreach_face_pixel(efa, offsets, uv_origin, width, mask->y, [&](const int x, const int y) {
    data[y * width + x] = fill_value;
    return true;
  });
}

/** Same image datablock, or the same file on disk (duplicate Image IDs are common). */
static bool image_is_paint_target(const Image *target, const Image *candidate)
{
  if (!target || !candidate) {
    return false;
  }
  if (target == candidate) {
    return true;
  }
  if (BKE_image_has_filepath(target) && BKE_image_has_filepath(candidate)) {
    char path_target[FILE_MAX];
    char path_candidate[FILE_MAX];
    BKE_image_user_file_path(nullptr, target, path_target);
    BKE_image_user_file_path(nullptr, candidate, path_candidate);
    if (path_target[0] && path_candidate[0] && BLI_path_cmp(path_target, path_candidate) == 0) {
      return true;
    }
  }
  return false;
}

/** Recursive search for an Image Texture node referencing \a image (includes node groups). */
static bool nodetree_uses_image(const bNodeTree *ntree, const Image *image)
{
  for (const bNode *node : ntree->all_nodes()) {
    if (node->type_legacy == SH_NODE_TEX_IMAGE && node->id) {
      if (image_is_paint_target(image, id_cast<Image *>(node->id))) {
        return true;
      }
    }
    if (ELEM(node->type_legacy, NODE_GROUP, NODE_CUSTOM_GROUP) && node->id) {
      if (nodetree_uses_image(id_cast<const bNodeTree *>(node->id), image)) {
        return true;
      }
    }
  }
  return false;
}

/** True when \a ma references \a image (paint slots or shader nodes). */
static bool material_uses_image(Material *ma, Object *ob, Scene *scene, const Image *image)
{
  if (!ma) {
    return false;
  }

  /* Nodetree first: BKE_texpaint_slot_refresh_cache skips slot build in IMAGE paint mode. */
  if (ma->nodetree && nodetree_uses_image(ma->nodetree, image)) {
    return true;
  }

  if (ma->texpaintslot == nullptr) {
    BKE_texpaint_slot_refresh_cache(scene, ma, ob);
  }

  if (ma->texpaintslot) {
    for (const int s : IndexRange(ma->tot_slots)) {
      if (image_is_paint_target(image, ma->texpaintslot[s].ima)) {
        return true;
      }
    }
  }

  return false;
}

static bool mesh_object_has_uv_maps(const Object *ob)
{
  if (!ob || ob->type != OB_MESH) {
    return false;
  }
  const Mesh *mesh = id_cast<const Mesh *>(ob->data);
  return mesh && !mesh->uv_map_names().is_empty();
}

/** True when \a ob is a mesh that paints onto \a image (material slots, imapaint, or paint_mode). */
static bool mesh_object_uses_image(Object *ob, Scene *scene, const Image *image)
{
  if (!ob || !scene || !scene->toolsettings || ob->type != OB_MESH) {
    return false;
  }

  Image *canvas = nullptr;
  ImageUser *canvas_image_user = nullptr;
  if (BKE_paint_canvas_image_get(
          &scene->toolsettings->paint_mode, ob, &canvas, &canvas_image_user) &&
      image_is_paint_target(image, canvas))
  {
    return true;
  }

  const ImagePaintSettings &imapaint = scene->toolsettings->imapaint;
  if (image_is_paint_target(image, imapaint.canvas)) {
    return true;
  }

  if (imapaint.mode == IMAGEPAINT_MODE_IMAGE && mesh_object_has_uv_maps(ob)) {
    /* Image Editor paints sima->image; imapaint.canvas may point at another datablock. */
    return true;
  }

  for (const int i : IndexRange(ob->totcol)) {
    if (material_uses_image(BKE_object_material_get(ob, i + 1), ob, scene, image)) {
      return true;
    }
  }

  return false;
}

static void image_paint_selection_object_add_unique(Vector<Object *> &objects, Object *ob)
{
  if (!ob || ob->type != OB_MESH) {
    return;
  }
  for (const Object *existing : objects) {
    if (existing == ob) {
      return;
    }
  }
  objects.append(ob);
}

struct ImagePaintObjectCollectData {
  const Image *image;
  Main *bmain;
  ViewLayer *view_layer;
  Vector<Object *> *objects;
};

static void image_paint_object_collect_from_user(Image *ima,
                                                 ID *id,
                                                 ImageUser * /*iuser*/,
                                                 void *userdata)
{
  ImagePaintObjectCollectData *data = static_cast<ImagePaintObjectCollectData *>(userdata);
  if (!image_is_paint_target(data->image, ima) || id == nullptr) {
    return;
  }

  auto try_add = [&](Object *ob) {
    if (ob && BKE_view_layer_base_find(data->view_layer, ob)) {
      image_paint_selection_object_add_unique(*data->objects, ob);
    }
  };

  switch (GS(id->name)) {
    case ID_OB:
      try_add(id_cast<Object *>(id));
      break;
    case ID_MA: {
      Material *ma = id_cast<Material *>(id);
      for (Object *ob = static_cast<Object *>(data->bmain->objects.first); ob;
           ob = static_cast<Object *>(ob->id.next))
      {
        if (!BKE_view_layer_base_find(data->view_layer, ob)) {
          continue;
        }
        for (const int i : IndexRange(ob->totcol)) {
          if (BKE_object_material_get(ob, i + 1) == ma) {
            try_add(ob);
            break;
          }
        }
      }
      break;
    }
    default:
      break;
  }
}

static Vector<Object *> image_paint_selection_canvas_objects_get(const bContext *C, const Image *image)
{
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  Vector<Object *> objects;

  const auto add_if_uses_image = [&](Object *ob) {
    if (!mesh_object_uses_image(ob, scene, image)) {
      return;
    }
    image_paint_selection_object_add_unique(objects, ob);
  };

  BKE_view_layer_synced_ensure(*bmain, scene, view_layer);

  /* Find meshes whose materials/nodes reference this image (same walk as BKE_image reload). */
  ImagePaintObjectCollectData walk_data{};
  walk_data.image = image;
  walk_data.bmain = bmain;
  walk_data.view_layer = view_layer;
  walk_data.objects = &objects;
  BKE_image_walk_all_users(bmain, &walk_data, image_paint_object_collect_from_user);

  for (Base &base : *BKE_view_layer_object_bases_get(view_layer)) {
    if (base.object) {
      add_if_uses_image(base.object);
    }
  }

  for (Object *ob :
       BKE_view_layer_array_from_objects_in_edit_mode_unique_data_with_uvs(*bmain, scene, view_layer, nullptr))
  {
    add_if_uses_image(ob);
  }

  add_if_uses_image(CTX_data_active_object(C));

  /* Image Editor paints sima->image directly; materials may reference a duplicate ID or
   * nothing at all while UV layout still lives on scene meshes. */
  if (objects.is_empty()) {
    for (Base &base : *BKE_view_layer_object_bases_get(view_layer)) {
      if (base.object && mesh_object_has_uv_maps(base.object)) {
        image_paint_selection_object_add_unique(objects, base.object);
      }
    }
  }

  return objects;
}

static BMUVOffsets image_paint_selection_uv_offsets_get(BMesh *bm, Object *ob, const Scene *scene)
{
  const ImagePaintSettings &imapaint = scene->toolsettings->imapaint;

  if (imapaint.mode == IMAGEPAINT_MODE_MATERIAL) {
    Material *ma = BKE_object_material_get(ob, ob->actcol);
    if (ma && ma->texpaintslot && ma->paint_active_slot < ma->tot_slots) {
      const char *uvname = ma->texpaintslot[ma->paint_active_slot].uvname;
      if (uvname && uvname[0]) {
        const int layer = CustomData_get_named_layer_index(&bm->ldata, CD_PROP_FLOAT2, uvname);
        if (layer != -1) {
          return BM_uv_map_offsets_from_layer(bm, layer);
        }
      }
    }
  }

  if (const std::optional<StringRef> uv_name = BKE_paint_canvas_uvmap_name_get(
          &scene->toolsettings->paint_mode, ob))
  {
    const int layer = CustomData_get_named_layer_index(&bm->ldata, CD_PROP_FLOAT2, uv_name->data());
    if (layer != -1) {
      return BM_uv_map_offsets_from_layer(bm, layer);
    }
  }

  return BM_uv_map_offsets_get(bm);
}

/**
 * Return true if the UV bounding box of \a efa overlaps \a uv_rect.
 * Used for geometric seed detection in subtract mode.
 */
static bool face_uv_intersects_rect(const BMFace *efa,
                                    const BMUVOffsets &offsets,
                                    const rctf &uv_rect)
{
  rctf face_uv_bounds;
  BLI_rctf_init_minmax(&face_uv_bounds);
  BMIter liter;
  BMLoop *l;
  BM_ITER_ELEM (l, &liter, const_cast<BMFace *>(efa), BM_LOOPS_OF_FACE) {
    const float *uv = BM_ELEM_CD_GET_FLOAT_P(l, offsets.uv);
    BLI_rctf_do_minmax_v(&face_uv_bounds, uv);
  }
  return BLI_rctf_isect(&face_uv_bounds, &uv_rect, nullptr);
}

/**
 * \param gesture_uv_bounds: When non-null, seed faces are those whose UV bounds intersect the
 * gesture region in UV space (box/lasso/circle). This matches Image Editor selection geometry.
 */
static void image_paint_selection_expand_uv_islands_for_object(
    const Depsgraph *depsgraph,
    Scene *scene,
    Object *ob,
    Image *image,
    const float fill_value,
    const float threshold,
    const rctf *gesture_uv_bounds)
{
  BMesh *bm = nullptr;
  bool owns_bm = false;

  if (ob->mode & OB_MODE_EDIT) {
    BMEditMesh *em = BKE_editmesh_from_object(ob);
    if (!em) {
      return;
    }
    bm = em->bm;
  }
  else {
    const Mesh *mesh = nullptr;
    if (depsgraph) {
      Object *ob_eval = DEG_get_evaluated(depsgraph, ob);
      mesh = BKE_object_get_evaluated_mesh(ob_eval);
    }
    if (!mesh) {
      mesh = id_cast<const Mesh *>(ob->data);
    }
    if (!mesh) {
      return;
    }
    const BMAllocTemplate allocsize = BMALLOC_TEMPLATE_FROM_ME(mesh);
    BMeshCreateParams create_params{};
    BMeshFromMeshParams convert_params{};
    convert_params.calc_face_normal = true;
    convert_params.calc_vert_normal = true;
    bm = BM_mesh_create(&allocsize, &create_params);
    BM_mesh_bm_from_me(bm, mesh, &convert_params);
    owns_bm = true;
  }

  const BMUVOffsets offsets = image_paint_selection_uv_offsets_get(bm, ob, scene);
  if (offsets.uv < 0) {
    if (owns_bm) {
      BM_mesh_free(bm);
    }
    return;
  }

  Vector<int> seed_faces;
  seed_faces.reserve(bm->totface);
  /* O(1) per-face dedup guard — avoids quadratic append_non_duplicates in tile loops. */
  Array<bool> face_seen(bm->totface, false);

  if (gesture_uv_bounds != nullptr) {
    BMIter fiter;
    BMFace *efa;
    int face_index;
    BM_ITER_MESH_INDEX (efa, &fiter, bm, BM_FACES_OF_MESH, face_index) {
      if (BM_elem_flag_test(efa, BM_ELEM_HIDDEN)) {
        continue;
      }
      if (face_uv_intersects_rect(efa, offsets, *gesture_uv_bounds)) {
        face_seen[face_index] = true;
        seed_faces.append(face_index);
      }
    }
  }
  else {
    /* Fallback: seed from faces overlapping already-selected mask pixels. */
    for (ImageTile *tile : ListBaseWrapper<ImageTile>(image->tiles)) {
      const float2 uv_origin(float((tile->tile_number - 1001) % 10),
                              float((tile->tile_number - 1001) / 10));
      const ImBuf *mask = BKE_image_paint_selection_mask_lookup(image, tile->tile_number);
      if (!mask) {
        continue;
      }
      BMIter fiter;
      BMFace *efa;
      int face_index;
      BM_ITER_MESH_INDEX (efa, &fiter, bm, BM_FACES_OF_MESH, face_index) {
        if (BM_elem_flag_test(efa, BM_ELEM_HIDDEN) || face_seen[face_index]) {
          continue;
        }
        if (face_uv_tri_intersects_mask(efa, offsets, uv_origin, mask, threshold)) {
          face_seen[face_index] = true;
          seed_faces.append(face_index);
        }
      }
    }
  }

  if (seed_faces.is_empty()) {
    if (owns_bm) {
      BM_mesh_free(bm);
    }
    return;
  }

  /* ED_uvedit_uv_islands_tag_from_face_indices calls uvedit_face_visible_test which
   * requires BM_ELEM_SELECT when UV sync is off.  A BMesh freshly converted from an
   * Object-mode mesh has no face selection state, so every seed would be rejected.
   * Mark all non-hidden faces as selected so the flood-fill can reach them. */
  if (owns_bm) {
    BMIter fiter_sel;
    BMFace *efa_sel;
    BM_ITER_MESH (efa_sel, &fiter_sel, bm, BM_FACES_OF_MESH) {
      if (!BM_elem_flag_test(efa_sel, BM_ELEM_HIDDEN)) {
        BM_elem_flag_enable(efa_sel, BM_ELEM_SELECT);
      }
    }
  }

  Array<bool> island_tags(bm->totface);
  ED_uvedit_uv_islands_tag_from_face_indices(
      scene, bm, offsets, seed_faces, 0, island_tags);

  /* Bucket island faces by the UDIM tile(s) their UVs fall on, in a single pass over the mesh.
   * This avoids re-scanning every face once per tile during rasterization (was O(tiles * faces)).
   * A face straddling a tile border is registered with every tile it overlaps; the rasterizer
   * clips to each tile's pixel bounds, so per-tile clipping stays correct. */
  Map<int, Vector<int>> tile_faces;
  {
    BMIter fiter;
    BMFace *efa;
    int face_index;
    BM_ITER_MESH_INDEX (efa, &fiter, bm, BM_FACES_OF_MESH, face_index) {
      if (!island_tags[face_index]) {
        continue;
      }
      rctf face_uv_bounds;
      BLI_rctf_init_minmax(&face_uv_bounds);
      BMIter liter;
      BMLoop *l;
      BM_ITER_ELEM (l, &liter, efa, BM_LOOPS_OF_FACE) {
        const float *uv = BM_ELEM_CD_GET_FLOAT_P(l, offsets.uv);
        BLI_rctf_do_minmax_v(&face_uv_bounds, uv);
      }
      const int tx_min = int(floorf(face_uv_bounds.xmin));
      const int tx_max = int(floorf(face_uv_bounds.xmax));
      const int ty_min = int(floorf(face_uv_bounds.ymin));
      const int ty_max = int(floorf(face_uv_bounds.ymax));
      for (int ty = ty_min; ty <= ty_max; ty++) {
        if (ty < 0) {
          continue;
        }
        for (int tx = tx_min; tx <= tx_max; tx++) {
          /* UDIM tiles span columns 0..9; ignore UVs outside the valid grid. */
          if (tx < 0 || tx > 9) {
            continue;
          }
          const int tile_number = 1001 + ty * 10 + tx;
          tile_faces.lookup_or_add_default(tile_number).append(face_index);
        }
      }
    }
  }

  /* `BM_face_at_index` below relies on the face table the tag step already ensured. */
  BM_mesh_elem_table_ensure(bm, BM_FACE);

  for (ImageTile *tile : ListBaseWrapper<ImageTile>(image->tiles)) {
    const Vector<int> *faces = tile_faces.lookup_ptr(tile->tile_number);
    if (faces == nullptr) {
      continue;
    }
    const float2 uv_origin(float((tile->tile_number - 1001) % 10),
                            float((tile->tile_number - 1001) / 10));

    ImBuf *mask;
    if (fill_value < 0.5f) {
      /* Subtract mode: only write to tiles that already have a selection mask.
       * Avoids allocating empty masks on tiles that were never selected. */
      mask = BKE_image_paint_selection_mask_lookup(image, tile->tile_number);
      if (!mask) {
        continue;
      }
    }
    else {
      ImageUser iuser{};
      iuser.tile = tile->tile_number;
      ImBuf *ibuf = BKE_image_acquire_ibuf(image, &iuser, nullptr);
      if (!ibuf) {
        continue;
      }
      mask = BKE_image_paint_selection_mask_get(image, tile->tile_number, ibuf->x, ibuf->y);
      BKE_image_release_ibuf(image, ibuf, nullptr);
    }

    for (const int face_index : *faces) {
      BMFace *efa = BM_face_at_index(bm, face_index);
      rasterize_face_to_mask(efa, offsets, uv_origin, mask, fill_value);
    }
  }

  if (owns_bm) {
    BM_mesh_free(bm);
  }
}

static void image_paint_selection_expand_uv_islands(bContext *C,
                                                    Image *image,
                                                    const eSelectOp sel_op,
                                                    const rctf *gesture_uv_bounds)
{
  Scene *scene = CTX_data_scene(C);
  if (!scene->toolsettings->imapaint.use_selection_uv_island) {
    return;
  }
  if (sel_op == SEL_OP_SUB && !scene->toolsettings->imapaint.use_selection_mask) {
    return;
  }

  const float fill_value = (sel_op == SEL_OP_SUB) ? 0.0f : 1.0f;
  const float threshold = SELECTION_MASK_THRESHOLD;
  const Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);

  Vector<Object *> objects = image_paint_selection_canvas_objects_get(C, image);

  for (Object *ob : objects) {
    image_paint_selection_expand_uv_islands_for_object(
        depsgraph, scene, ob, image, fill_value, threshold, gesture_uv_bounds);
  }
}

static bool image_paint_selection_should_rasterize_gesture(const Scene *scene,
                                                           const eSelectOp sel_op)
{
  const ImagePaintSettings &imapaint = scene->toolsettings->imapaint;
  if (!imapaint.use_selection_uv_island) {
    return true;
  }
  return ELEM(sel_op, SEL_OP_ADD, SEL_OP_SUB);
}

static bool image_paint_selection_has_any_tile_mask(Image *image)
{
  for (ImageTile *tile : ListBaseWrapper<ImageTile>(image->tiles)) {
    if (BKE_image_paint_selection_mask_lookup(image, tile->tile_number) != nullptr) {
      return true;
    }
  }
  return false;
}

static float2 image_paint_selection_tile_uv_origin_get(const ImageTile &tile)
{
  return image_select_udim_tile_uv_origin(tile.tile_number);
}

static bool image_paint_selection_tile_intersection_get(const float2 &uv_origin,
                                                        const rctf &gesture_uv_bounds,
                                                        rctf *r_intersection)
{
  rctf tile_rect;
  tile_rect.xmin = uv_origin.x;
  tile_rect.xmax = uv_origin.x + 1.0f;
  tile_rect.ymin = uv_origin.y;
  tile_rect.ymax = uv_origin.y + 1.0f;
  return BLI_rctf_isect(&tile_rect, &gesture_uv_bounds, r_intersection);
}

template<typename Fn>
static void image_paint_selection_foreach_mask_tile_in_bounds(SpaceImage *sima,
                                                               Image *image,
                                                               const rctf &gesture_uv_bounds,
                                                               Fn &&fn)
{
  for (ImageTile *tile : ListBaseWrapper<ImageTile>(image->tiles)) {
    ImageUser iuser = sima->iuser;
    iuser.tile = tile->tile_number;
    ImBuf *ibuf = BKE_image_acquire_ibuf(image, &iuser, nullptr);
    if (!ibuf) {
      continue;
    }

    const float2 uv_origin = image_paint_selection_tile_uv_origin_get(*tile);
    rctf tile_uv_rect;
    if (image_paint_selection_tile_intersection_get(uv_origin, gesture_uv_bounds, &tile_uv_rect)) {
      ImBuf *mask = BKE_image_paint_selection_mask_get(image, tile->tile_number, ibuf->x, ibuf->y);
      fn(uv_origin, tile_uv_rect, mask);
    }

    BKE_image_release_ibuf(image, ibuf, nullptr);
  }
}

/** \} */
/* -------------------------------------------------------------------- */
/** \name Select All
 * \{ */

static wmOperatorStatus image_select_all_exec(bContext *C, wmOperator * /*op*/)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima || !sima->runtime) {
    return OPERATOR_CANCELLED;
  }
  ImageSelectMoveState *&g_floating_state = sima->runtime->paint_select.move;

  /* Commit any floating move-selection fragment before changing the selection state,
   * so the Move Selection undo step is closed before "Select All" opens its own. */
  if (g_floating_state) {
    image_select_move_commit(C, g_floating_state);
    image_select_move_state_free(g_floating_state);
    g_floating_state = nullptr;
  }

  Image *image = sima->image;
  if (!image) {
    return OPERATOR_CANCELLED;
  }

  Scene *scene = CTX_data_scene(C);

  /* Only select the active tile, not all tiles. */
  ImageUser iuser = sima->iuser;
  /* For UDIM images, sima->iuser.tile is reset to 0 after each temporary acquire.
   * The true active tile is tracked by ima->active_tile_index. */
  if (image->source == IMA_SRC_TILED) {
    const ImageTile *active = static_cast<const ImageTile *>(
        BLI_findlink(&image->tiles, image->active_tile_index));
    iuser.tile = active ? active->tile_number :
                          static_cast<const ImageTile *>(image->tiles.first)->tile_number;
  }
  const ImageTile *active_tile = BKE_image_get_tile_from_iuser(image, &iuser);
  if (!active_tile) {
    return OPERATOR_CANCELLED;
  }
  iuser.tile = active_tile->tile_number;
  ImBuf *ibuf = BKE_image_acquire_ibuf(image, &iuser, nullptr);
  if (!ibuf) {
    return OPERATOR_CANCELLED;
  }

  ED_image_undo_push_begin_selection("Select All", image);

  ImagePaintSettings *imapaint = &scene->toolsettings->imapaint;
  imapaint->use_selection_mask = 1;

  BKE_image_paint_selection_mask_get(image, active_tile->tile_number, ibuf->x, ibuf->y);
  BKE_image_paint_selection_mask_fill(image, active_tile->tile_number, 1.0f);
  BKE_image_paint_selection_set_edge_policy(image, BKE_image_paint_selection_edge_policy_hard());

  BKE_image_release_ibuf(image, ibuf, nullptr);

  DEG_id_tag_update(&scene->id, ID_RECALC_EDITORS);
  WM_event_add_notifier(C, NC_WINDOW, nullptr);
  BKE_image_paint_selection_blend_mask_invalidate(image);
  ED_image_undo_push_end();
  return OPERATOR_FINISHED;
}

void PAINT_OT_image_select_all(wmOperatorType *ot)
{
  ot->name = "Select All";
  ot->idname = "PAINT_OT_image_select_all";
  ot->description = "Select the entire image as a paint mask";
  ot->exec = image_select_all_exec;
  ot->poll = image_paint_selection_poll;
  ot->flag = OPTYPE_REGISTER;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select None
 * \{ */

static wmOperatorStatus image_select_none_exec(bContext *C, wmOperator * /*op*/)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima || !sima->runtime) {
    return OPERATOR_CANCELLED;
  }
  ImageSelectMoveState *&g_floating_state = sima->runtime->paint_select.move;

  /* Commit any floating move-selection fragment before changing the selection state,
   * so the Move Selection undo step is closed before "Select None" opens its own. */
  if (g_floating_state) {
    image_select_move_commit(C, g_floating_state);
    image_select_move_state_free(g_floating_state);
    g_floating_state = nullptr;
  }

  Image *image = sima->image;

  if (image) {
    ED_image_undo_push_begin_selection("Select None", image);
  }

  if (image) {
    BKE_image_paint_selection_mask_free(image);
  }

  Scene *scene = CTX_data_scene(C);
  scene->toolsettings->imapaint.use_selection_mask = 0;

  DEG_id_tag_update(&scene->id, ID_RECALC_SYNC_TO_EVAL);
  WM_event_add_notifier(C, NC_WINDOW, nullptr);

  if (image) {
    ED_image_undo_push_end();
  }
  return OPERATOR_FINISHED;
}

void PAINT_OT_image_select_none(wmOperatorType *ot)
{
  ot->name = "Select None";
  ot->idname = "PAINT_OT_image_select_none";
  ot->description = "Deselect the entire image (remove paint mask)";
  ot->exec = image_select_none_exec;
  ot->poll = image_paint_selection_poll;
  ot->flag = OPTYPE_REGISTER;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Invert Selection
 * \{ */

static wmOperatorStatus image_select_invert_exec(bContext *C, wmOperator * /*op*/)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima || !sima->runtime) {
    return OPERATOR_CANCELLED;
  }
  ImageSelectMoveState *&g_floating_state = sima->runtime->paint_select.move;

  /* Commit any floating move-selection fragment before changing the selection state,
   * so the Move Selection undo step is closed before "Invert Selection" opens its own. */
  if (g_floating_state) {
    image_select_move_commit(C, g_floating_state);
    image_select_move_state_free(g_floating_state);
    g_floating_state = nullptr;
  }

  Image *image = sima->image;
  if (!image) {
    return OPERATOR_CANCELLED;
  }

  Scene *scene = CTX_data_scene(C);
  ImagePaintSettings *imapaint = &scene->toolsettings->imapaint;

  ED_image_undo_push_begin_selection("Invert Selection", image);

  imapaint->use_selection_mask = 1;

  for (ImageTile *tile : ListBaseWrapper<ImageTile>(image->tiles)) {
    ImageUser iuser = sima->iuser;
    iuser.tile = tile->tile_number;
    ImBuf *ibuf = BKE_image_acquire_ibuf(image, &iuser, nullptr);
    if (!ibuf) {
      continue;
    }

    BKE_image_paint_selection_mask_get(image, tile->tile_number, ibuf->x, ibuf->y);
    BKE_image_paint_selection_mask_invert(image, tile->tile_number);

    BKE_image_release_ibuf(image, ibuf, nullptr);
  }

  DEG_id_tag_update(&scene->id, ID_RECALC_EDITORS);
  WM_event_add_notifier(C, NC_WINDOW, nullptr);

  ED_image_undo_push_end();
  return OPERATOR_FINISHED;
}

void PAINT_OT_image_select_invert(wmOperatorType *ot)
{
  ot->name = "Invert Selection";
  ot->idname = "PAINT_OT_image_select_invert";
  ot->description = "Invert the current paint selection mask";
  ot->exec = image_select_invert_exec;
  ot->poll = image_paint_selection_poll;
  ot->flag = OPTYPE_REGISTER;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shared gesture-operator helpers
 *
 * Box / lasso / circle selection share the same invoke (delegate-to-move, then record the click
 * for simple-click detection), modal (drag-distance detection), RNA properties, and the
 * commit-floating / deselect / finish boilerplate. These helpers keep that logic in one place so a
 * new gesture shape only has to provide its own rasterization.
 * \{ */

/** Cursor travel (in pixels) above which a press-release is treated as a drag, not a simple click. */
constexpr int IMAGE_SELECT_CLICK_DRAG_THRESHOLD_PX = 3;

/**
 * Common gesture invoke: hand off to the move operator if the cursor is over a floating fragment,
 * otherwise record the press location and assume a simple click until the cursor moves far enough.
 * Returns true when the event was delegated (the caller should return #OPERATOR_FINISHED).
 */
static bool image_select_gesture_invoke_begin(bContext *C, wmOperator *op, const wmEvent *event)
{
  if (image_select_move_delegate_to_move_operator(C, event)) {
    return true;
  }
  RNA_int_set(op->ptr, "click_x", event->xy[0]);
  RNA_int_set(op->ptr, "click_y", event->xy[1]);
  RNA_boolean_set(op->ptr, "is_simple_click", true);
  return false;
}

/** Clear the simple-click flag once the cursor has travelled past the drag threshold. */
static void image_select_gesture_drag_detect(wmOperator *op, const wmEvent *event)
{
  if (event->type == MOUSEMOVE && op->customdata) {
    const int start_x = RNA_int_get(op->ptr, "click_x");
    const int start_y = RNA_int_get(op->ptr, "click_y");
    if (abs(event->xy[0] - start_x) > IMAGE_SELECT_CLICK_DRAG_THRESHOLD_PX ||
        abs(event->xy[1] - start_y) > IMAGE_SELECT_CLICK_DRAG_THRESHOLD_PX)
    {
      RNA_boolean_set(op->ptr, "is_simple_click", false);
    }
  }
}

/** Register the simple-click bookkeeping properties shared by every gesture operator. */
static void image_select_gesture_properties(wmOperatorType *ot)
{
  RNA_def_boolean(ot->srna, "is_simple_click", false, "Simple Click", "Click without drag");
  RNA_def_int(ot->srna, "click_x", 0, INT_MIN, INT_MAX, "Click X", "", INT_MIN, INT_MAX);
  RNA_def_int(ot->srna, "click_y", 0, INT_MIN, INT_MAX, "Click Y", "", INT_MIN, INT_MAX);
}

/** Commit and discard any floating move-selection fragment for this editor. */
static void image_select_commit_floating_move(bContext *C, SpaceImage *sima)
{
  ImageSelectMoveState *&state = sima->runtime->paint_select.move;
  if (state) {
    image_select_move_commit(C, state);
    image_select_move_state_free(state);
    state = nullptr;
  }
}

/** Clear the whole selection (used for simple-click and empty-gesture paths). */
static void image_select_apply_deselect(bContext *C, Scene *scene, Image *image)
{
  if (scene->toolsettings->imapaint.use_selection_mask) {
    ED_image_undo_push_begin_selection("Deselect", image);
    image_paint_selection_reset(C);
    ED_image_undo_push_end();
  }
}

/** Shared tail of a gesture exec: tag updates, apply the edge policy, and close the undo step. */
static void image_select_gesture_finish(bContext *C,
                                        Scene *scene,
                                        ARegion *region,
                                        Image *image,
                                        const PaintSelectionEdgePolicy &edge_policy)
{
  DEG_id_tag_update(&scene->id, ID_RECALC_EDITORS);
  WM_event_add_notifier(C, NC_WINDOW, nullptr);
  ED_region_tag_redraw(region);

  BKE_image_paint_selection_set_edge_policy(image, edge_policy);
  BKE_image_paint_selection_blend_mask_invalidate(image);
  ED_image_undo_push_end();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select Box
 * \{ */

static wmOperatorStatus image_select_box_exec(bContext *C, wmOperator *op)
{
  Scene *scene = CTX_data_scene(C);
  ARegion *region = CTX_wm_region(C);
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima || !sima->runtime || !region) {
    return OPERATOR_CANCELLED;
  }
  Image *image = sima->image;
  if (!image) {
    return OPERATOR_CANCELLED;
  }

  rctf rectf;
  WM_operator_properties_border_to_rctf(op, &rectf);

  const bool is_simple_click = RNA_boolean_get(op->ptr, "is_simple_click");
  if (is_simple_click ||
      (BLI_rctf_size_x(&rectf) == 0.0f && BLI_rctf_size_y(&rectf) == 0.0f))
  {
    image_select_commit_floating_move(C, sima);
    image_select_apply_deselect(C, scene, image);
    return OPERATOR_FINISHED;
  }

  /* Commit any floating move-selection fragment before starting a new selection. */
  image_select_commit_floating_move(C, sima);

  ui::view2d_region_to_view_rctf(&region->v2d, &rectf, &rectf);

  const eSelectOp sel_op = eSelectOp(RNA_enum_get(op->ptr, "mode"));
  const bool rasterize_gesture = image_paint_selection_should_rasterize_gesture(scene, sel_op);

  /* Auto-snap selection bounds to UDIM tile borders (integer UV coordinates) when close
   * enough. This makes it easy to select exactly one full tile or align to tile edges
   * without pixel-perfect cursor placement. Threshold: 2 % of one tile in UV units. */
  {
    const float snap_thresh = 0.02f;
    auto snap_to_udim_border = [](float v, float threshold) -> float {
      const float rounded = roundf(v);
      return (fabsf(v - rounded) < threshold) ? rounded : v;
    };
    rectf.xmin = snap_to_udim_border(rectf.xmin, snap_thresh);
    rectf.xmax = snap_to_udim_border(rectf.xmax, snap_thresh);
    rectf.ymin = snap_to_udim_border(rectf.ymin, snap_thresh);
    rectf.ymax = snap_to_udim_border(rectf.ymax, snap_thresh);
  }

  ED_image_undo_push_begin_selection("Box Select", image);

  /* Subtract: expand islands before filling pixels so we seed from the gesture geometry,
   * not from remaining selected pixels (which would wrongly deselect unrelated islands). */
  if (sel_op == SEL_OP_SUB) {
    image_paint_selection_expand_uv_islands(C, image, sel_op, &rectf);
  }

  if (sel_op == SEL_OP_SET) {
    BKE_image_paint_selection_mask_free(image);
  }

  ImagePaintSettings *imapaint = &scene->toolsettings->imapaint;
  imapaint->use_selection_mask = 1;

  if (rasterize_gesture) {
    image_paint_selection_foreach_mask_tile_in_bounds(
        sima, image, rectf, [&](const float2 &uv_origin, const rctf &tile_uv_rect, ImBuf *mask) {
          float *data = mask->float_data_for_write();
          const int x1 = int(roundf((tile_uv_rect.xmin - uv_origin.x) * mask->x));
          const int y1 = int(roundf((tile_uv_rect.ymin - uv_origin.y) * mask->y));
          const int x2 = int(roundf((tile_uv_rect.xmax - uv_origin.x) * mask->x));
          const int y2 = int(roundf((tile_uv_rect.ymax - uv_origin.y) * mask->y));
          const float fill_value = (sel_op == SEL_OP_SUB) ? 0.0f : 1.0f;

          for (int y = y1; y < y2; y++) {
            for (int x = x1; x < x2; x++) {
              if (x >= 0 && x < mask->x && y >= 0 && y < mask->y) {
                data[y * mask->x + x] = fill_value;
              }
            }
          }
        });
  }

  /* Add/set: expand to full UV islands touched by the gesture. */
  if (sel_op != SEL_OP_SUB) {
    image_paint_selection_expand_uv_islands(C, image, sel_op, &rectf);
  }
  if (sel_op == SEL_OP_SET && !rasterize_gesture) {
    imapaint->use_selection_mask = image_paint_selection_has_any_tile_mask(image) ? 1 : 0;
  }

  image_select_gesture_finish(
      C, scene, region, image, BKE_image_paint_selection_edge_policy_hard());
  return OPERATOR_FINISHED;
}

static wmOperatorStatus image_select_box_invoke(bContext *C,
                                                wmOperator *op,
                                                const wmEvent *event)
{
  if (image_select_gesture_invoke_begin(C, op, event)) {
    return OPERATOR_FINISHED;
  }
  return WM_gesture_box_invoke(C, op, event);
}

static wmOperatorStatus image_select_box_modal(bContext *C,
                                               wmOperator *op,
                                               const wmEvent *event)
{
  image_select_gesture_drag_detect(op, event);
  return WM_gesture_box_modal(C, op, event);
}

void PAINT_OT_image_select_box(wmOperatorType *ot)
{
  ot->name = "Select Box";
  ot->idname = "PAINT_OT_image_select_box";
  ot->description = "Select a rectangular region as a paint mask";

  ot->invoke = image_select_box_invoke;
  ot->modal = image_select_box_modal;
  ot->exec = image_select_box_exec;
  ot->cancel = WM_gesture_box_cancel;
  ot->poll = image_paint_selection_poll;
  ot->flag = OPTYPE_REGISTER;

  WM_operator_properties_gesture_box(ot);
  WM_operator_properties_select_operation_simple(ot);

  image_select_gesture_properties(ot);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select Lasso
 * \{ */

/**
 * Scanline polygon fill for a 1-channel float buffer.
 * Fills the interior of \a points with \a color.
 * Requires at least 3 points; silently does nothing otherwise.
 */
static void fill_polygon_float(ImBuf *ibuf, Span<int2> points, float color)
{
  if (points.size() < 3) {
    return;
  }

  const int width = ibuf->x;
  const int height = ibuf->y;

  int min_y = points[0].y;
  int max_y = points[0].y;
  for (const int2 &p : points) {
    min_y = min_ii(min_y, p.y);
    max_y = max_ii(max_y, p.y);
  }
  min_y = max_ii(min_y, 0);
  max_y = min_ii(max_y, height - 1);

  Vector<int> intersections;
  for (int y = min_y; y <= max_y; y++) {
    intersections.clear();
    const int n = points.size();
    for (int i = 0; i < n; i++) {
      const int next = (i + 1) % n;
      const int y1 = points[i].y;
      const int y2 = points[next].y;
      if ((y1 <= y && y2 > y) || (y2 <= y && y1 > y)) {
        const float x = float(y - y1) * float(points[next].x - points[i].x) /
                            float(y2 - y1) +
                        float(points[i].x);
        intersections.append(int(x));
      }
    }
    std::sort(intersections.begin(), intersections.end());

    for (int i = 0; i + 1 < int(intersections.size()); i += 2) {
      const int x1 = max_ii(intersections[i], 0);
      const int x2 = min_ii(intersections[i + 1], width - 1);
      float *row = ibuf->float_data_for_write() + y * width;
      for (int x = x1; x <= x2; x++) {
        row[x] = color;
      }
    }
  }
}

static wmOperatorStatus image_select_lasso_invoke(bContext *C,
                                                   wmOperator *op,
                                                   const wmEvent *event)
{
  if (image_select_gesture_invoke_begin(C, op, event)) {
    return OPERATOR_FINISHED;
  }
  return WM_gesture_lasso_invoke(C, op, event);
}

static wmOperatorStatus image_select_lasso_modal(bContext *C,
                                                  wmOperator *op,
                                                  const wmEvent *event)
{
  image_select_gesture_drag_detect(op, event);
  return WM_gesture_lasso_modal(C, op, event);
}

static wmOperatorStatus image_select_lasso_exec(bContext *C, wmOperator *op)
{
  Scene *scene = CTX_data_scene(C);
  ARegion *region = CTX_wm_region(C);
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima || !sima->runtime || !region) {
    return OPERATOR_CANCELLED;
  }
  Image *image = sima->image;
  if (!image) {
    return OPERATOR_CANCELLED;
  }

  const bool is_simple_click = RNA_boolean_get(op->ptr, "is_simple_click");
  if (is_simple_click) {
    image_select_commit_floating_move(C, sima);
    image_select_apply_deselect(C, scene, image);
    return OPERATOR_FINISHED;
  }

  /* Commit any floating move-selection fragment before starting a new selection. */
  image_select_commit_floating_move(C, sima);

  Array<int2> mcoords = WM_gesture_lasso_path_to_array(C, op);
  if (mcoords.is_empty() || int(mcoords.size()) < 3) {
    image_select_apply_deselect(C, scene, image);
    return OPERATOR_FINISHED;
  }

  const eSelectOp sel_op = eSelectOp(RNA_enum_get(op->ptr, "mode"));
  const bool rasterize_gesture = image_paint_selection_should_rasterize_gesture(scene, sel_op);

  ED_image_undo_push_begin_selection("Lasso Select", image);

  if (sel_op == SEL_OP_SET) {
    BKE_image_paint_selection_mask_free(image);
  }

  ImagePaintSettings *imapaint = &scene->toolsettings->imapaint;
  imapaint->use_selection_mask = 1;

  /* Convert lasso points to UV space once. */
  Vector<float2> uv_points;
  uv_points.reserve(mcoords.size());
  rctf lasso_uv_bounds;
  BLI_rctf_init_minmax(&lasso_uv_bounds);
  for (const int2 &p : mcoords) {
    float co[2] = {float(p.x), float(p.y)};
    ui::view2d_region_to_view(&region->v2d, co[0], co[1], &co[0], &co[1]);
    uv_points.append(float2(co[0], co[1]));
    BLI_rctf_do_minmax_v(&lasso_uv_bounds, co);
  }

  /* Subtract: expand islands before filling so we seed from gesture geometry. */
  if (sel_op == SEL_OP_SUB) {
    image_paint_selection_expand_uv_islands(C, image, sel_op, &lasso_uv_bounds);
  }

  if (rasterize_gesture) {
    image_paint_selection_foreach_mask_tile_in_bounds(
        sima,
        image,
        lasso_uv_bounds,
        [&](const float2 &uv_origin, const rctf & /*tile_uv_rect*/, ImBuf *mask) {
          /* Convert UV points to tile pixel space. */
          Vector<int2> tile_points;
          tile_points.reserve(uv_points.size());
          for (const float2 &uv : uv_points) {
            tile_points.append(int2(int(roundf((uv.x - uv_origin.x) * mask->x)),
                                    int(roundf((uv.y - uv_origin.y) * mask->y))));
          }

          const float color = (sel_op == SEL_OP_SUB) ? 0.0f : 1.0f;
          fill_polygon_float(mask, tile_points, color);
        });
  }

  /* Add/set: expand to full UV islands touched by the gesture. */
  if (sel_op != SEL_OP_SUB) {
    image_paint_selection_expand_uv_islands(C, image, sel_op, &lasso_uv_bounds);
  }
  if (sel_op == SEL_OP_SET && !rasterize_gesture) {
    imapaint->use_selection_mask = image_paint_selection_has_any_tile_mask(image) ? 1 : 0;
  }

  image_select_gesture_finish(
      C, scene, region, image, BKE_image_paint_selection_edge_policy_feathered());
  return OPERATOR_FINISHED;
}

void PAINT_OT_image_select_lasso(wmOperatorType *ot)
{
  ot->name = "Select Lasso";
  ot->idname = "PAINT_OT_image_select_lasso";
  ot->description = "Select a freehand region as a paint mask";

  ot->invoke = image_select_lasso_invoke;
  ot->modal = image_select_lasso_modal;
  ot->exec = image_select_lasso_exec;
  ot->poll = image_paint_selection_poll;
  ot->flag = OPTYPE_REGISTER;

  WM_operator_properties_gesture_lasso(ot);
  WM_operator_properties_select_operation_simple(ot);

  image_select_gesture_properties(ot);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select Circle
 * \{ */

/**
 * Rasterize a filled ellipse into a 1-channel float buffer.
 * Passing equal #rx and #ry produces a perfect circle.
 * Separate radii are needed because UV space is not always isotropic --
 * the Image Editor view2d can have a non-1:1 aspect ratio (e.g. when
 * multiple UDIM tiles are visible side-by-side), so the screen-pixel
 * radius maps to different UV extents along X and Y.
 */
static void fill_circle_float(ImBuf *ibuf, int cx, int cy, int rx, int ry, float color)
{
  const int width = ibuf->x;
  const int height = ibuf->y;
  const int x1 = max_ii(cx - rx, 0);
  const int x2 = min_ii(cx + rx, width - 1);
  const int y1 = max_ii(cy - ry, 0);
  const int y2 = min_ii(cy + ry, height - 1);
  const float rx2 = float(rx) * float(rx);
  const float ry2 = float(ry) * float(ry);

  for (int y = y1; y <= y2; y++) {
    const float dy = float(y - cy);
    const float dy2 = (dy * dy) / ry2;
    for (int x = x1; x <= x2; x++) {
      const float dx = float(x - cx);
      if ((dx * dx) / rx2 + dy2 <= 1.0f) {
        ibuf->float_data_for_write()[y * width + x] = color;
      }
    }
  }
}

static wmOperatorStatus image_select_circle_invoke(bContext *C,
                                                    wmOperator *op,
                                                    const wmEvent *event)
{
  if (image_select_gesture_invoke_begin(C, op, event)) {
    return OPERATOR_FINISHED;
  }
  return WM_gesture_circle_invoke(C, op, event);
}

static wmOperatorStatus image_select_circle_modal(bContext *C,
                                                   wmOperator *op,
                                                   const wmEvent *event)
{
  image_select_gesture_drag_detect(op, event);
  return WM_gesture_circle_modal(C, op, event);
}

static wmOperatorStatus image_select_circle_exec(bContext *C, wmOperator *op)
{
  Scene *scene = CTX_data_scene(C);
  ARegion *region = CTX_wm_region(C);
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima || !sima->runtime || !region) {
    return OPERATOR_CANCELLED;
  }
  Image *image = sima->image;
  if (!image) {
    return OPERATOR_CANCELLED;
  }

  const bool is_simple_click = RNA_boolean_get(op->ptr, "is_simple_click");
  const int mradius = RNA_int_get(op->ptr, "radius");

  if (is_simple_click || mradius <= 0) {
    image_select_commit_floating_move(C, sima);
    image_select_apply_deselect(C, scene, image);
    return OPERATOR_FINISHED;
  }

  /* Commit any floating move-selection fragment before starting a new selection. */
  image_select_commit_floating_move(C, sima);

  const eSelectOp sel_op = eSelectOp(RNA_enum_get(op->ptr, "mode"));
  const bool rasterize_gesture = image_paint_selection_should_rasterize_gesture(scene, sel_op);

  ED_image_undo_push_begin_selection("Circle Select", image);

  if (sel_op == SEL_OP_SET) {
    BKE_image_paint_selection_mask_free(image);
  }

  ImagePaintSettings *imapaint = &scene->toolsettings->imapaint;
  imapaint->use_selection_mask = 1;

  const int mx = RNA_int_get(op->ptr, "x");
  const int my = RNA_int_get(op->ptr, "y");

  /* Convert center to UV space. */
  float co_center[2] = {float(mx), float(my)};
  ui::view2d_region_to_view(&region->v2d, co_center[0], co_center[1], &co_center[0], &co_center[1]);

  /* Convert radius to UV space along X: measure a point one radius to the right. */
  float co_edge[2] = {float(mx + mradius), float(my)};
  ui::view2d_region_to_view(&region->v2d, co_edge[0], co_edge[1], &co_edge[0], &co_edge[1]);
  const float uv_radius = co_edge[0] - co_center[0];

  /* Convert radius to UV space along Y separately.
   * The Image Editor view2d can have a non-1:1 pixel-to-UV ratio (e.g. when multiple
   * UDIM tiles are shown side-by-side), so the horizontal and vertical UV extents of
   * the same screen-pixel radius differ. Without this correction the circle appears as
   * a flattened ellipse on all UDIM tiles except the first one. */
  float co_edge_y[2] = {float(mx), float(my + mradius)};
  ui::view2d_region_to_view(&region->v2d, co_edge_y[0], co_edge_y[1], &co_edge_y[0], &co_edge_y[1]);
  const float uv_radius_y = fabsf(co_edge_y[1] - co_center[1]);

  const float abs_uv_radius = fabsf(uv_radius);
  rctf circle_uv_bounds;
  circle_uv_bounds.xmin = co_center[0] - abs_uv_radius;
  circle_uv_bounds.xmax = co_center[0] + abs_uv_radius;
  circle_uv_bounds.ymin = co_center[1] - uv_radius_y;
  circle_uv_bounds.ymax = co_center[1] + uv_radius_y;

  /* Subtract: expand islands before filling so we seed from gesture geometry. */
  if (sel_op == SEL_OP_SUB) {
    image_paint_selection_expand_uv_islands(C, image, sel_op, &circle_uv_bounds);
  }

  if (rasterize_gesture) {
    image_paint_selection_foreach_mask_tile_in_bounds(
        sima,
        image,
        circle_uv_bounds,
        [&](const float2 &uv_origin, const rctf & /*tile_uv_rect*/, ImBuf *mask) {
          const int cx = int(roundf((co_center[0] - uv_origin.x) * mask->x));
          const int cy = int(roundf((co_center[1] - uv_origin.y) * mask->y));
          int rx = int(roundf(uv_radius * mask->x));
          if (rx <= 0) {
            rx = 1;
          }
          /* Compute Y pixel radius from the separately measured UV Y radius so the circle is
           * correct on non-square tiles and in views with a non-1:1 aspect ratio. */
          int ry = int(roundf(uv_radius_y * mask->y));
          if (ry <= 0) {
            ry = 1;
          }

          const float color = (sel_op == SEL_OP_SUB) ? 0.0f : 1.0f;
          fill_circle_float(mask, cx, cy, rx, ry, color);
        });
  }

  /* Add/set: expand to full UV islands touched by the gesture. */
  if (sel_op != SEL_OP_SUB) {
    image_paint_selection_expand_uv_islands(C, image, sel_op, &circle_uv_bounds);
  }
  if (sel_op == SEL_OP_SET && !rasterize_gesture) {
    imapaint->use_selection_mask = image_paint_selection_has_any_tile_mask(image) ? 1 : 0;
  }

  image_select_gesture_finish(
      C, scene, region, image, BKE_image_paint_selection_edge_policy_feathered());
  return OPERATOR_FINISHED;
}

void PAINT_OT_image_select_circle(wmOperatorType *ot)
{
  ot->name = "Select Circle";
  ot->idname = "PAINT_OT_image_select_circle";
  ot->description = "Select a circular region as a paint mask";

  ot->invoke = image_select_circle_invoke;
  ot->modal = image_select_circle_modal;
  ot->exec = image_select_circle_exec;
  ot->cancel = WM_gesture_circle_cancel;
  ot->poll = image_paint_selection_poll;
  ot->flag = OPTYPE_REGISTER;

  WM_operator_properties_gesture_circle(ot);
  WM_operator_properties_select_operation_simple(ot);

  image_select_gesture_properties(ot);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Session lifetime
 * \{ */

void paint_select_session_free(PaintSelectSession &session)
{
  if (session.move) {
    image_select_move_state_free(session.move);
    session.move = nullptr;
  }
  if (session.transform) {
    image_select_transform_state_free(session.transform);
    session.transform = nullptr;
  }
  if (session.gradient) {
    image_select_gradient_state_free(session.gradient);
    session.gradient = nullptr;
  }
  if (session.warp) {
    image_select_warp_state_free(session.warp);
    session.warp = nullptr;
  }
}

/** \} */

}  /* namespace blender */
