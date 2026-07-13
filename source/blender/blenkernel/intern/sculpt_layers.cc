/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "BKE_sculpt_layers.hh"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>

#include "CLG_log.h"

#include "MEM_guardedalloc.h"

#include "DNA_key_types.h"
#include "DNA_mesh_types.h"

#include "BLI_array.hh"
#include "BLI_listbase.h"
#include "BLI_string_utf8.h"
#include "BLI_string_utils.hh"
#include "BLI_task.hh"

#include "BLT_translation.hh"

#include "BKE_key.hh"
#include "BKE_multires_grid_resample.hh"

#include "BLO_read_write.hh"

static CLG_LogRef LOG = {"bke.sculpt_layers"};

namespace blender::bke::sculpt_layers {

static int unique_uid(const Mesh &mesh)
{
  int uid = 1;
  for (const SculptLayer &layer : mesh.sculpt_layers) {
    uid = std::max(uid, layer.uid + 1);
  }
  return uid;
}

SculptLayer *add(Mesh &mesh,
                 const char *name,
                 const short domain,
                 const int totelem,
                 const short level)
{
  SculptLayer *layer = MEM_new<SculptLayer>(__func__);
  STRNCPY_UTF8(layer->name, (name && name[0]) ? name : "Layer");
  layer->influence = 1.0f;
  layer->flag = SCULPT_LAYER_ENABLED;
  layer->domain = domain;
  layer->level = (domain == short(SCULPT_LAYER_DOMAIN_GRID)) ? level : 0;
  layer->uid = unique_uid(mesh);

  BLI_addtail(&mesh.sculpt_layers, layer);
  BLI_uniquename(&mesh.sculpt_layers,
                 layer,
                 layer->name,
                 '.',
                 offsetof(SculptLayer, name),
                 sizeof(layer->name));

  if (totelem > 0) {
    data_ensure(*layer, totelem);
  }
  active_set(mesh, layer);
  return layer;
}

void remove(Mesh &mesh, SculptLayer &layer)
{
  const int index = index_of(mesh, layer);
  BLI_remlink(&mesh.sculpt_layers, &layer);
  if (layer.data) {
    MEM_delete_void(layer.data);
  }
  MEM_delete(&layer);

  const int count = BLI_listbase_count(&mesh.sculpt_layers);
  mesh.sculpt_layers_active_index = std::clamp(index, 0, std::max(0, count - 1));
}

SculptLayer *duplicate(Mesh &mesh, const SculptLayer &src)
{
  SculptLayer *layer = MEM_dupalloc<SculptLayer>(&src);
  layer->next = layer->prev = nullptr;
  if (src.data) {
    layer->data = MEM_dupalloc_void(src.data);
  }
  layer->uid = unique_uid(mesh);

  BLI_insertlinkafter(&mesh.sculpt_layers, const_cast<SculptLayer *>(&src), layer);
  BLI_uniquename(&mesh.sculpt_layers,
                 layer,
                 layer->name,
                 '.',
                 offsetof(SculptLayer, name),
                 sizeof(layer->name));
  active_set(mesh, layer);
  return layer;
}

SculptLayer *active_get(Mesh &mesh)
{
  return static_cast<SculptLayer *>(
      BLI_findlink(&mesh.sculpt_layers, mesh.sculpt_layers_active_index));
}

const SculptLayer *active_get(const Mesh &mesh)
{
  return static_cast<const SculptLayer *>(
      BLI_findlink(&mesh.sculpt_layers, mesh.sculpt_layers_active_index));
}

void active_set(Mesh &mesh, const SculptLayer *layer)
{
  mesh.sculpt_layers_active_index = layer ? index_of(mesh, *layer) : -1;
}

int index_of(const Mesh &mesh, const SculptLayer &layer)
{
  return BLI_findindex(&mesh.sculpt_layers, &layer);
}

SculptLayer *find_by_uid(Mesh &mesh, const int uid)
{
  for (SculptLayer &layer : mesh.sculpt_layers) {
    if (layer.uid == uid) {
      return &layer;
    }
  }
  return nullptr;
}

bool solo_active(const Mesh &mesh)
{
  for (const SculptLayer &layer : mesh.sculpt_layers) {
    if (layer.flag & SCULPT_LAYER_SOLO_HIDDEN) {
      return true;
    }
  }
  return false;
}

MutableSpan<float3> data_ensure(SculptLayer &layer, const int totelem)
{
  if (layer.data != nullptr && layer.totelem == totelem) {
    /* Fast path: data already allocated and correctly sized. Kept allocation-free and
     * logging-free — the timing probe below only runs on the (rare) realloc path and only when
     * #SCULPT_LAYERS_DEBUG_LOG is enabled. */
    return {static_cast<float3 *>(layer.data), layer.totelem};
  }
#if SCULPT_LAYERS_DEBUG_LOG
  const auto func_start = std::chrono::high_resolution_clock::now();
  printf("[DEBUG-perf] data_ensure: realloc needed, old_totelem=%d, new_totelem=%d\n",
         layer.totelem, totelem);
#endif
  if (layer.data) {
    MEM_delete_void(layer.data);
  }
  layer.data = MEM_new_array_zeroed<float[3]>(size_t(totelem), __func__);
  layer.totelem = totelem;
#if SCULPT_LAYERS_DEBUG_LOG
  printf("[DEBUG-perf] data_ensure: total=%lld us\n",
         std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::high_resolution_clock::now() - func_start)
             .count());
#endif
  return {static_cast<float3 *>(layer.data), layer.totelem};
}

