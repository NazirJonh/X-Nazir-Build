/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The evaluator deliberately only keeps image buffers. The Outliner needs the node identity and
 * editable Factor socket too, so it walks the same graph shape independently.
 */

#include "BKE_paint_material_composite.hh"

#include <limits>
#include <utility>

#include "BKE_main.hh"
#include "BKE_node.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_material_layer_edit.hh"
#include "BKE_paint_material_resolve.hh"

#include "BLI_listbase_iterator.hh"
#include "BLI_string.h"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "paint_material_composite_internal.hh"
#include "paint_material_layer_idprops.hh"

namespace blender {

namespace {

struct LayerModelNode {
  const bNode *node = nullptr;
  const bNodeTree *owner_tree = nullptr;
  Image *image = nullptr;
  /** Nesting: 0 at the top level, one more inside each layer group. */
  int depth = 0;
  /** Identifier of the enclosing group node, or 0 at the top level. */
  int32_t parent_node_id = 0;
  /** True when this row is a folder of layers rather than a map (`08 §2.2`). */
  bool is_group = false;
  /** True when the row is a bare Image Texture at the bottom rather than a blended layer. */
  bool is_bare_base = false;
  /** For a group row, the group instance node -- what the row is named after. */
  const bNode *group_node = nullptr;
  CompositeBlend blend = CompositeBlend::Mix;
  float opacity = 1.0f;
  bool enabled = true;
  bool has_mask = false;
  bool supported = true;
  const char *unsupported_reason = nullptr;
  const bNodeSocket *factor = nullptr;
  /** The corrections between this row and its map, bottom to top (spec 18 §4.5). */
  Vector<const bNode *> content_correction_nodes;
  /** The corrections between this row and its coverage, bottom to top. */
  Vector<const bNode *> mask_correction_nodes;
};

void layer_model_append_unsupported(Vector<LayerModelNode> &r_layers,
                                    const bNode *node,
                                    const char *reason)
{
  LayerModelNode layer;
  layer.node = node;
  layer.owner_tree = node ? &node->owner_tree() : nullptr;
  layer.supported = false;
  layer.unsupported_reason = reason;
  r_layers.append(layer);
}

/**
 * The node feeding \a socket, reporting a muted one instead of routing through it.
 *
 * #composite_source_node_shallow follows a muted node's internal links, which is right for pixels
 * and wrong for a layer manager: a muted Mix node is a layer the user switched off, not a layer
 * that stopped existing. A row that vanishes when it is disabled cannot be enabled again.
 */
const bNode *layer_model_source_node_or_muted(const bNodeSocket &socket, bool &r_muted)
{
  r_muted = false;
  const bNodeSocket *current = &socket;
  /* A malformed tree can cycle; bound the walk rather than trust the data. */
  for (int step = 0; step < 64; step++) {
    if (current->directly_linked_links().is_empty()) {
      return nullptr;
    }
    const bNodeLink *link = current->directly_linked_links()[0];
    if (!link->is_available() || link->is_muted()) {
      return nullptr;
    }
    const bNode &from_node = *link->fromnode;
    if (from_node.is_reroute()) {
      current = static_cast<const bNodeSocket *>(from_node.inputs.first);
      continue;
    }
    r_muted = from_node.is_muted();
    return &from_node;
  }
  return nullptr;
}

void layer_model_read_mix(const bNode &node,
                          const CompositeMixNode &mix,
                          const bool muted,
                          LayerModelNode &r_layer)
{
  r_layer.node = &node;
  r_layer.owner_tree = &node.owner_tree();
  r_layer.blend = mix.blend;
  r_layer.enabled = !muted;
  if (mix.factor_opacity != nullptr) {
    /* Coverage and the layer's own opacity coexist: the row's editable value is the Multiply's
     * other input, not the linked socket that actually feeds the Mix's Factor. */
    r_layer.factor = mix.factor_opacity;
    r_layer.has_mask = true;
    r_layer.opacity =
        static_cast<const bNodeSocketValueFloat *>(mix.factor_opacity->default_value)->value;
  }
  else if (BKE_paint_material_source_socket(*mix.factor) != nullptr) {
    /* Linked with nothing to separate an opacity from -- legacy shape, nothing to edit. */
    r_layer.factor = nullptr;
    r_layer.has_mask = true;
  }
  else {
    r_layer.factor = mix.factor;
    r_layer.opacity = static_cast<const bNodeSocketValueFloat *>(mix.factor->default_value)->value;
  }
}

/**
 * The corrections hanging between \a socket and the row's own map, walked by their links (spec 18
 * §4.5): each step's source must be a correction of \a section, and the walk continues at what
 * that correction blends over. Appends the nodes met, topmost first, and returns the socket the
 * map itself feeds -- \a socket when there are no corrections.
 *
 * Follows links rather than #composite_source_node_shallow: a muted correction still hangs where
 * it hangs, and a row the user switched off cannot be a row the model stops listing.
 */
const bNodeSocket *layer_model_corrections_descend(
    const bNodeSocket &socket,
    const PaintMaterialCorrectionSection section,
    Vector<const bNode *> &r_corrections)
{
  const bNodeSocket *current = &socket;
  /* A malformed tree can cycle; bound the walk rather than trust the data. */
  for (int step = 0; step < 64; step++) {
    if (current->directly_linked_links().is_empty()) {
      return current;
    }
    const bNodeLink *link = current->directly_linked_links()[0];
    if (!link->is_available() || link->is_muted()) {
      return current;
    }
    const bNode &from_node = *link->fromnode;
    if (from_node.is_reroute()) {
      current = static_cast<const bNodeSocket *>(from_node.inputs.first);
      continue;
    }
    if (!bke::paint_layer::node_is_correction(from_node) ||
        bke::paint_layer::correction_section_get(from_node) != section)
    {
      return current;
    }
    r_corrections.append(&from_node);
    CompositeMixNode below;
    if (!composite_mix_node_read(from_node, below) || below.bottom == nullptr) {
      /* Not the shape the insert builds: stop here and let the map input speak for itself. */
      return current;
    }
    current = below.bottom;
  }
  return current;
}

/** The walk meets the topmost correction first; the rows read bottom to top, like the layers. */
void layer_model_corrections_reverse(Vector<const bNode *> &r_corrections)
{
  const int64_t size = r_corrections.size();
  for (const int64_t i : IndexRange(size / 2)) {
    std::swap(r_corrections[i], r_corrections[size - 1 - i]);
  }
}

/**
 * How deep layer groups may nest before the model gives up (`08 §2.2`, Q2).
 *
 * A limit rather than unbounded recursion: the graph is user data and may be cyclic through group
 * instances, and a row the UI cannot draw is better than a stack overflow.
 */
constexpr int LAYER_GROUP_NESTING_MAX = 8;

/** The layer group feeding \a socket, or null when its source is not one. */
const bNode *layer_model_group_from_socket(const bNodeSocket &socket)
{
  const bNode *source = composite_source_node_shallow(socket);
  if (source == nullptr || !BKE_paint_material_is_layer_group(*source)) {
    return nullptr;
  }
  return source;
}

/**
 * The socket inside \a group that its `Result` output is taken from: the top of the sub-stack.
 *
 * Found through the Group Output node rather than through the interface, because it is the link
 * into that node which says what the group actually returns.
 */
const bNodeSocket *layer_model_group_result_socket(const bNode &group)
{
  const bNodeTree *tree = reinterpret_cast<const bNodeTree *>(group.id);
  if (tree == nullptr) {
    return nullptr;
  }
  tree->ensure_topology_cache();
  for (const bNode &node : tree->nodes) {
    if (node.type_legacy != NODE_GROUP_OUTPUT) {
      continue;
    }
    for (const bNodeSocket *input : node.input_sockets()) {
      if (input->is_available() && !input->directly_linked_links().is_empty()) {
        return input;
      }
    }
  }
  return nullptr;
}

/**
 * Walk one channel chain, bottom-up, appending a row per layer.
 *
 * The chain_step argument only bounds the recursion along a chain; nesting and parent_node_id are
 * what the UI reads as hierarchy, and they only change when the walk descends into a layer group.
 */
void layer_model_collect(const bNodeSocket &socket,
                         Vector<LayerModelNode> &r_layers,
                         const int chain_step,
                         const int nesting = 0,
                         const int32_t parent_node_id = 0)
{
  if (chain_step > 64) {
    layer_model_append_unsupported(r_layers, nullptr, "Stack is too deep");
    return;
  }

  /* A muted Mix is still a layer, so it is looked for before the evaluator's walk skips it. */
  bool muted = false;
  const bNode *muted_source = layer_model_source_node_or_muted(socket, muted);
  if (muted && muted_source != nullptr) {
    CompositeMixNode muted_mix;
    if (composite_mix_node_read(*muted_source, muted_mix)) {
      /* Nothing under it means the bottom of a uniform chain; see the same test further down. */
      if (composite_source_node_shallow(*muted_mix.bottom) != nullptr) {
        layer_model_collect(*muted_mix.bottom, r_layers, chain_step + 1, nesting, parent_node_id);
      }
      LayerModelNode layer;
      layer_model_read_mix(*muted_source, muted_mix, true, layer);
      layer.depth = nesting;
      layer.parent_node_id = parent_node_id;

      /* A switched-off group is still a group: it keeps its rows, which is what the user turns
       * back on. Reading its top as an image would make the folder an unsupported row and take
       * every layer it holds off the list with it. */
      const bNode *group = layer_model_group_from_socket(*muted_mix.top);
      if (group != nullptr) {
        layer.is_group = true;
        layer.group_node = group;
        /* The folder's own mask corrections hang on the coverage input of the row's Multiply,
         * exactly as on any other row (spec 18 §4.5); the folder takes no content corrections
         * (D7), so only the mask section descends here. Without it the rows are missing from
         * the model, and remove or rename answer CorrectionNotFound. */
        if (muted_mix.factor_coverage != nullptr) {
          layer_model_corrections_descend(*muted_mix.factor_coverage,
                                          PaintMaterialCorrectionSection::Mask,
                                          layer.mask_correction_nodes);
          layer_model_corrections_reverse(layer.mask_correction_nodes);
        }
        if (nesting >= LAYER_GROUP_NESTING_MAX) {
          layer.supported = false;
          layer.unsupported_reason = "Layer groups are nested too deeply";
          r_layers.append(layer);
          return;
        }
        if (const bNodeSocket *inner = layer_model_group_result_socket(*group)) {
          layer_model_collect(*inner, r_layers, 0, nesting + 1, group->identifier);
        }
        r_layers.append(layer);
        return;
      }

      const ImageUser *iuser = nullptr;
      /* The corrections between this row and its map are rows of their own (spec 18 §4.5), and
       * the map itself is whatever the walk ends on. An unlinked map input is a channel this
       * layer does not have: a supported row with no map, whose unlinked coverage keeps it from
       * contributing anything -- not a broken shape. */
      const bNodeSocket *content_base = layer_model_corrections_descend(
          *muted_mix.top,
          PaintMaterialCorrectionSection::Content,
          layer.content_correction_nodes);
      if (muted_mix.factor_coverage != nullptr) {
        layer_model_corrections_descend(*muted_mix.factor_coverage,
                                        PaintMaterialCorrectionSection::Mask,
                                        layer.mask_correction_nodes);
      }
      if (content_base != nullptr && composite_source_node_shallow(*content_base) != nullptr &&
          !composite_image_from_socket(*content_base, layer.image, iuser))
      {
        layer.supported = false;
        layer.unsupported_reason = "Layer source is not an image";
      }
      layer_model_corrections_reverse(layer.content_correction_nodes);
      layer_model_corrections_reverse(layer.mask_correction_nodes);
      r_layers.append(layer);
      return;
    }
  }

  const bNode *shallow_source = composite_source_node_shallow(socket);
  if (shallow_source != nullptr && BKE_paint_material_is_layer_group(*shallow_source)) {
    /* A group at the bottom of a chain: a row of its own, with its sub-stack below it. Handled
     * here because the resolver would otherwise walk straight through the group instance and
     * report a node inside it as if it were a layer of this chain. The instance is the row
     * itself, with no Mix and so no coverage input of its own for mask corrections to hang on
     * -- a shape the edit path refuses, which is why no descent happens here. */
    LayerModelNode layer;
    layer.node = shallow_source;
    layer.owner_tree = &shallow_source->owner_tree();
    layer.depth = nesting;
    layer.parent_node_id = parent_node_id;
    layer.is_group = true;
    layer.group_node = shallow_source;
    if (nesting >= LAYER_GROUP_NESTING_MAX) {
      layer.supported = false;
      layer.unsupported_reason = "Layer groups are nested too deeply";
      r_layers.append(layer);
      return;
    }
    /* Children first: the list is bottom-up, and the UI walks it backwards, so a group has to sit
     * after the layers it holds for the group row to be reached before them. */
    if (const bNodeSocket *inner = layer_model_group_result_socket(*shallow_source)) {
      layer_model_collect(*inner, r_layers, 0, nesting + 1, shallow_source->identifier);
    }
    r_layers.append(layer);
    return;
  }
  const bool is_normal_combine = shallow_source != nullptr &&
                                 BKE_paint_material_is_normal_combine_group(*shallow_source);
  if (!is_normal_combine) {
    if (shallow_source == nullptr) {
      layer_model_append_unsupported(r_layers, shallow_source, "Unlinked stack input");
      return;
    }
    if (shallow_source->type_legacy == SH_NODE_TEX_IMAGE) {
      Image *image = nullptr;
      const ImageUser *iuser = nullptr;
      if (!composite_image_from_socket(socket, image, iuser)) {
        layer_model_append_unsupported(
            r_layers, shallow_source, "Bottom of stack is not an image");
        return;
      }
      LayerModelNode layer;
      layer.node = shallow_source;
      layer.owner_tree = &shallow_source->owner_tree();
      layer.image = image;
      layer.depth = nesting;
      layer.parent_node_id = parent_node_id;
      /* Wired straight into the channel: no Mix node, so no blend mode, opacity or mute of its
       * own, and nothing can be put under it until the chain is brought to the current shape. */
      layer.is_bare_base = true;
      r_layers.append(layer);
      return;
    }
  }

  CompositeMixNode mix;
  if (shallow_source == nullptr || !composite_mix_node_read(*shallow_source, mix)) {
    layer_model_append_unsupported(r_layers, shallow_source, "Unsupported stack node");
    return;
  }
  /* Nothing under this Mix node means it is the bottom of a uniform chain: it blends over the
   * transparency its own socket holds, and there is no row below it to list. */
  if (composite_source_node_shallow(*mix.bottom) != nullptr) {
    layer_model_collect(*mix.bottom, r_layers, chain_step + 1, nesting, parent_node_id);
  }

  LayerModelNode layer;
  layer_model_read_mix(*shallow_source, mix, false, layer);
  layer.depth = nesting;
  layer.parent_node_id = parent_node_id;

  const bNode *group = layer_model_group_from_socket(*mix.top);
  if (group != nullptr) {
    /* The group is a row of its own, and the layers it holds are its children. Listed after them
     * so that a reader walking the array backwards -- the order a layer manager lists a stack in
     * -- meets the group before the rows that belong to it. */
    layer.is_group = true;
    layer.group_node = group;
    /* The folder's own mask corrections hang on the coverage input of the row's Multiply, the
     * same place #correction_channel_insert puts them on any row (spec 18 §4.5); the folder
     * takes no content corrections (D7), so only the mask section descends here. Without it the
     * rows are missing from the model, and remove or rename answer CorrectionNotFound. */
    if (mix.factor_coverage != nullptr) {
      layer_model_corrections_descend(*mix.factor_coverage,
                                      PaintMaterialCorrectionSection::Mask,
                                      layer.mask_correction_nodes);
      layer_model_corrections_reverse(layer.mask_correction_nodes);
    }
    if (nesting >= LAYER_GROUP_NESTING_MAX) {
      layer.supported = false;
      layer.unsupported_reason = "Layer groups are nested too deeply";
      r_layers.append(layer);
      return;
    }
    if (const bNodeSocket *inner = layer_model_group_result_socket(*group)) {
      layer_model_collect(*inner, r_layers, 0, nesting + 1, group->identifier);
    }
    r_layers.append(layer);
    return;
  }

  const ImageUser *iuser = nullptr;
  /* The corrections between this row and its map are rows of their own (spec 18 §4.5), and the
   * map itself is whatever the walk ends on. An unlinked map input is a channel this layer does
   * not have: a supported row with no map, whose unlinked coverage keeps it from contributing
   * anything -- not a broken shape. */
  const bNodeSocket *content_base = layer_model_corrections_descend(
      *mix.top, PaintMaterialCorrectionSection::Content, layer.content_correction_nodes);
  if (mix.factor_coverage != nullptr) {
    layer_model_corrections_descend(
        *mix.factor_coverage, PaintMaterialCorrectionSection::Mask, layer.mask_correction_nodes);
  }
  if (content_base != nullptr && composite_source_node_shallow(*content_base) != nullptr &&
      !composite_image_from_socket(*content_base, layer.image, iuser))
  {
    layer.supported = false;
    layer.unsupported_reason = "Layer source is not an image";
  }
  layer_model_corrections_reverse(layer.content_correction_nodes);
  layer_model_corrections_reverse(layer.mask_correction_nodes);
  r_layers.append(layer);
}

bool layer_model_from_channel(const Material &material,
                              const int channel,
                              Vector<LayerModelNode> &r_layers)
{
  r_layers.clear();
  const bNodeSocket *socket = paint_material_channel_socket_find(material, channel);
  if (socket == nullptr) {
    return false;
  }
  layer_model_collect(*socket, r_layers, 0);
  return !r_layers.is_empty();
}

/** The name a user recognizes: their own node label, else the layer's map, else the node name. */
std::string layer_model_name_get(const LayerModelNode &layer)
{
  /* A label the user set wins over every derived name, groups included: renaming a row writes the
   * label, so reading the group's node tree first would make renaming a folder do nothing. */
  if (layer.node != nullptr && layer.node->label[0] != '\0') {
    return layer.node->label;
  }
  if (layer.group_node != nullptr && layer.group_node->id != nullptr) {
    /* An unlabeled group is named after its node tree: that is the name the Shader Editor shows
     * for it, and renaming it there should rename the row. */
    return layer.group_node->id->name + 2;
  }
  if (layer.node == nullptr) {
    return "Unsupported layer";
  }
  if (layer.image != nullptr) {
    return layer.image->id.name + 2;
  }
  return layer.node->name;
}

/**
 * The row's per-channel editable pointers and Disabled bit, taken from the Mix node of \a layer in
 * the chain of \a channel. Positions agree across channels (the stack's positional invariant), so
 * the caller hands in the node at the same index of every channel's chain.
 */
void layer_model_channel_props_add(const LayerModelNode &layer,
                                   const int channel,
                                   PaintMaterialLayerStackEntry &r_entry)
{
  if (layer.node == nullptr || !layer.supported || layer.is_bare_base) {
    return;
  }
  const bNodeTree *owner_tree = layer.owner_tree ? layer.owner_tree : &layer.node->owner_tree();
  ID &tree_id = const_cast<ID &>(owner_tree->id);
  if (layer.factor != nullptr) {
    r_entry.channel_factor_props.add_overwrite(
        channel,
        RNA_pointer_create_discrete(
            &tree_id, RNA_PaintMaterialLayerOpacity, const_cast<bNodeSocket *>(layer.factor)));
  }
  if (layer.node->typeinfo != nullptr && layer.node->typeinfo->rna_ext.srna != nullptr) {
    PointerRNA node_ptr = RNA_pointer_create_discrete(
        &tree_id, layer.node->typeinfo->rna_ext.srna, const_cast<bNode *>(layer.node));
    if (RNA_struct_find_property(&node_ptr, "blend_type") != nullptr) {
      r_entry.channel_blend_props.add_overwrite(channel, node_ptr);
    }
  }
  CompositeMixNode mix;
  PaintMaterialLayerChannelState state = PaintMaterialLayerChannelState::Absent;
  if (layer.is_group || channel < 0 || channel >= 32 ||
      !composite_mix_node_read(*layer.node, mix) || !composite_mix_channel_state_get(mix, state))
  {
    return;
  }
  if (state == PaintMaterialLayerChannelState::Disabled) {
    r_entry.disabled_channels_mask |= uint32_t(1) << channel;
  }
  else if (state == PaintMaterialLayerChannelState::Enabled) {
    /* Spec 18 I2': the base map being on makes the row contribute to this channel, corrections
     * or not -- a correction can only add channels on top of this, never take one away. Under
     * corrections the state is the base's own, read below them (spec 18 I1'). */
    r_entry.contributing_channels_mask |= uint32_t(1) << channel;
  }
}

/** The correction row of \a corrections carrying \a marker, or null when there is none. */
PaintMaterialLayerCorrectionEntry *correction_model_row_find(
    Vector<PaintMaterialLayerCorrectionEntry> &corrections, const bUUID &marker)
{
  for (PaintMaterialLayerCorrectionEntry &correction : corrections) {
    if (BLI_uuid_equal(correction.marker, marker)) {
      return &correction;
    }
  }
  return nullptr;
}

/** The correction row carrying \a marker anywhere in the stack, or null when there is none. */
PaintMaterialLayerCorrectionEntry *correction_model_by_marker_find(
    Vector<PaintMaterialLayerStackEntry> &r_entries, const bUUID &marker)
{
  for (PaintMaterialLayerStackEntry &entry : r_entries) {
    PaintMaterialLayerCorrectionEntry *correction = correction_model_row_find(
        entry.content_corrections, marker);
    if (correction == nullptr) {
      correction = correction_model_row_find(entry.mask_corrections, marker);
    }
    if (correction != nullptr) {
      return correction;
    }
  }
  return nullptr;
}

/** The UI row of one correction's Mix node; the per-channel state joins later, by marker. */
PaintMaterialLayerCorrectionEntry correction_model_entry_from_node(const bNode &node)
{
  PaintMaterialLayerCorrectionEntry correction;
  correction.marker = BKE_paint_material_layer_marker_get(node);
  correction.section = bke::paint_layer::correction_section_get(node);
  correction.effect = bke::paint_layer::correction_effect_get(node);
  /* The insert leaves the node's label empty -- the row is named by the model, not by the graph
   * -- so an unnamed correction reads as "Correction" instead of an empty cell. */
  correction.name = (node.label[0] != '\0') ? node.label : "Correction";
  correction.label = const_cast<char *>(node.label);
  CompositeMixNode mix;
  if (!composite_mix_node_read(node, mix)) {
    return correction;
  }
  correction.blend = mix.blend;
  correction.enabled = !node.is_muted();
  /* The same reading of coverage and opacity the layer rows use: the editable value is the
   * Multiply's constant when the per-channel shape is there, the bare Factor when it is not. */
  if (mix.factor_opacity != nullptr) {
    correction.opacity =
        static_cast<const bNodeSocketValueFloat *>(mix.factor_opacity->default_value)->value;
  }
  else if (mix.factor != nullptr && BKE_paint_material_source_socket(*mix.factor) == nullptr) {
    correction.opacity =
        static_cast<const bNodeSocketValueFloat *>(mix.factor->default_value)->value;
  }
  return correction;
}

/**
 * The correction row's per-channel editable pointers and Disabled bit, joined by marker: each
 * channel owns its own nodes for the correction, and only the marker says they are one row.
 */
void correction_model_channel_props_add(const bNode &node,
                                        const bNodeTree *owner_tree,
                                        const int channel,
                                        PaintMaterialLayerCorrectionEntry &r_correction)
{
  const bNodeTree *tree = (owner_tree != nullptr) ? owner_tree : &node.owner_tree();
  ID &tree_id = const_cast<ID &>(tree->id);
  CompositeMixNode mix;
  if (!composite_mix_node_read(node, mix)) {
    return;
  }
  const bNodeSocket *factor = mix.factor_opacity;
  if (factor == nullptr && mix.factor != nullptr &&
      BKE_paint_material_source_socket(*mix.factor) == nullptr)
  {
    factor = mix.factor;
  }
  if (factor != nullptr) {
    r_correction.channel_factor_props.add_overwrite(
        channel,
        RNA_pointer_create_discrete(
            &tree_id, RNA_PaintMaterialLayerOpacity, const_cast<bNodeSocket *>(factor)));
  }
  if (node.typeinfo != nullptr && node.typeinfo->rna_ext.srna != nullptr) {
    PointerRNA node_ptr = RNA_pointer_create_discrete(
        &tree_id, node.typeinfo->rna_ext.srna, const_cast<bNode *>(&node));
    if (RNA_struct_find_property(&node_ptr, "blend_type") != nullptr) {
      r_correction.channel_blend_props.add_overwrite(channel, node_ptr);
    }
  }
  PaintMaterialLayerChannelState state = PaintMaterialLayerChannelState::Absent;
  if (channel < 0 || channel >= 32 || !composite_mix_channel_state_get(mix, state)) {
    return;
  }
  if (state == PaintMaterialLayerChannelState::Disabled) {
    r_correction.disabled_channels_mask |= uint32_t(1) << channel;
  }
}

/** Whether \a nodes carry the same marker sequence as \a corrections, order included. */
bool correction_model_sequences_agree(
    const Vector<const bNode *> &nodes,
    const Vector<PaintMaterialLayerCorrectionEntry> &corrections)
{
  if (nodes.size() != corrections.size()) {
    return false;
  }
  for (const int i : corrections.index_range()) {
    if (!BLI_uuid_equal(BKE_paint_material_layer_marker_get(*nodes[i]),
                        corrections[i].marker))
    {
      return false;
    }
  }
  return true;
}

/**
 * Join \a channel_layer's corrections to the rows the reference channel built, by marker, and
 * flag the row when the channels disagree: the UI shows one row per correction, so channels that
 * cannot agree on which corrections a row has have no rows to show. The reference channel's
 * sequence is the one the row was built from, so it is what the others answer to.
 */
void correction_model_channel_join(const LayerModelNode &channel_layer,
                                   const int channel,
                                   PaintMaterialLayerStackEntry &r_entry)
{
  if (!correction_model_sequences_agree(channel_layer.content_correction_nodes,
                                        r_entry.content_corrections) ||
      !correction_model_sequences_agree(channel_layer.mask_correction_nodes,
                                        r_entry.mask_corrections))
  {
    r_entry.supported = false;
    r_entry.unsupported_reason = "Layer corrections disagree between channels";
    return;
  }
  for (const bNode *node : channel_layer.content_correction_nodes) {
    PaintMaterialLayerCorrectionEntry *correction = correction_model_row_find(
        r_entry.content_corrections, BKE_paint_material_layer_marker_get(*node));
    if (correction != nullptr) {
      correction_model_channel_props_add(*node, channel_layer.owner_tree, channel, *correction);
    }
  }
  for (const bNode *node : channel_layer.mask_correction_nodes) {
    PaintMaterialLayerCorrectionEntry *correction = correction_model_row_find(
        r_entry.mask_corrections, BKE_paint_material_layer_marker_get(*node));
    if (correction != nullptr) {
      correction_model_channel_props_add(*node, channel_layer.owner_tree, channel, *correction);
    }
  }
}

/**
 * The channels \a correction brings its row into (spec 18 I2'): a channel whose map it has on --
 * not switched off -- while the correction itself is not muted. The maps were joined by marker
 * across every channel and the Disabled bits collected with them, so this reads the row only
 * once both are done; a correction that is Absent in a channel has no map there and adds none.
 */
void correction_model_contributing_add(const PaintMaterialLayerCorrectionEntry &correction,
                                       PaintMaterialLayerStackEntry &r_entry)
{
  /* Only content brings pixels: a mask correction shapes coverage the row already has. */
  if (!correction.enabled || correction.section != PaintMaterialCorrectionSection::Content) {
    return;
  }
  for (const int channel : correction.channel_images.keys()) {
    if (correction.disabled_channels_mask & (uint32_t(1) << channel)) {
      continue;
    }
    r_entry.contributing_channels_mask |= uint32_t(1) << channel;
  }
}

PaintMaterialLayerStackEntry layer_model_entry_from_node(const Material &material,
                                                         const LayerModelNode &layer)
{
  PaintMaterialLayerStackEntry entry;
  entry.node_id = layer.node ? layer.node->identifier : 0;
  entry.owner_tree = layer.owner_tree;
  entry.material_sid = material.id.session_uid;
  entry.tree_sid = layer.owner_tree ? layer.owner_tree->id.session_uid : 0;
  entry.depth = layer.depth;
  entry.parent_node_id = layer.parent_node_id;
  entry.is_group = layer.is_group;
  entry.is_bare_base = layer.is_bare_base;
  entry.name = layer_model_name_get(layer);
  entry.blend = layer.blend;
  entry.opacity = layer.opacity;
  entry.enabled = layer.enabled;
  entry.has_mask = layer.has_mask;
  entry.supported = layer.supported;
  entry.unsupported_reason = layer.unsupported_reason;
  entry.marker = layer.node ? BKE_paint_material_layer_marker_get(*layer.node) : BLI_uuid_nil();
  if (layer.is_group && layer.group_node != nullptr && layer.group_node->id != nullptr &&
      GS(layer.group_node->id->name) == ID_NT)
  {
    entry.group_tree = id_cast<const bNodeTree *>(layer.group_node->id);
  }
  const bNodeTree *owner_tree = layer.owner_tree ?
                                    layer.owner_tree :
                                    (layer.node ? &layer.node->owner_tree() : nullptr);
  if (layer.node == nullptr || owner_tree == nullptr) {
    return entry;
  }
  /* A bare base has no Mix node of its own to carry a label, and an unsupported row is one this
   * file could not read in the first place; every other row is named through its node. */
  if (!layer.is_bare_base && layer.supported) {
    entry.label = const_cast<char *>(layer.node->label);
  }
  /* Color tag for group folders is stored on the node representing the stack row. */
  if (layer.is_group) {
    entry.color_tag = BKE_paint_material_layer_color_tag_get(*layer.node);
  }
  /* What the row is, and the colour a Fill row stands for; both live on the same node the layer
   * marker does, and a row without them reads as an unnamed Paint layer. */
  entry.kind = BKE_paint_material_layer_kind_get(*layer.node);
  BKE_paint_material_layer_fill_color_get(*layer.node, entry.fill_color);
  return entry;
}

}  // namespace

const bNodeSocket *paint_material_channel_socket_find(const Material &ma, const int channel)
{
  if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM) {
    /* #PAINT_LAYER_MAP_MASK and Ambient Occlusion have no Principled input to start from. */
    return nullptr;
  }
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
  const bNodeSocket *socket = bke::node_find_socket(
      *principled, SOCK_IN, UString::from_ptr_noinline(info.socket_name));
  if (socket == nullptr) {
    return nullptr;
  }
  if (channel != PAINT_MATERIAL_CHANNEL_NORMAL) {
    return socket;
  }
  /* The Principled Normal input carries an already transformed vector, which no stack of maps can
   * be recovered from; the maps sit one node earlier, on the Normal Map node's Color input. */
  const bNodeSocket *normal_source = BKE_paint_material_source_socket(*socket);
  if (normal_source == nullptr || normal_source->owner_node().type_legacy != SH_NODE_NORMAL_MAP) {
    return nullptr;
  }
  return bke::node_find_socket(normal_source->owner_node(), SOCK_IN, "Color"_ustr);
}

