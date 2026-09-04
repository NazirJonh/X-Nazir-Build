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
#include "BKE_node_legacy_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_material_resolve.hh"

#include "BLI_listbase_iterator.hh"
#include "BLI_string.h"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "paint_material_composite_internal.hh"

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
      if (!composite_image_from_socket(*muted_mix.top, layer.image, iuser)) {
        layer.supported = false;
        layer.unsupported_reason = "Layer source is not an image";
      }
      r_layers.append(layer);
      return;
    }
  }

  const bNode *shallow_source = composite_source_node_shallow(socket);
  if (shallow_source != nullptr && BKE_paint_material_is_layer_group(*shallow_source)) {
    /* A group at the bottom of a chain: a row of its own, with its sub-stack below it. Handled
     * here because the resolver would otherwise walk straight through the group instance and
     * report a node inside it as if it were a layer of this chain. */
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
        layer_model_append_unsupported(r_layers, shallow_source, "Bottom of stack is not an image");
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
  if (!composite_image_from_socket(*mix.top, layer.image, iuser)) {
    layer.supported = false;
    layer.unsupported_reason = "Layer source is not an image";
  }
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
  if (layer.group_node != nullptr && layer.group_node->id != nullptr) {
    /* A group is named after its node tree: that is the name the Shader Editor shows for it, and
     * renaming it there should rename the row. */
    return layer.group_node->id->name + 2;
  }
  if (layer.node == nullptr) {
    return "Unsupported layer";
  }
  if (layer.node->label[0] != '\0') {
    return layer.node->label;
  }
  if (layer.image != nullptr) {
    return layer.image->id.name + 2;
  }
  return layer.node->name;
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
  const bNodeTree *owner_tree = layer.owner_tree ? layer.owner_tree :
                                                    (layer.node ? &layer.node->owner_tree() : nullptr);
  if (layer.node == nullptr || owner_tree == nullptr) {
    return entry;
  }
  ID &tree_id = const_cast<ID &>(owner_tree->id);
  /* #layer.factor is only ever the constant a layer actually has to edit -- either a bare Factor,
   * or the Multiply's opacity input when coverage and opacity coexist -- and is null in the one
   * remaining case, a legacy link straight into Factor with nothing to separate an opacity from.
   * See #layer_model_read_mix. */
  if (layer.factor != nullptr) {
    /* #RNA_PaintMaterialLayerOpacity, not #RNA_NodeSocket's own "default_value": the layer stack
     * always shows opacity as 0-100%, independent of the user's Factor Display preference, which
     * a plain node-socket property would otherwise follow. */
    entry.factor_prop = RNA_pointer_create_discrete(
        &tree_id, RNA_PaintMaterialLayerOpacity, const_cast<bNodeSocket *>(layer.factor));
  }
  /* An unregistered node type has no typeinfo; a Mix group has one without a blend enum. */
  if (layer.node->typeinfo != nullptr && layer.node->typeinfo->rna_ext.srna != nullptr) {
    PointerRNA node_ptr = RNA_pointer_create_discrete(
        &tree_id, layer.node->typeinfo->rna_ext.srna, const_cast<bNode *>(layer.node));
    if (RNA_struct_find_property(&node_ptr, "blend_type") != nullptr) {
      entry.blend_prop = node_ptr;
    }
  }
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
    if (reference_layers[index].image != nullptr) {
      entry.channel_images.add_overwrite(reference_role, reference_layers[index].image);
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

  /* Ambient Occlusion and masks have no Principled socket, so their maps are found by tag rather
   * than by link. Collected in one pass: #Main can hold thousands of images and this runs on every
   * tree rebuild. */
  Vector<Image *> tagged_maps;
  for (Image &image : const_cast<Main &>(bmain).images) {
    if (ELEM(image.paint_layer_channel, PAINT_MATERIAL_CHANNEL_AO, PAINT_LAYER_MAP_MASK) &&
        !BLI_uuid_is_nil(image.paint_layer_id))
    {
      tagged_maps.append(&image);
    }
  }
  if (!tagged_maps.is_empty()) {
    for (PaintMaterialLayerStackEntry &entry : r_entries) {
      const Image *reference_image = entry.channel_images.lookup_default(reference_role, nullptr);
      if (reference_image == nullptr || BLI_uuid_is_nil(reference_image->paint_layer_id)) {
        continue;
      }
      for (Image *map : tagged_maps) {
        if (BLI_uuid_equal(map->paint_layer_id, reference_image->paint_layer_id)) {
          entry.channel_images.add_overwrite(map->paint_layer_channel, map);
        }
      }
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