MutableSpan<float3> data_get(SculptLayer &layer)
{
  if (layer.data == nullptr) {
    return {};
  }
  return {static_cast<float3 *>(layer.data), layer.totelem};
}

Span<float3> data_get(const SculptLayer &layer)
{
  if (layer.data == nullptr) {
    return {};
  }
  return {static_cast<const float3 *>(layer.data), layer.totelem};
}

void data_clear(SculptLayer &layer)
{
  data_get(layer).fill(float3(0.0f));
}

void apply_delta_mesh(const SculptLayer &layer, const float factor, MutableSpan<float3> positions)
{
  if (layer.data == nullptr || factor == 0.0f) {
    return;
  }
  const Span<float3> deltas(static_cast<const float3 *>(layer.data), layer.totelem);
  if (deltas.size() != positions.size()) {
    /* Stale layer whose element count no longer matches the mesh topology (e.g. a remesh that
     * bypassed the free hook). Skip it rather than corrupting a mismatched vertex range. */
    return;
  }
  /* Bandwidth-bound vertex pass; parallelize since the influence slider runs this on the whole mesh
   * per tick. #parallel_for stays serial below the grain size, so small meshes pay no overhead. */
  threading::parallel_for(positions.index_range(), 8192, [&](const IndexRange range) {
    for (const int64_t i : range) {
      positions[i] += deltas[i] * factor;
    }
  });
}

float effective(const SculptLayer &layer)
{
  return (layer.flag & SCULPT_LAYER_ENABLED) ? layer.influence : 0.0f;
}

void combine_layers_mesh(const Span<float3> base,
                         const ListBaseT<SculptLayer> &layers,
                         MutableSpan<float3> r_positions)
{
  BLI_assert(r_positions.size() == base.size());
  r_positions.copy_from(base);
  for (const SculptLayer &layer : layers) {
    if (layer.domain != SCULPT_LAYER_DOMAIN_VERT || layer.data == nullptr) {
      continue;
    }
    const float eff = effective(layer);
    if (eff == 0.0f) {
      continue;
    }
    const Span<float3> data(static_cast<const float3 *>(layer.data), layer.totelem);
    if (data.size() != r_positions.size()) {
      /* Stale layer whose element count no longer matches the mesh topology. Skip it rather than
       * composing deltas over a mismatched vertex range. */
      continue;
    }
    threading::parallel_for(r_positions.index_range(), 8192, [&](const IndexRange range) {
      for (const int64_t i : range) {
        r_positions[i] += data[i] * eff;
      }
    });
  }
}

void apply_vert_layers(const ListBaseT<SculptLayer> &layers, MutableSpan<float3> positions)
{
  for (const SculptLayer &layer : layers) {
    if (layer.domain != SCULPT_LAYER_DOMAIN_VERT) {
      continue;
    }
    apply_delta_mesh(layer, effective(layer), positions);
  }
}

