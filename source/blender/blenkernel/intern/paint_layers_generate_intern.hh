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
#include "paint_layers_runtime.hh"

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
inline constexpr const char *TREE_OWNER_PROP = "pbr_paint_layers_owner";
/** On the instance node in the material's embedded tree: the same owner uid. */
inline constexpr const char *INSTANCE_OWNER_PROP = "pbr_paint_layers_instance";
/** On a layer's own node group: the marker of the layer it holds. The tree also carries
 * #TREE_OWNER_PROP of the material, so a copy never adopts the source's groups. */
inline constexpr const char *TREE_LAYER_PROP = "pbr_paint_layers_layer_tree";
/** On a layer's own node group: its topology hash, low and high 32-bit words. The factory compares
 * it with #paint_layers_layer_topology_hash and skips the rebuild when they agree. */
inline constexpr const char *TREE_TOPOLOGY_LOW_PROP = "pbr_paint_layers_topology";
inline constexpr const char *TREE_TOPOLOGY_HIGH_PROP = "pbr_paint_layers_topology_hi";
/** On the root generated tree: the hash of everything the root's own nodes, links and interface
 * depend on. When it is unchanged the root is left alone and only changed layer groups rebuild. */
inline constexpr const char *TREE_ROOT_TOPOLOGY_LOW_PROP = "pbr_paint_layers_root_topology";
inline constexpr const char *TREE_ROOT_TOPOLOGY_HIGH_PROP = "pbr_paint_layers_root_topology_hi";
/** On a source-group wrapper: the session_uid of the source material it wraps. */
inline constexpr const char *TREE_SOURCE_PROP = "pbr_paint_layers_source";
/** On a source-group wrapper: the source tree state hash it was built from, low/high words. */
inline constexpr const char *TREE_SOURCE_HASH_LOW_PROP = "pbr_paint_layers_source_hash";
inline constexpr const char *TREE_SOURCE_HASH_HIGH_PROP = "pbr_paint_layers_source_hash_hi";
/** On a source-group wrapper: the value-sensitive source hash last synced into it, low/high. A
 * move means the values in the existing copy are stale and must be copied in place. */
inline constexpr const char *TREE_SOURCE_VALUES_LOW_PROP = "pbr_paint_layers_source_values";
inline constexpr const char *TREE_SOURCE_VALUES_HIGH_PROP = "pbr_paint_layers_source_values_hi";
/** On the root generated tree: the hash of the set of source materials the `MATERIAL` rows read.
 * A change means an ID reference of the graph appeared or disappeared, so its relations rebuild. */
inline constexpr const char *TREE_SOURCE_MATERIALS_LOW_PROP = "pbr_paint_layers_source_materials";
inline constexpr const char *TREE_SOURCE_MATERIALS_HIGH_PROP = "pbr_paint_layers_source_materials_hi";
/** On a generated group interface input: how its value is read from the description. */
inline constexpr const char *INPUT_ROLE_PROP = "pbr_paint_layers_role";
/** On a generated group interface input: the marker of the layer it stands for. */
inline constexpr const char *INPUT_MARKER_PROP = "pbr_paint_layers_layer";
/** On a generated group interface input: the channel a Fill constant stands for. */
inline constexpr const char *INPUT_CHANNEL_PROP = "pbr_paint_layers_channel";
/** On a pass-through value input a parent scope (a folder group or the root) carries to feed a
 * nested group's value socket. Absent on the value input of the layer's *own* group: that one is
 * the value's source of truth, the mirrors only relay it. The bit is what the root hash uses to
 * tell the two apart, so adding or removing a relay never changes the topology signature. */
inline constexpr const char *INPUT_MIRROR_PROP = "pbr_paint_layers_mirror";

/** #INPUT_ROLE_PROP value of a layer's opacity (enabled already folded in). */
inline constexpr const char *ROLE_OPACITY = "opacity";
/** #INPUT_ROLE_PROP value of a Fill layer's constant. */
inline constexpr const char *ROLE_FILL = "fill";
/** #INPUT_ROLE_PROP value of a correction's opacity (enabled already folded in). */
inline constexpr const char *ROLE_CORRECTION_OPACITY = "correction_opacity";
/** #INPUT_ROLE_PROP value of a Fill correction's constant colour. */
inline constexpr const char *ROLE_CORRECTION_FILL = "correction_fill";
/** #INPUT_ROLE_PROP value of a Hybrid Material row's live constant (ТЗ-26). */
inline constexpr const char *ROLE_LIVE_CONSTANT = "live_constant";

