#include "paint_layers_generate_intern.hh"
/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The paint-layer generator: `Material::paint_layers` (the DNA description) becomes a node tree.
 *
 * `paint_layers_tree_build` is the topology half: interface, per-layer nodes and the links between
 * them, with no #Main involved. `BKE_paint_layers_regenerate` is the #Main-side half: it owns the
 * generated group, the instance node in the material's embedded tree, and the routing into the
 * Principled BSDF.
 *
 * Topology and values are separated: the channel chains and the interface are a function of the
 * description's *structure* alone, while the animatable values (`opacity`, `enabled`, a Fill
 * constant) are inputs of each layer group's own interface. Their current values sit on that
 * group's instance node in its parent tree and are copied there by #BKE_paint_layers_values_sync
 * (called from the material evaluation too).
 */

#include "BKE_paint_layers_debug.hh"
#include "BKE_paint_layers_generate.hh"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <utility>

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_map.hh"
#include "BLI_math_vector.h"
#include "BLI_set.hh"
#include "BLI_string.h"
#include "BLI_string_ref.hh"
#include "BLI_string_utf8.h"
#include "BLI_threads.h"
#include "BLI_time.h"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "BKE_global.hh"
#include "BKE_idprop.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "IMB_colormanagement.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_interface.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_material_composite.hh"
#include "BKE_paint_material_resolve.hh"

#include "paint_layers_intern.hh"

#include "NOD_socket.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_tree_interface_types.h"
#include "DNA_node_types.h"
#include "DNA_scene_types.h"
#include "DNA_uuid_types.h"

#include "paint_material_composite_internal.hh"


