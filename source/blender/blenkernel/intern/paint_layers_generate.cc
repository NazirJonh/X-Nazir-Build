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

#include "BKE_paint_layers_generate.hh"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
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

namespace {

/** Defined below; declared here for #BKE_paint_layers_root_hash_invalidate. */
void tree_root_hash_set(bNodeTree &tree, uint64_t hash);

/** Defined below with the sampler budget; declared here because row_is_removed reads it. */
static bool budget_cleanup_active(const Material &ma);

/* -------------------------------------------------------------------- */
/** \name Markers the generator stamps its own data with
 *
 * Everything the generator creates carries its identity in an `IDProperty`, never in a name: a
 * node can be renamed and a tree can be duplicated, but a marker travels with the data.
 * \{ */

/** On the generated tree: the owner uid of the material it belongs to. */
constexpr const char *TREE_OWNER_PROP = "pbr_paint_layers_owner";
/** On the instance node in the material's embedded tree: the same owner uid. */
constexpr const char *INSTANCE_OWNER_PROP = "pbr_paint_layers_instance";
/** On a layer's own node group: the marker of the layer it holds. The tree also carries
 * #TREE_OWNER_PROP of the material, so a copy never adopts the source's groups. */
constexpr const char *TREE_LAYER_PROP = "pbr_paint_layers_layer_tree";
/** On a layer's own node group: its topology hash, low and high 32-bit words. The factory compares
 * it with #paint_layers_layer_topology_hash and skips the rebuild when they agree. */
constexpr const char *TREE_TOPOLOGY_LOW_PROP = "pbr_paint_layers_topology";
constexpr const char *TREE_TOPOLOGY_HIGH_PROP = "pbr_paint_layers_topology_hi";
/** On the root generated tree: the hash of everything the root's own nodes, links and interface
 * depend on. When it is unchanged the root is left alone and only changed layer groups rebuild. */
constexpr const char *TREE_ROOT_TOPOLOGY_LOW_PROP = "pbr_paint_layers_root_topology";
constexpr const char *TREE_ROOT_TOPOLOGY_HIGH_PROP = "pbr_paint_layers_root_topology_hi";
/** On a source-group wrapper: the session_uid of the source material it wraps. */
constexpr const char *TREE_SOURCE_PROP = "pbr_paint_layers_source";
/** On a source-group wrapper: the source tree state hash it was built from, low/high words. */
constexpr const char *TREE_SOURCE_HASH_LOW_PROP = "pbr_paint_layers_source_hash";
constexpr const char *TREE_SOURCE_HASH_HIGH_PROP = "pbr_paint_layers_source_hash_hi";
/** On a source-group wrapper: the value-sensitive source hash last synced into it, low/high. A
 * move means the values in the existing copy are stale and must be copied in place. */
constexpr const char *TREE_SOURCE_VALUES_LOW_PROP = "pbr_paint_layers_source_values";
constexpr const char *TREE_SOURCE_VALUES_HIGH_PROP = "pbr_paint_layers_source_values_hi";
/** On the root generated tree: the hash of the set of source materials the `MATERIAL` rows read.
 * A change means an ID reference of the graph appeared or disappeared, so its relations rebuild. */
constexpr const char *TREE_SOURCE_MATERIALS_LOW_PROP = "pbr_paint_layers_source_materials";
constexpr const char *TREE_SOURCE_MATERIALS_HIGH_PROP = "pbr_paint_layers_source_materials_hi";
/** On a generated group interface input: how its value is read from the description. */
constexpr const char *INPUT_ROLE_PROP = "pbr_paint_layers_role";
/** On a generated group interface input: the marker of the layer it stands for. */
constexpr const char *INPUT_MARKER_PROP = "pbr_paint_layers_layer";
/** On a generated group interface input: the channel a Fill constant stands for. */
constexpr const char *INPUT_CHANNEL_PROP = "pbr_paint_layers_channel";

/** #INPUT_ROLE_PROP value of a layer's opacity (enabled already folded in). */
constexpr const char *ROLE_OPACITY = "opacity";
/** #INPUT_ROLE_PROP value of a Fill layer's constant. */
constexpr const char *ROLE_FILL = "fill";
/** #INPUT_ROLE_PROP value of a correction's opacity (enabled already folded in). */
constexpr const char *ROLE_CORRECTION_OPACITY = "correction_opacity";
/** #INPUT_ROLE_PROP value of a Fill correction's constant colour. */
constexpr const char *ROLE_CORRECTION_FILL = "correction_fill";

/** \} */

/* -------------------------------------------------------------------- */
/** \name IDProperty helpers
 * \{ */

IDProperty *properties_ensure(IDProperty *&properties)
{
  if (properties == nullptr) {
    IDPropertyTemplate val = {0};
    properties = IDP_New(IDP_GROUP, &val, "RNA");
  }
  return properties;
}

void prop_string_set(IDProperty *&properties, const char *key, const char *value)
{
  IDProperty *group = properties_ensure(properties);
  IDProperty *prop = IDP_GetPropertyTypeFromGroup(group, key, IDP_STRING);
  if (prop != nullptr) {
    IDP_AssignString(prop, value);
    return;
  }
  IDP_AddToGroup(group, IDP_NewString(value, key));
}

void prop_int_set(IDProperty *&properties, const char *key, const int value)
{
  IDProperty *group = properties_ensure(properties);
  IDProperty *prop = IDP_GetPropertyTypeFromGroup(group, key, IDP_INT);
  if (prop != nullptr) {
    IDP_int_set(prop, value);
    return;
  }
  IDP_AddToGroup(group, IDP_NewInt(value, key));
}

const char *prop_string_get(const IDProperty *properties, const char *key)
{
  if (properties == nullptr) {
    return nullptr;
  }
  const IDProperty *prop = IDP_GetPropertyTypeFromGroup(properties, key, IDP_STRING);
  return (prop != nullptr) ? IDP_string_get(prop) : nullptr;
}

int prop_int_get(const IDProperty *properties, const char *key, const int fallback)
{
  if (properties == nullptr) {
    return fallback;
  }
  const IDProperty *prop = IDP_GetPropertyTypeFromGroup(properties, key, IDP_INT);
  return (prop != nullptr) ? IDP_int_get(prop) : fallback;
}

void uid_prop_set(IDProperty *&properties, const char *key, const bUUID &uid)
{
  char formatted[UUID_STRING_SIZE];
  BLI_uuid_format(formatted, uid);
  prop_string_set(properties, key, formatted);
}

bool uid_prop_get(const IDProperty *properties, const char *key, bUUID &r_uid)
{
  const char *formatted = prop_string_get(properties, key);
  if (formatted == nullptr) {
    return false;
  }
  return BLI_uuid_parse_string(&r_uid, formatted);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Small helpers
 * \{ */

bNodeSocket *socket_out(bNode &node, const StringRefNull name)
{
  return bke::node_find_socket(node, SOCK_OUT, UString::from_ptr_noinline(name.c_str()));
}

bNodeSocket *socket_in(bNode &node, const char *name)
{
  return bke::node_find_socket(node, SOCK_IN, UString::from_ptr_noinline(name));
}

/**
 * Warn once per (material, Custom row) that the row has no baked maps to substitute.
 *
 * A Custom group has no live CPU expression, so an unbaked row drops out of the stack entirely --
 * the result below passes through. That is easy to mistake for a broken node group, so the first
 * time it happens for a row the user is told why; the message is not repeated on every rebuild.
 */
bool custom_bake_missing_warn_once(const Material &ma, const MaterialPaintLayer &layer)
{
  static Set<uint64_t> warned;
  uint64_t key = ma.id.session_uid;
  key = key * 1000003ull ^ layer.marker.time_low;
  key = key * 1000003ull ^ layer.marker.time_mid;
  key = key * 1000003ull ^ layer.marker.time_hi_and_version;
  key = key * 1000003ull ^ layer.marker.clock_seq_hi_and_reserved;
  key = key * 1000003ull ^ layer.marker.clock_seq_low;
  return warned.add(key);
}

/**
 * A `ShaderNodeMix` in RGBA mode: the shader's `ramp_blend`, with the factor clamped to 0..1 and
 * the result left unclamped -- exactly the CPU's `ramp_blend`, so Add can exceed one and the byte
 * encode is what finally clamps.
 */
bNode *mix_node_add(bNodeTree &tree, const int ramp_blend, const float location_x, const float location_y)
{
  bNode *node = bke::node_add_static_node(nullptr, tree, SH_NODE_MIX);
  if (node == nullptr) {
    return nullptr;
  }
  NodeShaderMix *storage = static_cast<NodeShaderMix *>(node->storage);
  storage->data_type = SOCK_RGBA;
  storage->factor_mode = NODE_MIX_MODE_UNIFORM;
  storage->blend_type = ramp_blend;
  storage->clamp_factor = true;
  storage->clamp_result = false;
  node->location[0] = location_x;
  node->location[1] = location_y;
  return node;
}

/**
 * Whether a Material row in #PaintLayerMaterialMode::SourceGroup exposes \a channel: the resolver
 * can supply it, so the wrapper group carries a `COLOR:<CHANNEL>` output. This is what makes such a
 * row take part even though the hybrid helpers answer nothing for it.
 */
bool material_source_group_channel(const Material &ma,
                                   const MaterialPaintLayer &layer,
                                   const int channel,
                                   const PaintLayersRegenCache *cache)
{
  if (layer.kind != MA_PAINT_LAYER_KIND_MATERIAL || layer.material == nullptr ||
      BKE_paint_layers_material_mode(ma, layer, cache) != PaintLayerMaterialMode::SourceGroup)
  {
    return false;
  }
  MaterialSourceResolve resolve_local;
  const MaterialSourceResolve &resolve = PaintLayersRegenCache::resolve_get(layer.material, cache, resolve_local);
  /* The resolver is the one authority on whether the channel can be shown at all; a channel it
   * calls Unavailable (Normal not through a Normal Map, say) takes no part in either mode, so the
   * wired set cannot change when the row moves between Hybrid and SourceGroup. */
  return resolve.channels[channel] != ChannelResolution::Unavailable;
}

/** The instance socket of \a wrapper's `COLOR:<CHANNEL>` (or `COVERAGE`) output, or null. */
bNodeSocket *source_group_output(bNodeTree &wrapper,
                                 bNode &instance,
                                 const int channel,
                                 const bool coverage)
{
  wrapper.ensure_interface_cache();
  for (bNodeTreeInterfaceSocket *iface : wrapper.interface_outputs()) {
    if (iface->identifier == nullptr) {
      continue;
    }
    const char *role = custom_role_get(iface->properties);
    const PaintLayerCustomRole kind = BKE_paint_layers_custom_role_kind(role);
    if (coverage) {
      if (kind != PaintLayerCustomRole::Coverage) {
        continue;
      }
    }
    else {
      if (kind != PaintLayerCustomRole::Color ||
          BKE_paint_layers_custom_role_channel(role) != channel)
      {
        continue;
      }
    }
    return bke::node_find_socket(instance, SOCK_OUT, UString::from_ptr_noinline(iface->identifier));
  }
  return nullptr;
}

/**
 * Runtime set of row markers the current generated graph was built without. Not saved and not DNA.
 *
 * Disabling a row is a value edit (factor zero) and does not touch this set, so the row stays in the
 * graph. When the graph is rebuilt for another reason, `removed_rows_reconcile` records every
 * disabled row here and the build leaves them out. Enabling a recorded row clears its marker and
 * moves the topology hash, which is the one rebuild that brings it back. Because membership is only
 * honored while the row is disabled, an undo that re-enables a row can never leave it missing: the
 * stale marker is ignored and the moved hash forces the rebuild instead.
 */
static Map<uint32_t, Vector<bUUID>> &removed_rows_state()
{
  static Map<uint32_t, Vector<bUUID>> map;
  return map;
}

static bool removed_rows_contains(const Material &ma, const bUUID &marker)
{
  const Vector<bUUID> *markers = removed_rows_state().lookup_ptr(ma.id.session_uid);
  if (markers == nullptr) {
    return false;
  }
  for (const bUUID &other : *markers) {
    if (BLI_uuid_equal(other, marker)) {
      return true;
    }
  }
  return false;
}

/** Whether the graph is built without \a layer: it is disabled and a past rebuild left it out. */
static bool row_is_removed(const Material &ma, const MaterialPaintLayer &layer)
{
  /* A Pass Through folder is never dropped when hidden: its children stay in the graph and their
   * factor goes to zero, so hiding it is a value edit rather than a rebuild. A stale marker from
   * before the mode became Pass Through is ignored for the same reason. Only the over-budget pass
   * may drop one, and then budget_cleanup_active is set. */
  if (BKE_paint_layers_folder_is_pass_through(ma, layer) && !budget_cleanup_active(ma)) {
    return false;
  }
  return (layer.flag & MA_PAINT_LAYER_ENABLED) == 0 && removed_rows_contains(ma, layer.marker);
}

/** Record exactly the rows disabled at this rebuild, so the build omits them from here on. */
static void removed_rows_reconcile(const Material &ma)
{
  Vector<bUUID> markers;
  Vector<const MaterialPaintLayer *> layers;
  BKE_paint_layers_flatten(ma, layers);
  for (const MaterialPaintLayer *layer : layers) {
    /* A hidden Pass Through folder keeps its subtree in the graph with a zero factor, so it is
     * never written to the removed set -- unless the over-budget pass is dropping hidden rows. */
    if ((layer->flag & MA_PAINT_LAYER_ENABLED) == 0 &&
        (!BKE_paint_layers_folder_is_pass_through(ma, *layer) || budget_cleanup_active(ma)))
    {
      markers.append(layer->marker);
    }
  }
  removed_rows_state().add_overwrite(ma.id.session_uid, std::move(markers));
}

/**
 * Find \a target in \a list's subtree and return the product of the enabled states of the Pass
 * Through folders above it. Returns false when \a target is not there.
 *
 * Only Pass Through folders scale: an isolating folder already folds its own visibility into its
 * row's opacity, so scaling its descendants too would count it twice (harmlessly at zero, but not at
 * one). Descending into every folder is still needed to reach a target nested inside one.
 */
static bool pass_through_scale_find(const Material &ma,
                                    const MaterialPaintLayer &target,
                                    const ListBase &list,
                                    const float scale,
                                    float &r_scale)
{
  for (const MaterialPaintLayer &layer :
       *reinterpret_cast<const ListBaseT<MaterialPaintLayer> *>(&list))
  {
    if (&layer == &target) {
      r_scale = scale;
      return true;
    }
    if (!BKE_paint_layers_is_folder(layer)) {
      continue;
    }
    float child_scale = scale;
    if (BKE_paint_layers_folder_is_pass_through(ma, layer)) {
      child_scale = ((layer.flag & MA_PAINT_LAYER_ENABLED) != 0) ? scale : 0.0f;
    }
    if (pass_through_scale_find(ma, target, layer.children, child_scale, r_scale)) {
      return true;
    }
  }
  return false;
}

/** Record the scale of every row of \a list's subtree, by the same rule #pass_through_scale_find
 * applies to one target. */
static void pass_through_scales_fill(const Material &ma,
                                     const ListBase &list,
                                     const float scale,
                                     Map<const MaterialPaintLayer *, float> &r_scales)
{
  for (const MaterialPaintLayer &layer :
       *reinterpret_cast<const ListBaseT<MaterialPaintLayer> *>(&list))
  {
    r_scales.add(&layer, scale);
    if (!BKE_paint_layers_is_folder(layer)) {
      continue;
    }
    float child_scale = scale;
    if (BKE_paint_layers_folder_is_pass_through(ma, layer)) {
      child_scale = ((layer.flag & MA_PAINT_LAYER_ENABLED) != 0) ? scale : 0.0f;
    }
    pass_through_scales_fill(ma, layer.children, child_scale, r_scales);
  }
}

/** The Pass Through visibility multiplier for \a target; one when no Pass Through folder encloses
 * it. It scales the row's factor value so hiding a Pass Through folder stays a value edit. With a
 * \a cache the whole stack is walked once and every later row is a lookup. */
static float pass_through_scale_of(const Material &ma,
                                   const MaterialPaintLayer &target,
                                   const PaintLayersRegenCache *cache = nullptr)
{
  if (cache != nullptr) {
    if (!cache->pass_through_scales_valid) {
      pass_through_scales_fill(ma, ma.paint_layers, 1.0f, cache->pass_through_scales);
      cache->pass_through_scales_valid = true;
    }
    return cache->pass_through_scales.lookup_default(&target, 1.0f);
  }
  float scale = 1.0f;
  pass_through_scale_find(ma, target, ma.paint_layers, 1.0f, scale);
  return scale;
}

/**
 * Whether \a layer or anything nested under it takes part in \a channel.
 *
 * A folder carries no maps of its own, so its participation is the union of its children's -- and
 * a folder whose subtree paints the channel needs an opacity input of its own all the same. A row
 * that a valid bake stands in for takes part even without a channel record: a Material layer's
 * channels exist only as its baked maps.
 */
bool layer_subtree_has_channel(const Material &ma,
                               const MaterialPaintLayer &layer,
                               const int channel,
                               const PaintLayersRegenCache *cache)
{
  if (row_is_removed(ma, layer)) {
    return false;
  }
  Image *baked = nullptr;
  if (BKE_paint_layers_bake_substitute(ma, layer, channel, &baked)) {
    return true;
  }
  /* C-7: a Custom row with a stale saved bake still takes part. */
  if (BKE_paint_layers_bake_substitute_custom(ma, layer, channel, &baked, nullptr)) {
    return true;
  }
  /* A Material row shown live takes part through its live channel, constant or map. */
  float live_value[4];
  Image *live_image = nullptr;
  const ImageUser *live_iuser = nullptr;
  if (BKE_paint_layers_material_live_constant(ma, layer, channel, live_value, cache) ||
      BKE_paint_layers_material_live_image(ma, layer, channel, &live_image, &live_iuser, cache) ||
      material_source_group_channel(ma, layer, channel, cache))
  {
    return true;
  }
  if (paint_layer_channel_present(layer, channel)) {
    return true;
  }
  if (!BKE_paint_layers_is_folder(layer)) {
    return false;
  }
  for (const MaterialPaintLayer &child :
       *reinterpret_cast<const ListBaseT<MaterialPaintLayer> *>(&layer.children))
  {
    if (layer_subtree_has_channel(ma, child, channel, cache)) {
      return true;
    }
  }
  return false;
}

/** A unique interface name for \a base, so a renamed or duplicated layer never collides. */
void interface_name_unique(const bNodeTreeInterface &interface,
                           const char *base,
                           char *r_buffer,
                           const size_t buffer_size)
{
  auto name_taken = [&](const char *name) {
    bool taken = false;
    interface.foreach_item([&](const bNodeTreeInterfaceItem &item) {
      if (item.item_type == NodeTreeInterfaceItemType::Socket) {
        const auto &socket = reinterpret_cast<const bNodeTreeInterfaceSocket &>(item);
        if (socket.name != nullptr && STREQ(socket.name, name)) {
          taken = true;
          return false;
        }
      }
      return true;
    });
    return taken;
  };
  BLI_snprintf(r_buffer, buffer_size, "%s", base);
  for (int suffix = 2; name_taken(r_buffer); suffix++) {
    BLI_snprintf(r_buffer, buffer_size, "%s %d", base, suffix);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Build
 * \{ */

/**
 * One layer as the chain builder sees it: its source socket, and where its opacity comes from.
 *
 * The owner nodes are carried alongside the sockets because `bNodeSocket::owner_node()` reads a
 * runtime pointer that is only filled by the topology cache; the cache is stale while the tree is
 * being built, so asking a freshly created socket for its node would answer null.
 */
struct ChainLayer {
  const MaterialPaintLayer *layer = nullptr;
  bNode *source_node = nullptr;
  bNodeSocket *source = nullptr;
  /** The opacity interface input, before any per-pixel mask is folded in. */
  bNode *opacity_node = nullptr;
  bNodeSocket *opacity = nullptr;
  /** What the Mix/Combine factor links from: the opacity, or opacity times a mask map's alpha. */
  bNode *factor_node = nullptr;
  bNodeSocket *factor = nullptr;
};

/** A chain's result plus, for a folder's contents, the coverage it accumulated. */
struct ChainResult {
  ChainLayer chain;
  /** The node owning #coverage; null for the root chain, which carries no coverage. */
  bNode *coverage_node = nullptr;
  bNodeSocket *coverage = nullptr;
};

/**
 * Where a single row's nodes go: the tree and the group input its values are read from. A row's
 * body always builds inside its own layer group, so that group input carries the row's value inputs
 * (session 10e).
 */
struct RowTarget {
  bNodeTree *tree = nullptr;
  bNode *group_input = nullptr;
  float location_x = 0.0f;
  float location_y = 0.0f;
};

/** One built row: its source and factor, or invalid when the row takes part in nothing here. */
struct RowResult {
  bool valid = false;
  ChainLayer current;
  /** For a folder row, the coverage its contents accumulated; null otherwise. */
  bNode *folder_coverage_node = nullptr;
  bNodeSocket *folder_coverage = nullptr;
  /** True when the row was built inside its own layer group, not in the parent. */
  bool grouped = false;
  bNode *group_instance = nullptr;
  /** The instance sockets the parent chains through: Below in, Color/Coverage/Blend/Result out. */
  bNodeSocket *group_below = nullptr;
  bNodeSocket *group_color = nullptr;
  bNodeSocket *group_coverage = nullptr;
  bNodeSocket *group_blend = nullptr;
  bNodeSocket *group_result = nullptr;
};

/** A leaf's or folder's own `.PL …` node group, its nodes, and its instance in the parent tree. */
struct LayerGroup {
  bNodeTree *tree = nullptr;
  bNode *group_input = nullptr;
  bNode *group_output = nullptr;
  bNode *instance = nullptr;
  bNodeTree *parent_tree = nullptr;
  /** The factory handed back a tree whose topology hash already matches: do not rebuild it. */
  bool unchanged = false;
  /**
   * The interface sockets this build created or reused. A rebuilt group's interface is not cleared
   * (so its socket identifiers, and the root's links into it, stay stable); anything not touched
   * this build is removed once the group is fully built.
   */
  Set<bNodeTreeInterfaceSocket *> used_sockets;
};

/**
 * Whether \a layer is replaced whole by its row cache, so its opacity, fill and corrections are
 * inside the bake and get no inputs of their own. A Material layer's bake is its source's maps,
 * which are the row's content instead (#BKE_paint_layers_bake_substitute says the same).
 */
bool row_is_substituted(const Material &ma, const MaterialPaintLayer &layer)
{
  return layer.kind != MA_PAINT_LAYER_KIND_MATERIAL && BKE_paint_layers_bake_is_valid(ma, layer);
}

/**
 * Whether \a layer is replaced by a saved bake in \a channel, so its live nodes carry no content:
 * a valid per-channel row cache, or the stale Custom cache C-7 kept for a missing live expression.
 */
bool row_channel_substituted(const Material &ma,
                             const MaterialPaintLayer &layer,
                             const int channel,
                             Image **r_baked_color)
{
  if (BKE_paint_layers_bake_substitute(ma, layer, channel, r_baked_color) &&
      layer.bake->coverage != nullptr)
  {
    return true;
  }
  return BKE_paint_layers_bake_substitute_custom(ma, layer, channel, r_baked_color, nullptr);
}

/**
 * Whether a non-folder, non-substituted leaf row takes part in \a channel: it has a record, or a map,
 * or a constant a Fill carries. The build and the group decision both ask this, so they never
 * disagree about which rows exist.
 */
bool leaf_participates(const MaterialPaintLayer &layer, const int channel)
{
  if (!paint_layer_channel_present(layer, channel)) {
    return false;
  }
  if (paint_layer_channel_image(layer, channel) != nullptr) {
    return true;
  }
  if (BKE_paint_layers_kind_info(layer.kind).uses_fill_color) {
    return true;
  }
  const MaterialPaintLayerChannel *record = paint_layer_channel_find(layer, channel);
  return record != nullptr && record->value[3] > 0.0f;
}

/**
 * Whether \a layer's row gets a group of its own in \a channel: it is substituted by a bake, a
 * folder whose subtree takes part, or a leaf that paints something here. One helper for the build's
 * `group_this` and the root hash, so the two can never disagree about which instances the root has.
 */
bool layer_row_has_group(const Material &ma,
                         const MaterialPaintLayer &layer,
                         const int channel,
                         const PaintLayersRegenCache *cache)
{
  if (row_is_removed(ma, layer)) {
    return false;
  }
  Image *baked = nullptr;
  if (row_channel_substituted(ma, layer, channel, &baked)) {
    return true;
  }
  if (BKE_paint_layers_is_folder(layer)) {
    /* A Pass Through folder owns no group and no instance: it is expanded into the parent chain,
     * so the root's nodes must be exactly those of a stack without the folder. */
    if (BKE_paint_layers_folder_is_pass_through(ma, layer)) {
      return false;
    }
    return layer_subtree_has_channel(ma, layer, channel, cache);
  }
  float live_value[4];
  Image *live_image = nullptr;
  const ImageUser *live_iuser = nullptr;
  if (BKE_paint_layers_material_live_constant(ma, layer, channel, live_value, cache) ||
      BKE_paint_layers_material_live_image(ma, layer, channel, &live_image, &live_iuser, cache) ||
      material_source_group_channel(ma, layer, channel, cache))
  {
    return true;
  }
  return leaf_participates(layer, channel);
}

/**
 * The channels \a ma wires at all, in channel order: a channel nothing participates in gets no
 * output, so the material keeps whatever the user had on that Principled input. One helper for the
 * build and the topology hash, so the two can never disagree about which channels exist.
 */
Vector<int> paint_layers_wired_channels(const Material &ma, const PaintLayersRegenCache *cache)
{
  Vector<const MaterialPaintLayer *> layers;
  BKE_paint_layers_flatten(ma, layers);
  Vector<int> wired;
  for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
    for (const MaterialPaintLayer *layer : layers) {
      if (layer_subtree_has_channel(ma, *layer, int(info.channel), cache)) {
        wired.append(int(info.channel));
        break;
      }
    }
  }
  return wired;
}

/** Mix \a value into the FNV-1a style accumulator; order-sensitive, so lists hash in order. */
uint64_t topology_hash_mix(uint64_t hash, const uint64_t value)
{
  hash ^= value;
  hash *= 1099511628211ull;
  return hash;
}

/**
 * The set of source materials the `MATERIAL` rows of \a ma read, as one order-independent hash:
 * the sources' `session_uid`s, sorted and deduplicated so a reorder or a repeated source changes
 * nothing. Zero when no row reads a source. The depsgraph builds a relation per distinct source,
 * so a difference here is exactly a change of the graph's relations.
 */
uint64_t paint_layers_source_materials_hash(const Material &ma)
{
  Vector<const MaterialPaintLayer *> layers;
  BKE_paint_layers_flatten(ma, layers);
  Vector<uint32_t> source_uids;
  for (const MaterialPaintLayer *layer : layers) {
    if (layer->kind == MA_PAINT_LAYER_KIND_MATERIAL && layer->material != nullptr) {
      source_uids.append(layer->material->id.session_uid);
    }
  }
  if (source_uids.is_empty()) {
    return 0;
  }
  std::sort(source_uids.begin(), source_uids.end());
  uint64_t hash = 0;
  bool first = true;
  uint32_t previous = 0;
  for (const uint32_t uid : source_uids) {
    if (!first && uid == previous) {
      continue;
    }
    hash = topology_hash_mix(hash, uid);
    previous = uid;
    first = false;
  }
  return hash;
}

uint64_t topology_hash_float(uint64_t hash, const float value)
{
  return topology_hash_mix(hash, std::bit_cast<uint32_t>(value));
}

void topology_hash_string(uint64_t &hash, const char *text)
{
  if (text == nullptr) {
    hash = topology_hash_mix(hash, 0);
    return;
  }
  for (const char *c = text; *c != '\0'; c++) {
    hash = topology_hash_mix(hash, uint8_t(*c));
  }
  hash = topology_hash_mix(hash, 1);
}

void topology_hash_uid(uint64_t &hash, const bUUID &uid)
{
  char formatted[UUID_STRING_SIZE];
  BLI_uuid_format(formatted, uid);
  topology_hash_string(hash, formatted);
}

uint64_t topology_hash_map_id(const Image *image)
{
  return (image != nullptr) ? image->id.session_uid : 0;
}

/**
 * The topology of one effect or mask item: everything that decides the nodes it contributes.
 * Opacity and a Fill constant are inputs, so they are not here.
 */
uint64_t topology_hash_correction(uint64_t hash,
                                  const MaterialPaintLayer &correction,
                                  const Span<int> wired_channels,
                                  const bool mask_item)
{
  topology_hash_uid(hash, correction.marker);
  /* The correction's name reaches the group's mirror input names for its value inputs. */
  topology_hash_string(hash, correction.name);
  hash = topology_hash_mix(hash, uint64_t(uint8_t(correction.section)));
  hash = topology_hash_mix(hash, uint64_t(uint8_t(correction.effect)));
  hash = topology_hash_mix(hash, uint64_t(uint8_t(correction.blend)));
  /* Visibility is a value: a disabled effect or mask item stays in the chain with opacity zero
   * (#BKE_paint_layers_effective_opacity), so it must not move this hash. */
  const bool fill = BKE_paint_layers_source_type(correction) == PaintLayerSourceType::Constant;
  hash = topology_hash_mix(hash, fill ? 1 : 0);
  if (mask_item && !fill) {
    /* Whether the mask map is read as colour data decides whether the chain builds its Divide: a
     * data texture is left pre-multiplied by the Image Texture node, a non-data one is straightened
     * there. A change of the map's colorspace must therefore rebuild the group. */
    const Image *mask_image = paint_layer_mask_correction_image(
        correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
    const bool data = mask_image != nullptr &&
                      IMB_colormanagement_space_name_is_data(mask_image->colorspace_settings.name);
    hash = topology_hash_mix(hash, data ? 1 : 0);
  }
  for (const int channel : wired_channels) {
    hash = topology_hash_mix(hash, uint64_t(channel));
    /* A content correction blends by its channel override; a mask item by the row blend. */
    hash = topology_hash_mix(
        hash, uint64_t(BKE_paint_layers_channel_blend_effective(correction, channel)));
    const Image *image = mask_item ? paint_layer_mask_correction_image(correction, channel) :
                                     paint_layer_channel_image(correction, channel);
    hash = topology_hash_mix(hash, topology_hash_map_id(image));
    if (!mask_item && !fill) {
      /* A content effect reads its own map per channel; whether that map is data decides whether
       * the chain builds a Divide (see the mask branch above). */
      const bool data = image != nullptr &&
                        IMB_colormanagement_space_name_is_data(image->colorspace_settings.name);
      hash = topology_hash_mix(hash, data ? 1 : 0);
    }
  }
  return hash;
}

/**
 * The full topology hash of one row, recursing into its effects, mask items and -- for a folder --
 * its children. A folder's own chain is a function of which children take part in which channel and
 * of their grouping, so a child's topology is part of the folder's; rebuilding a folder group does
 * not touch the child groups' own nodes, so the two levels still skip independently.
 */
uint64_t topology_hash_layer(uint64_t hash,
                             const Material &ma,
                             const MaterialPaintLayer &layer,
                             const Span<int> wired_channels,
                             const PaintLayersRegenCache *cache)
{
  const bool is_folder = BKE_paint_layers_is_folder(layer);
  hash = topology_hash_mix(hash, uint64_t(uint8_t(layer.kind)));
  hash = topology_hash_mix(hash, uint64_t(uint8_t(layer.blend)));
  hash = topology_hash_mix(hash, uint64_t(uint8_t(layer.section)));
  hash = topology_hash_mix(hash, uint64_t(uint8_t(layer.effect)));
  /* Visibility is a value: disabling leaves the row in the graph with factor zero, so the flag is
   * not part of topology. A rebuild for another reason may drop the row (see the removed-rows set),
   * and enabling it back force-invalidates the stored root hash and rebuilds it in. */
  hash = topology_hash_mix(hash, is_folder ? 1 : 0);
  hash = topology_hash_mix(hash, row_is_substituted(ma, layer) ? 1 : 0);
  /* The name reaches the mirror input names a preserved group carries, so a rename invalidates. */
  topology_hash_string(hash, layer.name);

  for (const int channel : wired_channels) {
    Image *baked = nullptr;
    const bool substituted = row_channel_substituted(ma, layer, channel, &baked);
    const bool participates = is_folder ? layer_subtree_has_channel(ma, layer, channel, cache) :
                                          leaf_participates(layer, channel);
    hash = topology_hash_mix(hash, uint64_t(channel));
    hash = topology_hash_mix(hash, substituted ? 1 : 0);
    hash = topology_hash_mix(hash, participates ? 1 : 0);
    hash = topology_hash_mix(
        hash, uint64_t(BKE_paint_layers_channel_blend_effective(layer, channel)));
    hash = topology_hash_mix(hash, topology_hash_map_id(paint_layer_channel_image(layer, channel)));
    float live_value[4];
    const bool live_constant = BKE_paint_layers_material_live_constant(
        ma, layer, channel, live_value, cache);
    hash = topology_hash_mix(hash, live_constant ? 1 : 0);
    if (live_constant) {
      /* The value lives in another material, which #BKE_paint_layers_values_sync cannot see, so it
       * is hashed here as topology: a slider move rebuilds this one group instead of baking through
       * EEVEE. The root hash is unaffected, since the group's contract (Below, its outputs) is the
       * same. */
      for (const float component : live_value) {
        hash = topology_hash_float(hash, component);
      }
    }
    Image *live_map_image = nullptr;
    const ImageUser *live_map_iuser = nullptr;
    const bool live_map = BKE_paint_layers_material_live_image(
        ma, layer, channel, &live_map_image, &live_map_iuser, cache);
    hash = topology_hash_mix(hash, live_map ? 1 : 0);
    if (live_map) {
      /* Which map the row shows is topology, like any other map a row reads. */
      hash = topology_hash_mix(hash, topology_hash_map_id(live_map_image));
    }
    const PaintLayerMaterialMode material_mode = (layer.kind == MA_PAINT_LAYER_KIND_MATERIAL) ?
                                                     BKE_paint_layers_material_mode(ma, layer, cache) :
                                                     PaintLayerMaterialMode::Baked;
    hash = topology_hash_mix(hash, uint64_t(material_mode));
    if (material_mode == PaintLayerMaterialMode::SourceGroup && layer.material != nullptr) {
      /* Only the source's topology is part of this group's topology; a value edit is synced into
       * the wrapper in place and must not rebuild the row's group. */
      hash = topology_hash_mix(
          hash, BKE_paint_layers_source_material_topology_hash(*layer.material));
    }
    if (substituted) {
      hash = topology_hash_mix(hash, topology_hash_map_id(baked));
      hash = topology_hash_mix(
          hash,
          (layer.bake != nullptr) ? topology_hash_map_id(layer.bake->coverage) : 0);
    }
  }
  for (const MaterialPaintLayer *effect : BKE_paint_layers_effects(layer)) {
    hash = topology_hash_correction(hash, *effect, wired_channels, false);
  }
  for (const MaterialPaintLayer *mask_item : BKE_paint_layers_mask_items(layer)) {
    hash = topology_hash_correction(hash, *mask_item, wired_channels, true);
  }
  for (const MaterialPaintLayer &child :
       *reinterpret_cast<const ListBaseT<MaterialPaintLayer> *>(&layer.children))
  {
    hash = topology_hash_layer(hash, ma, child, wired_channels, cache);
  }
  return hash;
}

}  // namespace

bool BKE_paint_layers_row_removed_clear(Material &ma, const bUUID &marker)
{
  Vector<bUUID> *markers = removed_rows_state().lookup_ptr(ma.id.session_uid);
  if (markers == nullptr) {
    return false;
  }
  for (int i = 0; i < markers->size(); i++) {
    if (BLI_uuid_equal((*markers)[i], marker)) {
      markers->remove(i);
      return true;
    }
  }
  return false;
}

void BKE_paint_layers_root_hash_invalidate(Material &ma)
{
  if (ma.paint_layers_tree != nullptr) {
    tree_root_hash_set(*ma.paint_layers_tree, 0);
  }
}

uint64_t paint_layers_layer_topology_hash(const Material &ma,
                                          const MaterialPaintLayer &layer,
                                          const Span<int> wired_channels,
                                          const PaintLayersRegenCache *cache)
{
  return topology_hash_layer(1469598103934665603ull, ma, layer, wired_channels, cache);
}

/**
 * The hash of everything the root tree's own nodes and links depend on: the wired channels, the
 * top-level rows and their per-channel participation, and the interface signature of every layer
 * group's *contract* (its Below inputs and its outputs). A group's value inputs are skipped: they
 * are inputs of the group itself (session 10e), so they appear as a socket on an existing instance
 * and never change the root's nodes or links. A rebuilt group's internals are likewise absent, so a
 * map or correction edit inside one layer leaves the root hash unchanged and the root is kept.
 */
uint64_t paint_layers_root_topology_hash(
    const Material &ma,
    const Span<int> wired_channels,
    const Map<const MaterialPaintLayer *, bNodeTree *> &layer_trees,
    const PaintLayersRegenCache *cache)
{
  uint64_t hash = 1469598103934665603ull;
  for (const int channel : wired_channels) {
    hash = topology_hash_mix(hash, uint64_t(channel));
  }
  /* Top-level rows in order, and which of them take part in each channel. A Pass Through folder
   * contributes nothing of its own, so its children are hashed exactly as if the folder were absent;
   * that is what lets moving a row into one leave this hash -- and the kept root -- unchanged. */
  auto hash_row_list = [&](auto &&self, const ListBase &list) -> void {
    for (const MaterialPaintLayer &layer :
         *reinterpret_cast<const ListBaseT<MaterialPaintLayer> *>(&list))
    {
      if (BKE_paint_layers_folder_is_pass_through(ma, layer)) {
        if (!row_is_removed(ma, layer)) {
          self(self, layer.children);
        }
        continue;
      }
      topology_hash_uid(hash, layer.marker);
      for (const int channel : wired_channels) {
        const bool has_group = layer_row_has_group(ma, layer, channel, cache);
        hash = topology_hash_mix(hash, has_group ? 1 : 0);
      }
    }
  };
  hash_row_list(hash_row_list, ma.paint_layers);
  /* Every layer group's interface: names and types in creation order. It decides the root's
   * instance sockets and the links between them. */
  /* Only the groups the root instantiates count: the top-level rows, seen through Pass Through
   * folders exactly as in the walk above. The children of an isolating folder live inside that
   * folder's group, whose own interface is hashed here and whose topology hash already covers its
   * children. They are also present in \a layer_trees only when the folder's group was rebuilt in
   * this pass -- an untouched folder is reused without asking for its children -- so hashing them
   * made the value depend on whether the folder happened to rebuild, and the next pass rebuilt the
   * root again. */
  Vector<const MaterialPaintLayer *> layers;
  auto collect_root_layers = [&](auto &&self, const ListBase &list) -> void {
    for (const MaterialPaintLayer &layer :
         *reinterpret_cast<const ListBaseT<MaterialPaintLayer> *>(&list))
    {
      if (BKE_paint_layers_folder_is_pass_through(ma, layer)) {
        if (!row_is_removed(ma, layer)) {
          self(self, layer.children);
        }
        continue;
      }
      layers.append(&layer);
    }
  };
  collect_root_layers(collect_root_layers, ma.paint_layers);
  for (const MaterialPaintLayer *layer : layers) {
    bNodeTree *const *tree_ptr = layer_trees.lookup_ptr(layer);
    if (tree_ptr == nullptr || *tree_ptr == nullptr) {
      continue;
    }
    hash = topology_hash_mix(hash, 1);
    (*tree_ptr)->tree_interface.foreach_item([&](const bNodeTreeInterfaceItem &item) {
      if (item.item_type != NodeTreeInterfaceItemType::Socket) {
        return true;
      }
      const auto &socket = reinterpret_cast<const bNodeTreeInterfaceSocket &>(item);
      /* A value input is owned by the group; it never reaches the root's nodes or links. */
      if ((socket.flag & NODE_INTERFACE_SOCKET_INPUT) != 0 &&
          prop_string_get(socket.properties, INPUT_ROLE_PROP) != nullptr)
      {
        return true;
      }
      hash = topology_hash_mix(
          hash, (socket.flag & NODE_INTERFACE_SOCKET_INPUT) != 0 ? 1 : 2);
      topology_hash_string(hash, socket.name);
      topology_hash_string(hash, socket.socket_type);
      return true;
    });
  }
  return hash;
}

/** Whether \a node belongs to \a tree. Used to hold the "wrapper instance lives in the tree it is
 * linked into" invariant; a node only ever belongs to one tree. */
static bool node_belongs_to_tree(const bNodeTree &tree, const bNode &node)
{
  for (const bNode &candidate : tree.nodes) {
    if (&candidate == &node) {
      return true;
    }
  }
  return false;
}

void paint_layers_tree_build(const Material &ma,
                             bNodeTree &tree,
                             const PaintLayersBuildContext &ctx)
{
  /* The factory is the single path: without it a layer has nowhere to put its nodes. */
  BLI_assert_msg(bool(ctx.layer_tree_get), "paint_layers_tree_build needs a layer-tree factory");
  if (!ctx.layer_tree_get) {
    return;
  }

  /* Debug-only: a row's section has to match the list it sits in (effects vs mask_stack), or the
   * generator, the CPU compositor and the bake would disagree about what it is. */
  BKE_paint_layers_assert_consistent(ma);
  /* Which channels the description wires at all. A channel nothing participates in gets no output,
   * so the material keeps whatever the user had on that Principled input. */
  const PaintLayersRegenCache *const cache = ctx.regen_cache;
  const Vector<int> wired_channels = paint_layers_wired_channels(ma, cache);
  if (wired_channels.is_empty()) {
    return;
  }

  bNodeTreeInterface &interface = tree.tree_interface;

  /* Interface sockets are created before the group input/output nodes, so those nodes get their
   * sockets immediately and can be linked right away. */

  /* Values are interface inputs of the group that owns them (session 10e): a row's opacity and Fill
   * constant, and each effect's and mask item's opacity and constant. They are created on the owning
   * layer's own group when it is ensured; these maps let the row builder find the socket. */
  Map<const MaterialPaintLayer *, Map<int, bNodeTreeInterfaceSocket *>> opacity_inputs;
  Map<const MaterialPaintLayerChannel *, bNodeTreeInterfaceSocket *> fill_inputs;
  Map<const MaterialPaintLayer *, Map<int, bNodeTreeInterfaceSocket *>> correction_opacity_inputs;
  Map<const MaterialPaintLayer *, bNodeTreeInterfaceSocket *> correction_fill_inputs;


  /* One output per wired channel. */
  Map<int, bNodeTreeInterfaceSocket *> result_outputs;
  for (const int channel : wired_channels) {
    const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
        eMaterialPaintChannel(channel));
    char result_name[64];
    SNPRINTF(result_name, "Result %s", info.ui_name);
    bNodeTreeInterfaceSocket *socket = interface.add_socket(
        result_name, "", "NodeSocketColor", NODE_INTERFACE_SOCKET_OUTPUT, nullptr);
    if (socket != nullptr) {
      result_outputs.add(channel, socket);
    }
  }
  bNode *group_input = bke::node_add_node(nullptr, tree, "NodeGroupInput"_ustr);
  bNode *group_output = bke::node_add_node(nullptr, tree, "NodeGroupOutput"_ustr);
  if (group_input == nullptr || group_output == nullptr) {
    return;
  }
  group_input->location[0] = -400;
  group_output->location[0] = 600;

  /* The root target: a row's own group is created before the row is built, so the root target is
   * only the starting scope; its own group node carries the row's values. */
  RowTarget target;
  target.tree = &tree;
  target.group_input = group_input;

  /* A layer's or folder's group is created once per layer, across channels: one group holds every
   * channel's Below/Color/Coverage/Blend/Result and the value mirrors the row uses. The groups are
   * stored by pointer so a nested group's #LayerGroup::parent stays valid as the map grows. */
  Map<const MaterialPaintLayer *, LayerGroup *> layer_groups;
  Vector<std::unique_ptr<LayerGroup>> layer_group_storage;

  /* A Material row in SourceGroup mode instantiates its source wrapper once per row, across all
   * channels: every channel's chain reads one output of the same instance node. */
  Map<const MaterialPaintLayer *, bNode *> source_group_instances;
  Map<const MaterialPaintLayer *, bNodeTree *> source_group_trees;
  auto source_group_instance_get = [&](const MaterialPaintLayer &layer, bNodeTree &tree) -> bNode * {
    if (bNode **found = source_group_instances.lookup_ptr(&layer)) {
      return *found;
    }
    bNode *instance = nullptr;
    const char *failure = nullptr;
    if (!ctx.source_group_get) {
      failure = "no-factory";
    }
    else if (layer.material == nullptr) {
      failure = "no-wrapper";
    }
    else if (tree.typeinfo == nullptr || tree.typeinfo->group_idname == nullptr) {
      failure = "add-failed";
    }
    else {
      bNodeTree *wrapper = ctx.source_group_get(*layer.material);
      if (wrapper == nullptr) {
        failure = "no-wrapper";
      }
      else {
        instance = bke::node_add_node(nullptr, tree, tree.typeinfo->group_idname);
        if (instance == nullptr) {
          failure = "add-failed";
        }
        else {
          instance->id = &wrapper->id;
          id_us_plus(&wrapper->id);
          STRNCPY_UTF8(instance->label, layer.name);
          /* The instance sockets come from the wrapper's interface; build them now so the channel
           * chains can link into the COLOR/COVERAGE outputs during this same pass. */
          nodes::update_node_declaration_and_sockets(tree, *instance);
          source_group_trees.add(&layer, wrapper);
        }
      }
    }
    if (instance == nullptr) {
      printf("paint layers: row '%s': no wrapper instance (%s)\n",
             layer.name,
             (failure != nullptr) ? failure : "unknown");
    }
    source_group_instances.add(&layer, instance);
    return instance;
  };

  auto refresh_layer_group = [&](LayerGroup &group) {
    if (group.tree == nullptr) {
      return;
    }
    if (group.group_input != nullptr) {
      nodes::update_node_declaration_and_sockets(*group.tree, *group.group_input);
    }
    if (group.group_output != nullptr) {
      nodes::update_node_declaration_and_sockets(*group.tree, *group.group_output);
    }
    if (group.instance != nullptr && group.parent_tree != nullptr) {
      nodes::update_node_declaration_and_sockets(*group.parent_tree, *group.instance);
    }
  };

  /** An existing interface socket of \a group with \a name, \a socket_type and direction, or null. */
  auto group_interface_socket_find = [&](LayerGroup &group,
                                         const char *name,
                                         const StringRef socket_type,
                                         const NodeTreeInterfaceSocketFlag flag)
      -> bNodeTreeInterfaceSocket * {
    bNodeTreeInterfaceSocket *found = nullptr;
    group.tree->tree_interface.foreach_item([&](bNodeTreeInterfaceItem &item) {
      if (item.item_type != NodeTreeInterfaceItemType::Socket) {
        return true;
      }
      bNodeTreeInterfaceSocket &socket = reinterpret_cast<bNodeTreeInterfaceSocket &>(item);
      const bool want_input = (flag & NODE_INTERFACE_SOCKET_INPUT) != 0;
      if (((socket.flag & NODE_INTERFACE_SOCKET_INPUT) != 0) != want_input) {
        return true;
      }
      if (socket.name == nullptr || !STREQ(socket.name, name)) {
        return true;
      }
      if (socket.socket_type == nullptr || StringRef(socket.socket_type) != socket_type) {
        return true;
      }
      found = &socket;
      return false;
    });
    return found;
  };

  /* Add an interface socket to a layer group and grow its three nodes to match. A rebuilt group
   * keeps its old interface (see #LayerGroup::used_sockets), so a socket it already carries is
   * reused: that keeps its identifier, and with it the parent's links into the group, stable. */
  auto layer_group_add_socket = [&](LayerGroup &group,
                                    const char *base,
                                    const StringRef socket_type,
                                    const NodeTreeInterfaceSocketFlag flag)
      -> bNodeTreeInterfaceSocket * {
    if (bNodeTreeInterfaceSocket *existing = group_interface_socket_find(
            group, base, socket_type, flag))
    {
      group.used_sockets.add(existing);
      return existing;
    }
    char name[256];
    interface_name_unique(group.tree->tree_interface, base, name, sizeof(name));
    bNodeTreeInterfaceSocket *socket = group.tree->tree_interface.add_socket(
        name, "", socket_type, flag, nullptr);
    if (socket != nullptr) {
      group.used_sockets.add(socket);
      refresh_layer_group(group);
    }
    return socket;
  };

  /** Add a value input to \a group's interface, tagged so values_sync can find it. */
  auto layer_group_value_input = [&](LayerGroup &group,
                                     const char *base,
                                     const StringRef socket_type,
                                     const char *role,
                                     const bUUID &marker,
                                     const int channel) -> bNodeTreeInterfaceSocket * {
    bNodeTreeInterfaceSocket *socket = layer_group_add_socket(
        group, base, socket_type, NODE_INTERFACE_SOCKET_INPUT);
    if (socket == nullptr) {
      return nullptr;
    }
    uid_prop_set(socket->properties, INPUT_MARKER_PROP, marker);
    prop_string_set(socket->properties, INPUT_ROLE_PROP, role);
    prop_int_set(socket->properties, INPUT_CHANNEL_PROP, channel);
    return socket;
  };

  /* The values of \a layer live on its own group's interface (session 10e): a row's opacity and
   * Fill constant, and each effect's and mask item's opacity and constant. The set is decided by
   * topology alone, so editing a value never changes it. */
  auto create_value_inputs = [&](LayerGroup &group, const MaterialPaintLayer &layer) {
    /* A substituted row's values are inside its bake; a group input would apply them twice. */
    if (row_is_substituted(ma, layer)) {
      return;
    }
    for (const int channel : wired_channels) {
      if (!layer_subtree_has_channel(ma, layer, channel, cache)) {
        continue;
      }
      const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
          eMaterialPaintChannel(channel));
      char base[200];
      SNPRINTF(base,
               "%s %s Opacity",
               layer.name[0] != '\0' ? layer.name : "Layer",
               info.ui_name);
      bNodeTreeInterfaceSocket *socket = layer_group_value_input(
          group, base, "NodeSocketFloat", ROLE_OPACITY, layer.marker, channel);
      if (socket == nullptr) {
        continue;
      }
      if (socket->socket_data != nullptr) {
        static_cast<bNodeSocketValueFloat *>(socket->socket_data)->value =
            BKE_paint_layers_channel_opacity_effective(layer, channel) *
            pass_through_scale_of(ma, layer, cache);
      }
      opacity_inputs.lookup_or_add_default(&layer).add(channel, socket);
    }
    for (const int channel : wired_channels) {
      const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
          eMaterialPaintChannel(channel));
      if (!paint_layer_channel_present(layer, channel) ||
          paint_layer_channel_image(layer, channel) != nullptr)
      {
        continue;
      }
      if (!BKE_paint_layers_kind_info(layer.kind).uses_fill_color) {
        const MaterialPaintLayerChannel *record = paint_layer_channel_find(layer, channel);
        if (record == nullptr || record->value[3] <= 0.0f) {
          continue;
        }
      }
      char base[160];
      SNPRINTF(base,
               "%s %s",
               layer.name[0] != '\0' ? layer.name : "Layer",
               info.ui_name);
      bNodeTreeInterfaceSocket *socket = layer_group_value_input(
          group, base, "NodeSocketColor", ROLE_FILL, layer.marker, channel);
      if (socket == nullptr) {
        continue;
      }
      if (socket->socket_data != nullptr) {
        float color[4];
        paint_layer_channel_constant(layer, channel, color);
        copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(socket->socket_data)->value, color);
      }
      fill_inputs.add(paint_layer_channel_find(layer, channel), socket);
    }
    auto add_correction = [&](const MaterialPaintLayer &correction, const bool mask_item) {
      for (const int channel : wired_channels) {
        const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
            eMaterialPaintChannel(channel));
        char base[224];
        SNPRINTF(base,
                 "%s %s %s Opacity",
                 layer.name[0] != '\0' ? layer.name : "Layer",
                 correction.name[0] != '\0' ? correction.name : "Correction",
                 info.ui_name);
        bNodeTreeInterfaceSocket *socket = layer_group_value_input(
            group, base, "NodeSocketFloat", ROLE_CORRECTION_OPACITY, correction.marker, channel);
        if (socket == nullptr) {
          continue;
        }
        if (socket->socket_data != nullptr) {
          static_cast<bNodeSocketValueFloat *>(socket->socket_data)->value =
              mask_item ? BKE_paint_layers_effective_opacity(correction) :
                          BKE_paint_layers_channel_opacity_effective(correction, channel);
        }
        correction_opacity_inputs.lookup_or_add_default(&correction).add(channel, socket);
      }
      if (BKE_paint_layers_source_type(correction) != PaintLayerSourceType::Constant) {
        return;
      }
      char base[200];
      SNPRINTF(base,
               "%s %s Fill",
               layer.name[0] != '\0' ? layer.name : "Layer",
               correction.name[0] != '\0' ? correction.name : "Correction");
      bNodeTreeInterfaceSocket *socket = layer_group_value_input(
          group,
          base,
          "NodeSocketColor",
          ROLE_CORRECTION_FILL,
          correction.marker,
          PAINT_MATERIAL_CHANNEL_BASE_COLOR);
      if (socket != nullptr && socket->socket_data != nullptr) {
        float color[4];
        BKE_paint_layers_correction_constant(
            correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR, color);
        copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(socket->socket_data)->value, color);
      }
      correction_fill_inputs.add(&correction, socket);
    };
    for (const MaterialPaintLayer &effect :
         *reinterpret_cast<const ListBaseT<MaterialPaintLayer> *>(&layer.effects))
    {
      add_correction(effect, false);
    }
    for (const MaterialPaintLayer &mask_item :
         *reinterpret_cast<const ListBaseT<MaterialPaintLayer> *>(&layer.mask_stack))
    {
      add_correction(mask_item, true);
    }
  };

  auto layer_group_ensure = [&](const MaterialPaintLayer &layer,
                                bNodeTree &parent_tree) -> LayerGroup * {
    if (LayerGroup **found = layer_groups.lookup_ptr(&layer)) {
      return *found;
    }
    if (!ctx.layer_tree_get) {
      return nullptr;
    }
    bNodeTree *group_tree = ctx.layer_tree_get(layer);
    if (group_tree == nullptr || parent_tree.typeinfo == nullptr ||
        parent_tree.typeinfo->group_idname == nullptr)
    {
      return nullptr;
    }
    layer_group_storage.append(std::make_unique<LayerGroup>());
    LayerGroup &group = *layer_group_storage.last();
    layer_groups.add(&layer, &group);
    group.tree = group_tree;
    group.parent_tree = &parent_tree;
    group.unchanged = ctx.layer_tree_unchanged && ctx.layer_tree_unchanged(layer);
    /* A preserved group still holds its Group Input/Output nodes; a fresh or rebuilt one is empty
     * (the factory cleared it), so create them only when they are missing. */
    for (bNode &node : group_tree->nodes) {
      if (node.is_group_input()) {
        group.group_input = &node;
      }
      else if (node.is_group_output()) {
        group.group_output = &node;
      }
    }
    if (group.group_input == nullptr) {
      group.group_input = bke::node_add_node(nullptr, *group_tree, "NodeGroupInput"_ustr);
    }
    if (group.group_output == nullptr) {
      group.group_output = bke::node_add_node(nullptr, *group_tree, "NodeGroupOutput"_ustr);
    }
    bNode *instance = bke::node_add_node(nullptr, parent_tree, parent_tree.typeinfo->group_idname);
    if (instance != nullptr) {
      instance->id = &group_tree->id;
      /* The factory tree has no users; assigning the id makes this instance its only one. */
      id_us_plus(&group_tree->id);
      STRNCPY_UTF8(instance->label, layer.name);
      group.instance = instance;
    }
    refresh_layer_group(group);
    /* A preserved group already carries its values; only a rebuilt one re-creates them. */
    if (!group.unchanged) {
      create_value_inputs(group, layer);
    }
    return &group;
  };

  /** The parent-side view of a preserved group's contract for \a channel, without rebuilding it. */
  auto row_from_unchanged_group = [&](LayerGroup &group,
                                      const MaterialPaintLayer &layer,
                                      const int channel) -> RowResult {
    RowResult result;
    const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
        eMaterialPaintChannel(channel));
    group.tree->ensure_interface_cache();
    auto instance_socket = [&](const char *kind, const bool output) -> bNodeSocket * {
      char name[192];
      SNPRINTF(name, "%s %s", kind, info.ui_name);
      for (bNodeTreeInterfaceSocket *iface : (output ? group.tree->interface_outputs() :
                                                        group.tree->interface_inputs()))
      {
        if (iface->name != nullptr && iface->identifier != nullptr && STREQ(iface->name, name)) {
          return bke::node_find_socket(
              *group.instance,
              output ? SOCK_OUT : SOCK_IN,
              UString::from_ptr_noinline(iface->identifier));
        }
      }
      return nullptr;
    };
    result.group_instance = group.instance;
    result.group_below = instance_socket("Below", false);
    result.group_color = instance_socket("Color", true);
    result.group_coverage = instance_socket("Coverage", true);
    result.group_blend = instance_socket("Blend", true);
    result.group_result = instance_socket("Result", true);
    result.current.layer = &layer;
    result.grouped = true;
    result.valid = result.group_instance != nullptr && result.group_below != nullptr &&
                   result.group_color != nullptr && result.group_coverage != nullptr &&
                   result.group_result != nullptr;
    return result;
  };

  float location_y = 0.0f;
  for (const int channel : wired_channels) {
    bNodeTreeInterfaceSocket *const *result_iface = result_outputs.lookup_ptr(channel);
    if (result_iface == nullptr || (*result_iface)->identifier == nullptr) {
      continue;
    }
    bNodeSocket *result_socket = bke::node_find_socket(
        *group_output, SOCK_IN, UString::from_ptr_noinline((*result_iface)->identifier));
    if (result_socket == nullptr) {
      continue;
    }

    /* The bottom of every chain is a constant rather than the first layer's own map: a chain that
     * started at a bare image would have no Mix node of its own, and the layer's opacity could not
     * be applied at all. Normal starts from a flat tangent-space normal, everything else from
     * transparent. */
    bNode *bottom_node = bke::node_add_static_node(nullptr, tree, SH_NODE_RGB);
    if (bottom_node == nullptr) {
      continue;
    }
    bottom_node->location[0] = 0.0f;
    bottom_node->location[1] = location_y;
    /* The shared channel table: the CPU composite starts from the same value, so a partially
     * covered row fades towards the same colour on both sides. */
    float bottom_color[4];
    BKE_paint_layers_channel_bottom_color(eMaterialPaintChannel(channel), bottom_color);
    if (bNodeSocket *bottom_color_socket = socket_out(*bottom_node, "Color")) {
      if (bNodeSocketValueRGBA *value = static_cast<bNodeSocketValueRGBA *>(
              bottom_color_socket->default_value))
      {
        copy_v4_v4(value->value, bottom_color);
      }
    }
    ChainLayer previous;
    previous.source_node = bottom_node;
    previous.source = socket_out(*bottom_node, "Color");
    float location_x = 180.0f;

    /* Set while a folder's own row is built: its source is the sub-chain's straight result, and its
     * factor is multiplied by the sub-chain's coverage. */
    bNode *folder_source_node = nullptr;
    bNodeSocket *folder_source = nullptr;
    bNode *folder_coverage_node = nullptr;
    bNodeSocket *folder_coverage = nullptr;

    std::function<ChainResult(const ListBase &, ChainLayer, bool, const RowTarget &)> build_list =
        [&](const ListBase &list,
            ChainLayer previous,
            const bool premul,
            const RowTarget &parent_target) -> ChainResult {
      /* This list builds in the tree it is handed: the root, or a folder's own group. */
      bNodeTree &tree = *parent_target.tree;
      bNode *coverage_node = nullptr;
      bNodeSocket *coverage = nullptr;
      if (premul) {
        bNode *zero = bke::node_add_static_node(nullptr, tree, SH_NODE_VALUE);
        if (zero != nullptr) {
          coverage_node = zero;
          coverage = socket_out(*zero, "Value");
          if (coverage != nullptr && coverage->default_value != nullptr) {
            static_cast<bNodeSocketValueFloat *>(coverage->default_value)->value = 0.0f;
          }
        }
      }
      auto build_row = [&](const MaterialPaintLayer *layer,
                           const RowTarget &target,
                           const bool substituted,
                           Image *baked_color,
                           const bool premul,
                           LayerGroup *layer_group) -> RowResult {
        /* A row a past rebuild left out of the graph contributes nothing. */
        if (row_is_removed(ma, *layer)) {
          return {};
        }
        /* The aliases keep the row body unchanged; for a leaf target points at the row's own group,
         * where the value socket mirrors each root input the row uses. */
        bNodeTree &tree = *target.tree;
        bNode *group_input = target.group_input;
        /* The row's values are inputs of its own group; its Group Input node exposes them. */
        auto group_input_socket =
            [&](const bNodeTreeInterfaceSocket &iface) -> bNodeSocket * {
          if (group_input == nullptr || iface.identifier == nullptr) {
            return nullptr;
          }
          return bke::node_find_socket(
              *group_input, SOCK_OUT, UString::from_ptr_noinline(iface.identifier));
        };
        const float location_x = target.location_x;
        const float location_y = target.location_y;
        /* A Custom and a Material row alike have no generated subtree: a valid bake substitutes
         * above, and without one there is no channel record and the row drops out below. */

        folder_source_node = nullptr;
        folder_source = nullptr;
        folder_coverage_node = nullptr;
        folder_coverage = nullptr;
        /* The active Material row's live value for this channel, when it has one. A live constant
         * needs no sampler; a live map is the source's own texture. Both count as the row taking
         * part even without a channel record, so the early drop below must see them. */
        float live_value[4];
        Image *live_map_image = nullptr;
        const ImageUser *live_map_iuser = nullptr;
        const bool live_constant = BKE_paint_layers_material_live_constant(
            ma, *layer, channel, live_value, cache);
        const bool live_map = !live_constant &&
                              BKE_paint_layers_material_live_image(
                                  ma, *layer, channel, &live_map_image, &live_map_iuser, cache);
        /* A Material row whose whole source graph goes through the wrapper group. Its instance is
         * created once per row and reused for every channel. */
        const PaintLayerMaterialMode material_mode = (layer->kind == MA_PAINT_LAYER_KIND_MATERIAL) ?
                                                         BKE_paint_layers_material_mode(
                                                             ma, *layer, cache) :
                                                         PaintLayerMaterialMode::Baked;
        bNode *source_group_instance = nullptr;
        bNodeTree *source_group_tree = nullptr;
        bNodeSocket *source_group_socket = nullptr;
        /* A Material row only takes part in a channel the resolver can supply. Without this gate
         * the wrapper's own `COLOR:Normal` output (the socket exists even when the resolver calls
         * the channel Unavailable) would make the row look live there; and because an unavailable
         * channel gives the row no layer group, `target.tree` is the parent tree, while the wrapper
         * instance cached from the row's own group tree would then be linked into it -- a link
         * between two trees, which crashed the rebuild. */
        if (!substituted && material_mode == PaintLayerMaterialMode::SourceGroup &&
            material_source_group_channel(ma, *layer, channel, cache))
        {
          source_group_instance = source_group_instance_get(*layer, tree);
          source_group_tree = source_group_trees.lookup_default(layer, nullptr);
          if (source_group_instance != nullptr && source_group_tree != nullptr) {
            source_group_socket = source_group_output(
                *source_group_tree, *source_group_instance, channel, false);
          }
          /* The instance is cached once per row and reused across that row's channels, which all
           * build in the row's own group tree; it must never come from another tree. */
          BLI_assert(source_group_instance == nullptr ||
                     node_belongs_to_tree(tree, *source_group_instance));
        }

        if (!substituted && BKE_paint_layers_is_folder(*layer)) {
          /* A folder: its contents are built in isolation -- pre-multiplied colour and coverage --
           * and then its own row lays the isolated result over what is below (design §5). */
          bNode *p_zero = bke::node_add_static_node(nullptr, tree, SH_NODE_RGB);
          bNodeSocket *p_zero_out = (p_zero != nullptr) ? socket_out(*p_zero, "Color") : nullptr;
          if (p_zero_out == nullptr) {
            return {};
          }
          if (p_zero_out->default_value != nullptr) {
            copy_v4_fl(
                static_cast<bNodeSocketValueRGBA *>(p_zero_out->default_value)->value, 0.0f);
          }
          ChainLayer sub_previous;
          sub_previous.source_node = p_zero;
          sub_previous.source = p_zero_out;
          ChainResult sub = build_list(layer->children, sub_previous, true, target);
          if (sub.chain.source == nullptr || sub.coverage == nullptr) {
            return {};
          }
          /* S_folder = P / a; the Vector Math divide is per channel and safe (0 on zero), so an
           * empty folder answers zero and covers nothing. */
          bNode *divide = bke::node_add_node(nullptr, tree, "ShaderNodeVectorMath"_ustr);
          bNodeSocket *div_a = (divide != nullptr) ? socket_in(*divide, "Vector") : nullptr;
          bNodeSocket *div_b = (divide != nullptr) ? socket_in(*divide, "Vector_001") : nullptr;
          bNodeSocket *div_out = (divide != nullptr) ? socket_out(*divide, "Vector") : nullptr;
          if (div_out == nullptr) {
            return {};
          }
          divide->custom1 = NODE_VECTOR_MATH_DIVIDE;
          divide->location[0] = location_x;
          divide->location[1] = location_y - 480.0f;
          bke::node_add_link(tree, *sub.chain.source_node, *sub.chain.source, *divide, *div_a);
          bke::node_add_link(tree, *sub.coverage_node, *sub.coverage, *divide, *div_b);
          folder_source_node = divide;
          folder_source = div_out;
          folder_coverage_node = sub.coverage_node;
          folder_coverage = sub.coverage;
        }
        else if (!substituted && !BKE_paint_layers_is_folder(*layer) &&
                 !leaf_participates(*layer, channel) && !live_constant &&
                 source_group_instance == nullptr)
        {
          if (!paint_layer_channel_present(*layer, channel) &&
              layer->kind == MA_PAINT_LAYER_KIND_CUSTOM &&
              custom_bake_missing_warn_once(ma, *layer))
          {
            fprintf(stderr,
                    "Paint layers: Custom layer '%s' has no bake yet and is skipped until one "
                    "lands\n",
                    layer->name);
          }
          return {};
        }

      ChainLayer current;
      current.layer = layer;
      if (Map<int, bNodeTreeInterfaceSocket *> *opacity_by_channel =
              opacity_inputs.lookup_ptr(layer))
      {
        if (bNodeTreeInterfaceSocket **opacity_iface = opacity_by_channel->lookup_ptr(channel)) {
          current.opacity_node = group_input;
          current.opacity = group_input_socket(**opacity_iface);
        }
      }

      /* The row's own channel map, when it has one: its alpha is the row's per-pixel coverage, the
       * same read #composite_image_layers_build sets up with #color_alpha_coverage. */
      bNode *leaf_map_node = nullptr;
      if (substituted) {
        bNode *baked_color_node = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
        bNode *baked_coverage_node = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
        if (baked_color_node == nullptr || baked_coverage_node == nullptr) {
          return {};
        }
        baked_color_node->id = &baked_color->id;
        id_us_plus(&baked_color->id);
        baked_color_node->location[0] = location_x;
        baked_color_node->location[1] = location_y;
        baked_coverage_node->id = &layer->bake->coverage->id;
        id_us_plus(&layer->bake->coverage->id);
        baked_coverage_node->location[0] = location_x - 90.0f;
        baked_coverage_node->location[1] = location_y - 160.0f;
        current.source_node = baked_color_node;
        current.source = socket_out(*baked_color_node, "Color");
        /* The coverage map stores the scalar in grey RGB (alpha = 1, mask-correction convention);
         * the factor is the mean of the three channels, exactly as the CPU reads it. */
        bNode *separate = bke::node_add_node(nullptr, tree, "ShaderNodeSeparateXYZ"_ustr);
        bNode *add_xy = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
        bNode *add_z = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
        bNode *divide = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
        if (separate != nullptr && add_xy != nullptr && add_z != nullptr && divide != nullptr) {
          add_xy->custom1 = NODE_MATH_ADD;
          add_z->custom1 = NODE_MATH_ADD;
          divide->custom1 = NODE_MATH_DIVIDE;
          bNodeSocket *sep_vector = socket_in(*separate, "Vector");
          bNodeSocket *sep_x = socket_out(*separate, "X");
          bNodeSocket *sep_y = socket_out(*separate, "Y");
          bNodeSocket *sep_z = socket_out(*separate, "Z");
          bNodeSocket *xy_a = socket_in(*add_xy, "Value");
          bNodeSocket *xy_b = socket_in(*add_xy, "Value_001");
          bNodeSocket *z_a = socket_in(*add_z, "Value");
          bNodeSocket *z_b = socket_in(*add_z, "Value_001");
          bNodeSocket *d_a = socket_in(*divide, "Value");
          bNodeSocket *d_b = socket_in(*divide, "Value_001");
          if (sep_vector == nullptr || sep_x == nullptr || sep_y == nullptr || sep_z == nullptr ||
              xy_a == nullptr || xy_b == nullptr || z_a == nullptr || z_b == nullptr ||
              d_a == nullptr || d_b == nullptr)
          {
            return {};
          }
          bke::node_add_link(tree,
                             *baked_coverage_node,
                             *socket_out(*baked_coverage_node, "Color"),
                             *separate,
                             *sep_vector);
          bke::node_add_link(tree, *separate, *sep_x, *add_xy, *xy_a);
          bke::node_add_link(tree, *separate, *sep_y, *add_xy, *xy_b);
          bke::node_add_link(tree, *add_xy, *socket_out(*add_xy, "Value"), *add_z, *z_a);
          bke::node_add_link(tree, *separate, *sep_z, *add_z, *z_b);
          bke::node_add_link(tree, *add_z, *socket_out(*add_z, "Value"), *divide, *d_a);
          if (d_b->default_value != nullptr) {
            static_cast<bNodeSocketValueFloat *>(d_b->default_value)->value = 3.0f;
          }
          current.opacity_node = divide;
          current.opacity = socket_out(*divide, "Value");
        }
      }
      else {
        /* A constant answers first: it needs no sampler, so a channel the resolver calls Constant
         * never falls through to an Image result. */
        Image *image = (live_constant || live_map || source_group_instance != nullptr) ?
                           nullptr :
                           paint_layer_channel_image(*layer, channel);
        if (live_constant) {
          /* The active Material row shows its source's live constant rather than its baked map: the
           * value lives in another material, so it is built into the tree directly. */
          bNode *constant = bke::node_add_static_node(nullptr, tree, SH_NODE_RGB);
          bNodeSocket *constant_out = (constant != nullptr) ? socket_out(*constant, "Color") :
                                                              nullptr;
          if (constant == nullptr || constant_out == nullptr ||
              constant_out->default_value == nullptr)
          {
            return {};
          }
          constant->location[0] = location_x;
          constant->location[1] = location_y;
          copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(constant_out->default_value)->value,
                     live_value);
          current.source_node = constant;
          current.source = constant_out;
        }
        else if (live_map) {
          /* The row shows the source's own texture. Its sampling settings travel with it; no Divide
           * is built here, exactly like the row's own map branch below -- the Image Texture node
           * handles a non-data texture's un-premultiply itself. */
          bNode *map = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
          if (map == nullptr) {
            return {};
          }
          map->id = &live_map_image->id;
          id_us_plus(&live_map_image->id);
          map->location[0] = location_x;
          map->location[1] = location_y;
          if (NodeTexImage *dst = static_cast<NodeTexImage *>(map->storage)) {
            if (live_map_iuser != nullptr) {
              dst->iuser = *live_map_iuser;
            }
            MaterialSourceResolve resolve_local;
            const MaterialSourceResolve &resolve = PaintLayersRegenCache::resolve_get(
                layer->material, cache, resolve_local);
            const bNode *src_node = resolve.images[channel].node;
            if (const NodeTexImage *src_storage =
                    (src_node != nullptr) ? static_cast<const NodeTexImage *>(src_node->storage) :
                                            nullptr)
            {
              dst->interpolation = src_storage->interpolation;
              dst->extension = src_storage->extension;
              dst->projection = src_storage->projection;
            }
          }
          current.source_node = map;
          current.source = socket_out(*map, "Color");
          leaf_map_node = map;
        }
        else if (source_group_instance != nullptr) {
          /* The whole source graph goes through the wrapper's COLOR:<CHANNEL> output. No map, so
           * content coverage for this row comes from the wrapper's COVERAGE output below. */
          if (source_group_socket == nullptr) {
            printf("paint layers: row '%s' channel %d: wrapper has no COLOR output, row dropped\n",
                   layer->name,
                   channel);
            return {};
          }
          current.source_node = source_group_instance;
          current.source = source_group_socket;
        }
        else if (folder_source != nullptr) {
          /* The folder's own row: its source is the isolated sub-chain, not a map. */
          current.source_node = folder_source_node;
          current.source = folder_source;
        }
        else if (image != nullptr) {
          bNode *map = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
          if (map == nullptr) {
            return {};
          }
          map->id = &image->id;
          id_us_plus(&image->id);
          map->location[0] = location_x;
          map->location[1] = location_y;
          current.source_node = map;
          current.source = socket_out(*map, "Color");
          leaf_map_node = map;
        }
        else {
          const MaterialPaintLayerChannel *entry = paint_layer_channel_find(*layer, channel);
          if (bNodeTreeInterfaceSocket **fill_iface = fill_inputs.lookup_ptr(entry)) {
            current.opacity_node = group_input;
            current.source_node = group_input;
            current.source = group_input_socket(**fill_iface);
          }
        }
      }
      if (current.source == nullptr) {
        return {};
      }

      /* The grey of \a image (the mean of RGB) as a new node chain, the way the CPU reads a mask
       * item and a coverage map (#PaintMaterialCompositeImageLayer::mask_reads_grey,
       * ::coverage_image): masks are painted black and white, and a brush writes colour, not alpha. */
      auto grey_of_map = [&](Image &image,
                             const float offset_y) -> std::pair<bNode *, bNodeSocket *> {
        bNode *map = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
        bNode *separate = bke::node_add_node(nullptr, tree, "ShaderNodeSeparateXYZ"_ustr);
        bNode *add_xy = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
        bNode *add_z = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
        bNode *divide = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
        if (map == nullptr || separate == nullptr || add_xy == nullptr || add_z == nullptr ||
            divide == nullptr)
        {
          return {nullptr, nullptr};
        }
        map->id = &image.id;
        id_us_plus(&image.id);
        map->location[0] = location_x - 90.0f;
        map->location[1] = location_y + offset_y;
        add_xy->custom1 = NODE_MATH_ADD;
        add_z->custom1 = NODE_MATH_ADD;
        divide->custom1 = NODE_MATH_DIVIDE;
        bNodeSocket *map_color = socket_out(*map, "Color");
        bNodeSocket *sep_vector = socket_in(*separate, "Vector");
        bNodeSocket *sep_x = socket_out(*separate, "X");
        bNodeSocket *sep_y = socket_out(*separate, "Y");
        bNodeSocket *sep_z = socket_out(*separate, "Z");
        bNodeSocket *xy_a = socket_in(*add_xy, "Value");
        bNodeSocket *xy_b = socket_in(*add_xy, "Value_001");
        bNodeSocket *z_a = socket_in(*add_z, "Value");
        bNodeSocket *z_b = socket_in(*add_z, "Value_001");
        bNodeSocket *d_a = socket_in(*divide, "Value");
        bNodeSocket *d_b = socket_in(*divide, "Value_001");
        if (map_color == nullptr || sep_vector == nullptr || sep_x == nullptr ||
            sep_y == nullptr || sep_z == nullptr || xy_a == nullptr || xy_b == nullptr ||
            z_a == nullptr || z_b == nullptr || d_a == nullptr || d_b == nullptr)
        {
          return {nullptr, nullptr};
        }
        bke::node_add_link(tree, *map, *map_color, *separate, *sep_vector);
        bke::node_add_link(tree, *separate, *sep_x, *add_xy, *xy_a);
        bke::node_add_link(tree, *separate, *sep_y, *add_xy, *xy_b);
        bke::node_add_link(tree, *add_xy, *socket_out(*add_xy, "Value"), *add_z, *z_a);
        bke::node_add_link(tree, *separate, *sep_z, *add_z, *z_b);
        bke::node_add_link(tree, *add_z, *socket_out(*add_z, "Value"), *divide, *d_a);
        if (d_b->default_value != nullptr) {
          static_cast<bNodeSocketValueFloat *>(d_b->default_value)->value = 3.0f;
        }
        return {divide, socket_out(*divide, "Value")};
      };

      /* The factor base the mask stack builds on: one, times -- for a Material layer -- its
       * source's coverage (what the source's own transparency baked into), so a mask on a
       * transparent source limits it further and never re-bakes it. */
      bNode *layer_factor_node = nullptr;
      bNodeSocket *layer_factor_socket = nullptr;
      float live_alpha[4];
      Image *live_alpha_image = nullptr;
      const ImageUser *live_alpha_iuser = nullptr;
      if (!substituted && BKE_paint_layers_material_live_constant(
                              ma, *layer, PAINT_MATERIAL_CHANNEL_ALPHA, live_alpha, cache))
      {
        /* The source's alpha is a constant too, so the coverage stays live with it. When the
         * source's alpha is not constant while its other channels are, the coverage keeps its last
         * bake -- a bounded divergence: only channels that can be shown live are. */
        bNode *value = bke::node_add_static_node(nullptr, tree, SH_NODE_VALUE);
        bNodeSocket *value_out = (value != nullptr) ? socket_out(*value, "Value") : nullptr;
        if (value != nullptr && value_out != nullptr && value_out->default_value != nullptr) {
          value->location[0] = location_x - 90.0f;
          value->location[1] = location_y - 320.0f;
          static_cast<bNodeSocketValueFloat *>(value_out->default_value)->value = live_alpha[0];
          layer_factor_node = value;
          layer_factor_socket = value_out;
        }
      }
      else if (!substituted &&
               BKE_paint_layers_material_live_image(ma,
                                                    *layer,
                                                    PAINT_MATERIAL_CHANNEL_ALPHA,
                                                    &live_alpha_image,
                                                    &live_alpha_iuser,
                                                    cache))
      {
        /* The source's alpha is a live texture: the factor is that map's Alpha output, the same
         * output the CPU reads as its coverage. */
        bNode *map = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
        bNodeSocket *map_alpha = (map != nullptr) ? socket_out(*map, "Alpha") : nullptr;
        if (map != nullptr && map_alpha != nullptr) {
          map->id = &live_alpha_image->id;
          id_us_plus(&live_alpha_image->id);
          map->location[0] = location_x - 90.0f;
          map->location[1] = location_y - 320.0f;
          if (NodeTexImage *dst = static_cast<NodeTexImage *>(map->storage)) {
            if (live_alpha_iuser != nullptr) {
              dst->iuser = *live_alpha_iuser;
            }
            MaterialSourceResolve resolve_local;
            const MaterialSourceResolve &resolve = PaintLayersRegenCache::resolve_get(
                layer->material, cache, resolve_local);
            const bNode *src_node = resolve.images[PAINT_MATERIAL_CHANNEL_ALPHA].node;
            if (const NodeTexImage *src_storage =
                    (src_node != nullptr) ? static_cast<const NodeTexImage *>(src_node->storage) :
                                            nullptr)
            {
              dst->interpolation = src_storage->interpolation;
              dst->extension = src_storage->extension;
              dst->projection = src_storage->projection;
            }
          }
          layer_factor_node = map;
          layer_factor_socket = map_alpha;
        }
      }
      else if (!substituted && source_group_instance != nullptr && source_group_tree != nullptr) {
        /* The wrapper's own COVERAGE output is the row's factor. Without one it behaves like a
         * source with no live alpha: the baked coverage stands in. */
        bNodeSocket *coverage_out = source_group_output(
            *source_group_tree, *source_group_instance, PAINT_MATERIAL_CHANNEL_ALPHA, true);
        if (coverage_out != nullptr) {
          layer_factor_node = source_group_instance;
          layer_factor_socket = coverage_out;
        }
        else if (layer->kind == MA_PAINT_LAYER_KIND_MATERIAL && layer->bake != nullptr &&
                 layer->bake->coverage != nullptr)
        {
          std::tie(layer_factor_node, layer_factor_socket) = grey_of_map(*layer->bake->coverage,
                                                                         -320.0f);
        }
      }
      else if (!substituted && layer->kind == MA_PAINT_LAYER_KIND_MATERIAL &&
               layer->bake != nullptr && layer->bake->coverage != nullptr)
      {
        std::tie(layer_factor_node, layer_factor_socket) = grey_of_map(*layer->bake->coverage,
                                                                       -320.0f);
      }
      /* The row's own content coverage, kept apart from the mask: an unpainted texel of a fresh map
       * is transparent black and must show the rows below, and the content corrections raise this
       * coverage. The mask multiplies it in afterwards, so a mask always clips a correction. */
      bNode *content_cov_node = nullptr;
      bNodeSocket *content_cov = nullptr;
      if (leaf_map_node != nullptr) {
        content_cov = socket_out(*leaf_map_node, "Alpha");
        content_cov_node = (content_cov != nullptr) ? leaf_map_node : nullptr;
      }
      /* The per-pixel coverage the mask stack builds on: the Material source coverage when there is
       * one, one otherwise. Null until an item needs it. */
      auto ensure_factor_base = [&]() {
        if (layer_factor_socket != nullptr) {
          return;
        }
        bNode *white = bke::node_add_static_node(nullptr, tree, SH_NODE_RGB);
        if (white != nullptr) {
          white->location[0] = location_x - 60.0f;
          white->location[1] = location_y - 320.0f;
          bNodeSocket *white_out = socket_out(*white, "Color");
          if (white_out != nullptr && white_out->default_value != nullptr) {
            static_cast<bNodeSocketValueRGBA *>(white_out->default_value)->value[0] = 1.0f;
            static_cast<bNodeSocketValueRGBA *>(white_out->default_value)->value[1] = 1.0f;
            static_cast<bNodeSocketValueRGBA *>(white_out->default_value)->value[2] = 1.0f;
            static_cast<bNodeSocketValueRGBA *>(white_out->default_value)->value[3] = 1.0f;
          }
          layer_factor_node = white;
          layer_factor_socket = white_out;
        }
      };

      /* Content corrections adjust the colour the row paints with, before its own blend. Only the
       * exact subset the CPU composites: a Paint correction backed by a map, or a Fill correction
       * carrying a constant. On the Normal channel a content Paint correction rides the same
       * Normal Combine the row does, while a content Fill correction (a constant normal) is left
       * out here too, so the two never disagree about which rows contributed. */
      if (!substituted) {
        const bool normal_channel = channel == PAINT_MATERIAL_CHANNEL_NORMAL;
        for (const MaterialPaintLayer *effect : BKE_paint_layers_effects(*layer)) {
          const MaterialPaintLayer &correction = *effect;
          if (correction.effect != MA_PAINT_LAYER_EFFECT_PAINT &&
              correction.effect != MA_PAINT_LAYER_EFFECT_FILL)
          {
            continue;
          }
          const bool fill = BKE_paint_layers_source_type(correction) ==
                            PaintLayerSourceType::Constant;
          /* A constant normal makes no sense; the CPU skips this correction as well. */
          if (normal_channel && fill) {
            continue;
          }
          bNodeSocket *correction_opacity = nullptr;
          if (Map<int, bNodeTreeInterfaceSocket *> *opacity_by_channel =
                  correction_opacity_inputs.lookup_ptr(&correction))
          {
            if (bNodeTreeInterfaceSocket **opacity_iface =
                    opacity_by_channel->lookup_ptr(channel))
            {
              correction_opacity = group_input_socket(**opacity_iface);
            }
          }
          bNode *correction_source = nullptr;
          bNodeSocket *correction_color = nullptr;
          bNodeSocket *correction_alpha = nullptr;
          /* The node owning the colour socket: the group input for a Fill, the map for a Paint. */
          bNode *correction_color_node = nullptr;
          /* Whether the effect's map is read as colour data. The Image Texture node only
           * un-premultiplies a non-data texture, so a data map needs the chain's own Divide (the
           * same rule the mask chain follows). */
          bool content_map_is_data = false;
          if (fill) {
            if (bNodeTreeInterfaceSocket **fill_iface =
                    correction_fill_inputs.lookup_ptr(&correction))
            {
              correction_color = group_input_socket(**fill_iface);
              correction_color_node = group_input;
            }
            if (correction_color == nullptr) {
              continue;
            }
          }
          else {
            Image *correction_image = paint_layer_channel_image(correction, channel);
            if (correction_image == nullptr) {
              continue;
            }
            /* Maps from files saved before #IMA_GPU_LINEAR_PREMUL existed, or assigned by hand,
             * get it here; its texture is rebuilt because the storage format changes. */
            if ((correction_image->flag & IMA_GPU_LINEAR_PREMUL) == 0) {
              correction_image->flag |= IMA_GPU_LINEAR_PREMUL;
              BKE_image_free_gputextures(correction_image);
            }
            content_map_is_data = IMB_colormanagement_space_name_is_data(
                correction_image->colorspace_settings.name);
            correction_source = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
            if (correction_source != nullptr) {
              correction_source->id = &correction_image->id;
              id_us_plus(&correction_image->id);
              correction_source->location[0] = location_x;
              correction_source->location[1] = location_y - 160.0f;
              correction_color = socket_out(*correction_source, "Color");
              correction_alpha = socket_out(*correction_source, "Alpha");
              correction_color_node = correction_source;
            }
            if (correction_color == nullptr) {
              continue;
            }
          }
          /* On the Normal channel the correction is the same Normal Combine the row itself uses;
           * elsewhere the row's own blend mode is a MixRGB. */
          bNode *correction_mix = nullptr;
          bNodeSocket *mix_color1 = nullptr;
          bNodeSocket *mix_color2 = nullptr;
          bNodeSocket *mix_fac = nullptr;
          bNodeSocket *mix_out = nullptr;
          if (normal_channel) {
            if (ctx.normal_combine_group == nullptr || tree.typeinfo == nullptr ||
                tree.typeinfo->group_idname == nullptr)
            {
              continue;
            }
            correction_mix = bke::node_add_node(nullptr, tree, tree.typeinfo->group_idname);
            if (correction_mix == nullptr) {
              continue;
            }
            correction_mix->id = &ctx.normal_combine_group->id;
            id_us_plus(&ctx.normal_combine_group->id);
            correction_mix->location[0] = location_x + 120.0f;
            correction_mix->location[1] = location_y;
            /* A hand-assigned group only grows its instance sockets once its declaration is built;
             * this instantiates them from the group's interface without needing #Main or a whole
             * tree update. */
            nodes::update_node_declaration_and_sockets(tree, *correction_mix);
            mix_color1 = bke::node_find_socket(
                *correction_mix, SOCK_IN, UString::from_ptr_noinline(NORMAL_COMBINE_ID_A));
            mix_color2 = bke::node_find_socket(
                *correction_mix, SOCK_IN, UString::from_ptr_noinline(NORMAL_COMBINE_ID_B));
            mix_fac = bke::node_find_socket(
                *correction_mix, SOCK_IN, UString::from_ptr_noinline(NORMAL_COMBINE_ID_FACTOR));
            mix_out = bke::node_find_socket(
                *correction_mix, SOCK_OUT, UString::from_ptr_noinline(NORMAL_COMBINE_ID_RESULT));
          }
          else {
            correction_mix = mix_node_add(tree,
                                          BKE_paint_layers_blend_to_ramp(
                                              eMaterialPaintLayerBlend(BKE_paint_layers_channel_blend_effective(correction, channel))),
                                          location_x + 120.0f,
                                          location_y);
            if (correction_mix == nullptr) {
              continue;
            }
            mix_color1 = socket_in(*correction_mix, "A_Color");
            mix_color2 = socket_in(*correction_mix, "B_Color");
            mix_fac = socket_in(*correction_mix, "Factor_Float");
            mix_out = socket_out(*correction_mix, "Result_Color");
          }
          STRNCPY_UTF8(correction_mix->label, correction.name);
          if (mix_color1 == nullptr || mix_color2 == nullptr || mix_fac == nullptr ||
              mix_out == nullptr)
          {
            /* The sockets are the node's own declaration; a missing one is a build error. */
            BLI_assert_msg(false, "blend node sockets were not declared");
            continue;
          }
          bke::node_add_link(
              tree, *current.source_node, *current.source, *correction_mix, *mix_color1);
          /* A data map reaches the chain as `C * A` (the upload pre-multiplied it and the Image
           * Texture node leaves a data texture alone); divide by the map's alpha to get `C`, so the
           * coverage is applied once, exactly as the CPU reads the straight bytes. A non-data map
           * is already straight: the node un-premultiplied it, so nothing is built. */
          if (!fill && content_map_is_data && correction_source != nullptr &&
              correction_alpha != nullptr)
          {
            bNode *combine = bke::node_add_node(nullptr, tree, "ShaderNodeCombineXYZ"_ustr);
            bNode *straighten = bke::node_add_static_node(nullptr, tree, SH_NODE_VECTOR_MATH);
            if (combine != nullptr && straighten != nullptr) {
              bNodeSocket *combine_x = socket_in(*combine, "X");
              bNodeSocket *combine_y = socket_in(*combine, "Y");
              bNodeSocket *combine_z = socket_in(*combine, "Z");
              bNodeSocket *combine_out = socket_out(*combine, "Vector");
              bNodeSocket *vector_in = socket_in(*straighten, "Vector");
              bNodeSocket *divisor_in = socket_in(*straighten, "Vector_001");
              bNodeSocket *vector_out = socket_out(*straighten, "Vector");
              if (combine_x != nullptr && combine_y != nullptr && combine_z != nullptr &&
                  combine_out != nullptr && vector_in != nullptr && divisor_in != nullptr &&
                  vector_out != nullptr)
              {
                straighten->custom1 = NODE_VECTOR_MATH_DIVIDE;
                combine->location[0] = location_x;
                combine->location[1] = location_y - 240.0f;
                straighten->location[0] = location_x + 40.0f;
                straighten->location[1] = location_y - 160.0f;
                bke::node_add_link(
                    tree, *correction_source, *correction_alpha, *combine, *combine_x);
                bke::node_add_link(
                    tree, *correction_source, *correction_alpha, *combine, *combine_y);
                bke::node_add_link(
                    tree, *correction_source, *correction_alpha, *combine, *combine_z);
                bke::node_add_link(
                    tree, *correction_source, *correction_color, *straighten, *vector_in);
                bke::node_add_link(
                    tree, *combine, *combine_out, *straighten, *divisor_in);
                correction_color_node = straighten;
                correction_color = vector_out;
              }
            }
          }
          /* The colour arrives from a group input (Fill) or a map (Paint). */
          if (correction_color_node != nullptr) {
            bke::node_add_link(
                tree, *correction_color_node, *correction_color, *correction_mix, *mix_color2);
          }
          bNodeSocket *correction_coverage_socket = nullptr;
          bNode *correction_coverage_node = nullptr;
          if (correction_opacity != nullptr && group_input != nullptr) {
            if (fill) {
              /* A flat correction's factor is its opacity; it covers fully. */
              correction_coverage_socket = correction_opacity;
              correction_coverage_node = group_input;
              bke::node_add_link(
                  tree, *group_input, *correction_opacity, *correction_mix, *mix_fac);
            }
            else {
              bNode *correction_factor = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
              if (correction_factor == nullptr || correction_alpha == nullptr) {
                continue;
              }
              correction_factor->custom1 = NODE_MATH_MULTIPLY;
              correction_factor->location[0] = location_x + 40.0f;
              correction_factor->location[1] = location_y + 160.0f;
              bNodeSocket *factor_value = socket_in(*correction_factor, "Value");
              bNodeSocket *factor_coverage = socket_in(*correction_factor, "Value_001");
              bNodeSocket *factor_out = socket_out(*correction_factor, "Value");
              if (factor_value == nullptr || factor_coverage == nullptr || factor_out == nullptr) {
                continue;
              }
              bke::node_add_link(
                  tree, *group_input, *correction_opacity, *correction_factor, *factor_value);
              bke::node_add_link(tree,
                                 *correction_source,
                                 *correction_alpha,
                                 *correction_factor,
                                 *factor_coverage);
              bke::node_add_link(
                  tree, *correction_factor, *factor_out, *correction_mix, *mix_fac);
              correction_coverage_socket = factor_out;
              correction_coverage_node = correction_factor;
            }
          }
          /* A content correction changes the colour and, by the over model, the coverage the row
           * lays with: coverage = coverage + f * (1 - coverage). The base is the row's own
           * coverage: a folder's isolated result (design §5), or a leaf's mask map. A leaf with no
           * mask map covers fully, so there the update is the identity and nothing is built. */
          if (correction_coverage_socket != nullptr && correction_coverage_node != nullptr) {
            bNode *coverage_base_node = nullptr;
            bNodeSocket *coverage_base_socket = nullptr;
            const bool folder = BKE_paint_layers_is_folder(*layer);
            if (folder) {
              coverage_base_node = folder_coverage_node;
              coverage_base_socket = folder_coverage;
            }
            else {
              /* A leaf with no map alpha covers fully, so the update is the identity and nothing
               * is built; the mask is not part of this base, it multiplies the result below. */
              coverage_base_node = content_cov_node;
              coverage_base_socket = content_cov;
            }
            if (coverage_base_node != nullptr && coverage_base_socket != nullptr) {
              bNode *one_minus = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
              bNode *scaled = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
              bNode *joined = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
              if (one_minus != nullptr && scaled != nullptr && joined != nullptr) {
                one_minus->custom1 = NODE_MATH_SUBTRACT;
                scaled->custom1 = NODE_MATH_MULTIPLY;
                joined->custom1 = NODE_MATH_ADD;
                one_minus->location[0] = location_x + 150.0f;
                one_minus->location[1] = location_y - 420.0f;
                scaled->location[0] = location_x + 230.0f;
                scaled->location[1] = location_y - 420.0f;
                joined->location[0] = location_x + 310.0f;
                joined->location[1] = location_y - 420.0f;
                bNodeSocket *om_a = socket_in(*one_minus, "Value");
                bNodeSocket *om_b = socket_in(*one_minus, "Value_001");
                bNodeSocket *sc_a = socket_in(*scaled, "Value");
                bNodeSocket *sc_b = socket_in(*scaled, "Value_001");
                bNodeSocket *jo_a = socket_in(*joined, "Value");
                bNodeSocket *jo_b = socket_in(*joined, "Value_001");
                if (om_a != nullptr && om_b != nullptr && sc_a != nullptr && sc_b != nullptr &&
                    jo_a != nullptr && jo_b != nullptr)
                {
                  if (om_a->default_value != nullptr) {
                    static_cast<bNodeSocketValueFloat *>(om_a->default_value)->value = 1.0f;
                  }
                  bke::node_add_link(
                      tree, *coverage_base_node, *coverage_base_socket, *one_minus, *om_b);
                  bke::node_add_link(
                      tree, *correction_coverage_node, *correction_coverage_socket, *scaled, *sc_a);
                  bke::node_add_link(tree,
                                     *one_minus,
                                     *socket_out(*one_minus, "Value"),
                                     *scaled,
                                     *sc_b);
                  bke::node_add_link(
                      tree, *coverage_base_node, *coverage_base_socket, *joined, *jo_a);
                  bke::node_add_link(
                      tree, *scaled, *socket_out(*scaled, "Value"), *joined, *jo_b);
                  if (folder) {
                    folder_coverage_node = joined;
                    folder_coverage = socket_out(*joined, "Value");
                  }
                  else {
                    content_cov_node = joined;
                    content_cov = socket_out(*joined, "Value");
                  }
                }
              }
            }
          }
          current.source_node = correction_mix;
          current.source = mix_out;
        }
      }

      /* The layer factor is the mask times the content coverage; either alone when the other is
       * absent. */
      if (content_cov != nullptr && content_cov_node != nullptr) {
        if (layer_factor_socket == nullptr) {
          layer_factor_node = content_cov_node;
          layer_factor_socket = content_cov;
        }
        else {
          bNode *product = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
          bNodeSocket *p_a = (product != nullptr) ? socket_in(*product, "Value") : nullptr;
          bNodeSocket *p_b = (product != nullptr) ? socket_in(*product, "Value_001") : nullptr;
          bNodeSocket *p_out = (product != nullptr) ? socket_out(*product, "Value") : nullptr;
          if (p_a != nullptr && p_b != nullptr && p_out != nullptr) {
            product->custom1 = NODE_MATH_MULTIPLY;
            product->location[0] = location_x + 350.0f;
            product->location[1] = location_y - 420.0f;
            bke::node_add_link(tree, *layer_factor_node, *layer_factor_socket, *product, *p_a);
            bke::node_add_link(tree, *content_cov_node, *content_cov, *product, *p_b);
            layer_factor_node = product;
            layer_factor_socket = p_out;
          }
        }
      }

      /* Mask items are a coverage stack over the row factor: each item lays its own coverage
       * `F = F_below * (1 - A * op) + C * op`, where `C` is the mean of the map's raw Color (a
       * Non-Color map reads un-premultiplied, so this is the pre-multiplied color) and `A` its
       * Alpha, and `op` the row's own opacity. MULTIPLY lays `F * C` instead. Later list entries
       * lay over earlier ones. Every other blend mode reads as MIX; the opacity is row-level, not
       * per channel. */
      if (current.opacity != nullptr)
      {
        const bool has_mask_corrections = !BKE_paint_layers_mask_items(*layer).is_empty();
        if (has_mask_corrections) {
          ensure_factor_base();
          bNode *factor_node = layer_factor_node;
          bNodeSocket *factor_socket = layer_factor_socket;
          for (const MaterialPaintLayer *mask_item : BKE_paint_layers_mask_items(*layer)) {
            const MaterialPaintLayer &correction = *mask_item;
            const bool fill = BKE_paint_layers_source_type(correction) ==
                              PaintLayerSourceType::Constant;
            bNodeSocket *correction_opacity = nullptr;
            if (Map<int, bNodeTreeInterfaceSocket *> *opacity_by_channel =
                    correction_opacity_inputs.lookup_ptr(&correction))
            {
              if (bNodeTreeInterfaceSocket **opacity_iface =
                      opacity_by_channel->lookup_ptr(channel))
              {
                correction_opacity = group_input_socket(**opacity_iface);
              }
            }
            /* The raw Color whose mean is the correction's color `C`: the Fill constant or the
             * map's Color output read as-is. */
            bNode *gray_source_node = nullptr;
            bNodeSocket *gray_source_color = nullptr;
            bNode *correction_map = nullptr;
            bNodeSocket *correction_alpha = nullptr;
            /* Whether the map is read as colour data. The Image Texture node only un-premultiplies
             * a premultiplied texture when its colorspace is not data (node_shader_tex_image.cc),
             * so only a data map still needs the chain's own Divide. */
            bool mask_map_is_data = false;
            if (fill) {
              if (bNodeTreeInterfaceSocket **fill_iface =
                      correction_fill_inputs.lookup_ptr(&correction))
              {
                gray_source_color = group_input_socket(**fill_iface);
                gray_source_node = group_input;
              }
              if (gray_source_color == nullptr) {
                continue;
              }
            }
            else {
              Image *correction_image = paint_layer_mask_correction_image(correction, channel);
              if (correction_image == nullptr) {
                continue;
              }
              mask_map_is_data = IMB_colormanagement_space_name_is_data(
                  correction_image->colorspace_settings.name);
              correction_map = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
              if (correction_map == nullptr) {
                continue;
              }
              correction_map->id = &correction_image->id;
              id_us_plus(&correction_image->id);
              correction_map->location[0] = location_x - 220.0f;
              correction_map->location[1] = location_y - 320.0f;
              correction_alpha = socket_out(*correction_map, "Alpha");
              gray_source_color = socket_out(*correction_map, "Color");
              gray_source_node = correction_map;
            }
            bNode *correction_gray_node = nullptr;
            bNodeSocket *correction_gray = nullptr;
            {
              bNode *separate = bke::node_add_node(nullptr, tree, "ShaderNodeSeparateXYZ"_ustr);
              bNode *add_xy = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
              bNode *add_z = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
              bNode *divide = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
              if (separate == nullptr || add_xy == nullptr || add_z == nullptr ||
                  divide == nullptr)
              {
                continue;
              }
              bNodeSocket *sep_x = socket_out(*separate, "X");
              bNodeSocket *sep_y = socket_out(*separate, "Y");
              bNodeSocket *sep_z = socket_out(*separate, "Z");
              bNodeSocket *sep_vector = socket_in(*separate, "Vector");
              add_xy->custom1 = NODE_MATH_ADD;
              add_z->custom1 = NODE_MATH_ADD;
              divide->custom1 = NODE_MATH_DIVIDE;
              bNodeSocket *xy_a = socket_in(*add_xy, "Value");
              bNodeSocket *xy_b = socket_in(*add_xy, "Value_001");
              bNodeSocket *z_a = socket_in(*add_z, "Value");
              bNodeSocket *z_b = socket_in(*add_z, "Value_001");
              bNodeSocket *d_a = socket_in(*divide, "Value");
              bNodeSocket *d_b = socket_in(*divide, "Value_001");
              if (sep_x == nullptr || sep_y == nullptr || sep_z == nullptr ||
                  sep_vector == nullptr || xy_a == nullptr || xy_b == nullptr ||
                  z_a == nullptr || z_b == nullptr || d_a == nullptr || d_b == nullptr)
              {
                continue;
              }
              bke::node_add_link(
                  tree, *gray_source_node, *gray_source_color, *separate, *sep_vector);
              bke::node_add_link(tree, *separate, *sep_x, *add_xy, *xy_a);
              bke::node_add_link(tree, *separate, *sep_y, *add_xy, *xy_b);
              bke::node_add_link(tree, *add_xy, *socket_out(*add_xy, "Value"), *add_z, *z_a);
              bke::node_add_link(tree, *separate, *sep_z, *add_z, *z_b);
              bke::node_add_link(tree, *add_z, *socket_out(*add_z, "Value"), *divide, *d_a);
              if (d_b->default_value != nullptr) {
                static_cast<bNodeSocketValueFloat *>(d_b->default_value)->value = 3.0f;
              }
              correction_gray_node = divide;
              correction_gray = socket_out(*divide, "Value");
            }
            bNode *correction_mix = mix_node_add(
                tree, MA_RAMP_BLEND, location_x + 60.0f, location_y - 320.0f);
            if (correction_mix == nullptr || correction_gray == nullptr ||
                correction_gray_node == nullptr || factor_socket == nullptr)
            {
              continue;
            }
            bNodeSocket *mix_color1 = socket_in(*correction_mix, "A_Color");
            bNodeSocket *mix_color2 = socket_in(*correction_mix, "B_Color");
            bNodeSocket *mix_fac = socket_in(*correction_mix, "Factor_Float");
            bNodeSocket *mix_out = socket_out(*correction_mix, "Result_Color");
            if (mix_color1 == nullptr || mix_color2 == nullptr || mix_fac == nullptr ||
                mix_out == nullptr)
            {
              continue;
            }
            bke::node_add_link(tree, *factor_node, *factor_socket, *correction_mix, *mix_color1);
            /* B is what the factor mixes towards: the Fill constant or the map's straightened
             * grey. MULTIPLY mixes towards `F * C` instead, so the item darkens the factor by its
             * grey rather than replacing it with the grey; every other mode reads as MIX. */
            bNode *b_node = correction_gray_node;
            bNodeSocket *b_socket = correction_gray;
            if (!fill) {
              if (correction_alpha == nullptr || correction_map == nullptr) {
                continue;
              }
              if (mask_map_is_data) {
                /* The map is stored straight, but the texture upload pre-multiplied its bytes by A,
                 * so the sampled grey already carries A once; mixing it by `A * op` would apply A
                 * twice. The Image Texture node leaves a data texture pre-multiplied, so the chain
                 * straightens it: `mix(F, C / A, A * op)` is `F * (1 - A * op) + C * A * op`. Where
                 * A is 0 the factor is 0 too, and Math Divide yields 0 there. */
                bNode *straighten = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
                if (straighten == nullptr) {
                  continue;
                }
                straighten->custom1 = NODE_MATH_DIVIDE;
                straighten->location[0] = location_x - 20.0f;
                straighten->location[1] = location_y - 380.0f;
                bNodeSocket *straighten_value = socket_in(*straighten, "Value");
                bNodeSocket *straighten_alpha = socket_in(*straighten, "Value_001");
                bNodeSocket *straighten_out = socket_out(*straighten, "Value");
                if (straighten_value == nullptr || straighten_alpha == nullptr ||
                    straighten_out == nullptr)
                {
                  continue;
                }
                bke::node_add_link(
                    tree, *correction_gray_node, *correction_gray, *straighten, *straighten_value);
                bke::node_add_link(
                    tree, *correction_map, *correction_alpha, *straighten, *straighten_alpha);
                b_node = straighten;
                b_socket = straighten_out;
              }
              /* A non-data map is already straight: the Image Texture node un-premultiplied it,
               * because the chain also reads the Alpha output, so no Divide is built. */
            }
            if (correction.blend == MA_PAINT_LAYER_BLEND_MULTIPLY) {
              bNode *multiply = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
              if (multiply == nullptr) {
                continue;
              }
              multiply->custom1 = NODE_MATH_MULTIPLY;
              multiply->location[0] = location_x;
              multiply->location[1] = location_y - 420.0f;
              bNodeSocket *mul_a = socket_in(*multiply, "Value");
              bNodeSocket *mul_b = socket_in(*multiply, "Value_001");
              bNodeSocket *mul_out = socket_out(*multiply, "Value");
              if (mul_a == nullptr || mul_b == nullptr || mul_out == nullptr) {
                continue;
              }
              bke::node_add_link(tree, *factor_node, *factor_socket, *multiply, *mul_a);
              bke::node_add_link(tree, *b_node, *b_socket, *multiply, *mul_b);
              b_node = multiply;
              b_socket = mul_out;
            }
            bke::node_add_link(tree, *b_node, *b_socket, *correction_mix, *mix_color2);
            /* Fac is `A * op`: the map alpha times the correction row's own opacity input. The
             * per-channel sockets all carry the same row value, so the current channel's is
             * enough. A Fill covers fully, so its factor is the opacity alone. */
            if (correction_opacity == nullptr || group_input == nullptr) {
              continue;
            }
            if (fill) {
              bke::node_add_link(
                  tree, *group_input, *correction_opacity, *correction_mix, *mix_fac);
            }
            else {
              if (correction_alpha == nullptr || correction_map == nullptr) {
                continue;
              }
              bNode *fac_multiply = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
              if (fac_multiply == nullptr) {
                continue;
              }
              fac_multiply->custom1 = NODE_MATH_MULTIPLY;
              bNodeSocket *fac_value = socket_in(*fac_multiply, "Value");
              bNodeSocket *fac_coverage = socket_in(*fac_multiply, "Value_001");
              bNodeSocket *fac_out = socket_out(*fac_multiply, "Value");
              if (fac_value == nullptr || fac_coverage == nullptr || fac_out == nullptr) {
                continue;
              }
              bke::node_add_link(
                  tree, *group_input, *correction_opacity, *fac_multiply, *fac_value);
              bke::node_add_link(
                  tree, *correction_map, *correction_alpha, *fac_multiply, *fac_coverage);
              bke::node_add_link(tree, *fac_multiply, *fac_out, *correction_mix, *mix_fac);
            }
            layer_factor_node = correction_mix;
            layer_factor_socket = mix_out;
            /* The next item lays over this one: its base is this item's result. */
            factor_node = correction_mix;
            factor_socket = mix_out;
          }
        }
      }

      /* The factor is the correction/mask chain times the row's own opacity, or the opacity alone
       * when there is neither a mask map nor a correction. */
      if (layer_factor_socket != nullptr && current.opacity != nullptr &&
          current.opacity_node != nullptr)
      {
        bNode *opacity_multiply = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
        if (opacity_multiply != nullptr) {
          opacity_multiply->custom1 = NODE_MATH_MULTIPLY;
          opacity_multiply->location[0] = location_x + 200.0f;
          opacity_multiply->location[1] = location_y - 320.0f;
          bNodeSocket *value_a = socket_in(*opacity_multiply, "Value");
          bNodeSocket *value_b = socket_in(*opacity_multiply, "Value_001");
          bNodeSocket *value_out = socket_out(*opacity_multiply, "Value");
          if (value_a != nullptr && value_b != nullptr && value_out != nullptr) {
            bke::node_add_link(
                tree, *current.opacity_node, *current.opacity, *opacity_multiply, *value_a);
            bke::node_add_link(
                tree, *layer_factor_node, *layer_factor_socket, *opacity_multiply, *value_b);
            current.factor_node = opacity_multiply;
            current.factor = value_out;
          }
        }
      }
      else {
        current.factor_node = current.opacity_node;
        current.factor = current.opacity;
      }

      /* A folder's row blends by its own factor times the coverage its contents accumulated. */
      if (folder_coverage_node != nullptr && folder_coverage != nullptr) {
        bNode *coverage_multiply = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
        if (coverage_multiply != nullptr) {
          coverage_multiply->custom1 = NODE_MATH_MULTIPLY;
          coverage_multiply->location[0] = location_x + 150.0f;
          coverage_multiply->location[1] = location_y - 400.0f;
          bNodeSocket *cv_a = socket_in(*coverage_multiply, "Value");
          bNodeSocket *cv_b = socket_in(*coverage_multiply, "Value_001");
          bNodeSocket *cv_out = socket_out(*coverage_multiply, "Value");
          if (cv_a != nullptr && cv_b != nullptr && cv_out != nullptr) {
            if (current.factor != nullptr && current.factor_node != nullptr) {
              bke::node_add_link(
                  tree, *current.factor_node, *current.factor, *coverage_multiply, *cv_a);
            }
            else if (cv_a->default_value != nullptr) {
              static_cast<bNodeSocketValueFloat *>(cv_a->default_value)->value = 1.0f;
            }
            bke::node_add_link(
                tree, *folder_coverage_node, *folder_coverage, *coverage_multiply, *cv_b);
            current.factor_node = coverage_multiply;
            current.factor = cv_out;
          }
        }
        }

        RowResult result;

        /* A leaf in its own group: expose the row's colour, coverage and the two blends the parent
         * may chain through. Below is the group's blend base; Color and Coverage are its straight
         * result; Blend is the row blended at factor one (only the parent's premul chain uses it);
         * Result is the row laid over Below. */
        if (layer_group != nullptr && current.source != nullptr) {
          const MaterialPaintChannelInfo &channel_info = BKE_paint_material_channel_info(
              eMaterialPaintChannel(channel));
          auto add_group_socket = [&](const char *kind,
                                      const StringRef type,
                                      const NodeTreeInterfaceSocketFlag flag)
              -> bNodeTreeInterfaceSocket * {
            char base[192];
            SNPRINTF(base, "%s %s", kind, channel_info.ui_name);
            return layer_group_add_socket(*layer_group, base, type, flag);
          };
          bNodeTreeInterfaceSocket *below_iface = add_group_socket(
              "Below", "NodeSocketColor", NODE_INTERFACE_SOCKET_INPUT);
          bNodeTreeInterfaceSocket *color_iface = add_group_socket(
              "Color", "NodeSocketColor", NODE_INTERFACE_SOCKET_OUTPUT);
          bNodeTreeInterfaceSocket *coverage_iface = add_group_socket(
              "Coverage", "NodeSocketFloat", NODE_INTERFACE_SOCKET_OUTPUT);
          bNodeTreeInterfaceSocket *blend_iface = add_group_socket(
              "Blend", "NodeSocketColor", NODE_INTERFACE_SOCKET_OUTPUT);
          bNodeTreeInterfaceSocket *result_iface = add_group_socket(
              "Result", "NodeSocketColor", NODE_INTERFACE_SOCKET_OUTPUT);
          if (below_iface == nullptr || color_iface == nullptr || coverage_iface == nullptr ||
              blend_iface == nullptr || result_iface == nullptr)
          {
            return {};
          }
          bNodeSocket *below = bke::node_find_socket(
              *group_input, SOCK_OUT, UString::from_ptr_noinline(below_iface->identifier));
          bNodeSocket *color_out = bke::node_find_socket(
              *layer_group->group_output,
              SOCK_IN,
              UString::from_ptr_noinline(color_iface->identifier));
          bNodeSocket *coverage_out = bke::node_find_socket(
              *layer_group->group_output,
              SOCK_IN,
              UString::from_ptr_noinline(coverage_iface->identifier));
          bNodeSocket *blend_out = bke::node_find_socket(
              *layer_group->group_output,
              SOCK_IN,
              UString::from_ptr_noinline(blend_iface->identifier));
          bNodeSocket *result_out = bke::node_find_socket(
              *layer_group->group_output,
              SOCK_IN,
              UString::from_ptr_noinline(result_iface->identifier));
          if (below == nullptr || color_out == nullptr || coverage_out == nullptr ||
              blend_out == nullptr || result_out == nullptr)
          {
            return {};
          }
          bke::node_add_link(tree, *current.source_node, *current.source,
                             *layer_group->group_output, *color_out);
          if (current.factor != nullptr && current.factor_node != nullptr) {
            bke::node_add_link(tree, *current.factor_node, *current.factor,
                               *layer_group->group_output, *coverage_out);
          }
          else if (coverage_out->default_value != nullptr) {
            static_cast<bNodeSocketValueFloat *>(coverage_out->default_value)->value = 1.0f;
          }

          /* blend(Below, Color) at factor one; the parent feeds Below from its own chain. */
          auto add_row_blend = [&](bNodeSocket *factor,
                                   const float factor_default)
              -> std::pair<bNode *, bNodeSocket *> {
            if (channel == PAINT_MATERIAL_CHANNEL_NORMAL && ctx.normal_combine_group != nullptr &&
                tree.typeinfo != nullptr && tree.typeinfo->group_idname != nullptr)
            {
              bNode *combine = bke::node_add_node(nullptr, tree, tree.typeinfo->group_idname);
              if (combine == nullptr) {
                return {nullptr, nullptr};
              }
              combine->id = &ctx.normal_combine_group->id;
              id_us_plus(&ctx.normal_combine_group->id);
              combine->location[0] = location_x;
              combine->location[1] = location_y;
              nodes::update_node_declaration_and_sockets(tree, *combine);
              bNodeSocket *a_in = bke::node_find_socket(
                  *combine, SOCK_IN, UString::from_ptr_noinline(NORMAL_COMBINE_ID_A));
              bNodeSocket *b_in = bke::node_find_socket(
                  *combine, SOCK_IN, UString::from_ptr_noinline(NORMAL_COMBINE_ID_B));
              bNodeSocket *f_in = bke::node_find_socket(
                  *combine, SOCK_IN, UString::from_ptr_noinline(NORMAL_COMBINE_ID_FACTOR));
              bNodeSocket *r_out = bke::node_find_socket(
                  *combine, SOCK_OUT, UString::from_ptr_noinline(NORMAL_COMBINE_ID_RESULT));
              if (a_in == nullptr || b_in == nullptr || f_in == nullptr || r_out == nullptr) {
                return {nullptr, nullptr};
              }
              bke::node_add_link(tree, *group_input, *below, *combine, *a_in);
              bke::node_add_link(
                  tree, *current.source_node, *current.source, *combine, *b_in);
              if (factor != nullptr) {
                bke::node_add_link(tree, *current.factor_node, *factor, *combine, *f_in);
              }
              else if (f_in->default_value != nullptr) {
                static_cast<bNodeSocketValueFloat *>(f_in->default_value)->value = factor_default;
              }
              return {combine, r_out};
            }
            bNode *mix = mix_node_add(tree,
                                      BKE_paint_layers_blend_to_ramp(
                                          eMaterialPaintLayerBlend(BKE_paint_layers_channel_blend_effective(*layer, channel))),
                                      location_x,
                                      location_y);
            if (mix == nullptr) {
              return {nullptr, nullptr};
            }
            bNodeSocket *a_in = socket_in(*mix, "A_Color");
            bNodeSocket *b_in = socket_in(*mix, "B_Color");
            bNodeSocket *f_in = socket_in(*mix, "Factor_Float");
            bNodeSocket *r_out = socket_out(*mix, "Result_Color");
            if (a_in == nullptr || b_in == nullptr || f_in == nullptr || r_out == nullptr) {
              return {nullptr, nullptr};
            }
            bke::node_add_link(tree, *group_input, *below, *mix, *a_in);
            bke::node_add_link(tree, *current.source_node, *current.source, *mix, *b_in);
            if (factor != nullptr) {
              bke::node_add_link(tree, *current.factor_node, *factor, *mix, *f_in);
            }
            else if (f_in->default_value != nullptr) {
              static_cast<bNodeSocketValueFloat *>(f_in->default_value)->value = factor_default;
            }
            return {mix, r_out};
          };

          auto [result_node, result_source] = add_row_blend(current.factor, 1.0f);
          if (result_source == nullptr) {
            return {};
          }
          bke::node_add_link(
              tree, *result_node, *result_source, *layer_group->group_output, *result_out);
          if (premul) {
            auto [blend_node, blend_source] = add_row_blend(nullptr, 1.0f);
            if (blend_source != nullptr) {
              bke::node_add_link(
                  tree, *blend_node, *blend_source, *layer_group->group_output, *blend_out);
            }
          }

          result.grouped = true;
          result.group_instance = layer_group->instance;
          result.group_below = bke::node_find_socket(
              *layer_group->instance, SOCK_IN, UString::from_ptr_noinline(below_iface->identifier));
          result.group_color = bke::node_find_socket(
              *layer_group->instance, SOCK_OUT, UString::from_ptr_noinline(color_iface->identifier));
          result.group_coverage = bke::node_find_socket(
              *layer_group->instance,
              SOCK_OUT,
              UString::from_ptr_noinline(coverage_iface->identifier));
          result.group_blend = bke::node_find_socket(
              *layer_group->instance, SOCK_OUT, UString::from_ptr_noinline(blend_iface->identifier));
          result.group_result = bke::node_find_socket(
              *layer_group->instance, SOCK_OUT, UString::from_ptr_noinline(result_iface->identifier));
        }

        result.valid = true;
        result.current = current;
        result.folder_coverage_node = folder_coverage_node;
        result.folder_coverage = folder_coverage;
        return result;
      };
      for (const MaterialPaintLayer &layer_ref :
           *reinterpret_cast<const ListBaseT<MaterialPaintLayer> *>(&list))
      {
        const MaterialPaintLayer *layer = &layer_ref;
        if (BKE_paint_layers_folder_is_pass_through(ma, *layer))
        {
          if (row_is_removed(ma, *layer)) {
            /* The over-budget pass dropped this hidden folder; its inlined subtree goes with it. */
            continue;
          }
          /* A Pass Through folder is expanded in place: its children are built straight into the
           * parent's chain with the parent's own premul mode, exactly as if the folder were not
           * there. This is what keeps the generated shader identical when a row is moved into or
           * out of such a folder.
           *
           * Why inlining and not an isolated sub-chain: EEVEE compiles a material as one shader, so
           * any structural difference in the generated graph recompiles it; with a live Material row
           * carrying a large source graph that is seconds of stall. The isolated folder always edits
           * that structure (its own group instance, the P/a divide, the coverage overlay), while the
           * inlined form does not, so EEVEE can take the unchanged pass from its cache.
           *
           * Why it is correct: over is associative under Normal/Mix at full opacity with no mask,
           * correction or bake, so `over(below, P/a, a)` of the isolated folder equals laying the
           * children over `below` one after another. The predicate rejects every case where that
           * fails (opacity below one with several children in particular); a hidden Pass Through
           * folder scales each descendant's factor to zero through its value input, which is exact
           * and stays a value edit.
           *
           * What it costs and what to check when refactoring: switching a folder's mode is one
           * rebuild and one EEVEE compile, which is acceptable. The win depends on the build
           * producing byte-identical node and link order before and after a move; if it stops doing
           * so the compile returns silently. The CPU compositor and #BKE_paint_layers_bake_render_node
           * must keep giving the GPU's result -- see `composite_image_layers_build`, which flattens
           * the same folder. Alternatives considered and rejected: reserving correction slots (does
           * not avoid the code change) and an always-present opacity input (complicates every chain
           * for this rare case). */
          ChainResult sub = build_list(layer->children, previous, premul, parent_target);
          previous = sub.chain;
          coverage_node = sub.coverage_node;
          coverage = sub.coverage;
          continue;
        }
        Image *baked_color = nullptr;
        const bool substituted = row_channel_substituted(ma, *layer, channel, &baked_color);
        const bool is_folder = BKE_paint_layers_is_folder(*layer);

        RowTarget row_target = parent_target;
        row_target.location_x = location_x;
        row_target.location_y = location_y;
        LayerGroup *layer_group = nullptr;
        /* Every row gets its own group: a bake-substituted row, a folder that takes part, or a leaf
         * that paints something here. The same helper feeds the root hash, so the root and the build
         * never disagree about which instances exist. */
        const bool group_this = layer_row_has_group(ma, *layer, channel, cache);
        if (group_this) {
          layer_group = layer_group_ensure(*layer, *parent_target.tree);
        }
        RowResult row;
        if (layer_group != nullptr && layer_group->unchanged) {
          /* The group's topology is current: keep its nodes and interface and chain the existing
           * instance. */
          row = row_from_unchanged_group(*layer_group, *layer, channel);
        }
        else {
          if (layer_group != nullptr) {
            row_target.tree = layer_group->tree;
            row_target.group_input = layer_group->group_input;
          }
          row = build_row(layer, row_target, substituted, baked_color, premul, layer_group);
        }
        if (!row.valid) {
          continue;
        }
        ChainLayer current = row.current;
        bNode *folder_coverage_node = row.folder_coverage_node;
        bNodeSocket *folder_coverage = row.folder_coverage;
        if (row.grouped) {
          /* The parent chains the group through its instance: Color and Coverage are the row's
           * straight result, while Below/Blend/Result are the instance's own sockets. */
          current.source_node = row.group_instance;
          current.source = row.group_color;
          current.factor_node = row.group_instance;
          current.factor = row.group_coverage;
        }

      if (premul) {
        /* The isolated-group accumulation (design §5): S = P/a, c_eff = lerp(c, blend(S, c), a),
         * P = P(1-f) + c_eff*f, a = a(1-f) + f. */
        bNode *s_divide = bke::node_add_node(nullptr, tree, "ShaderNodeVectorMath"_ustr);
        bNodeSocket *sd_a = (s_divide != nullptr) ? socket_in(*s_divide, "Vector") : nullptr;
        bNodeSocket *sd_b = (s_divide != nullptr) ? socket_in(*s_divide, "Vector_001") : nullptr;
        bNodeSocket *sd_out = (s_divide != nullptr) ? socket_out(*s_divide, "Vector") : nullptr;
        if (sd_out == nullptr || coverage_node == nullptr || coverage == nullptr) {
          continue;
        }
        s_divide->custom1 = NODE_VECTOR_MATH_DIVIDE;
        s_divide->location[0] = location_x;
        s_divide->location[1] = location_y + 120.0f;
        bke::node_add_link(tree, *previous.source_node, *previous.source, *s_divide, *sd_a);
        bke::node_add_link(tree, *coverage_node, *coverage, *s_divide, *sd_b);

        /* blend_m(S, c) at full factor. */
        bNode *row_blend = nullptr;
        bNodeSocket *row_blend_out = nullptr;
        if (row.grouped) {
          /* S = P/a feeds the group's Below; its Blend output is blend_m(S, Color) at factor one. */
          if (row.group_below != nullptr) {
            bke::node_add_link(tree, *s_divide, *sd_out, *row.group_instance, *row.group_below);
          }
          row_blend = row.group_instance;
          row_blend_out = row.group_blend;
        }
        else if (channel == PAINT_MATERIAL_CHANNEL_NORMAL && ctx.normal_combine_group != nullptr &&
            tree.typeinfo != nullptr && tree.typeinfo->group_idname != nullptr)
        {
          bNode *combine = bke::node_add_node(nullptr, tree, tree.typeinfo->group_idname);
          if (combine != nullptr) {
            combine->id = &ctx.normal_combine_group->id;
            id_us_plus(&ctx.normal_combine_group->id);
            combine->location[0] = location_x;
            combine->location[1] = location_y - 120.0f;
            nodes::update_node_declaration_and_sockets(tree, *combine);
            bNodeSocket *a_in = bke::node_find_socket(
                *combine, SOCK_IN, UString::from_ptr_noinline(NORMAL_COMBINE_ID_A));
            bNodeSocket *b_in = bke::node_find_socket(
                *combine, SOCK_IN, UString::from_ptr_noinline(NORMAL_COMBINE_ID_B));
            bNodeSocket *f_in = bke::node_find_socket(
                *combine, SOCK_IN, UString::from_ptr_noinline(NORMAL_COMBINE_ID_FACTOR));
            bNodeSocket *r_out = bke::node_find_socket(
                *combine, SOCK_OUT, UString::from_ptr_noinline(NORMAL_COMBINE_ID_RESULT));
            if (a_in != nullptr && b_in != nullptr && f_in != nullptr && r_out != nullptr) {
              if (f_in->default_value != nullptr) {
                static_cast<bNodeSocketValueFloat *>(f_in->default_value)->value = 1.0f;
              }
              bke::node_add_link(tree, *s_divide, *sd_out, *combine, *a_in);
              bke::node_add_link(tree, *current.source_node, *current.source, *combine, *b_in);
              row_blend = combine;
              row_blend_out = r_out;
            }
          }
        }
        else {
          bNode *mix = mix_node_add(tree,
                                    BKE_paint_layers_blend_to_ramp(
                                        eMaterialPaintLayerBlend(BKE_paint_layers_channel_blend_effective(*current.layer, channel))),
                                    location_x,
                                    location_y - 120.0f);
          if (mix != nullptr) {
            bNodeSocket *fac = socket_in(*mix, "Factor_Float");
            bNodeSocket *color1 = socket_in(*mix, "A_Color");
            bNodeSocket *color2 = socket_in(*mix, "B_Color");
            bNodeSocket *color_out = socket_out(*mix, "Result_Color");
            if (fac != nullptr && color1 != nullptr && color2 != nullptr && color_out != nullptr) {
              if (fac->default_value != nullptr) {
                static_cast<bNodeSocketValueFloat *>(fac->default_value)->value = 1.0f;
              }
              bke::node_add_link(tree, *s_divide, *sd_out, *mix, *color1);
              bke::node_add_link(tree, *current.source_node, *current.source, *mix, *color2);
              row_blend = mix;
              row_blend_out = color_out;
            }
          }
        }
        if (row_blend == nullptr || row_blend_out == nullptr) {
          continue;
        }

        /* c_eff = lerp(c, blend, a). */
        bNode *c_eff = mix_node_add(tree, MA_RAMP_BLEND, location_x + 120.0f, location_y - 240.0f);
        bNodeSocket *eff_a = (c_eff != nullptr) ? socket_in(*c_eff, "A_Color") : nullptr;
        bNodeSocket *eff_b = (c_eff != nullptr) ? socket_in(*c_eff, "B_Color") : nullptr;
        bNodeSocket *eff_f = (c_eff != nullptr) ? socket_in(*c_eff, "Factor_Float") : nullptr;
        bNodeSocket *eff_out = (c_eff != nullptr) ? socket_out(*c_eff, "Result_Color") : nullptr;
        if (eff_out == nullptr) {
          continue;
        }
        bke::node_add_link(tree, *current.source_node, *current.source, *c_eff, *eff_a);
        bke::node_add_link(tree, *row_blend, *row_blend_out, *c_eff, *eff_b);
        bke::node_add_link(tree, *coverage_node, *coverage, *c_eff, *eff_f);

        /* P = lerp(P, c_eff, f). */
        bNode *p_mix = mix_node_add(tree, MA_RAMP_BLEND, location_x + 240.0f, location_y);
        bNodeSocket *pm_a = (p_mix != nullptr) ? socket_in(*p_mix, "A_Color") : nullptr;
        bNodeSocket *pm_b = (p_mix != nullptr) ? socket_in(*p_mix, "B_Color") : nullptr;
        bNodeSocket *pm_f = (p_mix != nullptr) ? socket_in(*p_mix, "Factor_Float") : nullptr;
        bNodeSocket *pm_out = (p_mix != nullptr) ? socket_out(*p_mix, "Result_Color") : nullptr;
        if (pm_out == nullptr) {
          continue;
        }
        bke::node_add_link(tree, *previous.source_node, *previous.source, *p_mix, *pm_a);
        bke::node_add_link(tree, *c_eff, *eff_out, *p_mix, *pm_b);
        if (current.factor != nullptr && current.factor_node != nullptr) {
          bke::node_add_link(tree, *current.factor_node, *current.factor, *p_mix, *pm_f);
        }
        previous = {current.layer, p_mix, pm_out, nullptr, nullptr, nullptr, nullptr};

        /* a = a + f*(1-a). */
        bNode *one_minus = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
        bNode *scaled = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
        bNode *joined = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
        bNodeSocket *om_a = (one_minus != nullptr) ? socket_in(*one_minus, "Value") : nullptr;
        bNodeSocket *om_b = (one_minus != nullptr) ? socket_in(*one_minus, "Value_001") : nullptr;
        bNodeSocket *sc_a = (scaled != nullptr) ? socket_in(*scaled, "Value") : nullptr;
        bNodeSocket *sc_b = (scaled != nullptr) ? socket_in(*scaled, "Value_001") : nullptr;
        bNodeSocket *jo_a = (joined != nullptr) ? socket_in(*joined, "Value") : nullptr;
        bNodeSocket *jo_b = (joined != nullptr) ? socket_in(*joined, "Value_001") : nullptr;
        if (om_b == nullptr || sc_a == nullptr || sc_b == nullptr || jo_a == nullptr ||
            jo_b == nullptr)
        {
          continue;
        }
        one_minus->custom1 = NODE_MATH_SUBTRACT;
        scaled->custom1 = NODE_MATH_MULTIPLY;
        joined->custom1 = NODE_MATH_ADD;
        if (om_a != nullptr && om_a->default_value != nullptr) {
          static_cast<bNodeSocketValueFloat *>(om_a->default_value)->value = 1.0f;
        }
        bke::node_add_link(tree, *coverage_node, *coverage, *one_minus, *om_b);
        if (current.factor != nullptr && current.factor_node != nullptr) {
          bke::node_add_link(tree, *current.factor_node, *current.factor, *scaled, *sc_a);
        }
        else if (sc_a->default_value != nullptr) {
          static_cast<bNodeSocketValueFloat *>(sc_a->default_value)->value = 1.0f;
        }
        bke::node_add_link(tree, *one_minus, *socket_out(*one_minus, "Value"), *scaled, *sc_b);
        bke::node_add_link(tree, *coverage_node, *coverage, *joined, *jo_a);
        bke::node_add_link(tree, *scaled, *socket_out(*scaled, "Value"), *joined, *jo_b);
        coverage_node = joined;
        coverage = socket_out(*joined, "Value");
      }
      else if (row.grouped) {
        /* The group's Result already blends the row over Below; the parent only feeds Below. */
        if (previous.source != nullptr && previous.source_node != nullptr &&
            row.group_below != nullptr)
        {
          bke::node_add_link(tree,
                             *previous.source_node,
                             *previous.source,
                             *row.group_instance,
                             *row.group_below);
        }
        previous = {
            current.layer, row.group_instance, row.group_result, nullptr, nullptr, nullptr, nullptr};
      }
      else {
        bool combined = false;
        if (channel == PAINT_MATERIAL_CHANNEL_NORMAL && ctx.normal_combine_group != nullptr &&
            tree.typeinfo != nullptr && tree.typeinfo->group_idname != nullptr)
        {
          bNode *combine = bke::node_add_node(nullptr, tree, tree.typeinfo->group_idname);
          if (combine != nullptr) {
            combine->id = &ctx.normal_combine_group->id;
            id_us_plus(&ctx.normal_combine_group->id);
            combine->location[0] = location_x;
            combine->location[1] = location_y;
            /* A hand-assigned group only grows its instance sockets once its declaration is built;
             * this instantiates them from the group's interface without needing #Main or a whole
             * tree update. */
            nodes::update_node_declaration_and_sockets(tree, *combine);
            bNodeSocket *socket_a = bke::node_find_socket(
                *combine, SOCK_IN, UString::from_ptr_noinline(NORMAL_COMBINE_ID_A));
            bNodeSocket *socket_b = bke::node_find_socket(
                *combine, SOCK_IN, UString::from_ptr_noinline(NORMAL_COMBINE_ID_B));
            bNodeSocket *factor = bke::node_find_socket(
                *combine, SOCK_IN, UString::from_ptr_noinline(NORMAL_COMBINE_ID_FACTOR));
            bNodeSocket *result = bke::node_find_socket(
                *combine, SOCK_OUT, UString::from_ptr_noinline(NORMAL_COMBINE_ID_RESULT));
            if (socket_a == nullptr || socket_b == nullptr || factor == nullptr ||
                result == nullptr)
            {
              /* The sockets are the group's own interface; a missing one is a build error, not a
               * shape to paper over with a Mix that would silently flatten the relief. */
              BLI_assert_msg(false, "Normal Combine instance sockets were not instantiated");
              continue;
            }
            bke::node_add_link(
                tree, *previous.source_node, *previous.source, *combine, *socket_a);
            bke::node_add_link(
                tree, *current.source_node, *current.source, *combine, *socket_b);
            if (current.factor != nullptr && current.factor_node != nullptr) {
              bke::node_add_link(tree, *current.factor_node, *current.factor, *combine, *factor);
            }
            previous = {current.layer, combine, result, nullptr, nullptr, nullptr, nullptr};
            combined = true;
          }
        }
        if (!combined) {
          bNode *mix = mix_node_add(tree,
                                    BKE_paint_layers_blend_to_ramp(
                                        eMaterialPaintLayerBlend(BKE_paint_layers_channel_blend_effective(*current.layer, channel))),
                                    location_x,
                                    location_y);
          if (mix != nullptr) {
            STRNCPY_UTF8(mix->label, current.layer->name);
            bNodeSocket *fac = socket_in(*mix, "Factor_Float");
            bNodeSocket *color1 = socket_in(*mix, "A_Color");
            bNodeSocket *color2 = socket_in(*mix, "B_Color");
            bNodeSocket *color_out = socket_out(*mix, "Result_Color");
            if (fac != nullptr && color1 != nullptr && color2 != nullptr && color_out != nullptr) {
              bke::node_add_link(tree, *previous.source_node, *previous.source, *mix, *color1);
              bke::node_add_link(tree, *current.source_node, *current.source, *mix, *color2);
              if (current.factor != nullptr && current.factor_node != nullptr) {
                bke::node_add_link(tree, *current.factor_node, *current.factor, *mix, *fac);
              }
              previous = {current.layer, mix, color_out, nullptr, nullptr, nullptr, nullptr};
            }
          }
        }
      }
      location_x += 180.0f;
      }
      return {previous, coverage_node, coverage};
    };
    ChainResult built = build_list(ma.paint_layers, previous, false, target);
    previous = built.chain;

    /* A partial factor interpolates the combine's encoded output against the base, which shortens
     * the tangent-space vector; one final decode-normalize-encode restores a unit normal, matching
     * what the shader's Normal Map node hands the BSDF and what a CPU export has to write. */
    if (channel == PAINT_MATERIAL_CHANNEL_NORMAL && previous.source != nullptr) {
      auto add_vector_math = [&](const int operation, const float scale, const float offset) {
        bNode *node = bke::node_add_node(nullptr, tree, "ShaderNodeVectorMath"_ustr);
        if (node == nullptr) {
          return node;
        }
        node->custom1 = operation;
        if (operation == NODE_VECTOR_MATH_MULTIPLY_ADD) {
          for (const char *name : {"Vector_001", "Vector_002"}) {
            bNodeSocket *socket = socket_in(*node, name);
            if (socket == nullptr || socket->default_value == nullptr) {
              continue;
            }
            const float value = (STREQ(name, "Vector_001")) ? scale : offset;
            copy_v3_fl(static_cast<bNodeSocketValueVector *>(socket->default_value)->value, value);
          }
        }
        return node;
      };
      bNode *decode = add_vector_math(NODE_VECTOR_MATH_MULTIPLY_ADD, 2.0f, -1.0f);
      bNode *normalize = add_vector_math(NODE_VECTOR_MATH_NORMALIZE, 0.0f, 0.0f);
      bNode *encode = add_vector_math(NODE_VECTOR_MATH_MULTIPLY_ADD, 0.5f, 0.5f);
      if (decode != nullptr && normalize != nullptr && encode != nullptr) {
        bNodeSocket *decode_in = socket_in(*decode, "Vector");
        bNodeSocket *decode_out = socket_out(*decode, "Vector");
        bNodeSocket *normalize_in = socket_in(*normalize, "Vector");
        bNodeSocket *normalize_out = socket_out(*normalize, "Vector");
        bNodeSocket *encode_in = socket_in(*encode, "Vector");
        bNodeSocket *encode_out = socket_out(*encode, "Vector");
        if (decode_in != nullptr && decode_out != nullptr && normalize_in != nullptr &&
            normalize_out != nullptr && encode_in != nullptr && encode_out != nullptr)
        {
          bke::node_add_link(
              tree, *previous.source_node, *previous.source, *decode, *decode_in);
          bke::node_add_link(tree, *decode, *decode_out, *normalize, *normalize_in);
          bke::node_add_link(tree, *normalize, *normalize_out, *encode, *encode_in);
          previous.source_node = encode;
          previous.source = encode_out;
        }
      }
    }

    if (previous.source != nullptr) {
      bke::node_add_link(
          tree, *previous.source_node, *previous.source, *group_output, *result_socket);
    }
    location_y += 240.0f;
  }

  /* A rebuilt group kept its old interface so its socket identifiers -- and the parent's links
   * into them -- stay stable; drop the sockets this build no longer uses so the interface matches
   * what a fresh build would produce (and its signature, which the root hash reads, is honest). */
  for (const auto &item : layer_groups.items()) {
    LayerGroup &group = *item.value;
    if (group.unchanged || group.tree == nullptr) {
      continue;
    }
    Vector<bNodeTreeInterfaceSocket *> stale;
    group.tree->tree_interface.foreach_item([&](bNodeTreeInterfaceItem &iface_item) {
      if (iface_item.item_type != NodeTreeInterfaceItemType::Socket) {
        return true;
      }
      auto &socket = reinterpret_cast<bNodeTreeInterfaceSocket &>(iface_item);
      if (!group.used_sockets.contains(&socket)) {
        stale.append(&socket);
      }
      return true;
    });
    for (bNodeTreeInterfaceSocket *socket : stale) {
      group.tree->tree_interface.remove_item(
          reinterpret_cast<bNodeTreeInterfaceItem &>(*socket));
    }
    if (!stale.is_empty()) {
      refresh_layer_group(group);
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Regenerate
 * \{ */

namespace {

bUUID tree_owner_uid_get(const bNodeTree &tree)
{
  bUUID uid = BLI_uuid_nil();
  uid_prop_get(tree.id.properties, TREE_OWNER_PROP, uid);
  return uid;
}

void tree_owner_uid_set(bNodeTree &tree, const bUUID &uid)
{
  uid_prop_set(tree.id.properties, TREE_OWNER_PROP, uid);
}

void instance_uid_set(bNode &node, const bUUID &uid)
{
  uid_prop_set(node.prop, INSTANCE_OWNER_PROP, uid);
}

/** Write \a hash as two 32-bit words; IDProperty has no 64-bit integer type. */
void tree_hash_set(bNodeTree &tree,
                   const char *low_key,
                   const char *high_key,
                   const uint64_t hash)
{
  prop_int_set(tree.id.properties, low_key, int(uint32_t(hash & 0xFFFFFFFFu)));
  prop_int_set(tree.id.properties, high_key, int(uint32_t(hash >> 32)));
}

/** Read a stored hash; false when the tree has never been stamped under those keys. */
bool tree_hash_get(bNodeTree &tree,
                   const char *low_key,
                   const char *high_key,
                   uint64_t &r_hash)
{
  const IDProperty *props = IDP_GetProperties(&tree.id);
  if (props == nullptr) {
    return false;
  }
  const IDProperty *low = IDP_GetPropertyTypeFromGroup(props, low_key, IDP_INT);
  const IDProperty *high = IDP_GetPropertyTypeFromGroup(props, high_key, IDP_INT);
  if (low == nullptr || high == nullptr) {
    return false;
  }
  r_hash = (uint64_t(uint32_t(IDP_int_get(high))) << 32) | uint64_t(uint32_t(IDP_int_get(low)));
  return true;
}

void tree_topology_hash_set(bNodeTree &tree, const uint64_t hash)
{
  tree_hash_set(tree, TREE_TOPOLOGY_LOW_PROP, TREE_TOPOLOGY_HIGH_PROP, hash);
}

bool tree_topology_hash_get(bNodeTree &tree, uint64_t &r_hash)
{
  return tree_hash_get(tree, TREE_TOPOLOGY_LOW_PROP, TREE_TOPOLOGY_HIGH_PROP, r_hash);
}

void tree_root_hash_set(bNodeTree &tree, const uint64_t hash)
{
  tree_hash_set(tree, TREE_ROOT_TOPOLOGY_LOW_PROP, TREE_ROOT_TOPOLOGY_HIGH_PROP, hash);
}

bool tree_root_hash_get(bNodeTree &tree, uint64_t &r_hash)
{
  return tree_hash_get(tree, TREE_ROOT_TOPOLOGY_LOW_PROP, TREE_ROOT_TOPOLOGY_HIGH_PROP, r_hash);
}

bNode *instance_find(const Material &ma, const bUUID &owner_uid)
{
  if (ma.nodetree == nullptr) {
    return nullptr;
  }
  for (bNode &node : ma.nodetree->nodes) {
    bUUID node_uid = BLI_uuid_nil();
    if (uid_prop_get(node.prop, INSTANCE_OWNER_PROP, node_uid) &&
        BLI_uuid_equal(node_uid, owner_uid))
    {
      return &node;
    }
  }
  return nullptr;
}

/** Remove every node of \a tree, leaving its interface untouched. */
void tree_clear_nodes(Main &bmain, bNodeTree &tree)
{
  Vector<bNode *> nodes;
  for (bNode &node : tree.nodes) {
    nodes.append(&node);
  }
  for (bNode *node : nodes) {
    bke::node_remove_node(&bmain, tree, *node, true);
  }
}

void tree_clear(Main &bmain, bNodeTree &tree)
{
  tree_clear_nodes(bmain, tree);
  tree.tree_interface.clear_items();
}

/** Delete the source-group wrappers of \a owner whose source no row reads any more. */
void source_groups_prune(Main &bmain, const Material &owner);

/** A layer group's own marker (the layer it stands for), or false when the tree is not one. */
bool layer_tree_marker_get(const bNodeTree &tree, bUUID &r_marker)
{
  return uid_prop_get(tree.id.properties, TREE_LAYER_PROP, r_marker);
}

/** Collect the layer groups a tree holds, descending into nested ones; folders will nest. */
void layer_trees_collect(bNodeTree &tree,
                         const bUUID &owner_uid,
                         Vector<bNodeTree *> &r_trees)
{
  for (bNode &node : tree.nodes) {
    if (!node.is_group() || node.id == nullptr || GS(node.id->name) != ID_NT) {
      continue;
    }
    bNodeTree *group = id_cast<bNodeTree *>(node.id);
    if (group == nullptr || !BLI_uuid_equal(tree_owner_uid_get(*group), owner_uid)) {
      continue;
    }
    bUUID marker = BLI_uuid_nil();
    if (!layer_tree_marker_get(*group, marker) || r_trees.contains(group)) {
      continue;
    }
    r_trees.append(group);
    layer_trees_collect(*group, owner_uid, r_trees);
  }
}

/** Copy the layer groups of a ripped tree, re-pointing the nodes at the copies. */
void layer_trees_copy(Main *bmain,
                      bNodeTree &tree,
                      const bUUID &src_owner,
                      const bUUID &dst_owner)
{
  for (bNode &node : tree.nodes) {
    if (!node.is_group() || node.id == nullptr || GS(node.id->name) != ID_NT) {
      continue;
    }
    bNodeTree *group = id_cast<bNodeTree *>(node.id);
    if (group == nullptr || !BLI_uuid_equal(tree_owner_uid_get(*group), src_owner)) {
      continue;
    }
    bUUID marker = BLI_uuid_nil();
    if (!layer_tree_marker_get(*group, marker)) {
      continue;
    }
    bNodeTree *group_copy = id_cast<bNodeTree *>(BKE_id_copy(bmain, &group->id));
    if (group_copy == nullptr) {
      continue;
    }
    /* #BKE_id_copy hands back a reference of its own; the node below is the user. */
    id_us_min(&group_copy->id);
    id_us_min(node.id);
    node.id = &group_copy->id;
    id_us_plus(&group_copy->id);
    tree_owner_uid_set(*group_copy, dst_owner);
    layer_trees_copy(bmain, *group_copy, src_owner, dst_owner);
  }
}

/** The Material Output node of \a ma's embedded tree, or null. */
bNode *material_output_find(Material &ma)
{
  if (ma.nodetree == nullptr) {
    return nullptr;
  }
  for (bNode &node : ma.nodetree->nodes) {
    if (node.type_legacy == SH_NODE_OUTPUT_MATERIAL) {
      return &node;
    }
  }
  return nullptr;
}

/** The Principled input a channel routes into, or null when there is none. */
bNodeSocket *principled_channel_socket(Material &ma, const int channel)
{
  ChannelUnavailableReason reason = ChannelUnavailableReason::None;
  const bNode *principled = BKE_paint_material_principled_find(ma, reason);
  if (principled == nullptr) {
    return nullptr;
  }
  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
      eMaterialPaintChannel(channel));
  if (info.socket_name == nullptr) {
    return nullptr;
  }
  return bke::node_find_socket(
      const_cast<bNode &>(*principled), SOCK_IN, UString::from_ptr_noinline(info.socket_name));
}

/** A Normal Map node in \a ma, created when missing. */
bNode *normal_map_ensure(Material &ma)
{
  for (bNode &node : ma.nodetree->nodes) {
    if (node.type_legacy == SH_NODE_NORMAL_MAP) {
      return &node;
    }
  }
  bNode *node = bke::node_add_static_node(nullptr, *ma.nodetree, SH_NODE_NORMAL_MAP);
  if (node != nullptr) {
    node->location[0] = 400.0f;
    node->location[1] = -400.0f;
  }
  return node;
}

/** A Bump node in \a ma, created when missing. */
bNode *bump_ensure(Material &ma)
{
  for (bNode &node : ma.nodetree->nodes) {
    if (node.type_legacy == SH_NODE_BUMP) {
      return &node;
    }
  }
  bNode *node = bke::node_add_static_node(nullptr, *ma.nodetree, SH_NODE_BUMP);
  if (node != nullptr) {
    node->location[0] = 400.0f;
    node->location[1] = -560.0f;
  }
  return node;
}

/** Ensure a Principled exists and drives the Surface input, per C-5. */
void principled_ensure(Material &ma, PaintLayersRegenerateReport &r_report)
{
  if (ma.nodetree == nullptr) {
    return;
  }
  ChannelUnavailableReason reason = ChannelUnavailableReason::None;
  if (BKE_paint_material_principled_find(ma, reason) != nullptr) {
    return;
  }
  bNode *output = material_output_find(ma);
  if (output == nullptr) {
    output = bke::node_add_static_node(nullptr, *ma.nodetree, SH_NODE_OUTPUT_MATERIAL);
    if (output == nullptr) {
      r_report.no_principled = true;
      return;
    }
    output->location[0] = 300.0f;
  }
  bNodeSocket *surface = socket_in(*output, "Surface");
  if (surface == nullptr) {
    r_report.no_principled = true;
    return;
  }
  /* A Surface taken by something other than a Principled is the user's graph: never replaced. */
  if (!surface->directly_linked_links().is_empty()) {
    r_report.no_principled = true;
    return;
  }
  bNode *principled = bke::node_add_static_node(nullptr, *ma.nodetree, SH_NODE_BSDF_PRINCIPLED);
  if (principled == nullptr) {
    r_report.no_principled = true;
    return;
  }
  principled->location[0] = 0.0f;
  principled->location[1] = 0.0f;
  bke::node_add_link(
      *ma.nodetree, *principled, *socket_out(*principled, "BSDF"), *output, *surface);
  r_report.created_principled = true;
}

/**
 * Link \a from_socket into \a input, replacing whatever was there.
 *
 * An input the generator owns is restored in Locked mode (default); in Unlocked mode a foreign
 * link is left alone and reported. A link already exactly of ours is a no-op.
 */
void input_link_restore(Material &ma,
                        bNodeSocket &input,
                        bNode &from_node,
                        bNodeSocket &from_socket,
                        PaintLayersRegenerateReport &r_report)
{
  const bool locked = (ma.paint_layers_flag & MA_PAINT_LAYERS_LOCKED) != 0;
  const Span<bNodeLink *> links = input.directly_linked_links();
  if (!links.is_empty()) {
    if (links[0]->fromnode == &from_node && links[0]->fromsock == &from_socket) {
      return;
    }
    if (!locked) {
      r_report.skipped_foreign_inputs = true;
      return;
    }
    r_report.restored_inputs = true;
  }
  bke::node_remove_socket_links(*ma.nodetree, input);
  bke::node_add_link(*ma.nodetree, from_node, from_socket, input.owner_node(), input);
}

/** The instance output socket behind the group output named \a result_name, or null. */
bNodeSocket *instance_output_find(bNodeTree &tree, bNode &instance, const char *result_name)
{
  tree.ensure_interface_cache();
  for (bNodeTreeInterfaceSocket *iface : tree.interface_outputs()) {
    if (iface->name != nullptr && STREQ(iface->name, result_name) && iface->identifier != nullptr) {
      return bke::node_find_socket(
          instance, SOCK_OUT, UString::from_ptr_noinline(iface->identifier));
    }
  }
  return nullptr;
}

/** Route every generated output into the material, channel by channel. */
void wire_instance_to_material(Material &ma,
                               bNodeTree &tree,
                               bNode &instance,
                               PaintLayersRegenerateReport &r_report)
{
  ChannelUnavailableReason reason = ChannelUnavailableReason::None;
  const bNode *principled_const = BKE_paint_material_principled_find(ma, reason);
  if (principled_const == nullptr) {
    r_report.no_principled = true;
    return;
  }
  bNode &principled = const_cast<bNode &>(*principled_const);

  for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
    if (info.channel == PAINT_MATERIAL_CHANNEL_CUSTOM ||
        info.channel == PAINT_MATERIAL_CHANNEL_AO)
    {
      continue;
    }
    char result_name[64];
    SNPRINTF(result_name, "Result %s", info.ui_name);
    bNodeSocket *instance_out = instance_output_find(tree, instance, result_name);
    if (instance_out == nullptr) {
      continue;
    }

    if (info.channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
      bNode *normal_map = normal_map_ensure(ma);
      if (normal_map == nullptr) {
        continue;
      }
      /* The node was just created on a tree whose topology cache predates it.
       * #input_link_restore reads a socket's owner through that cache, so refresh it before the
       * new node's sockets are used. */
      ma.nodetree->ensure_topology_cache();
      bNodeSocket *map_color = socket_in(*normal_map, "Color");
      bNodeSocket *map_normal = socket_out(*normal_map, "Normal");
      bNodeSocket *principled_normal = socket_in(principled, "Normal");
      if (map_color == nullptr || map_normal == nullptr || principled_normal == nullptr) {
        continue;
      }
      input_link_restore(ma, *map_color, instance, *instance_out, r_report);
      input_link_restore(ma, *principled_normal, *normal_map, *map_normal, r_report);
      continue;
    }
    if (info.channel == PAINT_MATERIAL_CHANNEL_HEIGHT) {
      bNode *bump = bump_ensure(ma);
      if (bump == nullptr) {
        continue;
      }
      /* See the Normal branch: the freshly created node is not in the topology cache yet. */
      ma.nodetree->ensure_topology_cache();
      bNodeSocket *bump_height = socket_in(*bump, "Height");
      bNodeSocket *bump_normal_out = socket_out(*bump, "Normal");
      bNodeSocket *principled_normal = socket_in(principled, "Normal");
      if (bump_height == nullptr || bump_normal_out == nullptr || principled_normal == nullptr) {
        continue;
      }
      input_link_restore(ma, *bump_height, instance, *instance_out, r_report);
      /* Height rides over the Normal stack: the Bump's Normal input takes whatever currently feeds
       * the Principled Normal, be that the Normal Map or a hand-built graph. */
      bNodeSocket *bump_normal_in = socket_in(*bump, "Normal");
      if (bump_normal_in != nullptr) {
        const Span<bNodeLink *> normal_links = principled_normal->directly_linked_links();
        if (normal_links.size() == 1) {
          input_link_restore(ma,
                             *bump_normal_in,
                             *normal_links[0]->fromnode,
                             *normal_links[0]->fromsock,
                             r_report);
        }
      }
      input_link_restore(ma, *principled_normal, *bump, *bump_normal_out, r_report);
      continue;
    }

    bNodeSocket *target = principled_channel_socket(ma, info.channel);
    if (target == nullptr) {
      continue;
    }
    input_link_restore(ma, *target, instance, *instance_out, r_report);
  }
}

/**
 * Re-declare every generated group instance reachable from \a tree so its sockets match its
 * group's current interface. A group that gained inputs (a new effect or mask) while the tree that
 * instantiates it was kept would otherwise be stale, and the node tree update's enum/interface pass
 * assumes the instance's inputs line up with the group's interface.
 */
void refresh_generated_instances(bNodeTree &tree, const bUUID &owner_uid, Set<bNodeTree *> &visited)
{
  if (!visited.add(&tree)) {
    return;
  }
  for (bNode &node : tree.nodes) {
    if (!node.is_group() || node.id == nullptr || GS(node.id->name) != ID_NT) {
      continue;
    }
    bNodeTree *group = id_cast<bNodeTree *>(node.id);
    if (group == nullptr || !BLI_uuid_equal(tree_owner_uid_get(*group), owner_uid)) {
      continue;
    }
    nodes::update_node_declaration_and_sockets(tree, node);
    refresh_generated_instances(*group, owner_uid, visited);
  }
}

}  // namespace

namespace {

const char *material_mode_name(const PaintLayerMaterialMode mode)
{
  switch (mode) {
    case PaintLayerMaterialMode::Hybrid:
      return "Hybrid";
    case PaintLayerMaterialMode::SourceGroup:
      return "SourceGroup";
    case PaintLayerMaterialMode::Baked:
      return "Baked";
  }
  return "Baked";
}

const char *source_group_refusal_name(const PaintLayersSourceGroupRefusal refusal)
{
  switch (refusal) {
    case PaintLayersSourceGroupRefusal::None:
      return "none";
    case PaintLayersSourceGroupRefusal::NoNodeTree:
      return "no-node-tree";
    case PaintLayersSourceGroupRefusal::NoPrincipled:
      return "no-principled";
    case PaintLayersSourceGroupRefusal::PrincipledInGroup:
      return "principled-in-group";
    case PaintLayersSourceGroupRefusal::SelfReference:
      return "self-reference";
    case PaintLayersSourceGroupRefusal::BuildFailed:
      return "build-failed";
    case PaintLayersSourceGroupRefusal::TooManyTextures:
      return "too-many-textures";
  }
  return "unknown";
}

/**
 * The previous pass's per-row modes, keyed by material `session_uid`, so the diagnostic below logs
 * a row only when its state changed. A static map rather than material runtime: the state has to
 * survive a runtime free and is a debug aid, and a session holds only a handful of layered
 * materials.
 */
Map<uint32_t, Vector<PaintLayersRegenerateReport::MaterialRowModeReport>> &previous_row_modes()
{
  static Map<uint32_t, Vector<PaintLayersRegenerateReport::MaterialRowModeReport>> map;
  return map;
}

const PaintLayersRegenerateReport::MaterialRowModeReport *find_previous_row_mode(
    const Vector<PaintLayersRegenerateReport::MaterialRowModeReport> &rows, const bUUID &marker)
{
  for (const PaintLayersRegenerateReport::MaterialRowModeReport &row : rows) {
    if (BLI_uuid_equal(row.marker, marker)) {
      return &row;
    }
  }
  return nullptr;
}

/** Log one line per Material row whose mode, wrapper, refusal or deferred state changed. */
void material_row_modes_log(
    const Material &ma, const Vector<PaintLayersRegenerateReport::MaterialRowModeReport> &rows)
{
  Vector<PaintLayersRegenerateReport::MaterialRowModeReport> &previous =
      previous_row_modes().lookup_or_add_default(ma.id.session_uid);
  for (const PaintLayersRegenerateReport::MaterialRowModeReport &row : rows) {
    const PaintLayersRegenerateReport::MaterialRowModeReport *old = find_previous_row_mode(
        previous, row.marker);
    if (old != nullptr && old->mode == row.mode && old->wrapper_built == row.wrapper_built &&
        old->refusal == row.refusal && old->deferred == row.deferred)
    {
      continue;
    }
    char group_depth[16];
    if (row.mode == PaintLayerMaterialMode::SourceGroup) {
      SNPRINTF(group_depth, "%d", row.group_depth);
    }
    else {
      STRNCPY(group_depth, "-");
    }
    printf("paint layers: row '%s' owner='%s' owner_tag=0x%x source='%s' source_uid=%u "
           "deferred=%d mode=%s wrapper=%s refusal=%s group_depth=%s\n",
           row.name,
           ma.id.name + 2,
           static_cast<unsigned int>(ma.id.tag),
           row.source_name,
           row.source_uid,
           row.deferred ? 1 : 0,
           material_mode_name(row.mode),
           row.wrapper_built ? "yes" : "no",
           source_group_refusal_name(row.refusal),
           group_depth);
  }
  previous_row_modes().add_overwrite(ma.id.session_uid, rows);
}

/** What a SourceGroup row actually embedded, kept to log only the rows whose state changed. */
struct SourceGroupEmbedState {
  bUUID marker;
  char name[64];
  char layer_tree[MAX_ID_NAME - 2];
  int wrapper_instances = 0;
  int linked_color_outputs = 0;
  int linked_coverage = 0;
};

Map<uint32_t, Vector<SourceGroupEmbedState>> &previous_source_group_embeds()
{
  static Map<uint32_t, Vector<SourceGroupEmbedState>> map;
  return map;
}

const SourceGroupEmbedState *find_previous_embed(const Vector<SourceGroupEmbedState> &states,
                                                 const bUUID &marker)
{
  for (const SourceGroupEmbedState &state : states) {
    if (BLI_uuid_equal(state.marker, marker)) {
      return &state;
    }
  }
  return nullptr;
}

/**
 * Log what a SourceGroup row actually embedded: its layer group, how many instances of the source
 * wrapper it holds and how many of the wrapper's `COLOR:<CHANNEL>`/`COVERAGE` outputs are linked.
 * A row that is live but shows nothing is the exact failure this diagnostic is meant to catch.
 */
void source_group_instances_log(
    const Material &ma,
    const Map<const MaterialPaintLayer *, bNodeTree *> &layer_trees,
    const Map<const Material *, bNodeTree *> &source_groups,
    const PaintLayersRegenCache *cache)
{
  Vector<const MaterialPaintLayer *> layers;
  BKE_paint_layers_flatten(ma, layers);
  Vector<SourceGroupEmbedState> states;
  for (const MaterialPaintLayer *layer : layers) {
    if (layer->kind != MA_PAINT_LAYER_KIND_MATERIAL ||
        BKE_paint_layers_material_mode(ma, *layer, cache) != PaintLayerMaterialMode::SourceGroup)
    {
      continue;
    }
    SourceGroupEmbedState state;
    state.marker = layer->marker;
    STRNCPY(state.name, layer->name);
    bNodeTree *layer_tree = layer_trees.lookup_default(layer, nullptr);
    bNodeTree *wrapper = (layer->material != nullptr) ?
                             source_groups.lookup_default(layer->material, nullptr) :
                             nullptr;
    STRNCPY(state.layer_tree, (layer_tree != nullptr) ? layer_tree->id.name + 2 : "none");
    if (layer_tree != nullptr && wrapper != nullptr) {
      layer_tree->ensure_topology_cache();
      wrapper->ensure_interface_cache();
      for (bNode &node : layer_tree->nodes) {
        if (!node.is_group() || node.id != &wrapper->id) {
          continue;
        }
        state.wrapper_instances++;
        for (bNodeSocket &out : node.outputs) {
          const bNodeTreeInterfaceSocket *iface = nullptr;
          for (const bNodeTreeInterfaceSocket *candidate : wrapper->interface_outputs()) {
            if (candidate->identifier != nullptr && STREQ(candidate->identifier, out.identifier)) {
              iface = candidate;
              break;
            }
          }
          if (iface == nullptr) {
            continue;
          }
          const char *role = prop_string_get(iface->properties, PAINT_LAYERS_CUSTOM_ROLE_PROP);
          if (role == nullptr || out.directly_linked_links().is_empty()) {
            continue;
          }
          if (STRPREFIX(role, "COLOR:")) {
            state.linked_color_outputs++;
          }
          else if (STREQ(role, "COVERAGE")) {
            state.linked_coverage++;
          }
        }
      }
    }
    states.append(state);
  }
  Vector<SourceGroupEmbedState> &previous =
      previous_source_group_embeds().lookup_or_add_default(ma.id.session_uid);
  for (const SourceGroupEmbedState &state : states) {
    const SourceGroupEmbedState *old = find_previous_embed(previous, state.marker);
    if (old != nullptr && STREQ(old->layer_tree, state.layer_tree) &&
        old->wrapper_instances == state.wrapper_instances &&
        old->linked_color_outputs == state.linked_color_outputs &&
        old->linked_coverage == state.linked_coverage)
    {
      continue;
    }
    printf("paint layers: row '%s' layer_tree='%s' wrapper_instances=%d linked_color_outputs=%d "
           "linked_coverage=%d\n",
           state.name,
           state.layer_tree,
           state.wrapper_instances,
           state.linked_color_outputs,
           state.linked_coverage);
  }
  previous_source_group_embeds().add_overwrite(ma.id.session_uid, states);
}

/* -------------------------------------------------------------------- */
/** \name Sampler budget and counting
 * \{ */

/**
 * The runtime sampler budget and the fallback state it drives. Not DNA: it is derived from the
 * budget on every regeneration and cleared when the budget grows, so it must never be saved.
 */
struct SamplerRuntimeState {
  /** Material-texture sampler allowance; zero disables the check. */
  int budget = 0;
  /** `GPU_max_textures()` as reported to #BKE_paint_layers_sampler_budget_set, for the log only. */
  int max_textures = 0;
  /** Owners whose disabled rows the last over-budget pass dropped, by material `session_uid`. */
  Set<uint32_t> cleanup_owners;
  /** The markers of rows forced onto their baked maps, per owner `session_uid`. */
  Map<uint32_t, Vector<bUUID>> forced_bake;
};

static SamplerRuntimeState &sampler_runtime()
{
  static SamplerRuntimeState state;
  return state;
}

/** Whether the last over-budget pass is dropping this owner's hidden rows. */
static bool budget_cleanup_active(const Material &ma)
{
  return sampler_runtime().cleanup_owners.contains(ma.id.session_uid);
}

static bool forced_bake_contains(const Material &ma, const bUUID &marker)
{
  const Vector<bUUID> *markers = sampler_runtime().forced_bake.lookup_ptr(ma.id.session_uid);
  if (markers == nullptr) {
    return false;
  }
  for (const bUUID &other : *markers) {
    if (BLI_uuid_equal(other, marker)) {
      return true;
    }
  }
  return false;
}

/** The sampler state EEVEE derives from an image node's extension, interpolation and projection. */
static uint32_t image_sampler_state_key(const int extension,
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
static uint32_t image_sampler_state_key(const NodeTexImage &tex)
{
  return image_sampler_state_key(tex.extension, tex.interpolation, tex.projection);
}

/**
 * The sampler state of the Image Texture nodes the generator creates for a row's own map, a
 * correction map or a baked map: all of them are added with the node type's default storage, which
 * is Repeat/Linear/Flat. Using this as #SamplerCounter::add_image's default is what lets a map that
 * appears both in the stack and in the user's tree dedup to one sampler.
 */
static uint32_t default_image_sampler_state()
{
  return image_sampler_state_key(
      SHD_IMAGE_EXTENSION_REPEAT, SHD_INTERP_LINEAR, SHD_PROJ_FLAT);
}

/** The sampler state EEVEE derives from an environment node's projection and interpolation. */
static uint32_t environment_sampler_state_key(const NodeTexImage &tex)
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

/** Count the samplers reachable from \a tree's own output nodes. */
static int sampler_count_tree(const bNodeTree &tree)
{
  SamplerCounter counter;
  counter.visit_tree(tree);
  return counter.total();
}

/** Remove any forced-bake marker of \a ma; used before recomputing the fallback from scratch. */
static void forced_bake_clear(const Material &ma)
{
  sampler_runtime().forced_bake.remove(ma.id.session_uid);
}

/** The markers \a ma's last pass pinned, before #forced_bake_clear drops them for this pass. */
static Vector<bUUID> forced_bake_markers(const Material &ma)
{
  const Vector<bUUID> *markers = sampler_runtime().forced_bake.lookup_ptr(ma.id.session_uid);
  return (markers != nullptr) ? *markers : Vector<bUUID>();
}

static void forced_bake_add(const Material &ma, const bUUID &marker)
{
  sampler_runtime().forced_bake.lookup_or_add_default(ma.id.session_uid).append(marker);
}

static void forced_bake_remove(const Material &ma, const bUUID &marker)
{
  Vector<bUUID> *markers = sampler_runtime().forced_bake.lookup_ptr(ma.id.session_uid);
  if (markers == nullptr) {
    return;
  }
  for (int i = 0; i < markers->size(); i++) {
    if (BLI_uuid_equal((*markers)[i], marker)) {
      markers->remove(i);
      return;
    }
  }
}

/** \} */

}  // namespace

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
  sampler_runtime().forced_bake.remove(ma.id.session_uid);
  sampler_runtime().cleanup_owners.remove(ma.id.session_uid);
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

static void values_sync_with_cache(Material &ma, const PaintLayersRegenCache *cache);

bool BKE_paint_layers_regenerate(Main &bmain,
                                 Material &ma,
                                 PaintLayersRegenerateReport *r_report)
{
  PaintLayersRegenerateReport report;
  const double regen_start = BLI_time_now_seconds();
  if (!paint_layers_is_layered(ma) || ma.nodetree == nullptr) {
    if (r_report != nullptr) {
      *r_report = report;
    }
    return false;
  }
  if (BLI_uuid_is_nil(ma.paint_layers_owner_uid)) {
    ma.paint_layers_owner_uid = BLI_uuid_generate_random();
  }

  bNodeTree *tree = ma.paint_layers_tree;
  const bool owned = tree != nullptr &&
                     BLI_uuid_equal(tree_owner_uid_get(*tree), ma.paint_layers_owner_uid);
  if (tree != nullptr && !owned) {
    /* Somebody replaced the pointer with a tree that is not ours. Leave their data alone and make a
     * fresh tree; the foreign one keeps whatever users it has. */
    ma.paint_layers_tree = nullptr;
    tree = nullptr;
    report.replaced_foreign_tree = true;
  }
  /* Collect the layer groups before any clear removes their instances; afterwards they are lost. */
  Vector<bNodeTree *> old_layer_trees;
  if (tree != nullptr) {
    layer_trees_collect(*tree, ma.paint_layers_owner_uid, old_layer_trees);
  }
  bool created_tree = false;
  if (tree == nullptr) {
    char name[MAX_ID_NAME - 2];
    SNPRINTF(name, "PBR Layers (%s)", ma.id.name + 2);
    tree = bke::node_tree_add_tree(&bmain, name, "ShaderNodeTree");
    if (tree == nullptr) {
      return false;
    }
    /* The tree is created with one user, which is this material from here on. */
    ma.paint_layers_tree = tree;
    tree_owner_uid_set(*tree, ma.paint_layers_owner_uid);
    created_tree = true;
  }
  /* An existing tree is not cleared yet: whether the root can be kept is decided after the layer
   * groups are (re)built, because that decision reads their final interfaces. */

  /* The Normal chain needs the shared combine group; create it only when a Normal row exists. */
  PaintLayersBuildContext ctx;
  /* What this call learns once (a source's resolve, a row's mode, the Pass Through scales) and hands
   * to every reader below. It lives and dies with this call; nothing keeps it between two. */
  PaintLayersRegenCache regen_cache;
  ctx.regen_cache = &regen_cache;  {
    Vector<const MaterialPaintLayer *> layers;
    BKE_paint_layers_flatten(ma, layers);
    for (const MaterialPaintLayer *layer : layers) {
      if (paint_layer_channel_present(*layer, PAINT_MATERIAL_CHANNEL_NORMAL)) {
        ctx.normal_combine_group = BKE_paint_material_normal_combine_group_ensure(bmain);
        break;
      }
    }
  }
  /* Capture the previous pass's forced set, then drop it before anything reads a mode so the
   * fallback re-derives it from scratch and lifts as soon as the budget allows. A row that was pinned
   * and whose bake has since gone invalid is carried forward below: it stays on its stale maps until
   * a fresh bake lands, instead of reviving live and forcing a rebuild loop. */
  const Vector<bUUID> previously_forced = forced_bake_markers(ma);
  forced_bake_clear(ma);
  sampler_runtime().cleanup_owners.remove(ma.id.session_uid);

  /* Wrapper groups for Material rows that show their whole source graph. The factory needs #Main
   * and runs on the main thread, so it is prepared here, like the Normal combine group, and handed
   * to the pure build. Each row's mode and, for SourceGroup, whether the wrapper was built are
   * recorded for the report; a refused source keeps its row on the baked maps. */
  Map<const Material *, bNodeTree *> source_groups;
  Map<const Material *, PaintLayersSourceGroupRefusal> source_group_refusals;
  bool source_groups_changed = false;
  /* Re-runnable: the sampler fallback changes some rows' modes, so the report is rebuilt once the
   * final forced set is known. Wrappers are cached, so a second call does not rebuild them. */
  auto populate_material_rows = [&]() {
    report.material_rows.clear();
    Vector<const MaterialPaintLayer *> layers;
    BKE_paint_layers_flatten(ma, layers);
    for (const MaterialPaintLayer *layer : layers) {
      if (layer->kind != MA_PAINT_LAYER_KIND_MATERIAL) {
        continue;
      }
      const PaintLayerMaterialMode mode = BKE_paint_layers_material_mode(ma, *layer, &regen_cache);
      PaintLayersSourceGroupRefusal refusal = PaintLayersSourceGroupRefusal::None;
      bool wrapper_built = false;
      int group_depth = 0;
      if (mode == PaintLayerMaterialMode::SourceGroup && layer->material != nullptr) {
        ChannelUnavailableReason path_reason = ChannelUnavailableReason::None;
        Vector<const bNode *> path;
        if (BKE_paint_material_principled_find(*layer->material, path_reason, &path) != nullptr) {
          group_depth = int(path.size());
        }
        bNodeTree *wrapper = nullptr;
        if (bNodeTree *const *found = source_groups.lookup_ptr(layer->material)) {
          wrapper = *found;
          refusal = source_group_refusals.lookup_default(layer->material,
                                                         PaintLayersSourceGroupRefusal::None);
        }
        else {
          bool wrapper_changed = false;
          wrapper = BKE_paint_layers_source_group_ensure(
              bmain, ma, *layer->material, refusal, &wrapper_changed);
          source_groups_changed |= wrapper_changed;
          source_groups.add(layer->material, wrapper);
          source_group_refusals.add(layer->material, refusal);
          if (wrapper == nullptr &&
              report.source_group_refusal == PaintLayersSourceGroupRefusal::None)
          {
            report.source_group_refusal = refusal;
          }
        }
        wrapper_built = wrapper != nullptr;
      }
      PaintLayersRegenerateReport::MaterialRowModeReport row;
      row.marker = layer->marker;
      STRNCPY(row.name, layer->name);
      row.mode = mode;
      row.refusal = refusal;
      row.wrapper_built = wrapper_built;
      row.group_depth = group_depth;
      row.source_name[0] = '\0';
      row.source_uid = 0;
      if (layer->material != nullptr) {
        STRNCPY(row.source_name, layer->material->id.name + 2);
        row.source_uid = layer->material->id.session_uid;
      }
      row.deferred = BKE_paint_layers_bake_row_is_deferred(ma, *layer);
      if (BKE_paint_layers_material_forced_bake(ma, *layer)) {
        row.refusal = PaintLayersSourceGroupRefusal::TooManyTextures;
      }
      report.material_rows.append(row);
    }
  };
  populate_material_rows();
  /* Named: #FunctionRef does not own the callable, so a temporary lambda would dangle. */
  const auto source_group_lookup = [&source_groups](const Material &source) -> bNodeTree * {
    return source_groups.lookup_default(&source, nullptr);
  };
  ctx.source_group_get = source_group_lookup;

  /* The sampler budget is runtime state; zero disables the check. The decision is made from an
   * estimate over the description -- never by building a preview -- so it does not mutate the layer
   * groups. It must run before `paint_layers_wired_channels`: the fallback moves some rows' modes,
   * and the wired set, every layer group's topology hash and the root hash are computed from the
   * final modes, or a second pass would see a different graph and loop. */
  const int budget = sampler_runtime().budget;
  int sampler_count = 0;
  int sampler_estimate_value = 0;
  int fallback_rows = 0;
  int removed_hidden = 0;

  /* A non-mutating estimate of the samplers the built graph would use: user nodes outside the stack
   * plus the rows' own sources, maps, corrections and baked maps. Shared images dedup by
   * `(Image, sampler state)` exactly as the counter and EEVEE do, and the parity rules -- which rows
   * a mode substitutes, which children an isolating folder expands -- come from the same predicates
   * the build uses, so the estimate tracks the graph rather than a copy of the rules. */
  auto sampler_estimate = [&]() -> int {
    SamplerCounter counter;
    if (ma.nodetree != nullptr) {
      counter.visit_tree_skipping(*ma.nodetree, ma.paint_layers_tree);
    }

    /* A correction's maps are built at most once per row, so adding each one once is enough. A
     * Constant (Fill) correction builds a group input, not a map: a stale image on it is ignored by
     * the build and must not be counted. */
    auto add_corrections = [&](const MaterialPaintLayer &layer) {
      for (const MaterialPaintLayer *effect : BKE_paint_layers_effects(layer)) {
        if (BKE_paint_layers_source_type(*effect) == PaintLayerSourceType::Constant) {
          continue;
        }
        for (int channel = 0; channel < PAINT_MATERIAL_CHANNEL_NUM; channel++) {
          if (Image *image = paint_layer_channel_image(*effect, channel)) {
            counter.add_image(*image);
          }
        }
      }
      for (const MaterialPaintLayer *mask_item : BKE_paint_layers_mask_items(layer)) {
        if (BKE_paint_layers_source_type(*mask_item) == PaintLayerSourceType::Constant) {
          continue;
        }
        if (Image *image = paint_layer_mask_correction_image(*mask_item, 0)) {
          counter.add_image(*image);
        }
      }
    };

    /* The state a Hybrid live map is shown with: the generator copies the source node's storage, so
     * the key has to come from that node, not from the default. */
    auto live_image_state = [&](const MaterialPaintLayer &layer, const int channel) {
      const MaterialSourceResolve &resolve = regen_cache.resolve(layer.material);
      if (const bNode *source_node = resolve.images[channel].node) {
        if (const NodeTexImage *storage = static_cast<const NodeTexImage *>(source_node->storage)) {
          return image_sampler_state_key(*storage);
        }
      }
      return default_image_sampler_state();
    };

    /* Whether the wrapper exposes a COVERAGE output: when it does, the build reads the row's factor
     * from the wrapper and never builds the baked coverage map. */
    auto wrapper_has_coverage = [&](const Material *source) -> bool {
      bNodeTree *wrapper = (source != nullptr) ? source_group_lookup(*source) : nullptr;
      if (wrapper == nullptr) {
        return false;
      }
      wrapper->ensure_interface_cache();
      for (bNodeTreeInterfaceSocket *iface : wrapper->interface_outputs()) {
        const char *role = custom_role_get(iface->properties);
        if (role != nullptr && STREQ(role, "COVERAGE")) {
          return true;
        }
      }
      return false;
    };

    /* Walk the stack so a dropped Pass Through folder takes its whole inlined subtree with it; a
     * flat list cannot tell that an enabled child sits under a removed folder. Returns whether
     * anything under \a list contributes a sampler: the build drops a row with no channel, no live
     * value and no wrapper, and then builds neither its coverage nor its corrections, so the
     * estimate must not count them either. */
    std::function<bool(const ListBase &)> walk = [&](const ListBase &list) -> bool {
      bool any = false;
      for (const MaterialPaintLayer &layer :
           *reinterpret_cast<const ListBaseT<MaterialPaintLayer> *>(&list))
      {
        if (row_is_removed(ma, layer)) {
          continue;
        }
        if (BKE_paint_layers_folder_is_pass_through(ma, layer)) {
          /* Inlined: the folder's own corrections are not built, only its children's. */
          any |= walk(layer.children);
          continue;
        }
        const bool is_folder = BKE_paint_layers_is_folder(layer);
        if (row_is_substituted(ma, layer)) {
          /* The generator replaces the whole row with its bake (`row_is_substituted`), so the build
           * expands no children and builds no corrections. This is the same predicate the build's
           * value-input path uses. */
          counter.add_baked_maps(layer);
          any = true;
          continue;
        }
        const PaintLayerMaterialMode mode = (layer.kind == MA_PAINT_LAYER_KIND_MATERIAL) ?
                                                BKE_paint_layers_material_mode(
                                                    ma, layer, &regen_cache) :
                                                PaintLayerMaterialMode::Baked;
        bool participates = false;
        bool any_substituted = false;
        for (int channel = 0; channel < PAINT_MATERIAL_CHANNEL_NUM; channel++) {
          Image *baked = nullptr;
          if (row_channel_substituted(ma, layer, channel, &baked) && baked != nullptr) {
            /* A per-channel cache (a Custom row's stale bake) replaces just this channel. */
            counter.add_image(*baked);
            any_substituted = true;
            participates = true;
            continue;
          }
          if (layer.kind == MA_PAINT_LAYER_KIND_MATERIAL && layer.material != nullptr) {
            if (mode == PaintLayerMaterialMode::SourceGroup) {
              /* One wrapper instance serves every channel; the visitor dedups its tree. */
              if (bNodeTree *wrapper = source_group_lookup(*layer.material)) {
                counter.add_tree(*wrapper);
                participates = true;
              }
              /* No wrapper (refused): the build falls back to the row's own maps. */
              else if (Image *image = paint_layer_channel_image(layer, channel)) {
                counter.add_image(*image);
                participates = true;
              }
              continue;
            }
            /* Hybrid shows a live constant (no sampler), a live map, or the row's baked map. */
            float live_value[4];
            Image *live_image = nullptr;
            const ImageUser *live_iuser = nullptr;
            if (BKE_paint_layers_material_live_constant(
                    ma, layer, channel, live_value, &regen_cache))
            {
              participates = true;
              continue;
            }
            if (BKE_paint_layers_material_live_image(
                    ma, layer, channel, &live_image, &live_iuser, &regen_cache))
            {
              counter.add_image(*live_image, live_image_state(layer, channel));
              participates = true;
              continue;
            }
          }
          if (Image *image = paint_layer_channel_image(layer, channel)) {
            counter.add_image(*image);
            participates = true;
          }
        }
        if (is_folder) {
          participates |= walk(layer.children);
        }
        if (participates) {
          if (any_substituted && layer.bake != nullptr && layer.bake->coverage != nullptr) {
            counter.add_image(*layer.bake->coverage);
          }
          if (layer.kind == MA_PAINT_LAYER_KIND_MATERIAL && layer.bake != nullptr &&
              layer.bake->coverage != nullptr &&
              !(mode == PaintLayerMaterialMode::SourceGroup &&
                wrapper_has_coverage(layer.material)))
          {
            /* The row's factor falls back to the baked coverage when the source's alpha is not
             * shown live and the wrapper has no COVERAGE output: a live constant needs no sampler, a
             * live image is counted in its own channel, and a wrapper coverage is in the wrapper. */
            float live_alpha_value[4];
            Image *live_alpha_image = nullptr;
            const ImageUser *live_alpha_iuser = nullptr;
            const bool live_alpha =
                BKE_paint_layers_material_live_constant(
                    ma, layer, PAINT_MATERIAL_CHANNEL_ALPHA, live_alpha_value, &regen_cache) ||
                BKE_paint_layers_material_live_image(ma,
                                                     layer,
                                                     PAINT_MATERIAL_CHANNEL_ALPHA,
                                                     &live_alpha_image,
                                                     &live_alpha_iuser,
                                                     &regen_cache);
            if (!live_alpha) {
              counter.add_image(*layer.bake->coverage);
            }
          }
          add_corrections(layer);
        }
        any |= participates;
      }
      return any;
    };
    walk(ma.paint_layers);
    return counter.total();
  };

  sampler_estimate_value = sampler_estimate();
  if (budget > 0 && sampler_estimate_value > budget) {
    /* 1. Drop disabled rows. A hidden Pass Through folder only leaves under pressure: without it
     *    its children stay in the graph with factor zero, which is the value-edit behavior. */
    bool any_disabled = false;
    {
      Vector<const MaterialPaintLayer *> all_layers;
      BKE_paint_layers_flatten(ma, all_layers);
      for (const MaterialPaintLayer *layer : all_layers) {
        if ((layer->flag & MA_PAINT_LAYER_ENABLED) == 0) {
          any_disabled = true;
          break;
        }
      }
    }
    if (any_disabled) {
      sampler_runtime().cleanup_owners.add(ma.id.session_uid);
      removed_rows_reconcile(ma);
      sampler_estimate_value = sampler_estimate();
    }
    /* 2. Keep a row pinned last pass on its stale maps while its bake is rebuilt. It must not revive
     *    live: that would rebuild the root on every source edit and re-enter the bake on the next
     *    pass -- the bake -> hash -> regeneration -> bake loop this state exists to break. */
    for (const bUUID &marker : previously_forced) {
      MaterialPaintLayer *layer = BKE_paint_layers_find(ma, marker);
      if (layer == nullptr || layer->kind != MA_PAINT_LAYER_KIND_MATERIAL ||
          layer->bake == nullptr || BKE_paint_layers_bake_is_valid(ma, *layer) ||
          forced_bake_contains(ma, marker))
      {
        continue;
      }
      forced_bake_add(ma, marker);
      sampler_estimate_value = sampler_estimate();
    }
    /* 3. Pin live SourceGroup rows onto their maps, largest sampler saving first. Only a pin that
     *    lowers the count is taken: pinning a row whose baked maps cost more than its live graph
     *    would raise the count instead. Ties break on the marker so the choice is deterministic. */
    if (sampler_estimate_value > budget) {
      struct FallbackCandidate {
        int gain = 0;
        bUUID marker = {};
      };
      Vector<FallbackCandidate> candidates;
      Vector<const MaterialPaintLayer *> all_layers;
      BKE_paint_layers_flatten(ma, all_layers);
      for (const MaterialPaintLayer *layer : all_layers) {
        if (layer->kind != MA_PAINT_LAYER_KIND_MATERIAL || layer->material == nullptr ||
            layer->bake == nullptr || !BKE_paint_layers_bake_is_valid(ma, *layer) ||
            BKE_paint_layers_material_mode(ma, *layer, &regen_cache) !=
                PaintLayerMaterialMode::SourceGroup ||
            source_group_lookup(*layer->material) == nullptr ||
            forced_bake_contains(ma, layer->marker))
        {
          continue;
        }
        /* Probe the row: measure the count with it pinned, then undo the pin. */
        const int before = sampler_estimate_value;
        forced_bake_add(ma, layer->marker);
        const int after = sampler_estimate();
        forced_bake_remove(ma, layer->marker);
        if (before > after) {
          candidates.append({before - after, layer->marker});
        }
      }
      std::sort(candidates.begin(),
                candidates.end(),
                [](const FallbackCandidate &a, const FallbackCandidate &b) {
                  if (a.gain != b.gain) {
                    return a.gain > b.gain;
                  }
                  char a_text[UUID_STRING_SIZE];
                  char b_text[UUID_STRING_SIZE];
                  BLI_uuid_format(a_text, a.marker);
                  BLI_uuid_format(b_text, b.marker);
                  return strcmp(a_text, b_text) < 0;
                });
      for (const FallbackCandidate &candidate : candidates) {
        if (sampler_estimate_value <= budget) {
          break;
        }
        forced_bake_add(ma, candidate.marker);
        fallback_rows++;
        /* The baked maps add samplers back, so re-estimate after every pin. */
        sampler_estimate_value = sampler_estimate();
      }
    }
    report.sampler_budget_exceeded = sampler_estimate_value > budget;
    Vector<const MaterialPaintLayer *> all_layers;
    BKE_paint_layers_flatten(ma, all_layers);
    for (const MaterialPaintLayer *layer : all_layers) {
      if ((layer->flag & MA_PAINT_LAYER_ENABLED) == 0 && row_is_removed(ma, *layer)) {
        removed_hidden++;
      }
    }
  }
  /* From here the forced set is final, so a row's mode can be remembered: every reader below asks it
   * many times per row (the wired set, each layer's hash, the build, the report). Before this point
   * the fallback was still moving modes, and a remembered one would have outlived the move. */
  regen_cache.modes_frozen = true;
  /* The modes may have moved (forced bake, hidden cleanup): refresh the report before building. */
  populate_material_rows();
  material_row_modes_log(ma, report.material_rows);

  /* The factory hands each layer a tree of its own, reusing an old one by layer marker. It outlives
   * the build, which only holds a non-owning reference to it. A reused tree whose stored topology
   * hash matches the description is handed back untouched and reported as unchanged, so the build
   * only restores the parent-side links and never re-creates its nodes. Wired channels and the layer
   * tree hashes are read here, after the fallback fixed every row's mode. */
  const Vector<int> wired_channels = paint_layers_wired_channels(ma, &regen_cache);
  Set<const MaterialPaintLayer *> unchanged_layers;
  Vector<bNodeTree *> used_layer_trees;
  Map<const MaterialPaintLayer *, bNodeTree *> layer_trees;
  bool groups_created = false;
  bool groups_deleted = false;
  auto layer_tree_get = [&](const MaterialPaintLayer &layer) -> bNodeTree * {
    char name[MAX_ID_NAME - 2];
    if (BKE_paint_layers_is_folder(layer)) {
      SNPRINTF(name, ".PL Folder %s", layer.name[0] != '\0' ? layer.name : "Folder");
    }
    else {
      SNPRINTF(name, ".PL Layer %s", layer.name[0] != '\0' ? layer.name : "Layer");
    }
    const uint64_t topology = paint_layers_layer_topology_hash(
        ma, layer, wired_channels, &regen_cache);
    for (bNodeTree *candidate : old_layer_trees) {
      if (used_layer_trees.contains(candidate)) {
        continue;
      }
      bUUID marker = BLI_uuid_nil();
      if (layer_tree_marker_get(*candidate, marker) && BLI_uuid_equal(marker, layer.marker)) {
        uint64_t stored = 0;
        if (tree_topology_hash_get(*candidate, stored) && stored == topology) {
          used_layer_trees.append(candidate);
          unchanged_layers.add(&layer);
          layer_trees.add(&layer, candidate);
          return candidate;
        }
        /* Clear the nodes but keep the interface: a rebuilt group reuses its sockets by name, so
         * their identifiers -- and the parent's links into them -- survive. Unused sockets are
         * pruned at the end of the build. */
        printf("paint layers regen diff: layer '%s' kind=%d section=%d old=%llx new=%llx\n",
               layer.name,
               int(layer.kind),
               int(layer.section),
               static_cast<unsigned long long>(stored),
               static_cast<unsigned long long>(topology));
        tree_clear_nodes(bmain, *candidate);
        if (!STREQ(candidate->id.name + 2, name)) {
          BKE_id_rename(bmain, candidate->id, name);
        }
        tree_topology_hash_set(*candidate, topology);
        used_layer_trees.append(candidate);
        layer_trees.add(&layer, candidate);
        return candidate;
      }
    }
    bNodeTree *fresh = bke::node_tree_add_tree(&bmain, name, "ShaderNodeTree");
    if (fresh == nullptr) {
      return nullptr;
    }
    /* The only user must be the instance node, which adds its own reference when assigned. */
    id_us_min(&fresh->id);
    tree_owner_uid_set(*fresh, ma.paint_layers_owner_uid);
    uid_prop_set(fresh->id.properties, TREE_LAYER_PROP, layer.marker);
    tree_topology_hash_set(*fresh, topology);
    used_layer_trees.append(fresh);
    layer_trees.add(&layer, fresh);
    groups_created = true;
    return fresh;
  };
  auto layer_tree_unchanged = [&](const MaterialPaintLayer &layer) {
    return unchanged_layers.contains(&layer);
  };
  ctx.layer_tree_get = layer_tree_get;
  ctx.layer_tree_unchanged = layer_tree_unchanged;

  /* Build once with the final modes and cleanup. This rebuilds the groups whose hash moved (in
   * place) and gives their final interfaces without touching the real root. */
  bNodeTree *scratch = bke::node_tree_add_tree(&bmain, "PBR Layers Scratch", "ShaderNodeTree");
  bNodeTree &build_target = (scratch != nullptr) ? *scratch : *tree;
  paint_layers_tree_build(ma, build_target, ctx);
  /* The build may have grown a group's interface (a new effect or mask). Every instance of that
   * group, in the real root included, must have matching sockets before any tree update: an update
   * of a layer tree also visits the trees that use it, and the node tree update's interface pass
   * assumes an instance's inputs line up with its group's interface. */
  {
    Set<bNodeTree *> refreshed;
    refresh_generated_instances(*tree, ma.paint_layers_owner_uid, refreshed);
  }
  for (bNodeTree *layer_tree : used_layer_trees) {
    Set<bNodeTree *> refreshed;
    refresh_generated_instances(*layer_tree, ma.paint_layers_owner_uid, refreshed);
    BKE_ntree_update_tag_all(layer_tree);
    BKE_ntree_update_after_single_tree_change(bmain, *layer_tree);
    DEG_id_tag_update(&layer_tree->id, ID_RECALC_SYNC_TO_EVAL);
  }

  const uint64_t root_hash = paint_layers_root_topology_hash(
      ma, wired_channels, layer_trees, &regen_cache);
  uint64_t stored_root = 0;
  const bool have_stored_root = !created_tree && tree_root_hash_get(*tree, stored_root);
  /* Undo safety net: a row recorded as removed but enabled again (memfile undo preserves the row
   * and its session state but not our runtime set) must not stay missing. Forget the marker and
   * force the rebuild that puts it back. An extra rebuild is fine; a missing row never is. */
  bool undo_forces_rebuild = false;
  {
    Vector<const MaterialPaintLayer *> all_layers;
    BKE_paint_layers_flatten(ma, all_layers);
    for (const MaterialPaintLayer *layer : all_layers) {
      if ((layer->flag & MA_PAINT_LAYER_ENABLED) != 0 &&
          removed_rows_contains(ma, layer->marker))
      {
        BKE_paint_layers_row_removed_clear(ma, layer->marker);
        undo_forces_rebuild = true;
      }
    }
  }
  const bool keep_root = !undo_forces_rebuild && !created_tree && have_stored_root &&
                         stored_root == root_hash;
  if (!keep_root && have_stored_root) {
    printf("paint layers regen diff: root old=%llx new=%llx undo=%d\n",
           static_cast<unsigned long long>(stored_root),
           static_cast<unsigned long long>(root_hash),
           int(undo_forces_rebuild));
  }
  if (keep_root) {
    /* The root's nodes, links and interface already match: discard the scratch and leave it. */
    if (scratch != nullptr) {
      BKE_id_free(&bmain, scratch);
    }
    BLI_assert_msg(root_hash == stored_root, "a kept root must match its own stored hash");
    BKE_ntree_update_tag_all(tree);
    BKE_ntree_update_after_single_tree_change(bmain, *tree);
  }
  else {
    /* Full root rebuild: the groups are current now, so the build only lays out the root skeleton
     * and reuses every group untouched. The rebuild is the only chance to drop rows the user has
     * disabled: record them, so the build leaves them out, and store the hash of what was built. */
    if (scratch != nullptr) {
      BKE_id_free(&bmain, scratch);
    }
    for (bNodeTree *layer_tree : used_layer_trees) {
      if (!old_layer_trees.contains(layer_tree)) {
        old_layer_trees.append(layer_tree);
      }
    }
    used_layer_trees.clear();
    unchanged_layers.clear();
    layer_trees.clear();
    removed_rows_reconcile(ma);
    tree_clear(bmain, *tree);
    paint_layers_tree_build(ma, *tree, ctx);
    for (bNodeTree *layer_tree : used_layer_trees) {
      BKE_ntree_update_tag_all(layer_tree);
      BKE_ntree_update_after_single_tree_change(bmain, *layer_tree);
      DEG_id_tag_update(&layer_tree->id, ID_RECALC_SYNC_TO_EVAL);
    }
    tree_root_hash_set(*tree, root_hash);
    BKE_ntree_update_tag_all(tree);
    BKE_ntree_update_after_single_tree_change(bmain, *tree);
  }
  printf("paint layers regen: root=%s groups_created=%d groups_deleted=%d source_groups_changed=%d "
         "total=%.2fms\n",
         keep_root ? "kept" : "rebuilt",
         int(groups_created),
         int(groups_deleted),
         int(source_groups_changed),
         (BLI_time_now_seconds() - regen_start) * 1000.0);
  std::function<void(const ListBase &, const char *)> log_layers =
      [&](const ListBase &list, const char *path) {
        for (const MaterialPaintLayer &layer :
             *reinterpret_cast<const ListBaseT<MaterialPaintLayer> *>(&list))
        {
          char full[192];
          SNPRINTF(full, "%s%s", path, layer.name);
          const char *state = "none";
          if (layer_trees.contains(&layer)) {
            state = unchanged_layers.contains(&layer) ? "kept" : "rebuilt";
          }
          printf("paint layers regen: %s '%s'=%s\n",
                 BKE_paint_layers_is_folder(layer) ? "folder" : "layer",
                 full,
                 state);
          if (BKE_paint_layers_is_folder(layer)) {
            char child_path[192];
            SNPRINTF(child_path, "%s/", full);
            log_layers(layer.children, child_path);
          }
        }
      };
  log_layers(ma.paint_layers, "");

  for (bNodeTree *layer_tree : old_layer_trees) {
    if (used_layer_trees.contains(layer_tree)) {
      continue;
    }
    /* The layer is gone and its group has no users; a tree the user linked keeps its users. */
    if (ID_REAL_USERS(&layer_tree->id) <= 0) {
      BKE_id_delete(&bmain, layer_tree);
      groups_deleted = true;
    }
  }
  for (bNodeTree *layer_tree : used_layer_trees) {
    /* The build asked for a group but did not instantiate it, so nothing owns it. */
    if (ID_REAL_USERS(&layer_tree->id) <= 0) {
      BKE_id_delete(&bmain, layer_tree);
      groups_deleted = true;
    }
  }
  /* Source-group wrappers are pruned by the same rule: no row of this owner reads their source any
   * more, so nothing keeps them. */
  source_groups_prune(bmain, ma);
  source_group_instances_log(ma, layer_trees, source_groups, &regen_cache);

  /* The instance: found by marker, re-pointed when it names a different tree. */
  bNode *instance = instance_find(ma, ma.paint_layers_owner_uid);
  if (instance == nullptr) {
    if (ma.nodetree->typeinfo == nullptr || ma.nodetree->typeinfo->group_idname == nullptr) {
      return false;
    }
    instance = bke::node_add_node(nullptr, *ma.nodetree, ma.nodetree->typeinfo->group_idname);
    if (instance == nullptr) {
      return false;
    }
    instance->location[0] = -200.0f;
    instance->location[1] = 0.0f;
    instance->id = &tree->id;
    id_us_plus(&tree->id);
    instance_uid_set(*instance, ma.paint_layers_owner_uid);
    /* A group assigned after the node was created only grows its sockets once the updater is told
     * the node's group changed; a plain tree update does not notice. */
    BKE_ntree_update_tag_node_property(ma.nodetree, instance);
  }
  else if (instance->id != &tree->id) {
    if (instance->id != nullptr) {
      id_us_min(instance->id);
    }
    instance->id = &tree->id;
    id_us_plus(&tree->id);
    instance_uid_set(*instance, ma.paint_layers_owner_uid);
    BKE_ntree_update_tag_node_property(ma.nodetree, instance);
  }
  BKE_ntree_update_after_single_tree_change(bmain, *ma.nodetree);

  /* The owner's own node tree is rewritten from here. Only a row that reads its owner as its source
   * could have cached an answer about it, but dropping it costs one resolve and is always right. */
  regen_cache.invalidate_for_owner(ma);
  principled_ensure(ma, report);
  wire_instance_to_material(ma, *tree, *instance, report);
  values_sync_with_cache(ma, &regen_cache);

  BKE_ntree_update_after_single_tree_change(bmain, *ma.nodetree);

  /* One calibration line per structural rebuild: it tells the user what to set the reserved sampler
   * budget from. `over` is the count above the budget, or zero when the check is off. The count is
   * taken only now, after the stack instance is wired to the Principled: only then is the generated
   * graph reachable from a Material Output, so the count sees the user's own nodes, the stack, every
   * layer group and the wrappers -- exactly what EEVEE will allocate. Comparing it with the
   * description estimate is the calibration check for the counter and the estimate. */
  /* Re-derive the estimate now that the build reconciled the removed rows: the fallback may have
   * dropped disabled rows, and only the post-build removed set knows them. Reported unconditionally
   * so a caller (and the tests) can compare it with the finished count. */
  sampler_estimate_value = sampler_estimate();
  report.sampler_estimate = sampler_estimate_value;
  if (!keep_root) {
    const int max_textures = sampler_runtime().max_textures;
    sampler_count = BKE_paint_layers_sampler_count(ma);
    const int estimate = sampler_estimate_value;
    const int over = (budget > 0 && sampler_count > budget) ? sampler_count - budget : 0;
    printf("paint layers samplers: material='%s' count=%d estimate=%d budget=%d max=%d over=%d "
           "fallback_rows=%d removed_hidden=%d%s.\n",
           ma.id.name + 2,
           sampler_count,
           estimate,
           budget,
           max_textures,
           over,
           fallback_rows,
           removed_hidden,
           (sampler_count != estimate) ? " MISMATCH" : "");
  }

  /* Debug-only: a kept root's interface must still be exactly what a full build would produce, so
   * its value inputs and every group's interface signature hash the same as before. values_sync
   * only writes socket values, never topology. */
  if (keep_root) {
    const uint64_t recheck = paint_layers_root_topology_hash(
        ma, wired_channels, layer_trees, &regen_cache);
    BLI_assert_msg(recheck == stored_root, "a kept root's interface drifted");
    UNUSED_VARS(recheck);
  }

  ma.paint_layers_flag &= ~MA_PAINT_LAYERS_REGEN;

  /* A Material row's source material is in the depsgraph only because this layered material's
   * `build_material` puts it there, so a source that appeared, disappeared or was swapped is a
   * change of the graph's relations. The generated tree itself may be untouched (the root is kept),
   * so this pass is the only one that notices; the stored set hash forces the rebuild when it
   * moved. */
  const uint64_t source_materials = paint_layers_source_materials_hash(ma);
  uint64_t stored_source_materials = 0;
  const bool have_stored_source_materials = tree_hash_get(*tree,
                                                          TREE_SOURCE_MATERIALS_LOW_PROP,
                                                          TREE_SOURCE_MATERIALS_HIGH_PROP,
                                                          stored_source_materials);
  const bool sources_changed = !have_stored_source_materials ||
                               stored_source_materials != source_materials;
  tree_hash_set(
      *tree, TREE_SOURCE_MATERIALS_LOW_PROP, TREE_SOURCE_MATERIALS_HIGH_PROP, source_materials);

  /* A wrapper and its nested path copies are IDs of their own. Without a relations tag the
   * evaluated layer group would keep referencing the old ID (the source is no longer in the graph
   * under that pointer), and without a SYNC_TO_EVAL tag its evaluated copy would keep the old
   * body; either way the viewport would not show the rebuilt wrapper. */
  if (source_groups_changed) {
    for (const auto item : source_groups.items()) {
      if (item.value != nullptr) {
        DEG_id_tag_update(&item.value->id, ID_RECALC_SYNC_TO_EVAL);
      }
      const int source_uid = int(item.key->id.session_uid);
      for (bNodeTree &wrapper_copy : bmain.nodetrees) {
        if (&wrapper_copy == item.value ||
            !BLI_uuid_equal(tree_owner_uid_get(wrapper_copy), ma.paint_layers_owner_uid) ||
            prop_int_get(wrapper_copy.id.properties, TREE_SOURCE_PROP, 0) != source_uid)
        {
          continue;
        }
        DEG_id_tag_update(&wrapper_copy.id, ID_RECALC_SYNC_TO_EVAL);
      }
    }
  }

  /* The embedded tree and the generated group were rewritten in place. A shading tag alone
   * re-evaluates the existing evaluated copies without re-copying them, so the viewport would keep
   * the group node's old sockets and never show the new rows; a final render, which builds its own
   * copies, would. Only an ID reference that appeared or disappeared is a relation change: a kept
   * root re-uses every group, so its relations are untouched and re-tagging them would force a
   * needless relations rebuild. */
  DEG_id_tag_update(&tree->id, ID_RECALC_SYNC_TO_EVAL);
  DEG_id_tag_update(&ma.id, ID_RECALC_SHADING | ID_RECALC_SYNC_TO_EVAL);
  if (groups_created || groups_deleted || created_tree || report.replaced_foreign_tree ||
      sources_changed || source_groups_changed)
  {
    DEG_relations_tag_update(&bmain);
    report.relations_changed = true;
  }
  if (r_report != nullptr) {
    *r_report = report;
  }
  return true;
}

void BKE_paint_layers_regenerate_tagged(Main &bmain, const PaintModeSettings *paint_mode)
{
  /* Rule K-1: regeneration creates IDs and writes node trees, so it may only run on the main
   * thread. #BKE_scene_graph_update_tagged and #BKE_scene_graph_update_for_newframe_ex are also
   * reached from render and preview worker threads, so the guard belongs here rather than at each
   * call site. */
  if (!BLI_thread_is_main()) {
    return;
  }
  /* Only a material that owns its description and its tree may be regenerated. A localized or
   * evaluated copy shares the pointer with the original (see #BKE_paint_layers_generate_copy_data),
   * and rebuilding it from a copy would rewrite the original's tree. */
  const int no_regen_tags = ID_TAG_LOCALIZED | ID_TAG_COPIED_ON_EVAL | ID_TAG_NO_MAIN;
  for (Material &ma : bmain.materials) {
    if ((ma.id.tag & no_regen_tags) != 0) {
      continue;
    }
    if (!paint_layers_is_layered(ma)) {
      continue;
    }
    /* Drain the bake subscriptions: a pixel edit to a source map shows up here and marks the
     * material for the planner, the same point the tree and slots are brought current. Then the
     * planner re-bakes the rows whose stored hash no longer matches, on the main thread. */
    BKE_paint_layers_bake_notice_changes(ma);
    BKE_paint_layers_bake_ensure(bmain, ma);
    const bool needs_regen = (ma.paint_layers_flag & MA_PAINT_LAYERS_REGEN) != 0 ||
                             ma.paint_layers_tree == nullptr;
    const bool needs_slots = (ma.paint_layers_flag & MA_PAINT_LAYERS_SLOTS_STALE) != 0;
    if (!needs_regen && !needs_slots) {
      continue;
    }
    if (needs_regen) {
      BKE_paint_layers_regenerate(bmain, ma);
    }
    if (needs_slots) {
      if (paint_mode != nullptr) {
        BKE_paint_layers_texpaint_slots_refresh(&ma, paint_mode);
      }
      else {
        /* No settings to rebuild from: drop the cache rather than leave dangling #Image*. */
        MEM_SAFE_DELETE(ma.texpaintslot);
        ma.tot_slots = 0;
        ma.paint_layers_flag &= ~MA_PAINT_LAYERS_SLOTS_STALE;
      }
    }
  }
}

namespace {

/** Write one value socket of \a instance from its row, or return false for an unknown role. */
bool values_sync_socket(Material &ma,
                        bNode &instance,
                        const bNodeTreeInterfaceSocket &iface,
                        bNodeSocket &socket,
                        const PaintLayersRegenCache *cache)
{
  const char *role = prop_string_get(iface.properties, INPUT_ROLE_PROP);
  if (role == nullptr) {
    return false;
  }
  bUUID marker = BLI_uuid_nil();
  if (!uid_prop_get(iface.properties, INPUT_MARKER_PROP, marker)) {
    return false;
  }
  const MaterialPaintLayer *layer = BKE_paint_layers_find(ma, marker);
  if (layer == nullptr || socket.default_value == nullptr) {
    return false;
  }
  if (STREQ(role, ROLE_OPACITY)) {
    const int channel = prop_int_get(iface.properties, INPUT_CHANNEL_PROP, -1);
    if (channel < 0) {
      return false;
    }
    static_cast<bNodeSocketValueFloat *>(socket.default_value)->value =
        BKE_paint_layers_channel_opacity_effective(*layer, channel) *
        pass_through_scale_of(ma, *layer, cache);
    return true;
  }
  if (STREQ(role, ROLE_FILL)) {
    const int channel = prop_int_get(iface.properties, INPUT_CHANNEL_PROP, -1);
    if (channel < 0) {
      return false;
    }
    float color[4];
    paint_layer_channel_constant(*layer, channel, color);
    if (bNodeSocketValueRGBA *value = static_cast<bNodeSocketValueRGBA *>(socket.default_value)) {
      copy_v4_v4(value->value, color);
    }
    return true;
  }
  if (STREQ(role, ROLE_CORRECTION_OPACITY)) {
    const int channel = prop_int_get(iface.properties, INPUT_CHANNEL_PROP, -1);
    if (channel < 0) {
      return false;
    }
    /* A mask correction carries the row opacity on every channel socket. */
    const bool mask_correction = BKE_paint_layers_role(*layer) == PaintLayerRole::MaskItem;
    static_cast<bNodeSocketValueFloat *>(socket.default_value)->value =
        mask_correction ? BKE_paint_layers_effective_opacity(*layer) :
                          BKE_paint_layers_channel_opacity_effective(*layer, channel);
    return true;
  }
  if (STREQ(role, ROLE_CORRECTION_FILL)) {
    float color[4];
    BKE_paint_layers_correction_constant(*layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, color);
    if (bNodeSocketValueRGBA *value = static_cast<bNodeSocketValueRGBA *>(socket.default_value)) {
      copy_v4_v4(value->value, color);
    }
    return true;
  }
  return false;
}

/**
 * Copy the description's values into \a instance's input sockets, which live in \a parent_tree, and
 * then recurse into the group's own tree for the layer groups nested in it. Every parent tree whose
 * sockets actually changed is collected in \a r_written.
 */
void values_sync_instance(Material &ma,
                          bNodeTree &parent_tree,
                          bNode &instance,
                          Set<bNodeTree *> &r_written,
                          const PaintLayersRegenCache *cache)
{
  bNodeTree *group_tree = id_cast<bNodeTree *>(instance.id);
  if (group_tree == nullptr) {
    return;
  }
  group_tree->ensure_interface_cache();
  bool wrote = false;
  for (bNodeTreeInterfaceSocket *iface : group_tree->interface_inputs()) {
    if (iface->identifier == nullptr) {
      continue;
    }
    bNodeSocket *socket = bke::node_find_socket(
        instance, SOCK_IN, UString::from_ptr_noinline(iface->identifier));
    if (socket == nullptr) {
      continue;
    }
    wrote |= values_sync_socket(ma, instance, *iface, *socket, cache);
  }
  if (wrote) {
    r_written.add(&parent_tree);
  }
  for (bNode &node : group_tree->nodes) {
    if (!node.is_group() || node.id == nullptr || GS(node.id->name) != ID_NT ||
        BKE_paint_material_is_normal_combine_group(node))
    {
      continue;
    }
    values_sync_instance(ma, *group_tree, node, r_written, cache);
  }
}

}  // namespace

void BKE_paint_layers_values_sync(Material &ma)
{
  values_sync_with_cache(ma, nullptr);
}

static void values_sync_with_cache(Material &ma, const PaintLayersRegenCache *cache)
{
  /* Custom parameters live on the description, not in the generated tree (a Custom layer has no
   * instance there); give missing ones their socket defaults before syncing the rest. */
  BKE_paint_layers_custom_properties_sync(ma);

  bNode *instance = instance_find(ma, ma.paint_layers_owner_uid);
  if (instance == nullptr || ma.nodetree == nullptr) {
    return;
  }
  /* The values live on each layer group's instance, in the root generated tree or a folder's tree,
   * never in the material's embedded tree any more (session 10e). */
  Set<bNodeTree *> written;
  values_sync_instance(ma, *ma.nodetree, *instance, written, cache);
  for (bNodeTree *tree : written) {
    DEG_id_tag_update(&tree->id, ID_RECALC_SYNC_TO_EVAL);
  }
}

namespace {

/** The `COLOR:<CHANNEL>` / `COVERAGE` role string a wrapper output carries for \a channel. */
const char *source_group_socket_type(const eMaterialPaintChannel channel)
{
  if (channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
    return "NodeSocketVector";
  }
  return BKE_paint_material_channel_info(channel).is_color ? "NodeSocketColor" : "NodeSocketFloat";
}

/** The longest group path the wrapper walks before refusing; guards corrupt data. */
constexpr int PAINT_LAYERS_SOURCE_GROUP_MAX_DEPTH = 8;

/**
 * Add or find an output socket by name, reusing the existing one so its identifier survives. The
 * role is set only when non-null: `COLOR:<CHANNEL>`/`COVERAGE` is the wrapper's outward contract
 * and belongs on the root alone, never on the intermediate group copies.
 */
bNodeTreeInterfaceSocket *source_group_interface_output(bNodeTree &group,
                                                        const char *name,
                                                        const char *socket_type,
                                                        const char *role)
{
  group.ensure_interface_cache();
  for (bNodeTreeInterfaceSocket *socket : group.interface_outputs()) {
    if (socket->name != nullptr && STREQ(socket->name, name)) {
      if (role != nullptr) {
        prop_string_set(socket->properties, PAINT_LAYERS_CUSTOM_ROLE_PROP, role);
      }
      return socket;
    }
  }
  bNodeTreeInterfaceSocket *socket = group.tree_interface.add_socket(
      name, "", socket_type, NODE_INTERFACE_SOCKET_OUTPUT, nullptr);
  if (socket != nullptr && role != nullptr) {
    prop_string_set(socket->properties, PAINT_LAYERS_CUSTOM_ROLE_PROP, role);
  }
  return socket;
}

bNodeTreeInterfaceSocket *source_group_interface_output_find(bNodeTree &group, const char *name)
{
  group.ensure_interface_cache();
  for (bNodeTreeInterfaceSocket *socket : group.interface_outputs()) {
    if (socket->name != nullptr && STREQ(socket->name, name)) {
      return socket;
    }
  }
  return nullptr;
}

/** One channel the wrapper exposes: a colour channel, or the coverage from Alpha. */
struct SourceGroupChannel {
  int channel;
  /** The Principled input the value is read from ("Base Color", "Alpha", ...). */
  const char *principled_socket;
  /** The display name the root wrapper's output socket takes. */
  const char *ui_name;
  bool coverage;
};

Vector<SourceGroupChannel> source_group_channels(const bNode &principled)
{
  Vector<SourceGroupChannel> channels;
  for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
    if (info.socket_name == nullptr) {
      continue;
    }
    if (bke::node_find_socket(const_cast<bNode &>(principled),
                              SOCK_IN,
                              UString::from_ptr_noinline(info.socket_name)) != nullptr)
    {
      channels.append({int(info.channel), info.socket_name, info.ui_name, false});
    }
  }
  if (bke::node_find_socket(const_cast<bNode &>(principled), SOCK_IN, "Alpha"_ustr) != nullptr) {
    channels.append({int(PAINT_MATERIAL_CHANNEL_ALPHA), "Alpha", "Coverage", true});
  }
  return channels;
}

/** The deterministic output name of \a spec: the contract name at the root, a private one below. */
void source_group_output_name(const SourceGroupChannel &spec,
                              const bool is_root,
                              char r_name[160])
{
  if (is_root) {
    BLI_snprintf(r_name, 160, "%s", spec.ui_name);
  }
  else {
    BLI_snprintf(r_name, 160, ".PL %s", spec.ui_name);
  }
}

/** The outward role string of \a spec, written into \a r_role. */
const char *source_group_role(const SourceGroupChannel &spec, char r_role[128])
{
  if (spec.coverage) {
    BLI_snprintf(r_role, 128, "COVERAGE");
    return r_role;
  }
  const char *identifier = BKE_paint_layers_custom_channel_identifier(spec.channel);
  BLI_snprintf(r_role, 128, "COLOR:%s", identifier != nullptr ? identifier : "");
  return r_role;
}

/** Copy \a src's default into \a dst when they are the same socket type. */
void socket_default_copy(bNodeSocket &dst, const bNodeSocket &src)
{
  if (dst.default_value == nullptr || src.default_value == nullptr || dst.type != src.type) {
    return;
  }
  switch (src.type) {
    case SOCK_FLOAT:
      *static_cast<bNodeSocketValueFloat *>(dst.default_value) =
          *static_cast<const bNodeSocketValueFloat *>(src.default_value);
      break;
    case SOCK_RGBA:
      copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(dst.default_value)->value,
                 static_cast<const bNodeSocketValueRGBA *>(src.default_value)->value);
      break;
    case SOCK_VECTOR:
      copy_v3_v3(static_cast<bNodeSocketValueVector *>(dst.default_value)->value,
                 static_cast<const bNodeSocketValueVector *>(src.default_value)->value);
      break;
    default:
      break;
  }
}

/** Copy \a src's nodes and links into \a dst (cleared first), mapping originals to copies. */
bool source_group_copy_nodes(Main &bmain,
                             bNodeTree &dst,
                             const bNodeTree &src,
                             Map<const bNode *, bNode *> &r_node_map)
{
  tree_clear_nodes(bmain, dst);
  Map<const bNodeSocket *, bNodeSocket *> socket_map;
  for (const bNode &node : src.nodes) {
    if (node.type_legacy == SH_NODE_OUTPUT_MATERIAL) {
      continue;
    }
    bNode *copy = bke::node_copy_with_mapping(
        &dst, node, 0, std::nullopt, std::nullopt, socket_map);
    if (copy == nullptr) {
      return false;
    }
    r_node_map.add(&node, copy);
  }
  /* `node_copy_with_mapping` does not remap `node->parent` (a Frame node), so a copied node would
   * otherwise keep a pointer into the source tree; a later tree copy would walk it and crash. A
   * parent outside the copied set (a skipped Material Output is no parent, but be safe) becomes
   * null. Frame nodes themselves are copied -- only Material Output is skipped -- so nesting,
   * including frames inside frames, is preserved. */
  for (const auto item : r_node_map.items()) {
    item.value->parent = r_node_map.lookup_default(item.key->parent, nullptr);
  }
  for (const bNodeLink &link : src.links) {
    bNode *from = r_node_map.lookup_default(link.fromnode, nullptr);
    bNode *to = r_node_map.lookup_default(link.tonode, nullptr);
    bNodeSocket *from_socket = socket_map.lookup_default(link.fromsock, nullptr);
    bNodeSocket *to_socket = socket_map.lookup_default(link.tosock, nullptr);
    if (from == nullptr || to == nullptr || from_socket == nullptr || to_socket == nullptr) {
      continue;
    }
    bke::node_add_link(dst, *from, *from_socket, *to, *to_socket);
  }
  /* The reference copy path (#ntree_copy_data) ensures every node's declaration once the links are
   * in place; do the same so a copied node's sockets are usable in this pass. */
  for (const auto item : r_node_map.items()) {
    bke::node_declaration_ensure(dst, *item.value);
  }
  return true;
}

/** The Group Output of \a tree, created when the copied tree has none (a material root). */
bNode *source_group_group_output(bNodeTree &tree)
{
  for (bNode &node : tree.nodes) {
    if (node.is_group_output()) {
      return &node;
    }
  }
  return bke::node_add_node(nullptr, tree, "NodeGroupOutput"_ustr);
}

/** Expose \a channels in \a tree, reading them straight from \a principled. */
bool source_group_wire_principled(bNodeTree &tree,
                                  bNode &principled,
                                  const Vector<SourceGroupChannel> &channels,
                                  const bool is_root)
{
  bNode *group_output = source_group_group_output(tree);
  if (group_output == nullptr) {
    return false;
  }
  Vector<bNodeTreeInterfaceSocket *> ifaces;
  for (const SourceGroupChannel &spec : channels) {
    char name[160];
    source_group_output_name(spec, is_root, name);
    char role[128];
    ifaces.append(source_group_interface_output(
        tree,
        name,
        source_group_socket_type(eMaterialPaintChannel(spec.channel)),
        is_root ? source_group_role(spec, role) : nullptr));
  }
  nodes::update_node_declaration_and_sockets(tree, *group_output);
  for (const int i : channels.index_range()) {
    if (ifaces[i] == nullptr || ifaces[i]->identifier == nullptr) {
      continue;
    }
    bNodeSocket *out_in = bke::node_find_socket(
        *group_output, SOCK_IN, UString::from_ptr_noinline(ifaces[i]->identifier));
    if (out_in == nullptr) {
      continue;
    }
    const bNodeSocket *input = bke::node_find_socket(
        principled, SOCK_IN, UString::from_ptr_noinline(channels[i].principled_socket));
    if (input == nullptr) {
      continue;
    }
    const Span<const bNodeLink *> links = input->directly_linked_links();
    if (!links.is_empty()) {
      bke::node_add_link(tree, *links[0]->fromnode, *links[0]->fromsock, *group_output, *out_in);
    }
    else {
      socket_default_copy(*out_in, *input);
    }
  }
  return true;
}

/** Expose \a channels in \a tree from \a instance, whose group is \a child. */
bool source_group_wire_instance(bNodeTree &tree,
                                bNode &instance,
                                bNodeTree &child,
                                const Vector<SourceGroupChannel> &channels,
                                const bool is_root)
{
  bNode *group_output = source_group_group_output(tree);
  if (group_output == nullptr) {
    return false;
  }
  Vector<bNodeTreeInterfaceSocket *> ifaces;
  for (const SourceGroupChannel &spec : channels) {
    char name[160];
    source_group_output_name(spec, is_root, name);
    char role[128];
    ifaces.append(source_group_interface_output(
        tree,
        name,
        source_group_socket_type(eMaterialPaintChannel(spec.channel)),
        is_root ? source_group_role(spec, role) : nullptr));
  }
  nodes::update_node_declaration_and_sockets(tree, *group_output);
  for (const int i : channels.index_range()) {
    if (ifaces[i] == nullptr || ifaces[i]->identifier == nullptr) {
      continue;
    }
    char child_name[160];
    source_group_output_name(channels[i], false, child_name);
    bNodeTreeInterfaceSocket *child_iface = source_group_interface_output_find(child, child_name);
    if (child_iface == nullptr || child_iface->identifier == nullptr) {
      continue;
    }
    bNodeSocket *instance_out = bke::node_find_socket(
        instance, SOCK_OUT, UString::from_ptr_noinline(child_iface->identifier));
    bNodeSocket *out_in = bke::node_find_socket(
        *group_output, SOCK_IN, UString::from_ptr_noinline(ifaces[i]->identifier));
    if (instance_out != nullptr && out_in != nullptr) {
      bke::node_add_link(tree, instance, *instance_out, *group_output, *out_in);
    }
  }
  return true;
}

/**
 * Rebuild \a tree as a copy of \a orig_tree and expose \a channels through it. For every group on
 * \a path the shared original group in the copy is replaced by a private copy (marked with
 * #TREE_SOURCE_PROP so #source_groups_prune collects it), the recursion descends, and each level
 * propagates the child's new outputs up to its own Group Output. Only \a is_root carries the
 * `COLOR:<CHANNEL>`/`COVERAGE` roles.
 */
bool source_group_build_level(Main &bmain,
                              bNodeTree &tree,
                              const bNodeTree &orig_tree,
                              const Vector<const bNode *> &path,
                              const int depth,
                              const bNode *principled,
                              const Vector<SourceGroupChannel> &channels,
                              const bool is_root,
                              const bUUID &owner_uid,
                              const int source_uid,
                              const char *source_name)
{
  Map<const bNode *, bNode *> node_map;
  if (!source_group_copy_nodes(bmain, tree, orig_tree, node_map)) {
    return false;
  }
  /* `source_group_copy_nodes` lays the copied links down but does not build the link runtime;
   * `source_group_wire_principled` reads `directly_linked_links`, so without this the copied
   * Principled's source is not seen and the Group Output keeps the socket defaults instead of the
   * values the group is fed. */
  tree.ensure_topology_cache();
  if (depth >= int(path.size())) {
    bNode *copied_principled = node_map.lookup_default(principled, nullptr);
    if (copied_principled == nullptr) {
      return false;
    }
    return source_group_wire_principled(tree, *copied_principled, channels, is_root);
  }
  const bNode *orig_group = path[depth];
  bNode *instance = node_map.lookup_default(orig_group, nullptr);
  if (instance == nullptr || orig_group->id == nullptr || GS(orig_group->id->name) != ID_NT) {
    return false;
  }
  const bNodeTree *orig_child = reinterpret_cast<const bNodeTree *>(orig_group->id);
  if (orig_child == nullptr) {
    return false;
  }
  char child_name[MAX_ID_NAME - 2];
  SNPRINTF(child_name, ".PL Source %s %s", source_name, orig_child->id.name + 2);
  bNodeTree *child = bke::node_tree_add_tree(&bmain, child_name, "ShaderNodeTree");
  if (child == nullptr) {
    return false;
  }
  /* The copy's Group Input and the parent instance's input sockets are built from the group's
   * interface, and a fresh tree has only an empty one. Copy the source group's before the body is
   * built, or the copied Group Input has no sockets, the links into it are lost and the Principled
   * falls back to its defaults. The copy is an ID of its own that references any pointer default
   * value (Image/Object/Material) itself, so it takes its own user reference: flag 0, as
   * `ntree_copy_data` does. `free_data` drops the empty interface the fresh tree was born with
   * first; `copy_data` re-tags the item cache itself. */
  child->tree_interface.free_data();
  child->tree_interface.copy_data(orig_child->tree_interface, 0);
  /* The instance is the only user; the copy starts owning its own text-less tree. */
  id_us_min(&child->id);
  tree_owner_uid_set(*child, owner_uid);
  prop_int_set(child->id.properties, TREE_SOURCE_PROP, source_uid);
  if (instance->id != nullptr) {
    id_us_min(instance->id);
  }
  instance->id = &child->id;
  id_us_plus(&child->id);
  if (!source_group_build_level(bmain,
                                *child,
                                *orig_child,
                                path,
                                depth + 1,
                                principled,
                                channels,
                                false,
                                owner_uid,
                                source_uid,
                                source_name))
  {
    return false;
  }
  /* The child gained its outputs; the instance only sees them after a declaration update. */
  nodes::update_node_declaration_and_sockets(tree, *instance);
  return source_group_wire_instance(tree, *instance, *child, channels, is_root);
}

/**
 * Rebuild \a group's body as a copy of \a source's tree with a Group Output in place of every
 * Material Output, then expose the Principled's channels, walking \a path through any nested
 * groups. The interface is left in place (outputs are reused by name), so a rebuild keeps the
 * socket identifiers an instance would link into.
 */
bool source_group_build(Main &bmain,
                        bNodeTree &group,
                        Material &owner,
                        const Material &source,
                        const bNode *principled,
                        const Vector<const bNode *> &path,
                        const int source_uid)
{
  source.nodetree->ensure_topology_cache();
  const Vector<SourceGroupChannel> channels = source_group_channels(*principled);
  return source_group_build_level(bmain,
                                  group,
                                  *source.nodetree,
                                  path,
                                  0,
                                  principled,
                                  channels,
                                  true,
                                  owner.paint_layers_owner_uid,
                                  source_uid,
                                  source.id.name + 2);
}

/** Delete the wrapper trees of \a owner whose source is no longer referenced by any row. */
void source_groups_prune(Main &bmain, const Material &owner)
{
  Set<uint32_t> referenced;
  Vector<const MaterialPaintLayer *> layers;
  BKE_paint_layers_flatten(owner, layers);
  for (const MaterialPaintLayer *layer : layers) {
    if (layer->kind == MA_PAINT_LAYER_KIND_MATERIAL && layer->material != nullptr) {
      referenced.add(layer->material->id.session_uid);
    }
  }
  Vector<bNodeTree *> orphans;
  for (bNodeTree &tree : bmain.nodetrees) {
    if (!BLI_uuid_equal(tree_owner_uid_get(tree), owner.paint_layers_owner_uid)) {
      continue;
    }
    const int source_uid = prop_int_get(tree.id.properties, TREE_SOURCE_PROP, 0);
    if (source_uid == 0 || referenced.contains(uint32_t(source_uid))) {
      continue;
    }
    orphans.append(&tree);
  }
  /* Several passes: deleting a wrapper frees its instance nodes, which drops the user count of the
   * path group copies and lets the next pass delete those too. */
  for (int pass = 0; pass < PAINT_LAYERS_SOURCE_GROUP_MAX_DEPTH && !orphans.is_empty(); pass++) {
    Vector<bNodeTree *> remaining;
    bool deleted = false;
    for (bNodeTree *tree : orphans) {
      if (ID_REAL_USERS(&tree->id) <= 0) {
        BKE_id_delete(&bmain, tree);
        deleted = true;
      }
      else {
        remaining.append(tree);
      }
    }
    orphans = std::move(remaining);
    if (!deleted) {
      break;
    }
  }
}

}  // namespace