/** One owner's forced-bake markers, keeping the owning material so a same-uid free is safe. */
struct ForcedBakeState {
  const Material *owner = nullptr;
  Vector<bUUID> markers;
};

struct SamplerRuntimeState {
  /** Material-texture sampler allowance; zero disables the check. */
  int budget = 0;
  /** `GPU_max_textures()` as reported to #BKE_paint_layers_sampler_budget_set, for the log only. */
  int max_textures = 0;
  /** Owners whose disabled rows the last over-budget pass dropped, by material `session_uid`. */
  Map<uint32_t, const Material *> cleanup_owners;
  /** The markers of rows forced onto their baked maps, per owner `session_uid`. */
  Map<uint32_t, ForcedBakeState> forced_bake;
};

bNode *bump_ensure(Material &ma)
;
bNode *instance_find(const Material &ma, const bUUID &owner_uid)
;
bNode *material_output_find(Material &ma)
;
bNode *mix_node_add(bNodeTree &tree, const int ramp_blend, const float location_x, const float location_y)
;
bNode *normal_map_ensure(Material &ma)
;
bNodeSocket *instance_output_find(bNodeTree &tree, bNode &instance, const char *result_name)
;
bNodeSocket *principled_channel_socket(Material &ma, const int channel)
;
bNodeSocket *socket_in(bNode &node, const char *name)
;
bNodeSocket *socket_out(bNode &node, const StringRefNull name)
;
bNodeSocket *source_group_output(bNodeTree &wrapper,
                                 bNode &instance,
                                 const int channel,
                                 const bool coverage)
;
bool budget_cleanup_active(const Material &ma)
;
void budget_cleanup_owner_set(const Material &ma, bool active)
;
bool custom_bake_missing_warn_once(const Material &ma, const MaterialPaintLayer &layer)
;
bool forced_bake_contains(const Material &ma, const bUUID &marker)
;
bool layer_row_has_group(const Material &ma,
                         const MaterialPaintLayer &layer,
                         const int channel,
                         const PaintLayersRegenCache *cache)
;
bool layer_subtree_has_channel(const Material &ma,
                               const MaterialPaintLayer &layer,
                               const int channel,
                               const PaintLayersRegenCache *cache)
;
bool layer_tree_marker_get(const bNodeTree &tree, bUUID &r_marker)
;
bool leaf_participates(const Material &ma, const MaterialPaintLayer &layer, const int channel)
;
bool material_source_group_channel(const Material &ma,
                                   const MaterialPaintLayer &layer,
                                   const int channel,
                                   const PaintLayersRegenCache *cache)
;
bool pass_through_scale_find(const Material &ma,
                                    const MaterialPaintLayer &target,
                                    const ListBaseT<MaterialPaintLayer> &list,
                                    const float scale,
                                    float &r_scale)
;
bool removed_rows_contains(const Material &ma, const bUUID &marker)
;
bool row_channel_substituted(const Material &ma,
                             const MaterialPaintLayer &layer,
                             const int channel,
                             Image **r_baked_color)
;
bool row_is_removed(const Material &ma, const MaterialPaintLayer &layer)
;
bool row_is_substituted(const Material &ma, const MaterialPaintLayer &layer)
;
bool tree_hash_get(bNodeTree &tree,
                   const char *low_key,
                   const char *high_key,
                   uint64_t &r_hash)
;
bool tree_root_hash_get(bNodeTree &tree, uint64_t &r_hash)
;
bool tree_topology_hash_get(bNodeTree &tree, uint64_t &r_hash)
;
bool uid_prop_get(const IDProperty *properties, const char *key, bUUID &r_uid)
;
bUUID tree_owner_uid_get(const bNodeTree &tree)
;
const char *prop_string_get(const IDProperty *properties, const char *key)
;
float pass_through_scale_of(const Material &ma,
                                   const MaterialPaintLayer &target,
                                   const PaintLayersRegenCache *cache)
;
SamplerRuntimeState &sampler_runtime()
;
IDProperty *properties_ensure(IDProperty *&properties)
;
int prop_int_get(const IDProperty *properties, const char *key, const int fallback)
;
int sampler_count_tree(const bNodeTree &tree)
;
uint32_t default_image_sampler_state()
;
uint32_t environment_sampler_state_key(const NodeTexImage &tex)
;
uint32_t image_sampler_state_key(const int extension,
                                        const int interpolation,
                                        const int projection)
;
uint32_t image_sampler_state_key(const NodeTexImage &tex)
;
uint64_t paint_layers_source_materials_hash(const Material &ma)
;
uint64_t topology_hash_correction(uint64_t hash,
                                  const Material &ma,
                                  const MaterialPaintLayer &correction,
                                  const Span<int> wired_channels,
                                  const bool mask_item,
                                  const PaintLayersRegenCache *cache)
