/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Topology hashes of a layered material's generated trees.
 *
 * A row's group and the root are a function of the description's structure alone. Their values
 * (opacity, a Fill constant, the enabled flag) are group inputs and never move these hashes, which
 * is what lets a value edit keep every group and only sync the values
 * (#BKE_paint_layers_values_sync).
 */

#include "BKE_paint_layers_generate.hh"

#include "BLI_hash.hh"
#include "BLI_listbase_iterator.hh"
#include "BLI_span.hh"
#include "BLI_string_ref.hh"
#include "BLI_uuid.h"

#include "BKE_idprop.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_gradient.hh"

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_uuid_types.h"

#include "paint_layers_generate_intern.hh"
#include "paint_layers_generate_layout.hh"
#include "paint_layers_intern.hh"

namespace blender::bke::paint_layers {

/* -------------------------------------------------------------------- */
/** \name Hash accumulation
 * \{ */

uint64_t topology_hash_mix(const uint64_t hash, const uint64_t value)
{
  /* Why not #get_default_hash(hash, value): that XORs its arguments, so the fold would be
   * commutative and reordering rows would leave the root hash -- and the generated chain -- as it
   * was. */
  return hash ^ (value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2));
}

void topology_hash_string(uint64_t &hash, const char *text)
{
  hash = topology_hash_mix(hash, (text != nullptr) ? get_default_hash(StringRef(text)) : 0);
}

void topology_hash_uid(uint64_t &hash, const bUUID &uid)
{
  hash = topology_hash_mix(hash, UUID(uid).hash());
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Stored hashes
 * \{ */

void tree_hash_set(bNodeTree &tree, const char *low_key, const char *high_key, const uint64_t hash)
{
  prop_int_set(tree.id.properties, low_key, int(uint32_t(hash & 0xFFFFFFFFu)));
  prop_int_set(tree.id.properties, high_key, int(uint32_t(hash >> 32)));
}

bool tree_hash_get(bNodeTree &tree, const char *low_key, const char *high_key, uint64_t &r_hash)
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

void tree_hash_clear(bNodeTree &tree)
{
  IDProperty *props = IDP_GetProperties(&tree.id);
  if (props == nullptr) {
    return;
  }
  for (const char *key : {TREE_TOPOLOGY_LOW_PROP,
                          TREE_TOPOLOGY_HIGH_PROP,
                          TREE_ROOT_TOPOLOGY_LOW_PROP,
                          TREE_ROOT_TOPOLOGY_HIGH_PROP,
                          TREE_SOURCE_STRUCTURE_LOW_PROP,
                          TREE_SOURCE_STRUCTURE_HIGH_PROP})
  {
    if (IDProperty *prop = IDP_GetPropertyFromGroup(props, key)) {
      IDP_FreeFromGroup(props, prop);
    }
  }
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

/** \} */

/* -------------------------------------------------------------------- */
/** \name Topology of a row and of the root
 * \{ */

/**
 * Mix what the UV wrap decides into \a hash: nothing while the wrap repeats on both axes (or the
 * mapping does not apply), the mode otherwise. A clip adds the factor's nodes; Repeat mixes
 * nothing, so a row that never clips hashes as it did before the option existed.
 */
static void mapping_wrap_hash_mix(uint64_t &hash, const Material &ma, const MaterialPaintLayer &row)
{
  const int16_t wrap = BKE_paint_layers_mapping_wrap_get(row);
  if (wrap != MA_PAINT_LAYER_MAPPING_WRAP_REPEAT && BKE_paint_layers_mapping_remaps(ma, row)) {
    hash = topology_hash_mix(hash, 0x100 + uint64_t(wrap));
  }
}

/**
 * Which point a reader names, and whether the reference resolves: the tap sockets are only built
 * for a valid reference. \a tag tells the places a reader can sit in apart.
 *
 * A reader of a content point also hashes which channels the source takes part in, as that is its
 * interface: a group gets one input per tap the source publishes, and a mask reader wires the
 * coverage of the first participating channel. A source that gained or lost a channel changes
 * both, even though the reference itself did not move. \a channels_tag separates those places.
 */
static void topology_hash_anchor_point_reader(uint64_t &hash,
                                              const Material &ma,
                                              const MaterialPaintLayer &reader,
                                              const Span<int> wired_channels,
                                              const uint64_t tag,
                                              const uint64_t channels_tag,
                                              const bool is_mask_item = false)
{
  const PaintLayerAnchorPointRef ref = paint_layer_anchor_point_resolve(ma, reader);
  hash = topology_hash_mix(hash, tag);
  topology_hash_uid(hash, reader.anchor_point_source);
  hash = topology_hash_mix(hash, uint64_t(ref.issue));
  hash = topology_hash_mix(hash, uint64_t(ref.target));
  if (ref.issue != PaintLayerAnchorPointIssue::None || ref.source_row == nullptr ||
      ref.target != PaintLayerAnchorPointTarget::Content)
  {
    return;
  }
  hash = topology_hash_mix(hash, channels_tag);
  if (is_mask_item) {
    hash = topology_hash_mix(hash,
                             uint64_t(paint_layer_anchor_point_mask_read_channel(ma, reader)));
  }
  for (const int channel : wired_channels) {
    hash = topology_hash_mix(hash, uint64_t(channel));
    hash = topology_hash_mix(hash, row_participates(ma, *ref.source_row, channel) ? 1 : 0);
  }
}

/** A marker is a Group Output only while something reads it. */
static void topology_hash_anchor_point_marker(uint64_t &hash,
                                              const Material &ma,
                                              const MaterialPaintLayer &marker,
                                              const uint64_t tag)
{
  hash = topology_hash_mix(hash, tag);
  hash = topology_hash_mix(hash, paint_layer_anchor_point_is_read(ma, marker.marker) ? 1 : 0);
}

/**
 * Mix what a Gradient payload decides the nodes: its ramp lives in a ColorRamp node's storage and
 * its space picks the coordinate output, so both are topology. The values are inputs.
 */
static void gradient_payload_hash_mix(uint64_t &hash, const MaterialPaintLayer &row)
{
  if (!BKE_paint_layers_gradient_supported(row) || row.gradient == nullptr) {
    return;
  }
  /* A ColorRamp node holds the ramp in its storage, not in a socket: a stop or an interpolation
   * edit is a different node, so the group is rebuilt rather than synced in place. A scalar channel
   * has its own value ramp node, so its ramp is topology too. */
  hash = topology_hash_mix(hash, BKE_PAINT_LAYERS_GRADIENT_HASH_VERSION);
  hash = topology_hash_mix(hash, uint64_t(uint8_t(row.gradient->space)));
  hash = topology_hash_mix(hash, BKE_paint_layers_gradient_ramp_hash(row.gradient->ramp));
  for (const int i : IndexRange(row.channels_num)) {
    const MaterialPaintLayerChannel &record = row.channels[i];
    if (record.gradient_ramp == nullptr) {
      continue;
    }
    hash = topology_hash_mix(hash, uint64_t(uint8_t(record.channel)));
    hash = topology_hash_mix(hash, BKE_paint_layers_gradient_ramp_hash(*record.gradient_ramp));
  }
}

uint64_t paint_layers_layer_topology_hash(const Material &ma,
                                          const MaterialPaintLayer &layer,
                                          const Span<int> wired_channels,
                                          const PaintLayerFormMap &forms)
{
  uint64_t hash = 0;
  hash = topology_hash_mix(hash, uint64_t(layout::kLayoutVersion));
  /* Source and blend decide the nodes; the enabled flag and every value are inputs, so they are
   * deliberately absent. A blend mode is structure only through #paint_layer_blend_shape: the
   * mode itself is the `blend_type` of an existing Mix node, kept current by the blend sync. */
  hash = topology_hash_mix(hash, uint64_t(uint8_t(layer.source)));
  if (layer.source == MA_PAINT_LAYER_SOURCE_MESH_MAP) {
    /* Which geometry map the row reads is topology: another map is another node. */
    hash = topology_hash_mix(hash, uint64_t(uint8_t(layer.mesh_map_type)));
  }
  /* Which filter an adjustment is decides its nodes; its parameters are inputs. */
  hash = topology_hash_mix(hash, uint64_t(uint16_t(layer.kind_subtype)));
  gradient_payload_hash_mix(hash, layer);
  hash = topology_hash_mix(hash, paint_layer_blend_shape(layer));
  /* The runtime tier changes which node a participating channel gets (the dynamic group against a
   * static Mix). Mixing its mask here keeps that transition out of #paint_layer_blend_shape, where
   * it would also move for channels nothing builds. */
  hash = topology_hash_mix(hash, paint_layer_blend_dynamic_mask(ma, layer));
  topology_hash_string(hash, layer.name);
  /* An applied mapping is a Mapping node and its inputs; its values are inputs, so only whether it
   * applies is structure. The same answer builds the interface, so the two cannot disagree. */
  hash = topology_hash_mix(hash, BKE_paint_layers_mapping_remaps(ma, layer) ? 1 : 0);
  mapping_wrap_hash_mix(hash, ma, layer);
  /* A row inside an isolating folder is built in the isolated form, with sockets of its own. */
  hash = topology_hash_mix(hash, forms.lookup_default(&layer, {}).in_isolating_folder ? 1 : 0);
  /* A Pass Through folder builds no group, an isolating one does: the mode is structure, although
   * what decides it (mask, blend, opacity) is not all in the hash below. */
  hash = topology_hash_mix(hash, BKE_paint_layers_folder_is_pass_through(layer) ? 1 : 0);
  if ((layer.object_mask_flag & MA_PAINT_LAYER_OBJECT_MASK_ACTIVE) != 0) {
    /* Mixed only when set, so every row without an object mask keeps its hash. The mode picks the
     * Attribute field and the tile nodes. The owner uid is in the Attribute node's name, so a
     * material copy with a fresh uid rebuilds. */
    hash = topology_hash_mix(hash, 0x200 + uint64_t(uint8_t(layer.object_mask_type)));
    topology_hash_uid(hash, ma.paint_layers_owner_uid);
  }
  /* The Height record's effective mode is nodes of its own: the delta-space chain for a mode, and
   * for Compare the won mask multiplied into every other channel's factor. The scale is an input,
   * not structure. A row built from its bake takes the same chain over its baked map, so both
   * shapes are hashed on the one rule. */
  if (wired_channels.contains(PAINT_MATERIAL_CHANNEL_HEIGHT) &&
      (row_participates(ma, layer, PAINT_MATERIAL_CHANNEL_HEIGHT) ||
       paint_layer_row_baked_channel(ma, layer, PAINT_MATERIAL_CHANNEL_HEIGHT, nullptr)))
  {
    const int height_blend = BKE_paint_layers_channel_height_blend_effective(
        layer, PAINT_MATERIAL_CHANNEL_HEIGHT);
    hash = topology_hash_mix(hash, 0x800 + uint64_t(uint8_t(height_blend)));
  }
  if (paint_layer_row_substituted(ma, layer)) {
    /* A baked row is built from its bake's maps alone: which channels have one, which maps, and
     * whether the live row had a map of its own (its alpha then covers) is structure; the
     * corrections, the mask and the rest of the description are inside the bake. */
    hash = topology_hash_mix(hash, 0x400);
    for (const int channel : wired_channels) {
      hash = topology_hash_mix(hash, uint64_t(channel));
      Image *baked = nullptr;
      if (paint_layer_row_baked_channel(ma, layer, channel, &baked)) {
        topology_hash_string(hash, baked->id.name);
        const MaterialPaintLayerChannel *record = paint_layer_channel_find(layer, channel);
        hash = topology_hash_mix(hash, (record != nullptr && record->image != nullptr) ? 1 : 0);
      }
      else {
        hash = topology_hash_mix(hash, 0);
      }
    }
    topology_hash_string(hash, layer.bake->coverage->id.name);
    return hash;
  }
  for (const int channel : wired_channels) {
    hash = topology_hash_mix(hash, uint64_t(channel));
    hash = topology_hash_mix(hash, row_participates(ma, layer, channel) ? 1 : 0);
    /* A map and a flat value are different nodes (an Image Texture against a value input), and the
     * texture node names its image: assigning, swapping or removing a map is structure, even when
     * the channel took part before and after. */
    const MaterialPaintLayerChannel *record = paint_layer_channel_find(layer, channel);
    if (record != nullptr && record->image != nullptr) {
      topology_hash_string(hash, record->image->id.name);
      /* The DirectX green flip is nodes of its own. */
      if (channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
        hash = topology_hash_mix(
            hash, (record->flag & MA_PAINT_LAYER_CHANNEL_NORMAL_DIRECTX) != 0 ? 1 : 0);
      }
    }
    else {
      hash = topology_hash_mix(hash, 0);
    }
  }
  if (paint_layer_kind_has_flag(layer, PAINT_LAYER_KIND_READS_SOURCE)) {
    /* A Material row's nodes follow its source: which channels are a value, a texture or a
     * rendered map, which image each reads and how, and the same for the coverage. The values are
     * inputs. */
    hash = topology_hash_mix(hash, 0x500);
    hash = topology_hash_mix(hash, paint_layer_material_live_signature(ma, layer));
  }
  if (paint_layer_kind_reads_anchor_point(layer)) {
    /* Which point a reader names, and whether the reference resolves: the tap sockets are only
     * built for a valid reference. The values above and below the marker are inputs. */
    topology_hash_anchor_point_reader(hash, ma, layer, wired_channels, 0x600, 0x604);
  }
  /* A content correction is nodes of its own (a map, a Mix and a coverage fold) in each channel it
   * lays a map in, so which ones do, and which map, is structure; its opacity is an input. Its
   * marker names those inputs, so a removed and re-added correction is a different shape. */
  for (const MaterialPaintLayer &effect : layer.effects) {
    topology_hash_uid(hash, effect.marker);
    if (paint_layer_kind_is_anchor_point_marker(effect)) {
      /* A marker is a Group Output only while something reads it; which point that is, and where,
       * is structure. Its own marker is above, so moving it moves this hash. */
      topology_hash_anchor_point_marker(hash, ma, effect, 0x601);
      continue;
    }
    /* The correction's own mode is a node property; only Replace changes the node. */
    hash = topology_hash_mix(hash, paint_layer_blend_shape(effect));
    /* A correction in the runtime tier builds the dynamic group in the channels it lays. */
    hash = topology_hash_mix(hash, paint_layer_blend_dynamic_mask(ma, effect));
    /* A flat colour and a map are different nodes, so the source is structure. */
    hash = topology_hash_mix(hash, uint64_t(uint8_t(effect.source)));
    if (effect.source == MA_PAINT_LAYER_SOURCE_MESH_MAP) {
      /* Which geometry map the row reads is topology: another map is another node. */
      hash = topology_hash_mix(hash, uint64_t(uint8_t(effect.mesh_map_type)));
    }
    /* Which filter it is decides its nodes; its parameters are inputs. */
    hash = topology_hash_mix(hash, uint64_t(uint16_t(effect.kind_subtype)));
    hash = topology_hash_mix(hash, BKE_paint_layers_mapping_remaps(ma, effect) ? 1 : 0);
    mapping_wrap_hash_mix(hash, ma, effect);
    gradient_payload_hash_mix(hash, effect);
    const bool reads_source = paint_layer_kind_has_flag(effect, PAINT_LAYER_KIND_READS_SOURCE);
    if (reads_source) {
      /* A correction that reads a material has nodes that follow its source, as a Material row's
       * do: which channels are a value, a texture, a rendered map or the source's graph, and which
       * image each reads and how. The values are inputs. */
      hash = topology_hash_mix(hash, 0x501);
      hash = topology_hash_mix(hash, paint_layer_material_live_signature(ma, effect));
    }
    const bool reads_anchor_point = paint_layer_kind_reads_anchor_point(effect);
    if (reads_anchor_point) {
      topology_hash_anchor_point_reader(hash, ma, effect, wired_channels, 0x602, 0x605);
    }
    for (const int channel : wired_channels) {
      hash = topology_hash_mix(hash, paint_layer_effect_filters(ma, effect, channel) ? 1 : 0);
      if (paint_layer_effect_lays(ma, effect, channel)) {
        /* A reader has no records either: what it lays is the slice it names. */
        const MaterialPaintLayerChannel *record = paint_layer_channel_find(effect, channel);
        if (reads_source || reads_anchor_point) {
          hash = topology_hash_mix(hash, 2);
        }
        else if (record->image != nullptr) {
          topology_hash_string(hash, record->image->id.name);
        }
        else {
          /* A flat colour: an input, not a node that names anything. */
          hash = topology_hash_mix(hash, 1);
        }
      }
      else {
        hash = topology_hash_mix(hash, 0);
      }
      /* A row kept only for its corrections starts from zero coverage instead of its constant. */
      hash = topology_hash_mix(hash, paint_layer_row_empty_base(ma, layer, channel) ? 1 : 0);
    }
  }
  /* A mask is nodes (an Image Texture and its grey chain against a value input), so its presence
   * and whether it has a map are structure; its constant, opacity and enabled flag are inputs. */
  if (const MaterialPaintLayer *mask = BKE_paint_layers_mask_base(layer)) {
    hash = topology_hash_mix(hash, 1);
    /* The inversion is a node of its own. */
    hash = topology_hash_mix(hash, paint_layer_mask_item_inverted(*mask) ? 1 : 0);
    hash = topology_hash_mix(hash, BKE_paint_layers_mapping_remaps(ma, *mask) ? 1 : 0);
    mapping_wrap_hash_mix(hash, ma, *mask);
    const MaterialPaintLayerChannel *record = paint_layer_channel_find(
        *mask, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
    if (record != nullptr && record->image != nullptr) {
      topology_hash_string(hash, record->image->id.name);
    }
    else {
      hash = topology_hash_mix(hash, 0);
    }
  }
  else {
    hash = topology_hash_mix(hash, 0);
  }
  /* A mask correction is nodes of its own (a map and its grey chain against a value input, and a
   * Mix whose mode is a node property), so its place and whether it has a map are structure; its
   * constant, blend, opacity and enabled flag are inputs. One that lays nothing yet has no nodes. */
  for (const MaterialPaintLayer &item : layer.mask_stack) {
    if ((item.flag & MA_PAINT_LAYER_MASK_BASE) != 0) {
      continue;
    }
    topology_hash_uid(hash, item.marker);
    if (paint_layer_kind_is_anchor_point_marker(item)) {
      topology_hash_anchor_point_marker(hash, ma, item, 0x603);
      continue;
    }
    if (paint_layer_kind_reads_anchor_point(item)) {
      topology_hash_anchor_point_reader(hash, ma, item, wired_channels, 0x606, 0x607, true);
      continue;
    }
    /* A filter item is nodes of its own, and which filter decides them. */
    hash = topology_hash_mix(hash, uint64_t(uint8_t(item.source)));
    if (item.source == MA_PAINT_LAYER_SOURCE_MESH_MAP) {
      /* Which geometry map the row reads is topology: another map is another node. */
      hash = topology_hash_mix(hash, uint64_t(uint8_t(item.mesh_map_type)));
    }
    hash = topology_hash_mix(hash, uint64_t(uint16_t(item.kind_subtype)));
    /* A map-coverage item builds one comparison per stored colour, so the count is structure; each
     * colour's value and enabled flag are inputs the graph reads, so they stay out of the hash. */
    if (paint_layer_kind_is_map_coverage(item)) {
      hash = topology_hash_mix(hash, 0x300 + uint64_t(item.id_colors.count()));
      /* Advanced Color ID grows one tolerance input per colour. */
      hash = topology_hash_mix(hash, paint_layer_mask_item_color_id_advanced(item) ? 1 : 0);
    }
    /* The inversion is a node of its own. */
    hash = topology_hash_mix(hash, paint_layer_mask_item_inverted(item) ? 1 : 0);
    hash = topology_hash_mix(hash, BKE_paint_layers_mapping_remaps(ma, item) ? 1 : 0);
    mapping_wrap_hash_mix(hash, ma, item);
    gradient_payload_hash_mix(hash, item);
    const MaterialPaintLayerChannel *record = paint_layer_channel_find(
        item, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
    if (paint_layer_kind_has_flag(item, PAINT_LAYER_KIND_READS_SOURCE)) {
      /* The nodes follow the source, as a Material row's do; the values are inputs. */
      hash = topology_hash_mix(hash, 0x502);
      hash = topology_hash_mix(hash,
                               paint_layer_mask_item_lays(ma, item) ?
                                   paint_layer_material_live_signature(ma, item) :
                                   0);
    }
    else if (!paint_layer_mask_item_lays(item) || record == nullptr) {
      hash = topology_hash_mix(hash, 0);
    }
    else if (record->image != nullptr) {
      topology_hash_string(hash, record->image->id.name);
    }
    else {
      hash = topology_hash_mix(hash, 1);
    }
  }
  return hash;
}

uint64_t paint_layer_blend_shape(const MaterialPaintLayer &layer)
{
  /* A mask item's blend is the type of its coverage Mix, which its row always builds. */
  if (layer.role == MA_PAINT_LAYER_ROLE_MASK_ITEM) {
    return 0;
  }
  /* Replace is a Mix where the Normal channel otherwise builds the combine group. */
  uint64_t shape = BKE_paint_layers_normal_replace(layer) ? 1 : 0;
  /* An inlined folder has no group of its own, so its own blend has nowhere to live. */
  if (BKE_paint_layers_is_folder(layer)) {
    shape = shape * 2 + (BKE_paint_layers_folder_is_pass_through(layer) ? 1 : 0);
  }
  return shape;
}

uint64_t paint_layers_root_topology_hash(const Material &ma, const Span<int> wired_channels)
{
  uint64_t hash = 0;
  hash = topology_hash_mix(hash, uint64_t(layout::kLayoutVersion));
  for (const int channel : wired_channels) {
    hash = topology_hash_mix(hash, uint64_t(channel));
  }
  /* The rows in order, each with its own topology: adding, removing or reordering a row moves this,
   * a value edit does not. A row that takes part in no wired channel builds no group and is left
   * out, so a change to it cannot make the root rebuild for nothing. */
  const PaintLayerFormMap forms = paint_layers_form_map(ma);
  BKE_paint_layers_foreach(
      ma, [&](const MaterialPaintLayer &layer, const MaterialPaintLayer *parent) -> bool {
        bool participates = false;
        for (const int channel : wired_channels) {
          if (row_participates(ma, layer, channel)) {
            participates = true;
            break;
          }
        }
        if (!participates) {
          return true;
        }
        /* The parent is part of the shape: moving a row into or out of a folder rewires the chain
         * even when the order of the rows does not change. */
        topology_hash_uid(hash, layer.marker);
        topology_hash_uid(hash, parent != nullptr ? parent->marker : bUUID{});
        hash = topology_hash_mix(hash,
                                 paint_layers_layer_topology_hash(ma, layer, wired_channels, forms));
        return true;
      });
  return hash;
}

/** \} */

}  // namespace blender::bke::paint_layers
