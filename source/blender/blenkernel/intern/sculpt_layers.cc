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
#include <type_traits>

#include "CLG_log.h"

#include "MEM_guardedalloc.h"

#include "DNA_key_types.h"
#include "DNA_mesh_types.h"

#include "BLI_array.hh"
#include "BLI_listbase.h"
#include "BLI_map.hh"
#include "BLI_set.hh"
#include "BLI_string_utf8.h"
#include "BLI_string_utils.hh"
#include "BLI_task.hh"

#include "BLT_translation.hh"

#include "BKE_key.hh"
#include "BKE_multires_grid_resample.hh"

#include "BLO_read_write.hh"

static CLG_LogRef LOG = {"bke.sculpt_layers"};

namespace blender::bke::sculpt_layers {

/* No destructor ever runs on a layer: #free_list and #remove release the nodes with
 * #MEM_delete_void, because the blend-file reader allocates its own layers C-style and the free
 * side has to accept both origins. See the allocation notes in `BKE_sculpt_layers.hh`. */
static_assert(std::is_trivially_destructible_v<SculptLayer>,
              "SculptLayer must remain trivially destructible (see the allocation notes in "
              "BKE_sculpt_layers.hh)");

/* Groups are freed with #MEM_delete_void as well (see #group_free_list), for the same reason: the
 * blend-file reader allocates them C-style. Asserted separately so that adding a non-trivial member
 * to the group breaks the build here rather than corrupting the free path silently. */
static_assert(std::is_trivially_destructible_v<SculptLayerGroup>,
              "SculptLayerGroup must remain trivially destructible (see the allocation notes in "
              "BKE_sculpt_layers.hh)");

static int unique_uid(const Mesh &mesh)
{
  int uid = 1;
  for (const SculptLayer &layer : mesh.sculpt_layers) {
    uid = std::max(uid, layer.uid + 1);
  }
  return uid;
}

/**
 * Standalone copy of \a src, with the list links cleared and its own data buffer.
 *
 * Every layer duplication routes through here. A shallow struct copy alone would leave the two
 * layers sharing one #SculptLayer::data allocation, which #free_list would then release twice, so
 * the deep copy of the buffer must not be separable from the copy of the struct.
 *
 * The copy keeps \a src's uid; a caller inserting it into a mesh assigns a fresh one.
 */
static SculptLayer *layer_copy(const SculptLayer &src)
{
  SculptLayer *layer = MEM_new<SculptLayer>(__func__, src);
  layer->next = layer->prev = nullptr;
  if (src.data) {
    layer->data = MEM_dupalloc_void(src.data);
  }
  return layer;
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
  /* Hand the active marker to a neighbour, but only when the removed layer held it: unlike the
   * positional index this replaced, a uid keeps identifying the same layer no matter what happens
   * elsewhere in the list, so removing some *other* layer must leave the active one alone. */
  const bool was_active = mesh.sculpt_layers_active_uid == layer.uid;
  const SculptLayer *successor = layer.next ? layer.next : layer.prev;

  BLI_remlink(&mesh.sculpt_layers, &layer);
  if (layer.data) {
    MEM_delete_void(layer.data);
  }
  MEM_delete_void(static_cast<void *>(&layer));

  if (was_active) {
    active_set(mesh, successor);
  }
}

SculptLayer *duplicate(Mesh &mesh, const SculptLayer &src)
{
  SculptLayer *layer = layer_copy(src);
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
  return find_by_uid(mesh, mesh.sculpt_layers_active_uid);
}

const SculptLayer *active_get(const Mesh &mesh)
{
  return find_by_uid(mesh, mesh.sculpt_layers_active_uid);
}

void active_set(Mesh &mesh, const SculptLayer *layer)
{
  mesh.sculpt_layers_active_uid = layer ? layer->uid : 0;
}

SculptLayer *find_by_uid(Mesh &mesh, const int uid)
{
  if (uid == 0) {
    return nullptr;
  }
  for (SculptLayer &layer : mesh.sculpt_layers) {
    if (layer.uid == uid) {
      return &layer;
    }
  }
  return nullptr;
}

const SculptLayer *find_by_uid(const Mesh &mesh, const int uid)
{
  if (uid == 0) {
    return nullptr;
  }
  for (const SculptLayer &layer : mesh.sculpt_layers) {
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

static int group_unique_uid(const Mesh &mesh)
{
  int uid = 1;
  for (const SculptLayerGroup &group : mesh.sculpt_layer_groups) {
    uid = std::max(uid, group.uid + 1);
  }
  return uid;
}

SculptLayerGroup *group_find_by_uid(Mesh &mesh, const int uid)
{
  return const_cast<SculptLayerGroup *>(group_find_by_uid(const_cast<const Mesh &>(mesh), uid));
}

const SculptLayerGroup *group_find_by_uid(const Mesh &mesh, const int uid)
{
  if (uid == 0) {
    return nullptr;
  }
  for (const SculptLayerGroup &group : mesh.sculpt_layer_groups) {
    if (group.uid == uid) {
      return &group;
    }
  }
  return nullptr;
}

void group_name_ensure_unique(const Mesh &mesh, SculptLayerGroup &group)
{
  BLI_uniquename_cb(
      [&](const StringRefNull check_name) {
        for (const SculptLayerGroup &other : mesh.sculpt_layer_groups) {
          if (&other != &group && other.parent_uid == group.parent_uid &&
              check_name == other.name)
          {
            return true;
          }
        }
        return false;
      },
      "Group",
      '.',
      group.name,
      sizeof(group.name));
}

SculptLayerGroup *group_add(Mesh &mesh, const char *name, const int parent_uid)
{
  SculptLayerGroup *group = MEM_new<SculptLayerGroup>(__func__);
  STRNCPY_UTF8(group->name, (name && name[0]) ? name : "Group");
  group->flag = SCULPT_LAYER_GROUP_ENABLED | SCULPT_LAYER_GROUP_EXPANDED;
  group->uid = group_unique_uid(mesh);
  group->parent_uid = parent_uid;

  BLI_addtail(&mesh.sculpt_layer_groups, group);
  group_name_ensure_unique(mesh, *group);
  return group;
}

void group_remove(Mesh &mesh, SculptLayerGroup &group)
{
  BLI_remlink(&mesh.sculpt_layer_groups, &group);
  MEM_delete_void(static_cast<void *>(&group));
}

bool group_is_descendant_of(const Mesh &mesh, const SculptLayerGroup &group, const int ancestor_uid)
{
  if (ancestor_uid == 0) {
    /* Everything is below the root. */
    return true;
  }
  const SculptLayerGroup *current = &group;
  /* Bounded by the group count rather than by reaching the root: a cycle in stored data (a corrupt
   * file, a future editing bug) must not hang the UI, which calls this from a drop check. */
  for (int guard = BLI_listbase_count(&mesh.sculpt_layer_groups); current && guard >= 0; guard--) {
    if (current->uid == ancestor_uid) {
      return true;
    }
    current = group_find_by_uid(mesh, current->parent_uid);
  }
  return false;
}

bool is_stale(const SculptLayer &layer, const int elem_num)
{
  return layer.data != nullptr && layer.totelem != elem_num;
}

int element_count(const Mesh &mesh, const SculptLayer &layer)
{
  if (layer.domain == SCULPT_LAYER_DOMAIN_GRID) {
    return grid_totelem(mesh.corners_num, layer.level);
  }
  return mesh.verts_num;
}

bool is_stale(const Mesh &mesh, const SculptLayer &layer)
{
  return is_stale(layer, element_count(mesh, layer));
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
  if (is_stale(layer, positions.size())) {
    /* Skip rather than corrupt a mismatched vertex range. */
    return;
  }
  const Span<float3> deltas(static_cast<const float3 *>(layer.data), layer.totelem);
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
  /* A layer inside a disabled folder contributes nothing, but keeps its own #SCULPT_LAYER_ENABLED
   * bit — re-enabling the folder must restore exactly what was visible before. One flag test rather
   * than a walk up the group chain, because this runs per vertex / per grid point. */
  if (layer.flag & SCULPT_LAYER_GROUP_HIDDEN) {
    return 0.0f;
  }
  return (layer.flag & SCULPT_LAYER_ENABLED) ? layer.influence : 0.0f;
}

/**
 * Effective enabled state of the group \a uid — its own state with every ancestor's folded in —
 * memoized in \a r_cache.
 *
 * Recursive rather than a single pass in list order, because #SculptLayerGroup::parent_uid may name
 * a group that physically sits later in #Mesh::sculpt_layer_groups: a list-order pass would read a
 * parent that has not been resolved yet. \a visiting holds the chain currently being resolved, so a
 * cycle in stored data breaks instead of overflowing the stack.
 */
static bool group_effective_enabled(const Mesh &mesh,
                                    const int uid,
                                    Map<int, bool> &r_cache,
                                    Set<int> &visiting)
{
  if (uid == 0) {
    /* The root is always enabled. */
    return true;
  }
  if (const bool *cached = r_cache.lookup_ptr(uid)) {
    return *cached;
  }
  if (!visiting.add(uid)) {
    /* A cycle, only reachable from corrupt data. Resolving to true keeps the corruption from hiding
     * layers, and true is also the identity of the `&&` below — so every group in the cycle ends up
     * with the conjunction of the cycle's own enabled bits, whichever member the walk entered at,
     * rather than an answer that depends on where it started. */
    return true;
  }
  const SculptLayerGroup *group = group_find_by_uid(mesh, uid);
  /* A dangling uid (the group was removed without reparenting) is treated as the root. */
  const bool enabled = !group ||
                       ((group->flag & SCULPT_LAYER_GROUP_ENABLED) &&
                        group_effective_enabled(mesh, group->parent_uid, r_cache, visiting));
  visiting.remove(uid);
  r_cache.add(uid, enabled);
  return enabled;
}

void resync_group_hidden(Mesh &mesh)
{
  /* Effective state per group uid, resolved on demand and memoized so that each group costs one
   * resolve no matter how deep it sits. */
  Map<int, bool> effective_enabled;
  Set<int> visiting;

  for (const SculptLayerGroup &group : mesh.sculpt_layer_groups) {
    group_effective_enabled(mesh, group.uid, effective_enabled, visiting);
  }

  for (SculptLayer &layer : mesh.sculpt_layers) {
    const bool hidden = !group_effective_enabled(mesh, layer.group_uid, effective_enabled, visiting);
    SET_FLAG_FROM_TEST(layer.flag, hidden, SCULPT_LAYER_GROUP_HIDDEN);
  }
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
    if (is_stale(layer, r_positions.size())) {
      /* Skip rather than compose deltas over a mismatched vertex range. */
      continue;
    }
    const Span<float3> data(static_cast<const float3 *>(layer.data), layer.totelem);
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
        !is_stale(layer, mesh.verts_num) && effective(layer) != 0.0f)
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
        !is_stale(layer, mesh.verts_num))
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
  if (is_stale(layer, mesh.verts_num)) {
    return;
  }
  const Span<float3> deltas(static_cast<const float3 *>(layer.data), layer.totelem);
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
    if (is_stale(layer, r_base.size())) {
      /* Skip rather than derive the base over a mismatched vertex range. */
      continue;
    }
    const Span<float3> data(static_cast<const float3 *>(layer.data), layer.totelem);
    threading::parallel_for(r_base.index_range(), 8192, [&](const IndexRange range) {
      for (const int64_t i : range) {
        r_base[i] -= data[i] * eff;
      }
    });
  }
}