void apply_vert_layers_eval(Mesh &mesh)
{
  apply_vert_layers(mesh.sculpt_layers, mesh.vert_positions_for_write());
}

/* Shared body of the two shape-key transition helpers: `positions[i] += sign * sum(...)`. Bails out
 * before touching the positions when the mesh carries no vertex-layer data, so the copy-on-write of
 * #Mesh::vert_positions_for_write is not paid on the (common) mesh without layers. */
static void shift_vert_layers_in_positions(Mesh &mesh, const float sign)
{
  bool any = false;
  for (const SculptLayer &layer : mesh.sculpt_layers) {
    if (layer.domain == SCULPT_LAYER_DOMAIN_VERT && layer.data != nullptr &&
        effective(layer) != 0.0f)
    {
      any = true;
      break;
    }
  }
  if (!any) {
    return;
  }
  MutableSpan<float3> positions = mesh.vert_positions_for_write();
  for (const SculptLayer &layer : mesh.sculpt_layers) {
    if (layer.domain != SCULPT_LAYER_DOMAIN_VERT) {
      continue;
    }
    apply_delta_mesh(layer, sign * effective(layer), positions);
  }
  mesh.tag_positions_changed();
}

void strip_vert_layers_from_positions(Mesh &mesh)
{
  shift_vert_layers_in_positions(mesh, -1.0f);
}

void bake_vert_layers_into_positions(Mesh &mesh)
{
  shift_vert_layers_in_positions(mesh, 1.0f);
}

KeyBlock *bake_vert_layers_into_new_shape_key(Mesh &mesh)
{
  Key *key = mesh.key;
  if (key == nullptr || key->type != KEY_RELATIVE) {
    return nullptr;
  }
  bool any = false;
  for (const SculptLayer &layer : mesh.sculpt_layers) {
    if (layer.domain == SCULPT_LAYER_DOMAIN_VERT && layer.data != nullptr &&
        layer.totelem == mesh.verts_num)
    {
      any = true;
      break;
    }
  }
  if (!any) {
    return nullptr;
  }

  KeyBlock *kb = BKE_keyblock_add_ctime(key, DATA_("Sculpt Layers"), false);
  /* The positions are the un-layered basis while shape keys are present, so this seeds the block
   * with the basis and the layer sum below turns it into `basis + sum`. */
  BKE_keyblock_convert_from_mesh(&mesh, key, kb);
  if (kb->data == nullptr || kb->totelem != mesh.verts_num) {
    return kb;
  }
  apply_vert_layers(mesh.sculpt_layers,
                    MutableSpan(static_cast<float3 *>(kb->data), kb->totelem));

  /* Relative to the reference key, so the block's delta is the layer sum alone, and at full value
   * so the surface is unchanged by the bake. */
  kb->relative = short(std::max(BLI_findindex(&key->block, key->refkey), 0));
  kb->curval = 1.0f;
  return kb;
}

void apply_vert_layer_to_shape_keys(Mesh &mesh, const SculptLayer &layer, const float factor)
{
  if (mesh.key == nullptr || layer.domain != SCULPT_LAYER_DOMAIN_VERT || layer.data == nullptr ||
      factor == 0.0f)
  {
    return;
  }
  const Span<float3> deltas(static_cast<const float3 *>(layer.data), layer.totelem);
  if (deltas.size() != mesh.verts_num) {
    /* Stale layer whose element count no longer matches the topology. */
    return;
  }
  apply_delta_mesh(layer, factor, mesh.vert_positions_for_write());
  mesh.tag_positions_changed();

  for (KeyBlock &kb : mesh.key->block) {
    if (kb.data == nullptr || kb.totelem != mesh.verts_num) {
      continue;
    }
    MutableSpan<float3> data(static_cast<float3 *>(kb.data), kb.totelem);
    threading::parallel_for(data.index_range(), 8192, [&](const IndexRange range) {
      for (const int64_t i : range) {
        data[i] += deltas[i] * factor;
      }
    });
  }
}