/* -------------------------------------------------------------------- */
/** \name Source Group Value Sync
 *
 * A value edit in the source updates the existing wrapper in place: a rebuild would replace the
 * private path copies, recompile the shader and lose the generated group's identity. Only a
 * topology edit rebuilds. Nodes match by name and sockets by identifier, both of which the copy
 * preserves; groups off the path to the Principled are shared with the source and need no sync.
 * \{ */

/** Copy one scalar socket value; true when it changed. Void pointers because both #bNodeSocket and
 * #bNodeTreeInterfaceSocket store the same #bNodeSocketValue* behind them. */
static bool source_group_value_copy(const eNodeSocketDatatype type, void *dst_data, const void *src_data)
{
  if (dst_data == nullptr || src_data == nullptr) {
    return false;
  }
  switch (type) {
    case SOCK_FLOAT: {
      bNodeSocketValueFloat &dst = *static_cast<bNodeSocketValueFloat *>(dst_data);
      const bNodeSocketValueFloat &src = *static_cast<const bNodeSocketValueFloat *>(src_data);
      if (dst.value == src.value && dst.subtype == src.subtype) {
        return false;
      }
      dst = src;
      return true;
    }
    case SOCK_INT: {
      bNodeSocketValueInt &dst = *static_cast<bNodeSocketValueInt *>(dst_data);
      const bNodeSocketValueInt &src = *static_cast<const bNodeSocketValueInt *>(src_data);
      if (dst.value == src.value && dst.subtype == src.subtype) {
        return false;
      }
      dst = src;
      return true;
    }
    case SOCK_BOOLEAN: {
      bNodeSocketValueBoolean &dst = *static_cast<bNodeSocketValueBoolean *>(dst_data);
      const bNodeSocketValueBoolean &src = *static_cast<const bNodeSocketValueBoolean *>(src_data);
      if (dst.value == src.value) {
        return false;
      }
      dst = src;
      return true;
    }
    case SOCK_VECTOR: {
      bNodeSocketValueVector &dst = *static_cast<bNodeSocketValueVector *>(dst_data);
      const bNodeSocketValueVector &src = *static_cast<const bNodeSocketValueVector *>(src_data);
      if (dst.dimensions == src.dimensions && equals_v3v3(dst.value, src.value)) {
        return false;
      }
      dst = src;
      return true;
    }
    case SOCK_INT_VECTOR: {
      bNodeSocketValueIntVector &dst = *static_cast<bNodeSocketValueIntVector *>(dst_data);
      const bNodeSocketValueIntVector &src =
          *static_cast<const bNodeSocketValueIntVector *>(src_data);
      if (dst.dimensions == src.dimensions && dst.value[0] == src.value[0] &&
          dst.value[1] == src.value[1] && dst.value[2] == src.value[2])
      {
        return false;
      }
      dst = src;
      return true;
    }
    case SOCK_RGBA: {
      bNodeSocketValueRGBA &dst = *static_cast<bNodeSocketValueRGBA *>(dst_data);
      const bNodeSocketValueRGBA &src = *static_cast<const bNodeSocketValueRGBA *>(src_data);
      if (equals_v4v4(dst.value, src.value)) {
        return false;
      }
      dst = src;
      return true;
    }
    case SOCK_ROTATION: {
      bNodeSocketValueRotation &dst = *static_cast<bNodeSocketValueRotation *>(dst_data);
      const bNodeSocketValueRotation &src = *static_cast<const bNodeSocketValueRotation *>(src_data);
      if (equals_v3v3(dst.value_euler, src.value_euler)) {
        return false;
      }
      dst = src;
      return true;
    }
    case SOCK_STRING: {
      bNodeSocketValueString &dst = *static_cast<bNodeSocketValueString *>(dst_data);
      const bNodeSocketValueString &src = *static_cast<const bNodeSocketValueString *>(src_data);
      if (STREQ(dst.value, src.value)) {
        return false;
      }
      BLI_strncpy(dst.value, src.value, sizeof(dst.value));
      return true;
    }
    case SOCK_MENU: {
      /* Only the chosen value; the rest of the struct is a runtime pointer to the enum items. */
      bNodeSocketValueMenu &dst = *static_cast<bNodeSocketValueMenu *>(dst_data);
      const bNodeSocketValueMenu &src = *static_cast<const bNodeSocketValueMenu *>(src_data);
      if (dst.value == src.value) {
        return false;
      }
      dst.value = src.value;
      return true;
    }
    default:
      return false;
  }
}