bool BKE_paint_material_layer_stack_from_material(
    const Main &bmain, const Material &material, Vector<PaintMaterialLayerStackEntry> &r_entries)
{
  r_entries.clear();
  if (material.nodetree == nullptr) {
    return false;
  }

  /* The channel that is actually wired gives the stack its shape; Base Color leads the pass list
   * because a layered material always wires it. */
  Vector<LayerModelNode> reference_layers;
  int reference_role = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  for (const int role : BKE_paint_material_composite_passes()) {
    if (layer_model_from_channel(material, role, reference_layers)) {
      reference_role = role;
      break;
    }
  }
  if (reference_layers.is_empty()) {
    return false;
  }

  /* Ordinals are int16 so that a UI can use one as a row key without a second mapping table. A
   * stack this deep is not a real material, but the cast has to stay defined anyway. */
  constexpr int max_ordinal = std::numeric_limits<int16_t>::max();
  /* Top-level rows are numbered by their position in the chain, so that an ordinal means the same
   * layer to the graph editor; rows inside a group are numbered from their own range. */
  int top_level_num = 0;
  int nested_num = 0;
  for (const int index : reference_layers.index_range()) {
    if (index >= max_ordinal) {
      break;
    }
    PaintMaterialLayerStackEntry entry = layer_model_entry_from_node(material,
                                                                    reference_layers[index]);
    if (reference_layers[index].depth == 0) {
      if (top_level_num >= PAINT_LAYER_GROUP_CHILD_ORDINAL_BASE) {
        break;
      }
      entry.ordinal = int16_t(top_level_num++);
    }
    else {
      entry.ordinal = int16_t(PAINT_LAYER_GROUP_CHILD_ORDINAL_BASE + nested_num++);
    }
    layer_model_channel_props_add(reference_layers[index], reference_role, entry);
    if (reference_layers[index].image != nullptr) {
      entry.channel_images.add_overwrite(reference_role, reference_layers[index].image);
    }
    /* The correction rows hang under this row (spec 18 §4.5): the reference channel's nodes give
     * them their identity, and every other channel joins its own nodes to them by marker. */
    for (const bNode *node : reference_layers[index].content_correction_nodes) {
      entry.content_corrections.append(correction_model_entry_from_node(*node));
      correction_model_channel_props_add(*node,
                                         reference_layers[index].owner_tree,
                                         reference_role,
                                         entry.content_corrections.last());
    }
    for (const bNode *node : reference_layers[index].mask_correction_nodes) {
      entry.mask_corrections.append(correction_model_entry_from_node(*node));
      correction_model_channel_props_add(*node,
                                         reference_layers[index].owner_tree,
                                         reference_role,
                                         entry.mask_corrections.last());
    }
    r_entries.append(std::move(entry));
  }

  for (const int role : BKE_paint_material_composite_passes()) {
    if (role == reference_role) {
      continue;
    }
    Vector<LayerModelNode> channel_layers;
    if (!layer_model_from_channel(material, role, channel_layers)) {
      continue;
    }
    if (channel_layers.size() == reference_layers.size()) {
      for (const int index : r_entries.index_range()) {
        layer_model_channel_props_add(channel_layers[index], role, r_entries[index]);
        correction_model_channel_join(channel_layers[index], role, r_entries[index]);
      }
    }
    for (const int index : r_entries.index_range()) {
      const LayerModelNode *match = nullptr;
      const Image *reference_image = reference_layers[index].image;
      if (reference_image != nullptr && !BLI_uuid_is_nil(reference_image->paint_layer_id)) {
        for (const LayerModelNode &candidate : channel_layers) {
          if (candidate.image != nullptr &&
              BLI_uuid_equal(candidate.image->paint_layer_id, reference_image->paint_layer_id))
          {
            match = &candidate;
            break;
          }
        }
      }
      else if (channel_layers.index_range().contains(index)) {
        /* No UUID to match on: fall back to the position in the chain, which is what the evaluator
         * assumes too when it borrows a wired channel's shape. */
        match = &channel_layers[index];
      }
      if (match != nullptr && match->image != nullptr) {
        r_entries[index].channel_images.add_overwrite(role, match->image);
      }
    }
  }

  /* Maps with no link to travel by are found by tag rather than by link. A correction's map
   * belongs to the correction row -- its marker is what the tag carries -- and the rest keep the
   * old rule: a map belongs to the layer whose marker it carries, and a channel's own map reached
   * its layer through the UUID match in the pass above, so only the two roles without a Principled
   * socket are matched here. Collected in one pass: #Main can hold thousands of images and this
   * runs on every tree rebuild. */
  Vector<Image *> tagged_maps;
  for (Image &image : const_cast<Main &>(bmain).images) {
    if (image.paint_layer_channel < 0 || image.paint_layer_channel > PAINT_LAYER_MAP_MASK ||
        BLI_uuid_is_nil(image.paint_layer_id))
    {
      continue;
    }
    tagged_maps.append(&image);
  }
  for (Image *map : tagged_maps) {
    PaintMaterialLayerCorrectionEntry *correction = correction_model_by_marker_find(
        r_entries, map->paint_layer_id);
    if (correction != nullptr) {
      /* Keyed by the channel the map paints, whatever role the tag carries. */
      correction->channel_images.add_overwrite(map->paint_layer_channel, map);
      continue;
    }
    if (!ELEM(map->paint_layer_channel, PAINT_MATERIAL_CHANNEL_AO, PAINT_LAYER_MAP_MASK)) {
      continue;
    }
    for (PaintMaterialLayerStackEntry &entry : r_entries) {
      if (BLI_uuid_is_nil(entry.marker)) {
        continue;
      }
      if (BLI_uuid_equal(map->paint_layer_id, entry.marker)) {
        entry.channel_images.add_overwrite(map->paint_layer_channel, map);
        if (map->paint_layer_channel == PAINT_LAYER_MAP_MASK) {
          /* The mask's on/off state travels with the image, not the links: a switched-off mask
           * has no link to read. */
          entry.mask_enabled = map->paint_layer_mask_disabled == 0;
        }
      }
    }
  }

  /* Spec 18 I2': a row contributes to the channels its base map is on -- collected per channel by
   * the props pass above -- and to the ones a correction of its own paints into. The maps and
   * Disabled bits the corrections gathered across every channel are what that answer reads, so
   * this runs after them. */
  for (PaintMaterialLayerStackEntry &entry : r_entries) {
    for (const PaintMaterialLayerCorrectionEntry &correction : entry.content_corrections) {
      correction_model_contributing_add(correction, entry);
    }
    for (const PaintMaterialLayerCorrectionEntry &correction : entry.mask_corrections) {
      correction_model_contributing_add(correction, entry);
    }
  }
  return !r_entries.is_empty();
}

bool BKE_paint_material_has_layer_stack(const Material &material)
{
  if (material.nodetree == nullptr) {
    return false;
  }
  Vector<LayerModelNode> layers;
  for (const int role : BKE_paint_material_composite_passes()) {
    if (layer_model_from_channel(material, role, layers)) {
      return true;
    }
  }
  return false;
}

}  // namespace blender