;
uint64_t topology_hash_layer(uint64_t hash,
                             const Material &ma,
                             const MaterialPaintLayer &layer,
                             const Span<int> wired_channels,
                             const PaintLayersRegenCache *cache)
;
uint64_t topology_hash_map_id(const Image *image)
;
uint64_t topology_hash_mix(uint64_t hash, const uint64_t value)
;
Vector<bUUID> forced_bake_markers(const Material &ma)
;
Vector<int> paint_layers_wired_channels(const Material &ma, const PaintLayersRegenCache *cache)
;
void forced_bake_add(const Material &ma, const bUUID &marker)
;
void forced_bake_clear(const Material &ma)
;
void forced_bake_remove(const Material &ma, const bUUID &marker)
;
void generated_uv_maps_wire(bNodeTree &tree, const char *uv_name)
;
void input_link_restore(Material &ma,
                        bNodeSocket &input,
                        bNode &from_node,
                        bNodeSocket &from_socket,
                        PaintLayersRegenerateReport &r_report)
;
void instance_uid_set(bNode &node, const bUUID &uid)
;
void interface_name_unique(const bNodeTreeInterface &interface,
                           const char *base,
                           char *r_buffer,
                           const size_t buffer_size)
;
void layer_trees_collect(bNodeTree &tree,
                         const bUUID &owner_uid,
                         Vector<bNodeTree *> &r_trees)
;
void layer_trees_copy(Main *bmain,
                      bNodeTree &tree,
                      const bUUID &src_owner,
                      const bUUID &dst_owner)
;
void pass_through_scales_fill(const Material &ma,
                                     const ListBaseT<MaterialPaintLayer> &list,
                                     const float scale,
                                     Map<const MaterialPaintLayer *, float> &r_scales)
;
void principled_ensure(Material &ma, PaintLayersRegenerateReport &r_report)
;
void prop_int_set(IDProperty *&properties, const char *key, const int value)
;
void prop_string_set(IDProperty *&properties, const char *key, const char *value)
;
void refresh_generated_instances(bNodeTree &tree, const bUUID &owner_uid, Set<bNodeTree *> &visited)
;
void removed_rows_reconcile(Material &ma)
;
void source_groups_prune(Main &bmain, const Material &owner)
;
void topology_hash_string(uint64_t &hash, const char *text)
;
void topology_hash_uid(uint64_t &hash, const bUUID &uid)
;
void tree_clear_nodes(Main &bmain, bNodeTree &tree)
;
void tree_clear(Main &bmain, bNodeTree &tree)
;
void tree_hash_set(bNodeTree &tree,
                   const char *low_key,
                   const char *high_key,
                   const uint64_t hash)
;
void tree_owner_uid_set(bNodeTree &tree, const bUUID &uid)
;
void tree_root_hash_set(bNodeTree &tree, const uint64_t hash)
;
void tree_topology_hash_set(bNodeTree &tree, const uint64_t hash)
;
void uid_prop_set(IDProperty *&properties, const char *key, const bUUID &uid)
;
void values_sync_with_cache(Material &ma, const PaintLayersRegenCache *cache)
;
void wire_instance_to_material(Material &ma,
                               bNodeTree &tree,
                               bNode &instance,
                               PaintLayersRegenerateReport &r_report)
;

class SamplerCounter {
  Map<const Image *, Set<uint32_t>> image_states_;
  /** Tiled uses keyed the same way, so the extra mapping sampler is added per unique entry. */
  Map<const Image *, Set<uint32_t>> tiled_states_;
  Set<const bNode *> visited_nodes_;
  Set<const bNodeTree *> visited_groups_;
  const bNodeTree *skip_group_ = nullptr;
  bool has_colorband_ = false;
  bool has_sky_ = false;

  static bool node_is_output(const bNode &node)
  {
    return node.type_legacy == SH_NODE_OUTPUT_MATERIAL || node.type_legacy == SH_NODE_OUTPUT_WORLD ||
           node.type_legacy == SH_NODE_OUTPUT_LIGHT || node.is_group_output();
  }