/** Copy \a src's non-structural `node->id` into \a dst for a leaf node; true when it changed. A
 * group's `id` is the path copy the wrapper owns, never the source's group, so it is left alone. */
static bool source_group_node_id_copy(bNode &dst, const bNode &src)
{
  if (src.is_group() || dst.id == src.id) {
    return false;
  }
  if (dst.id != nullptr) {
    id_us_min(dst.id);
  }
  dst.id = src.id;
  if (dst.id != nullptr) {
    id_us_plus(dst.id);
  }
  return true;
}

/** Copy \a src's node storage into \a dst through the type's own copy and free callbacks, so a
 * #CurveMapping and the like keep their API allocation. True when it changed. */
static bool source_group_node_storage_copy(bNodeTree &dst_tree, bNode &dst, const bNode &src)
{
  if (src.typeinfo == nullptr || src.typeinfo->copyfunc == nullptr) {
    return false;
  }
  /* Compare first: a node whose storage already matches must not be freed and re-copied, or the
   * sync would tag it (and its tree) on every value edit of a sibling. */
  if (BKE_paint_layers_source_node_storage_hash(dst) ==
      BKE_paint_layers_source_node_storage_hash(src))
  {
    return false;
  }
  if (src.storage == nullptr) {
    if (dst.storage == nullptr) {
      return false;
    }
    if (src.typeinfo->freefunc != nullptr) {
      src.typeinfo->freefunc(&dst);
    }
    dst.storage = nullptr;
    return true;
  }
  if (dst.storage != nullptr) {
    if (src.typeinfo->freefunc == nullptr) {
      /* The old storage cannot be released, so a copy would leak it: leave it as it was. */
      return false;
    }
    src.typeinfo->freefunc(&dst);
    dst.storage = nullptr;
  }
  src.typeinfo->copyfunc(&dst_tree, &dst, &src);
  return dst.storage != nullptr;
}

