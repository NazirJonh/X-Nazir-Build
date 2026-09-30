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

bUUID value_slot_or_marker(const IDProperty *properties, const bUUID &marker)
{
  bUUID slot = BLI_uuid_nil();
  if (uid_prop_get(properties, INPUT_SLOT_PROP, slot) && !BLI_uuid_is_nil(slot)) {
    return slot;
  }
  return marker;
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
  if (layer.source != MA_PAINT_LAYER_SOURCE_MATERIAL || layer.material == nullptr ||
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

bool removed_rows_contains(const Material &ma, const bUUID &marker)
{
  const MaterialPaintLayersRuntime *runtime = paint_layers_runtime_get(ma);
  if (runtime == nullptr) {
    return false;
  }
  for (const bUUID &other : runtime->removed_rows) {
    if (BLI_uuid_equal(other, marker)) {
      return true;
    }
  }
  return false;
}

/** Whether the graph is built without \a layer: it is disabled and a past rebuild left it out. */
bool row_is_removed(const Material &ma, const MaterialPaintLayer &layer)
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
void removed_rows_reconcile(Material &ma)
{
  Vector<bUUID> markers;
  Vector<const MaterialPaintLayer *> layers;
  BKE_paint_layers_flatten(ma, layers);
  for (const MaterialPaintLayer *layer : layers) {
    /* A hidden Pass Through folder keeps its subtree in the graph with a zero factor, so it is
     * never written to the removed set -- unless the over-budget pass is dropping hidden rows. */
    /* The row the user is working in, and the folders around it, stay in the graph while hidden:
     * hiding it is only a check of how the result looks, and showing it again must not rebuild. */
    if (!budget_cleanup_active(ma) &&
        BKE_paint_layers_subtree_contains(*layer, ma.active_layer_marker))
    {
      continue;
    }
    if ((layer->flag & MA_PAINT_LAYER_ENABLED) == 0 &&
        (!BKE_paint_layers_folder_is_pass_through(ma, *layer) || budget_cleanup_active(ma)))
    {
      markers.append(layer->marker);
    }
  }
  paint_layers_runtime_ensure(ma).removed_rows = std::move(markers);
}

/**
 * Find \a target in \a list's subtree and return the product of the enabled states of the Pass
 * Through folders above it. Returns false when \a target is not there.
 *
 * Only Pass Through folders scale: an isolating folder already folds its own visibility into its
 * row's opacity, so scaling its descendants too would count it twice (harmlessly at zero, but not at
 * one). Descending into every folder is still needed to reach a target nested inside one.
 */
bool pass_through_scale_find(const Material &ma,
                                    const MaterialPaintLayer &target,
                                    const ListBaseT<MaterialPaintLayer> &list,
                                    const float scale,
                                    float &r_scale)
{
  for (const MaterialPaintLayer &layer :
       list)
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
void pass_through_scales_fill(const Material &ma,
                                     const ListBaseT<MaterialPaintLayer> &list,
                                     const float scale,
                                     Map<const MaterialPaintLayer *, float> &r_scales)
{
  for (const MaterialPaintLayer &layer :
       list)
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
float pass_through_scale_of(const Material &ma,
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
  if (paint_layer_channel_present(ma, layer, channel)) {
    return true;
  }
  if (!BKE_paint_layers_is_folder(layer)) {
    return false;
  }
  for (const MaterialPaintLayer &child :
       layer.children)
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
 * Whether \a layer is replaced whole by its row cache, so its opacity, fill and corrections are
 * inside the bake and get no inputs of their own. A Material layer's bake is its source's maps,
 * which are the row's content instead (#BKE_paint_layers_bake_substitute says the same).
 */
bool row_is_substituted(const Material &ma, const MaterialPaintLayer &layer)
{
  return layer.source != MA_PAINT_LAYER_SOURCE_MATERIAL && BKE_paint_layers_bake_is_valid(ma, layer);
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
bool leaf_participates(const Material &ma, const MaterialPaintLayer &layer, const int channel)
{
  if (!paint_layer_channel_present(ma, layer, channel)) {
    return false;
  }
  if (paint_layer_channel_image(ma, layer, channel) != nullptr) {
    return true;
  }
  if (BKE_paint_layers_role(layer) == PaintLayerRole::Layer &&
      BKE_paint_layers_kind_info(layer.source).uses_fill_color)
  {
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
  return leaf_participates(ma, layer, channel);
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

/* Forward declaration: a Stack correction's topology recurses into its children through
 * #topology_hash_layer (defined below, after #topology_hash_correction), which in turn recurses
 * into a Layer folder's own effects/mask items through #topology_hash_correction -- the two are
 * mutually recursive. */
}  // namespace bke::paint_layers
}  // namespace blender