  void visit_node(const bNode &node)
  {
    if ((node.flag & NODE_MUTED) != 0) {
      return;
    }
    if (!visited_nodes_.add(&node)) {
      return;
    }
    if (node.is_group() && node.id != nullptr && GS(node.id->name) == ID_NT) {
      const bNodeTree &group = *reinterpret_cast<const bNodeTree *>(node.id);
      if (&group != skip_group_ && visited_groups_.add(&group)) {
        visit_tree(group);
      }
    }
    else {
      collect_node(node);
    }
    for (const bNodeSocket &input : node.inputs) {
      for (const bNodeLink *link : input.directly_linked_links()) {
        if (link->fromnode != nullptr) {
          visit_node(*link->fromnode);
        }
      }
    }
  }

  void collect_node(const bNode &node)
  {
    switch (node.type_legacy) {
      case SH_NODE_TEX_IMAGE: {
        collect_image(node, false);
        break;
      }
      case SH_NODE_TEX_ENVIRONMENT: {
        collect_image(node, true);
        break;
      }
      case SH_NODE_TEX_SKY: {
        const NodeTexSky *storage = static_cast<const NodeTexSky *>(node.storage);
        if (storage != nullptr && ELEM(storage->sky_model,
                                       SHD_SKY_SINGLE_SCATTERING,
                                       SHD_SKY_MULTIPLE_SCATTERING))
        {
          has_sky_ = true;
        }
        break;
      }
      case SH_NODE_VALTORGB:
      case SH_NODE_CURVE_RGB:
      case SH_NODE_CURVE_VEC:
      case SH_NODE_CURVE_FLOAT:
      case SH_NODE_BLACKBODY:
      case SH_NODE_WAVELENGTH:
      case SH_NODE_VOLUME_PRINCIPLED: {
        /* All of these sample the single per-material colorband texture
         * (`gpu_node_graph.cc:730-741`, `gpu_material.cc`). */
        has_colorband_ = true;
        break;
      }
      default:
        break;
    }
  }

  void collect_image(const bNode &node, const bool environment)
  {
    if (node.id == nullptr || GS(node.id->name) != ID_IM) {
      return;
    }
    const Image *image = reinterpret_cast<const Image *>(node.id);
    const NodeTexImage *storage = static_cast<const NodeTexImage *>(node.storage);
    if (storage == nullptr) {
      return;
    }
    if (environment) {
      image_states_.lookup_or_add_default(image).add(
          environment_sampler_state_key(*storage));
      return;
    }
    uint32_t state = image_sampler_state_key(*storage);
    image_states_.lookup_or_add_default(image).add(state);
    /* `node_shader_tex_image.cc:100`: UDIM only for a tiled image with a flat projection, and the
     * mapping array adds a second sampler (`gpu_codegen.cc:238-239`). */
    if (image->source == IMA_SRC_TILED && storage->projection == SHD_PROJ_FLAT) {
      tiled_states_.lookup_or_add_default(image).add(state);
    }
  }

  public:
  void visit_tree(const bNodeTree &tree)
  {
    /* `directly_linked_links` reads the topology cache, so build it before walking backwards. */
    tree.ensure_topology_cache();
    for (const bNode &node : tree.nodes) {
      if (node_is_output(node)) {
        visit_node(node);
      }
    }
  }

  /** Visit \a tree but do not descend into \a skip_group, whose rows are counted separately. */
  void visit_tree_skipping(const bNodeTree &tree, const bNodeTree *skip_group)
  {
    skip_group_ = skip_group;
    visit_tree(tree);
    skip_group_ = nullptr;
  }

  /** Add one image sampler for \a image, in \a state (the generator's default by default). */
  void add_image(const Image &image, const uint32_t state = default_image_sampler_state())
  {
    image_states_.lookup_or_add_default(&image).add(state);
  }

  /** Add every baked map of \a layer: each is an Image Texture with the default sampler state. */
  void add_baked_maps(const MaterialPaintLayer &layer)
  {
    if (layer.bake == nullptr) {
      return;
    }
    for (const int i : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
      if (layer.bake->images[i] != nullptr) {
        add_image(*layer.bake->images[i]);
      }
    }
    if (layer.bake->coverage != nullptr) {
      add_image(*layer.bake->coverage);
    }
  }

  void add_tree(const bNodeTree &tree)
  {
    visit_tree(tree);
  }

  int total() const
  {
    int count = 0;
    for (const auto &item : image_states_.items()) {
      count += item.value.size();
      const Set<uint32_t> *tiled = tiled_states_.lookup_ptr(item.key);
      if (tiled != nullptr) {
        for (const uint32_t state : *tiled) {
          if (item.value.contains(state)) {
            count += 1;
          }
        }
      }
    }
    if (has_colorband_) {
      count += 1;
    }
    if (has_sky_) {
      count += 1;
    }
    return count;
  }
};
}  // namespace bke::paint_layers
}  // namespace blender