/** Refresh the wrapper's channel constants from an already-synced copied Principled. A channel
 * whose Principled input is linked is fed through that link and needs no default; an unlinked one
 * is read from the Group Output's default, which the build copied once and the sync must renew. */
static bool source_group_refresh_channel_defaults(bNodeTree &dst_tree,
                                                  bNode &principled_copy,
                                                  const Vector<SourceGroupChannel> &channels,
                                                  const bool is_root)
{
  bool changed = false;
  bNode *group_output = source_group_group_output(dst_tree);
  if (group_output == nullptr) {
    return false;
  }
  dst_tree.ensure_topology_cache();
  for (const SourceGroupChannel &spec : channels) {
    char name[160];
    source_group_output_name(spec, is_root, name);
    bNodeTreeInterfaceSocket *iface = source_group_interface_output_find(dst_tree, name);
    if (iface == nullptr || iface->identifier == nullptr) {
      continue;
    }
    bNodeSocket *out_in = bke::node_find_socket(
        *group_output, SOCK_IN, UString::from_ptr_noinline(iface->identifier));
    bNodeSocket *input = bke::node_find_socket(
        principled_copy, SOCK_IN, UString::from_ptr_noinline(spec.principled_socket));
    if (out_in == nullptr || input == nullptr || !input->directly_linked_links().is_empty()) {
      continue;
    }
    if (source_group_value_copy(out_in->type, out_in->default_value, input->default_value)) {
      BKE_ntree_update_tag_node_property(&dst_tree, group_output);
      changed = true;
    }
  }
  return changed;
}