void resample_grid_layers(Mesh &mesh, const int grids_num, const int new_level)
{
  bool warned = false;
  for (SculptLayer &layer : mesh.sculpt_layers) {
    if (layer.domain != SCULPT_LAYER_DOMAIN_GRID) {
      continue;
    }
    if (layer.data == nullptr) {
      layer.level = short(std::max(new_level, 0));
      layer.totelem = 0;
      continue;
    }
    if (new_level <= 0) {
      /* No subdivision levels left: there is nothing for a grid layer to displace. */
      MEM_delete_void(layer.data);
      layer.data = nullptr;
      layer.totelem = 0;
      layer.level = 0;
      if (!warned) {
        CLOG_WARN(&LOG, "Grid sculpt layer data cleared: all subdivision levels were removed.");
        warned = true;
      }
      continue;
    }
    if (layer.level == short(new_level) &&
        layer.totelem == grid_totelem(grids_num, new_level)) {
      continue;
    }
    if (layer.level <= 0 || layer.totelem != grid_totelem(grids_num, layer.level)) {
      /* Stale metadata or base topology change: the data cannot be mapped. */
      MEM_delete_void(layer.data);
      layer.data = nullptr;
      layer.totelem = 0;
      layer.level = short(new_level);
      if (!warned) {
        CLOG_WARN(&LOG,
                  "Grid sculpt layer data reset: stored level no longer matches the mesh topology.");
        warned = true;
      }
      continue;
    }
    const Span<float3> src(static_cast<const float3 *>(layer.data), layer.totelem);
    const Array<float3> resampled = (layer.level < new_level) ?
                                        grid_upsample(src, layer.level, new_level, grids_num) :
                                        grid_subsample(src, layer.level, new_level, grids_num);
    MEM_delete_void(layer.data);
    layer.data = nullptr;
    layer.totelem = 0;
    data_ensure(layer, int(resampled.size()));
    data_get(layer).copy_from(resampled);
    layer.level = short(new_level);
  }
}

void derive_base_mesh(const Span<float3> positions,
                      const ListBaseT<SculptLayer> &layers,
                      MutableSpan<float3> r_base)
{
  BLI_assert(r_base.size() == positions.size());
  r_base.copy_from(positions);
  for (const SculptLayer &layer : layers) {
    if (layer.domain != SCULPT_LAYER_DOMAIN_VERT || layer.data == nullptr) {
      continue;
    }
    const float eff = effective(layer);
    if (eff == 0.0f) {
      continue;
    }
    const Span<float3> data(static_cast<const float3 *>(layer.data), layer.totelem);
    if (data.size() != r_base.size()) {
      /* Stale layer whose element count no longer matches the mesh topology. Skip it rather than
       * deriving the base over a mismatched vertex range. */
      continue;
    }
    threading::parallel_for(r_base.index_range(), 8192, [&](const IndexRange range) {
      for (const int64_t i : range) {
        r_base[i] -= data[i] * eff;
      }
    });
  }
}

void copy_list(ListBaseT<SculptLayer> *dst, const ListBaseT<SculptLayer> *src)
{
  BLI_listbase_clear(dst);
  BLI_duplicatelist(dst, src);
  for (SculptLayer &layer : *dst) {
    if (layer.data) {
      layer.data = MEM_dupalloc_void(layer.data);
    }
  }
}

void free_list(ListBaseT<SculptLayer> *layers)
{
  while (SculptLayer *layer = static_cast<SculptLayer *>(BLI_pophead(layers))) {
    if (layer->data) {
      MEM_delete_void(layer->data);
    }
    MEM_delete(layer);
  }
}

void blend_write(BlendWriter *writer, ListBaseT<SculptLayer> *layers)
{
  writer->write_struct_list_by_name("SculptLayer", layers);
  for (const SculptLayer &layer : *layers) {
    if (layer.data) {
      writer->write_float3_array(layer.totelem, static_cast<const float *>(layer.data));
    }
  }
}

void blend_read(BlendDataReader *reader, ListBaseT<SculptLayer> *layers)
{
  BLO_read_struct_list(reader, SculptLayer, layers);
  for (SculptLayer &layer : *layers) {
    float3 *data = static_cast<float3 *>(layer.data);
    BLO_read_array_and_validate_size(reader, &data, &layer.totelem);
    layer.data = data;
  }
}

}  // namespace blender::bke::sculpt_layers