void copy_list(ListBaseT<SculptLayer> *dst, const ListBaseT<SculptLayer> *src)
{
  /* Copied one node at a time through #layer_copy rather than with #BLI_duplicatelist: the latter
   * would shallow-copy the nodes and leave every layer's data buffer shared with the source.
   * Mirrors #BKE_defgroup_copy_list for the neighboring #Mesh::vertex_group_names. */
  dst->clear_no_delete();
  for (const SculptLayer &layer : *src) {
    BLI_addtail(dst, layer_copy(layer));
  }
}

void free_list(ListBaseT<SculptLayer> *layers)
{
  while (SculptLayer *layer = static_cast<SculptLayer *>(BLI_pophead(layers))) {
    if (layer->data) {
      MEM_delete_void(layer->data);
    }
    MEM_delete_void(static_cast<void *>(layer));
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

void group_copy_list(ListBaseT<SculptLayerGroup> *dst, const ListBaseT<SculptLayerGroup> *src)
{
  /* A group owns no heap buffer, so unlike #copy_list a shallow per-node copy is complete; the uids
   * carry over, which is what keeps every layer's #SculptLayer::group_uid pointing at the copy of
   * the group it named in the source. */
  dst->clear_no_delete();
  for (const SculptLayerGroup &group : *src) {
    SculptLayerGroup *copy = MEM_new<SculptLayerGroup>(__func__, group);
    copy->next = copy->prev = nullptr;
    BLI_addtail(dst, copy);
  }
}

void group_free_list(ListBaseT<SculptLayerGroup> *groups)
{
  while (SculptLayerGroup *group = static_cast<SculptLayerGroup *>(BLI_pophead(groups))) {
    MEM_delete_void(static_cast<void *>(group));
  }
}

void group_blend_write(BlendWriter *writer, ListBaseT<SculptLayerGroup> *groups)
{
  writer->write_struct_list_by_name("SculptLayerGroup", groups);
}

void group_blend_read(BlendDataReader *reader, ListBaseT<SculptLayerGroup> *groups)
{
  BLO_read_struct_list(reader, SculptLayerGroup, groups);
}

}  // namespace blender::bke::sculpt_layers