/**
 * Copy every value of \a src_tree into the matching nodes of \a dst_tree, once each, then recurse
 * into the private copies of the groups on the path to the Principled. Matching is by node name and
 * socket identifier, which #source_group_copy_nodes preserves. Returns the number of nodes changed.
 */
static int source_group_values_sync_tree(Main &bmain,
                                         const bNodeTree &src_tree,
                                         bNodeTree &dst_tree,
                                         const bNode *principled,
                                         const Vector<SourceGroupChannel> &channels,
                                         const bool is_root)
{
  int synced = 0;
  bool tree_changed = false;
  Map<StringRefNull, bNode *> by_name;
  for (bNode &node : dst_tree.nodes) {
    by_name.add(node.name, &node);
  }
  for (const bNode *src : src_tree.all_nodes()) {
    bNode *dst = by_name.lookup_default(src->name, nullptr);
    if (dst == nullptr || dst->type_legacy != src->type_legacy) {
      continue;
    }
    bool self_changed = false;
    if (dst->custom1 != src->custom1) {
      dst->custom1 = src->custom1;
      self_changed = true;
    }
    if (dst->custom2 != src->custom2) {
      dst->custom2 = src->custom2;
      self_changed = true;
    }
    if (dst->custom3 != src->custom3) {
      dst->custom3 = src->custom3;
      self_changed = true;
    }
    if (dst->custom4 != src->custom4) {
      dst->custom4 = src->custom4;
      self_changed = true;
    }
    self_changed |= source_group_node_id_copy(*dst, *src);
    self_changed |= source_group_node_storage_copy(dst_tree, *dst, *src);
    for (const bNodeSocket &src_sock : src->inputs) {
      bNodeSocket *dst_sock = bke::node_find_socket(
          *dst, SOCK_IN, src_sock.identifier_ustr());
      if (dst_sock != nullptr &&
          source_group_value_copy(dst_sock->type, dst_sock->default_value, src_sock.default_value))
      {
        self_changed = true;
      }
    }
    for (const bNodeSocket &src_sock : src->outputs) {
      bNodeSocket *dst_sock = bke::node_find_socket(
          *dst, SOCK_OUT, src_sock.identifier_ustr());
      if (dst_sock != nullptr &&
          source_group_value_copy(dst_sock->type, dst_sock->default_value, src_sock.default_value))
      {
        self_changed = true;
      }
    }
    if (self_changed) {
      BKE_ntree_update_tag_node_property(&dst_tree, dst);
      synced++;
      tree_changed = true;
    }
    if (src == principled) {
      /* The channel constants the wrapper reads are copies of this Principled's inputs. */
      self_changed |= source_group_refresh_channel_defaults(dst_tree, *dst, channels, is_root);
    }
    if (src->is_group() && dst->is_group() && src->id != nullptr && dst->id != nullptr &&
        src->id != dst->id)
    {
      const bNodeTree *src_child = id_cast<const bNodeTree *>(src->id);
      bNodeTree *dst_child = id_cast<bNodeTree *>(dst->id);
      if (src_child != nullptr && dst_child != nullptr) {
        synced += source_group_values_sync_tree(
            bmain, *src_child, *dst_child, principled, channels, false);
      }
    }
  }
  /* Group interface inputs move too; the wrapper's own outputs are its channel contract and are
   * never touched. A missing identifier means the interface changed and a rebuild handles it. */
  src_tree.ensure_interface_cache();
  dst_tree.ensure_interface_cache();
  Map<StringRefNull, bNodeTreeInterfaceSocket *> iface_by_id;
  for (bNodeTreeInterfaceSocket *socket : dst_tree.interface_inputs()) {
    if (socket->identifier != nullptr) {
      iface_by_id.add(socket->identifier, socket);
    }
  }
  for (const bNodeTreeInterfaceSocket *src_socket : src_tree.interface_inputs()) {
    if (src_socket->identifier == nullptr) {
      continue;
    }
    bNodeTreeInterfaceSocket *dst_socket = iface_by_id.lookup_default(src_socket->identifier,
                                                                      nullptr);
    const bke::bNodeSocketType *src_type =
        src_socket->socket_typeinfo();
    if (dst_socket != nullptr && src_type != nullptr) {
      if (source_group_value_copy(
              src_type->type, dst_socket->socket_data, src_socket->socket_data))
      {
        tree_changed = true;
      }
    }
  }
  /* Only a tree that actually changed is re-declared and pushed to its evaluated copy; a sync that
   * matched every value leaves the tree (and its users) untouched. */
  if (tree_changed) {
    BKE_ntree_update_after_single_tree_change(bmain, dst_tree);
    DEG_id_tag_update(&dst_tree.id, ID_RECALC_SYNC_TO_EVAL);
  }
  return synced;
}