namespace blender {
namespace bke::paint_layers {


/* -------------------------------------------------------------------- */
/** \name Sampler budget and counting
 * \{ */

/**
 * The runtime sampler budget and the fallback state it drives. Not DNA: it is derived from the
 * budget on every regeneration and cleared when the budget grows, so it must never be saved.
 */

SamplerRuntimeState &sampler_runtime()
{
  static SamplerRuntimeState state;
  return state;
}

/**
 * Guards the per-material halves of #sampler_runtime() (`cleanup_owners`, `forced_bake`). They are
 * written by the K-1 regeneration on the main thread and read from `BKE_material_eval`, which runs
 * in the depsgraph task pool and on evaluated copies, so an unlocked Map was a data race. The
 * global `budget`/`max_textures` settings stay outside it: only the main thread touches them.
 */
std::mutex &sampler_runtime_mutex()
{
  static std::mutex mutex;
  return mutex;
}

/** Whether the last over-budget pass is dropping this owner's hidden rows. */
bool budget_cleanup_active(const Material &ma)
{
  std::lock_guard lock(sampler_runtime_mutex());
  return sampler_runtime().cleanup_owners.contains(ma.id.session_uid);
}

void budget_cleanup_owner_set(const Material &ma, const bool active)
{
  std::lock_guard lock(sampler_runtime_mutex());
  if (active) {
    sampler_runtime().cleanup_owners.add_overwrite(ma.id.session_uid, &ma);
  }
  else {
    sampler_runtime().cleanup_owners.remove(ma.id.session_uid);
  }
}

bool forced_bake_contains(const Material &ma, const bUUID &marker)
{
  std::lock_guard lock(sampler_runtime_mutex());
  const ForcedBakeState *state = sampler_runtime().forced_bake.lookup_ptr(ma.id.session_uid);
  if (state == nullptr) {
    return false;
  }
  for (const bUUID &other : state->markers) {
    if (BLI_uuid_equal(other, marker)) {
      return true;
    }
  }
  return false;
}

/** The sampler state EEVEE derives from an image node's extension, interpolation and projection. */
uint32_t image_sampler_state_key(const int extension,
                                        const int interpolation,
                                        const int projection)
{
  /* `node_shader_tex_image.cc:67-95` maps `extension` onto the extend mode and lumps every
   * interpolation other than Closest into one filtering; Sphere/Tube additionally drop mipmapping
   * (`:131,:141`), which is the only place `projection` reaches the sampler state. Encoding the
   * resulting filtering class, not the raw interpolation, keeps Smoother and Linear as one key. */
  const bool closest = interpolation == SHD_INTERP_CLOSEST;
  const bool no_mipmap = ELEM(projection, SHD_PROJ_SPHERE, SHD_PROJ_TUBE);
  const uint32_t filtering = closest ? 1u : (no_mipmap ? 2u : 3u);
  return (uint32_t(extension) & 0xFFu) | (filtering << 8);
}

/** The state EEVEE derives from \a tex's storage, the key the generator's Image Texture nodes use. */
uint32_t image_sampler_state_key(const NodeTexImage &tex)
{
  return image_sampler_state_key(tex.extension, tex.interpolation, tex.projection);
}

/**
 * The sampler state of the Image Texture nodes the generator creates for a row's own map, a
 * correction map or a baked map: all of them are added with the node type's default storage, which
 * is Repeat/Linear/Flat. Using this as #SamplerCounter::add_image's default is what lets a map that
 * appears both in the stack and in the user's tree dedup to one sampler.
 */
uint32_t default_image_sampler_state()
{
  return image_sampler_state_key(
      SHD_IMAGE_EXTENSION_REPEAT, SHD_INTERP_LINEAR, SHD_PROJ_FLAT);
}

/** The sampler state EEVEE derives from an environment node's projection and interpolation. */
uint32_t environment_sampler_state_key(const NodeTexImage &tex)
{
  const bool closest = tex.interpolation == SHD_INTERP_CLOSEST;
  return 0x10000u | (uint32_t(tex.projection) << 8) | (closest ? 1u : 0u);
}

/**
 * A reachability walk over a node tree that counts the samplers EEVEE would allocate, following
 * `gpu_node_graph.cc`. Only nodes reachable from an output are visited, muted nodes are treated as
 * absent, and every image/colorband/sky sampler is deduplicated the way `gpu_node_graph_add_texture`
 * does. Nested groups are entered once each.
 */

/** Count the samplers reachable from \a tree's own output nodes. */
int sampler_count_tree(const bNodeTree &tree)
{
  SamplerCounter counter;
  counter.visit_tree(tree);
  return counter.total();
}

/** Remove any forced-bake marker of \a ma; used before recomputing the fallback from scratch. */
void forced_bake_clear(const Material &ma)
{
  std::lock_guard lock(sampler_runtime_mutex());
  sampler_runtime().forced_bake.remove(ma.id.session_uid);
}

/** The markers \a ma's last pass pinned, before #forced_bake_clear drops them for this pass. */
Vector<bUUID> forced_bake_markers(const Material &ma)
{
  std::lock_guard lock(sampler_runtime_mutex());
  const ForcedBakeState *state = sampler_runtime().forced_bake.lookup_ptr(ma.id.session_uid);
  return (state != nullptr) ? state->markers : Vector<bUUID>();
}

void forced_bake_add(const Material &ma, const bUUID &marker)
{
  std::lock_guard lock(sampler_runtime_mutex());
  ForcedBakeState &state = sampler_runtime().forced_bake.lookup_or_add_default(ma.id.session_uid);
  state.owner = &ma;
  state.markers.append(marker);
}

void forced_bake_remove(const Material &ma, const bUUID &marker)
{
  std::lock_guard lock(sampler_runtime_mutex());
  ForcedBakeState *state = sampler_runtime().forced_bake.lookup_ptr(ma.id.session_uid);
  if (state == nullptr) {
    return;
  }
  for (int i = 0; i < state->markers.size(); i++) {
    if (BLI_uuid_equal(state->markers[i], marker)) {
      state->markers.remove(i);
      return;
    }
  }
}

/** \} */

}  // namespace bke::paint_layers
using namespace bke::paint_layers;
int BKE_paint_layers_sampler_count(const Material &ma)
{
  if (ma.nodetree == nullptr) {
    return 0;
  }
  return sampler_count_tree(*ma.nodetree);
}

bool BKE_paint_layers_material_forced_bake(const Material &ma, const MaterialPaintLayer &layer)
{
  return forced_bake_contains(ma, layer.marker);
}

void BKE_paint_layers_sampler_state_free(const Material &ma)
{
  std::lock_guard lock(sampler_runtime_mutex());
  /* Only the owner may drop its entry: a same-uid old ID freed during a memfile undo must leave
   * the re-read ID's forced-bake/cleanup state in place. */
  if (const ForcedBakeState *state =
          sampler_runtime().forced_bake.lookup_ptr(ma.id.session_uid);
      state != nullptr && state->owner == &ma)
  {
    sampler_runtime().forced_bake.remove(ma.id.session_uid);
  }
  if (const Material *const *owner =
          sampler_runtime().cleanup_owners.lookup_ptr(ma.id.session_uid);
      owner != nullptr && *owner == &ma)
  {
    sampler_runtime().cleanup_owners.remove(ma.id.session_uid);
  }
}

void BKE_paint_layers_sampler_state_owner_transfer(Material &dst, Material &src)
{
  std::lock_guard lock(sampler_runtime_mutex());
  ForcedBakeState *state = sampler_runtime().forced_bake.lookup_ptr(src.id.session_uid);
  if (state != nullptr && state->owner == &src) {
    state->owner = &dst;
  }
  const Material **owner = sampler_runtime().cleanup_owners.lookup_ptr(src.id.session_uid);
  if (owner != nullptr && *owner == &src) {
    *owner = &dst;
  }
}


void BKE_paint_layers_sampler_budget_set(const int budget, const int max_textures)
{
  sampler_runtime().budget = budget;
  sampler_runtime().max_textures = max_textures;
}

int BKE_paint_layers_sampler_budget_get()
{
  return sampler_runtime().budget;
}

int BKE_paint_layers_sampler_max_get()
{
  return sampler_runtime().max_textures;
}

}  // namespace blender