/** \} */

bNodeTree *BKE_paint_layers_source_group_ensure(Main &bmain,
                                                Material &owner,
                                                Material &source,
                                                PaintLayersSourceGroupRefusal &r_refusal,
                                                bool *r_changed,
                                                bool *r_values_synced)
{
  if (r_changed != nullptr) {
    *r_changed = false;
  }
  if (r_values_synced != nullptr) {
    *r_values_synced = false;
  }
  r_refusal = PaintLayersSourceGroupRefusal::None;
  if (&source == &owner) {
    r_refusal = PaintLayersSourceGroupRefusal::SelfReference;
    return nullptr;
  }
  if (source.nodetree == nullptr) {
    r_refusal = PaintLayersSourceGroupRefusal::NoNodeTree;
    return nullptr;
  }
  ChannelUnavailableReason reason = ChannelUnavailableReason::None;
  Vector<const bNode *> group_path;
  const bNode *principled = BKE_paint_material_principled_find(source, reason, &group_path);
  if (principled == nullptr) {
    r_refusal = PaintLayersSourceGroupRefusal::NoPrincipled;
    return nullptr;
  }
  if (int(group_path.size()) > PAINT_LAYERS_SOURCE_GROUP_MAX_DEPTH) {
    r_refusal = PaintLayersSourceGroupRefusal::PrincipledInGroup;
    return nullptr;
  }

  if (BLI_uuid_is_nil(owner.paint_layers_owner_uid)) {
    owner.paint_layers_owner_uid = BLI_uuid_generate_random();
  }
  const uint64_t source_values = BKE_paint_layers_source_material_tree_hash(source);
  const uint64_t source_topology = BKE_paint_layers_source_material_topology_hash(source);
  const int source_uid = int(source.id.session_uid);
  char name[MAX_ID_NAME - 2];
  SNPRINTF(name, ".PL Source %s", source.id.name + 2);

  /* The cache key is (owner, source): two owners wrapping one source get their own groups. */
  bNodeTree *existing = nullptr;
  for (bNodeTree &tree : bmain.nodetrees) {
    if (!BLI_uuid_equal(tree_owner_uid_get(tree), owner.paint_layers_owner_uid)) {
      continue;
    }
    if (prop_int_get(tree.id.properties, TREE_SOURCE_PROP, 0) != source_uid) {
      continue;
    }
    existing = &tree;
    break;
  }
  if (existing != nullptr) {
    uint64_t stored_topology = 0;
    uint64_t stored_values = 0;
    const bool have_topology = tree_hash_get(
        *existing, TREE_SOURCE_HASH_LOW_PROP, TREE_SOURCE_HASH_HIGH_PROP, stored_topology);
    const bool have_values = tree_hash_get(
        *existing, TREE_SOURCE_VALUES_LOW_PROP, TREE_SOURCE_VALUES_HIGH_PROP, stored_values);
    if (have_topology && stored_topology == source_topology) {
      if (have_values && stored_values == source_values) {
        return existing;
      }
      /* Same topology, new values: copy them into the existing copies. No rebuild, so the wrapper
       * keeps its ID, its nodes and its generated interface, and the shader is not recompiled. */
      const Vector<SourceGroupChannel> channels = source_group_channels(*principled);
      const int synced = source_group_values_sync_tree(
          bmain, *source.nodetree, *existing, principled, channels, true);
      tree_hash_set(
          *existing, TREE_SOURCE_VALUES_LOW_PROP, TREE_SOURCE_VALUES_HIGH_PROP, source_values);
      DEG_id_tag_update(&owner.id, ID_RECALC_SYNC_TO_EVAL);
      printf("paint layers: source group '%s' for owner '%s' values synced nodes=%d\n",
             source.id.name + 2,
             owner.id.name + 2,
             synced);
      if (r_values_synced != nullptr) {
        *r_values_synced = true;
      }
      return existing;
    }
    /* The rebuild replaces the copy of every group on the path with a fresh one, so the previous
     * copies lose their only user (the instance node that pointed at them) and would leak. Collect
     * them now, before the build; delete the emptied ones once it is done. */
    Vector<bNodeTree *> old_copies;
    for (bNodeTree &tree : bmain.nodetrees) {
      if (&tree == existing ||
          !BLI_uuid_equal(tree_owner_uid_get(tree), owner.paint_layers_owner_uid) ||
          prop_int_get(tree.id.properties, TREE_SOURCE_PROP, 0) != source_uid)
      {
        continue;
      }
      old_copies.append(&tree);
    }
    if (!source_group_build(bmain, *existing, owner, source, principled, group_path, source_uid)) {
      r_refusal = PaintLayersSourceGroupRefusal::BuildFailed;
      return nullptr;
    }
    tree_hash_set(*existing, TREE_SOURCE_HASH_LOW_PROP, TREE_SOURCE_HASH_HIGH_PROP, source_topology);
    tree_hash_set(
        *existing, TREE_SOURCE_VALUES_LOW_PROP, TREE_SOURCE_VALUES_HIGH_PROP, source_values);
    if (!STREQ(existing->id.name + 2, name)) {
      BKE_id_rename(bmain, existing->id, name);
    }
    /* Several passes: deleting a copy frees the instance nodes inside it, which drops the user of
     * the next copy down and lets the following pass delete that one too. A new copy is still
     * referenced by its instance, so its real users keep it. */
    int removed_copies = 0;
    for (int pass = 0; pass < PAINT_LAYERS_SOURCE_GROUP_MAX_DEPTH && !old_copies.is_empty(); pass++) {
      Vector<bNodeTree *> remaining;
      bool deleted = false;
      for (bNodeTree *copy : old_copies) {
        if (ID_REAL_USERS(&copy->id) <= 0) {
          BKE_id_delete(&bmain, copy);
          deleted = true;
          removed_copies++;
        }
        else {
          remaining.append(copy);
        }
      }
      old_copies = std::move(remaining);
      if (!deleted) {
        break;
      }
    }
    printf("paint layers: source group '%s' for owner '%s' %s hash=%llx path_depth=%d "
           "removed_copies=%d\n",
           source.id.name + 2,
           owner.id.name + 2,
           "rebuilt",
           static_cast<unsigned long long>(source_topology),
           int(group_path.size()),
           removed_copies);
    if (r_changed != nullptr) {
      *r_changed = true;
    }
    return existing;
  }

  bNodeTree *group = bke::node_tree_add_tree(&bmain, name, "ShaderNodeTree");
  if (group == nullptr) {
    return nullptr;
  }
  /* The only user will be the instance node, which adds its own reference when assigned. */
  id_us_min(&group->id);
  tree_owner_uid_set(*group, owner.paint_layers_owner_uid);
  prop_int_set(group->id.properties, TREE_SOURCE_PROP, source_uid);
  tree_hash_set(*group, TREE_SOURCE_HASH_LOW_PROP, TREE_SOURCE_HASH_HIGH_PROP, source_topology);
  tree_hash_set(
      *group, TREE_SOURCE_VALUES_LOW_PROP, TREE_SOURCE_VALUES_HIGH_PROP, source_values);
  if (!source_group_build(bmain, *group, owner, source, principled, group_path, source_uid)) {
    BKE_id_free(&bmain, group);
    r_refusal = PaintLayersSourceGroupRefusal::BuildFailed;
    return nullptr;
  }
  printf("paint layers: source group '%s' for owner '%s' %s hash=%llx path_depth=%d "
         "removed_copies=%d\n",
         source.id.name + 2,
         owner.id.name + 2,
         "created",
         static_cast<unsigned long long>(source_topology),
         int(group_path.size()),
         0);
  if (r_changed != nullptr) {
    *r_changed = true;
  }
  return group;
}

void BKE_paint_layers_generate_copy_data(Main *bmain,
                                         Material &ma_dst,
                                         const Material &ma_src,
                                         const int copy_flag)
{
  if (ma_src.paint_layers_tree == nullptr ||
      (copy_flag & LIB_ID_CREATE_NO_USER_REFCOUNT) != 0)
  {
    ma_dst.paint_layers_tree = ma_src.paint_layers_tree;
    return;
  }
  if ((copy_flag & LIB_ID_COPY_SET_COPIED_ON_WRITE) != 0) {
    /* The depsgraph copies both IDs and relinks the reference; sharing the pointer here is what
     * makes the evaluated material use the evaluated tree. */
    ma_dst.paint_layers_tree = ma_src.paint_layers_tree;
    return;
  }

  bNodeTree *tree_copy = id_cast<bNodeTree *>(BKE_id_copy(bmain, &ma_src.paint_layers_tree->id));
  if (tree_copy == nullptr) {
    ma_dst.paint_layers_tree = nullptr;
    return;
  }
  /* #BKE_id_copy hands back a reference of its own; the generic copy pass below adds the single
   * reference the material owns, so drop the returned one to end at exactly that. */
  id_us_min(&tree_copy->id);
  ma_dst.paint_layers_tree = tree_copy;
  ma_dst.paint_layers_owner_uid = BLI_uuid_generate_random();
  tree_owner_uid_set(*tree_copy, ma_dst.paint_layers_owner_uid);
  /* Descend into the layer groups while the source owner still names them; a copy shares no tree
   * with the original, and the layer markers stay the same. */
  layer_trees_copy(
      bmain, *tree_copy, ma_src.paint_layers_owner_uid, ma_dst.paint_layers_owner_uid);

  /* The copy's embedded tree carries an instance node still pointing at the source tree; re-point
   * it before the generic copy pass walks the destination, so the copy owns only its own tree. */
  if (ma_dst.nodetree != nullptr) {
    for (bNode &node : ma_dst.nodetree->nodes) {
      bUUID node_uid = BLI_uuid_nil();
      if (!uid_prop_get(node.prop, INSTANCE_OWNER_PROP, node_uid) ||
          !BLI_uuid_equal(node_uid, ma_src.paint_layers_owner_uid))
      {
        continue;
      }
      if (node.id != nullptr) {
        id_us_min(node.id);
      }
      node.id = &tree_copy->id;
      id_us_plus(&tree_copy->id);
      instance_uid_set(node, ma_dst.paint_layers_owner_uid);
    }
  }
}

}  // namespace blender
