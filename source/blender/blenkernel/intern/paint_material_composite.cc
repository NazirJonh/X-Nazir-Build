/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "BKE_paint_material_composite.hh"

#include "BKE_idprop.hh"
#include "BKE_image.hh"
#include "BKE_image_partial_update.hh"
#include "BKE_main.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_paint.hh"
#include "BKE_paint_material_resolve.hh"

#include "paint_material_composite_internal.hh"

#include "BLI_hash.hh"
#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_map.hh"
#include "BLI_math_base.h"
#include "BLI_math_color.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_rect.h"
#include "BLI_string.h"
#include "BLI_task.hh"
#include "BLI_time.h"
#include "BLI_utildefines.h"
#include "BLI_uuid.h"

#include "BLT_translation.hh"

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_tree_interface_types.h"
#include "DNA_node_types.h"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include <cstring>
#include <memory>

namespace blender {

/* The image change log the composite cache subscribes to. Brought in wholesale because the switch
 * over #ePartialUpdateCollectResult reads badly with the full qualification on every label. */
using namespace bke::image::partial_update;

/* -------------------------------------------------------------------- */
/** \name Stack Derivation
 *
 * Recognizes the one graph shape this module can reproduce: image layers stacked with Mix nodes.
 * Everything here only reads the tree, so it is as cheap as the resolver and may run on a redraw.
 * \{ */

/** The subset of Mix blend modes that #blend_layer_byte reproduces exactly. */
static bool composite_blend_from_ramp_blend(const int ramp_blend, CompositeBlend &r_blend)
{
  switch (ramp_blend) {
    case MA_RAMP_BLEND:
      r_blend = CompositeBlend::Mix;
      return true;
    case MA_RAMP_MULT:
      r_blend = CompositeBlend::Multiply;
      return true;
    case MA_RAMP_OVERLAY:
      r_blend = CompositeBlend::Overlay;
      return true;
    case MA_RAMP_ADD:
      r_blend = CompositeBlend::Add;
      return true;
    default:
      /* Screen, Difference, Hue and the rest have no byte blend function here. Reporting them as
       * not-a-stack sends the channel to the bake, which evaluates them properly, rather than
       * showing the user a composite that quietly differs from the render. */
      return false;
  }
}

/* -------------------------------------------------------------------- */
/** \name Normal Combine Group
 * \{ */

/** Name of the ID property that marks the group, and the value that identifies this one. */
static const char *NORMAL_COMBINE_PROP = "pbr_paint_node_group";
static const char *NORMAL_COMBINE_VALUE = "NORMAL_COMBINE";
/** A folder of layers, composited on transparency; see `08 §2.2`. */
static const char *LAYER_GROUP_VALUE = "LAYER_GROUP";
/* NOTE: DO NOT translate, it is what an existing group is found by. */
static const char *NORMAL_COMBINE_TREE_NAME = "PBR Normal Combine";

bool BKE_paint_material_is_layer_group(const bNode &node)
{
  if (!node.is_group() || node.id == nullptr || GS(node.id->name) != ID_NT) {
    return false;
  }
  const IDProperty *properties = IDP_GetProperties(const_cast<ID *>(node.id));
  if (properties == nullptr) {
    return false;
  }
  /* The same marker property the Normal Combine group uses, with a different value: one place to
   * look to know whether a group node is one of ours, and which. */
  const IDProperty *marker = IDP_GetPropertyTypeFromGroup(
      properties, NORMAL_COMBINE_PROP, IDP_STRING);
  return marker != nullptr && STREQ(IDP_string_get(marker), LAYER_GROUP_VALUE);
}

bool BKE_paint_material_is_normal_combine_group(const bNode &node)
{
  if (!node.is_group() || node.id == nullptr || GS(node.id->name) != ID_NT) {
    return false;
  }
  const IDProperty *properties = IDP_GetProperties(const_cast<ID *>(node.id));
  if (properties == nullptr) {
    return false;
  }
  const IDProperty *marker = IDP_GetPropertyTypeFromGroup(
      properties, NORMAL_COMBINE_PROP, IDP_STRING);
  return marker != nullptr && STREQ(IDP_string_get(marker), NORMAL_COMBINE_VALUE);
}

/** The group already in \a bmain, or null. Found by its marker, so a rename does not lose it. */
static bNodeTree *normal_combine_group_find(Main &bmain)
{
  for (bNodeTree &ntree : bmain.nodetrees) {
    const IDProperty *properties = IDP_GetProperties(&ntree.id);
    if (properties == nullptr) {
      continue;
    }
    const IDProperty *marker = IDP_GetPropertyTypeFromGroup(
        properties, NORMAL_COMBINE_PROP, IDP_STRING);
    if (marker != nullptr && STREQ(IDP_string_get(marker), NORMAL_COMBINE_VALUE)) {
      return &ntree;
    }
  }
  return nullptr;
}

/** A Vector Math node set to \a operation. */
static bNode *normal_combine_vector_math_add(bNodeTree &group,
                                             const int operation,
                                             const float2 location)
{
  bNode *node = bke::node_add_node(nullptr, group, "ShaderNodeVectorMath"_ustr);
  node->custom1 = operation;
  node->location[0] = location.x;
  node->location[1] = location.y;
  return node;
}

/** Sets the second and third Vector inputs of a Multiply Add, which decode or encode a normal. */
static void normal_combine_range_map_set(bNode &node, const float scale, const float offset)
{
  bNodeSocket *scale_socket = bke::node_find_socket(node, SOCK_IN, "Vector_001"_ustr);
  bNodeSocket *offset_socket = bke::node_find_socket(node, SOCK_IN, "Vector_002"_ustr);
  copy_v3_fl(static_cast<bNodeSocketValueVector *>(scale_socket->default_value)->value, scale);
  copy_v3_fl(static_cast<bNodeSocketValueVector *>(offset_socket->default_value)->value, offset);
}

bNodeTree *BKE_paint_material_normal_combine_group_ensure(Main &bmain)
{
  if (bNodeTree *existing = normal_combine_group_find(bmain)) {
    return existing;
  }

  bNodeTree *group = bke::node_tree_add_tree(&bmain, NORMAL_COMBINE_TREE_NAME, "ShaderNodeTree");
  IDProperty *properties = IDP_EnsureProperties(&group->id);
  IDPropertyTemplate value = {0};
  value.string.str = NORMAL_COMBINE_VALUE;
  value.string.len = int(strlen(NORMAL_COMBINE_VALUE)) + 1;
  value.string.subtype = IDP_STRING_SUB_UTF8;
  IDP_AddToGroup(properties, IDP_New(IDP_STRING, &value, NORMAL_COMBINE_PROP));

  /* Interface identifiers are handed out in creation order: `Socket_0` .. `Socket_3`. */
  group->tree_interface.add_socket(
      DATA_("A"), "", "NodeSocketColor", NODE_INTERFACE_SOCKET_INPUT, nullptr);
  group->tree_interface.add_socket(
      DATA_("B"), "", "NodeSocketColor", NODE_INTERFACE_SOCKET_INPUT, nullptr);
  bNodeTreeInterfaceSocket *factor = group->tree_interface.add_socket(
      DATA_("Factor"), "", "NodeSocketFloat", NODE_INTERFACE_SOCKET_INPUT, nullptr);
  auto &factor_data = *static_cast<bNodeSocketValueFloat *>(factor->socket_data);
  factor_data.subtype = PROP_FACTOR;
  factor_data.min = 0.0f;
  factor_data.max = 1.0f;
  factor_data.value = 1.0f;
  group->tree_interface.add_socket(
      DATA_("Result"), "", "NodeSocketColor", NODE_INTERFACE_SOCKET_OUTPUT, nullptr);

  bNode *group_input = bke::node_add_node(nullptr, *group, "NodeGroupInput"_ustr);
  group_input->location[0] = -800;
  bNode *group_output = bke::node_add_node(nullptr, *group, "NodeGroupOutput"_ustr);
  group_output->location[0] = 400;

  /* Decode both maps out of [0, 1] and into vectors. */
  bNode *decode_a = normal_combine_vector_math_add(
      *group, NODE_VECTOR_MATH_MULTIPLY_ADD, float2(-600.0f, 120.0f));
  normal_combine_range_map_set(*decode_a, 2.0f, -1.0f);
  bNode *decode_b = normal_combine_vector_math_add(
      *group, NODE_VECTOR_MATH_MULTIPLY_ADD, float2(-600.0f, -120.0f));
  normal_combine_range_map_set(*decode_b, 2.0f, -1.0f);

  /* Whiteout: the slopes add, the two Z multiply. */
  bNode *slopes = normal_combine_vector_math_add(
      *group, NODE_VECTOR_MATH_ADD, float2(-400.0f, 120.0f));
  bNode *depths = normal_combine_vector_math_add(
      *group, NODE_VECTOR_MATH_MULTIPLY, float2(-400.0f, -120.0f));

  bNode *split_slopes = bke::node_add_node(nullptr, *group, "ShaderNodeSeparateXYZ"_ustr);
  split_slopes->location[0] = -220;
  split_slopes->location[1] = 120;
  bNode *split_depths = bke::node_add_node(nullptr, *group, "ShaderNodeSeparateXYZ"_ustr);
  split_depths->location[0] = -220;
  split_depths->location[1] = -120;
  bNode *rebuilt = bke::node_add_node(nullptr, *group, "ShaderNodeCombineXYZ"_ustr);
  rebuilt->location[0] = -60;

  bNode *normalize = normal_combine_vector_math_add(
      *group, NODE_VECTOR_MATH_NORMALIZE, float2(100.0f, 0.0f));
  bNode *encode = normal_combine_vector_math_add(
      *group, NODE_VECTOR_MATH_MULTIPLY_ADD, float2(240.0f, 0.0f));
  normal_combine_range_map_set(*encode, 0.5f, 0.5f);

  /* The factor interpolates between the layer below and the combined result, so that a masked
   * layer fades out to what it covers rather than to a flat normal. */
  bNode *mix = bke::node_add_node(nullptr, *group, "ShaderNodeMix"_ustr);
  auto &mix_storage = *static_cast<NodeShaderMix *>(mix->storage);
  mix_storage.data_type = SOCK_RGBA;
  mix->location[0] = 240;
  mix->location[1] = 180;

  auto link = [&](bNode &from, const char *from_socket, bNode &to, const char *to_socket) {
    bke::node_add_link(
        *group,
        from,
        *bke::node_find_socket(from, SOCK_OUT, UString::from_ptr_noinline(from_socket)),
        to,
        *bke::node_find_socket(to, SOCK_IN, UString::from_ptr_noinline(to_socket)));
  };

  link(*group_input, "Socket_0", *decode_a, "Vector");
  link(*group_input, "Socket_1", *decode_b, "Vector");
  link(*decode_a, "Vector", *slopes, "Vector");
  link(*decode_b, "Vector", *slopes, "Vector_001");
  link(*decode_a, "Vector", *depths, "Vector");
  link(*decode_b, "Vector", *depths, "Vector_001");
  link(*slopes, "Vector", *split_slopes, "Vector");
  link(*depths, "Vector", *split_depths, "Vector");
  link(*split_slopes, "X", *rebuilt, "X");
  link(*split_slopes, "Y", *rebuilt, "Y");
  link(*split_depths, "Z", *rebuilt, "Z");
  link(*rebuilt, "Vector", *normalize, "Vector");
  link(*normalize, "Vector", *encode, "Vector");
  link(*group_input, "Socket_0", *mix, "A_Color");
  link(*encode, "Vector", *mix, "B_Color");
  link(*group_input, "Socket_2", *mix, "Factor_Float");
  link(*mix, "Result_Color", *group_output, "Socket_3");

  BKE_ntree_update_tag_all(group);
  return group;
}

/** \} */

bool composite_mix_node_read(const bNode &node, CompositeMixNode &r_mix)
{
  if (BKE_paint_material_is_normal_combine_group(node)) {
    /* Sockets by name: the group is the engine's own, and its interface names are the contract
     * an add-on wiring it up sees. */
    r_mix.factor = bke::node_find_socket(node, SOCK_IN, "Factor"_ustr);
    r_mix.bottom = bke::node_find_socket(node, SOCK_IN, "A"_ustr);
    r_mix.top = bke::node_find_socket(node, SOCK_IN, "B"_ustr);
    r_mix.blend = CompositeBlend::NormalCombine;
    return r_mix.factor != nullptr && r_mix.bottom != nullptr && r_mix.top != nullptr;
  }
  if (node.type_legacy == SH_NODE_MIX_RGB_LEGACY) {
    r_mix.blend_supported = composite_blend_from_ramp_blend(node.custom1, r_mix.blend);
    r_mix.factor = bke::node_find_socket(node, SOCK_IN, "Fac"_ustr);
    r_mix.bottom = bke::node_find_socket(node, SOCK_IN, "Color1"_ustr);
    r_mix.top = bke::node_find_socket(node, SOCK_IN, "Color2"_ustr);
    return r_mix.factor != nullptr && r_mix.bottom != nullptr && r_mix.top != nullptr;
  }
  if (node.type_legacy == SH_NODE_MIX) {
    const NodeShaderMix *storage = static_cast<const NodeShaderMix *>(node.storage);
    if (storage == nullptr || storage->data_type != SOCK_RGBA) {
      return false;
    }
    /* A per-component factor is three independent mixes, which the byte blend functions do not
     * express. */
    if (storage->factor_mode != NODE_MIX_MODE_UNIFORM) {
      return false;
    }
    r_mix.blend_supported = composite_blend_from_ramp_blend(storage->blend_type, r_mix.blend);
    r_mix.factor = bke::node_find_socket(node, SOCK_IN, "Factor_Float"_ustr);
    r_mix.bottom = bke::node_find_socket(node, SOCK_IN, "A_Color"_ustr);
    r_mix.top = bke::node_find_socket(node, SOCK_IN, "B_Color"_ustr);
    return r_mix.factor != nullptr && r_mix.bottom != nullptr && r_mix.top != nullptr;
  }
  return false;
}

/**
 * The image driving \a socket, or false when its source is not a plain sampleable image.
 *
 * The same rule as the resolver's image case: only a #ShaderNodeTexImage counts, and a tiled
 * (UDIM) image has no single buffer to composite.
 */
bool composite_image_from_socket(const bNodeSocket &socket,
                                 Image *&r_image,
                                 const ImageUser *&r_iuser,
                                 bool *r_from_alpha)
{
  const bNodeSocket *source = BKE_paint_material_source_socket(socket);
  if (source == nullptr) {
    return false;
  }
  if (r_from_alpha != nullptr) {
    /* Which output the link left the node by. A layer stack drives the factor from the layer's
     * Alpha, and reading its Color there instead would modulate the layer by its own brightness
     * -- the difference between a stack that covers correctly and one that does not. */
    *r_from_alpha = source->identifier_ustr() == "Alpha"_ustr;
  }
  const bNode &node = source->owner_node();
  if (node.type_legacy != SH_NODE_TEX_IMAGE || node.id == nullptr || GS(node.id->name) != ID_IM) {
    return false;
  }
  const NodeTexImage *storage = static_cast<const NodeTexImage *>(node.storage);
  if (storage == nullptr) {
    return false;
  }
  Image *image = id_cast<Image *>(node.id);
  if (image->source == IMA_SRC_TILED) {
    return false;
  }
  r_image = image;
  r_iuser = &storage->iuser;
  return true;
}

/**
 * The node feeding \a socket, skipping reroutes and muted nodes but never entering a group.
 *
 * #BKE_paint_material_source_socket answers "what value arrives here", which for a group means the
 * node inside it. This answers "what node produced it", which is what a group used as an operation
 * -- the normal combine -- has to be recognized by.
 */
const bNode *composite_source_node_shallow(const bNodeSocket &socket)
{
  const bNodeSocket *current = &socket;
  /* A malformed tree can in principle cycle; bound the walk rather than trust the data. */
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
    if (from_node.is_muted()) {
      const bNodeLink *internal = nullptr;
      for (const bNodeLink &candidate : from_node.internal_links()) {
        if (candidate.tosock == link->fromsock) {
          internal = &candidate;
          break;
        }
      }
      if (internal == nullptr) {
        return nullptr;
      }
      current = internal->fromsock;
      continue;
    }
    return &from_node;
  }
  return nullptr;
}

/**
 * Collect the stack feeding \a socket into \a r_layers, bottom first.
 *
 * Recurses down the Mix chain first so that the deepest image -- the bottom of the stack -- is
 * appended before anything that covers it.
 */
static bool composite_stack_collect(const bNodeSocket &socket,
                                    Vector<PaintMaterialCompositeImageLayer> &r_layers,
                                    int depth);

/**
 * Append the sub-stack of \a group, reached through \a socket, to \a r_layers.
 *
 * The group's own output socket says which channel to follow: the instance's outputs and the Group
 * Output's inputs come from the same interface and therefore share identifiers, so this needs no
 * channel names.
 */
static bool composite_stack_collect_group(const bNodeSocket &socket,
                                          const bNode &group,
                                          Vector<PaintMaterialCompositeImageLayer> &r_layers,
                                          const int depth)
{
  const bNodeTree *group_tree = reinterpret_cast<const bNodeTree *>(group.id);
  if (group_tree == nullptr || socket.directly_linked_links().is_empty()) {
    return false;
  }
  const bNodeSocket *from = socket.directly_linked_links()[0]->fromsock;
  if (from == nullptr) {
    return false;
  }

  group_tree->ensure_topology_cache();
  for (const bNode &node : group_tree->nodes) {
    if (node.type_legacy != NODE_GROUP_OUTPUT) {
      continue;
    }
    if (const bNodeSocket *result = bke::node_find_socket(
            node, SOCK_IN, from->identifier_ustr()))
    {
      return composite_stack_collect(*result, r_layers, depth + 1);
    }
  }
  return false;
}

static bool composite_stack_collect(const bNodeSocket &socket,
                                    Vector<PaintMaterialCompositeImageLayer> &r_layers,
                                    const int depth)
{
  /* A malformed tree can cycle, and a very deep chain is not worth compositing anyway. */
  if (depth > 64) {
    return false;
  }

  /* The normal combine group has to be recognized before the socket walk resolves through it:
   * #BKE_paint_material_source_socket descends into a group and reports the node inside that
   * happens to feed the output, which says nothing about what the group as a whole does. */
  const bNode *shallow_source = composite_source_node_shallow(socket);
  if (shallow_source != nullptr && BKE_paint_material_is_layer_group(*shallow_source)) {
    /* A layer group is a sub-stack composited on transparency and laid over the rest as one
     * layer. Flattening it into this list is only the same picture when the node above it does
     * nothing more than stack the two -- see below, where that is checked; here the group is
     * simply walked into, which is that already-checked case. */
    return composite_stack_collect_group(socket, *shallow_source, r_layers, depth);
  }
  const bool is_combine_group = shallow_source != nullptr &&
                                BKE_paint_material_is_normal_combine_group(*shallow_source);

  if (!is_combine_group) {
    const bNodeSocket *source = BKE_paint_material_source_socket(socket);
    if (source == nullptr) {
      /* An unlinked input is a constant. Nothing to composite and nothing to paint on. */
      return false;
    }
    const bNode &node = source->owner_node();
    if (node.type_legacy == SH_NODE_TEX_IMAGE) {
      PaintMaterialCompositeImageLayer layer;
      if (!composite_image_from_socket(socket, layer.color_image, layer.color_iuser)) {
        return false;
      }
      /* The bottom layer has nothing under it: its own blend and opacity would have no meaning. */
      layer.is_bare_base = true;
      r_layers.append(layer);
      return true;
    }
    shallow_source = &node;
  }

  CompositeMixNode mix;
  if (!composite_mix_node_read(*shallow_source, mix)) {
    return false;
  }
  if (!mix.blend_supported) {
    /* Screen, Difference, Hue and the rest have no byte blend function here. Reporting them as
     * not-a-stack sends the channel to the bake, which evaluates them properly, rather than
     * showing the user a composite that quietly differs from the render. */
    return false;
  }
  /* Nothing under this Mix node means it is the bottom of a uniform chain: what it blends over is
   * the transparency its own socket holds, so the list simply starts here. */
  if (composite_source_node_shallow(*mix.bottom) != nullptr &&
      !composite_stack_collect(*mix.bottom, r_layers, depth + 1))
  {
    return false;
  }

  PaintMaterialCompositeImageLayer layer;
  layer.blend = mix.blend;

  const bNode *top_source = composite_source_node_shallow(*mix.top);
  if (top_source != nullptr && BKE_paint_material_is_layer_group(*top_source)) {
    /* An isolated group flattens into this list only when the node above it does nothing but
     * stack the two: any other blend, or a factor below one, composites the group as a whole and
     * is not the same as compositing its layers one after another. Reporting those as not-a-stack
     * sends the channel to the bake, which evaluates the graph properly, rather than showing a
     * composite that quietly differs from the render. */
    if (mix.blend != CompositeBlend::Mix) {
      return false;
    }
    if (BKE_paint_material_source_socket(*mix.factor) != nullptr) {
      return false;
    }
    const float factor =
        static_cast<const bNodeSocketValueFloat *>(mix.factor->default_value)->value;
    if (factor < 1.0f) {
      return false;
    }
    return composite_stack_collect_group(*mix.top, *top_source, r_layers, depth);
  }

  if (!composite_image_from_socket(*mix.top, layer.color_image, layer.color_iuser)) {
    return false;
  }
  if (BKE_paint_material_source_socket(*mix.factor) != nullptr) {
    /* A linked factor is a mask, and only an image one can be sampled per pixel. */
    if (!composite_image_from_socket(
            *mix.factor, layer.mask_image, layer.mask_iuser, &layer.mask_from_alpha))
    {
      return false;
    }
  }
  else {
    layer.opacity = static_cast<const bNodeSocketValueFloat *>(mix.factor->default_value)->value;
  }
  r_layers.append(layer);
  return true;
}

/**
 * The stack \a channel is wired as in \a ma's node tree, or false when it is not wired as one.
 */
static bool composite_stack_from_graph(const Material &ma,
                                       const int channel,
                                       Vector<PaintMaterialCompositeImageLayer> &r_layers)
{
  r_layers.clear();

  if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM) {
    /* #PAINT_LAYER_MAP_MASK and "none" are not channels and have no socket to start from. */
    return false;
  }
  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
      eMaterialPaintChannel(channel));
  if (info.socket_name == nullptr) {
    return false;
  }
  ChannelUnavailableReason reason = ChannelUnavailableReason::None;
  const bNode *principled = BKE_paint_material_principled_find(ma, reason);
  if (principled == nullptr) {
    return false;
  }
  const bNodeSocket *socket = bke::node_find_socket(
      *principled, SOCK_IN, UString::from_ptr_noinline(info.socket_name));
  if (socket == nullptr) {
    return false;
  }

  if (channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
    /* The Principled Normal input carries an already transformed vector, which no stack of maps
     * can be recovered from. The maps themselves sit one node earlier, on the Normal Map node's
     * Color input, in the same encoded space a stroke paints -- so that is where the chain is
     * read from, exactly as the resolver reads a single normal map from there. */
    const bNodeSocket *normal_source = BKE_paint_material_source_socket(*socket);
    if (normal_source == nullptr || normal_source->owner_node().type_legacy != SH_NODE_NORMAL_MAP)
    {
      return false;
    }
    socket = bke::node_find_socket(normal_source->owner_node(), SOCK_IN, "Color"_ustr);
    if (socket == nullptr) {
      return false;
    }
  }

  if (!composite_stack_collect(*socket, r_layers, 0)) {
    r_layers.clear();
    return false;
  }
  return !r_layers.is_empty();
}

/** The image tagged as \a channel of the paint layer \a layer_id, or null. */
static Image *composite_layer_map_find(const Main &bmain, const bUUID &layer_id, const int channel)
{
  for (Image &image : const_cast<Main &>(bmain).images) {
    if (image.paint_layer_channel != channel) {
      continue;
    }
    if (BLI_uuid_equal(image.paint_layer_id, layer_id)) {
      return &image;
    }
  }
  return nullptr;
}

/**
 * Assemble \a channel from the paint layers themselves rather than from the graph.
 *
 * For a channel the shader has no input for -- Ambient Occlusion, a layer mask -- there is no
 * chain to walk, but the layers still exist and are still stacked in a definite order. That order,
 * and how each layer blends, is a property of the layer stack rather than of any one channel, so
 * it is taken from \a reference_layers (the channel that *is* wired) and each layer's own map for
 * \a channel is looked up by its #Image.paint_layer_id.
 *
 * A layer with no map for this channel contributes nothing and is skipped, rather than failing the
 * whole stack: a user who baked AO for one layer only should still see that layer's AO.
 */
static bool composite_stack_from_layer_maps(
    const Main &bmain,
    const int channel,
    Span<PaintMaterialCompositeImageLayer> reference_layers,
    Vector<PaintMaterialCompositeImageLayer> &r_layers)
{
  r_layers.clear();
  for (const PaintMaterialCompositeImageLayer &reference : reference_layers) {
    if (reference.color_image == nullptr || BLI_uuid_is_nil(reference.color_image->paint_layer_id))
    {
      continue;
    }
    Image *map = composite_layer_map_find(bmain, reference.color_image->paint_layer_id, channel);
    if (map == nullptr) {
      continue;
    }
    PaintMaterialCompositeImageLayer layer = reference;
    layer.color_image = map;
    /* The reference layer's #ImageUser belongs to its own Image Texture node; this map has no node
     * of its own to take one from, so the default applies. */
    layer.color_iuser = nullptr;
    if (reference.mask_image == reference.color_image) {
      /* The reference masked itself -- a layer stack driving the factor from its own alpha. The
       * same relationship holds for this channel's map. */
      layer.mask_image = map;
      layer.mask_iuser = nullptr;
    }
    r_layers.append(layer);
  }
  return !r_layers.is_empty();
}

bool BKE_paint_material_composite_stack_from_material(
    const Main &bmain,
    const Material &ma,
    const int channel,
    Vector<PaintMaterialCompositeImageLayer> &r_layers)
{
  /* A display mode, not a role: there is no channel to walk and no layer map to fall back to.
   * Answered here so that a caller which reaches this by mistake degrades to the plain image
   * rather than resolving Base Color's layers under a wrong name. */
  if (channel == PAINT_LAYER_PASS_COMBINED) {
    r_layers.clear();
    return false;
  }

  if (composite_stack_from_graph(ma, channel, r_layers)) {
    return true;
  }

  /* No chain for this channel. The layers are still there, so ask the channel that does have one
   * for the stack's shape. Base Color first: it is the one a layered material always wires. */
  Vector<PaintMaterialCompositeImageLayer> reference_layers;
  bool has_reference = composite_stack_from_graph(
      ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, reference_layers);
  if (!has_reference) {
    for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
      if (composite_stack_from_graph(ma, info.channel, reference_layers)) {
        has_reference = true;
        break;
      }
    }
  }
  if (!has_reference) {
    r_layers.clear();
    return false;
  }

  return composite_stack_from_layer_maps(bmain, channel, reference_layers, r_layers);
}

Span<int> BKE_paint_material_composite_passes()
{
  /* Base Color first, then the scalars and colours a PBR material is normally authored with, in
   * Principled's own order, then the two roles that are not Principled inputs at all. Ambient
   * Occlusion is baked by the user rather than wired, and a mask belongs to the layer, not to the
   * shader.
   *
   * Height is deliberately absent: it has a descriptor and an identifier, but no part in the
   * Combined preview's shading, and listing it would offer a pass the preview visibly ignores. */
  static const int passes[] = {
      PAINT_MATERIAL_CHANNEL_BASE_COLOR,
      PAINT_MATERIAL_CHANNEL_METALLIC,
      PAINT_MATERIAL_CHANNEL_ROUGHNESS,
      PAINT_MATERIAL_CHANNEL_SPECULAR,
      PAINT_MATERIAL_CHANNEL_NORMAL,
      PAINT_MATERIAL_CHANNEL_AO,
      PAINT_MATERIAL_CHANNEL_ALPHA,
      PAINT_MATERIAL_CHANNEL_EMISSION,
      PAINT_LAYER_MAP_MASK,
  };
  return Span<int>(passes, ARRAY_SIZE(passes));
}

Span<int> BKE_paint_material_display_passes()
{
  /* Combined leads, as it does in the Compositor. Kept out of #BKE_paint_material_composite_passes
   * because every consumer of that list treats its values as roles -- indexing a per-channel
   * array, looking the value up in the channel descriptor table, resolving a layer map -- and none
   * of those is meaningful for a display mode. A second list means no existing loop has to learn
   * about it. */
  static const int passes[] = {
      PAINT_LAYER_PASS_COMBINED,
      PAINT_MATERIAL_CHANNEL_BASE_COLOR,
      PAINT_MATERIAL_CHANNEL_METALLIC,
      PAINT_MATERIAL_CHANNEL_ROUGHNESS,
      PAINT_MATERIAL_CHANNEL_SPECULAR,
      PAINT_MATERIAL_CHANNEL_NORMAL,
      PAINT_MATERIAL_CHANNEL_AO,
      PAINT_MATERIAL_CHANNEL_ALPHA,
      PAINT_MATERIAL_CHANNEL_EMISSION,
      PAINT_LAYER_MAP_MASK,
  };
  return Span<int>(passes, ARRAY_SIZE(passes));
}

void BKE_paint_material_layer_maps_get(const Main &bmain,
                                       const Material &ma,
                                       const bUUID &layer_id,
                                       MutableSpan<Image *> r_maps)
{
  r_maps.fill(nullptr);
  if (BLI_uuid_is_nil(layer_id)) {
    return;
  }

  /* A wired channel already says which image belongs to which layer, so its map is read from the
   * stack rather than from a tag an add-on may never have written. */
  Vector<PaintMaterialCompositeImageLayer> layers;
  for (const int role : BKE_paint_material_composite_passes()) {
    if (!composite_stack_from_graph(ma, role, layers)) {
      continue;
    }
    for (const PaintMaterialCompositeImageLayer &layer : layers) {
      if (layer.color_image != nullptr &&
          BLI_uuid_equal(layer.color_image->paint_layer_id, layer_id))
      {
        r_maps[role] = layer.color_image;
        break;
      }
    }
  }

  /* #Image.paint_layer_channel answers the rest: a baked Ambient Occlusion map and the layer's
   * mask are part of the layer without being part of the shader graph. */
  for (Image &image : const_cast<Main &>(bmain).images) {
    if (!r_maps.index_range().contains(image.paint_layer_channel)) {
      continue;
    }
    if (r_maps[image.paint_layer_channel] == nullptr &&
        BLI_uuid_equal(image.paint_layer_id, layer_id))
    {
      r_maps[image.paint_layer_channel] = &image;
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Evaluation
 * \{ */

/**
 * Layer buffers are required to be byte RGBA.
 *
 * A float buffer is not rejected for lack of a conversion but for lack of a *correct* one: its
 * values are scene-referred, and turning them into the display-referred bytes the composite is
 * made of is a colour management step, not a multiply. Converting the #Image in place, as the
 * only cheap alternative, would also mutate data the compositing caller does not own. A material
 * whose layers are float therefore goes to the bake, which renders through the display pipeline
 * anyway.
 */
static bool composite_ibuf_is_byte_rgba(const ImBuf *ibuf, const int width, const int height)
{
  if (ibuf == nullptr || ibuf->byte_buffer.data == nullptr) {
    return false;
  }
  if (ibuf->x != width || ibuf->y != height) {
    return false;
  }
  return ELEM(ibuf->channels, 0, 4);
}

/** Masks are read as a factor, so a float mask needs no colour transform and is accepted. */
static bool composite_mask_ibuf_is_valid(const ImBuf *ibuf, const int width, const int height)
{
  if (ibuf == nullptr) {
    return false;
  }
  if (ibuf->x != width || ibuf->y != height) {
    return false;
  }
  return ibuf->byte_buffer.data != nullptr || ibuf->float_buffer.data != nullptr;
}

static float mask_factor_at(
    const ImBuf *mask_ibuf, const bool from_alpha, const int x, const int y, const float influence)
{
  if (mask_ibuf == nullptr || influence <= 0.0f) {
    return 1.0f;
  }
  const int channels = mask_ibuf->channels == 0 ? 4 : mask_ibuf->channels;
  const int64_t offset = (int64_t(y) * mask_ibuf->x + x) * channels;
  float mask_value;

  if (mask_ibuf->byte_buffer.data != nullptr) {
    const uchar *pixel = mask_ibuf->byte_data() + offset;
    if (from_alpha) {
      mask_value = channels == 4 ? float(pixel[3]) / 255.0f : 1.0f;
    }
    else {
      mask_value = (float(pixel[0]) + float(pixel[1]) + float(pixel[2])) / (3.0f * 255.0f);
    }
  }
  else {
    const float *pixel = mask_ibuf->float_buffer.data + offset;
    if (from_alpha) {
      mask_value = channels == 4 ? pixel[3] : 1.0f;
    }
    else {
      mask_value = (pixel[0] + pixel[1] + pixel[2]) / 3.0f;
    }
  }

  mask_value = clamp_f(mask_value, 0.0f, 1.0f);
  return (1.0f - influence) + influence * mask_value;
}

/**
 * Lay the tangent-space normal \a top over \a bottom, both encoded in [0, 1].
 *
 * The whiteout blend: decode both, add the detail map's slope to the base map's, keep the product
 * of their z, renormalize. \a fac interpolates in normal space rather than on the encoded bytes,
 * so a partial factor tilts the result towards the base normal instead of towards grey.
 */
static void blend_normal_combine(const float bottom[4], const float top[4], float r_rgb[3])
{
  const float3 base = float3(bottom[0], bottom[1], bottom[2]) * 2.0f - 1.0f;
  const float3 detail = float3(top[0], top[1], top[2]) * 2.0f - 1.0f;
  const float3 combined = math::normalize(
      float3(base.x + detail.x, base.y + detail.y, base.z * detail.z));
  copy_v3_v3(r_rgb, combined * 0.5f + 0.5f);
}

/** One component of #CompositeBlend::Overlay, straight from `node_mix_overlay`. */
static float blend_overlay_channel(const float bottom, const float top, const float fac)
{
  const float facm = 1.0f - fac;
  if (bottom < 0.5f) {
    return bottom * (facm + 2.0f * fac * top);
  }
  return 1.0f - (facm + 2.0f * fac * (1.0f - top)) * (1.0f - bottom);
}

/**
 * Blend one pixel of \a src_top into \a dst, exactly as the Mix node would.
 *
 * These are the formulas of `gpu_shader_material_mix_color.glsl`, not an alpha-over composite.
 * The distinction is the whole correctness of this module: the node interpolates by the factor
 * alone and never treats the top layer's alpha as coverage, so a stack that wants its layers to
 * cover each other routes that alpha into the factor -- and a compositor that also applied it
 * implicitly would apply it twice.
 *
 * Only Mix carries the top's alpha into the result; the others keep the bottom's, again matching
 * the node.
 *
 * \note Byte, and therefore in the buffers' own encoding rather than in the scene-linear space
 * the shader mixes in. The two agree wherever \a fac is 0 or 1 -- which is the whole of a hard
 * layer edge -- and drift by at most a rounding step at partial coverage.
 */
static void blend_layer_byte(uchar dst[4],
                             const uchar src_top[4],
                             const CompositeBlend blend,
                             const float opacity,
                             const float mask_factor)
{
  const float fac = clamp_f(opacity * mask_factor, 0.0f, 1.0f);
  if (fac == 0.0f) {
    return;
  }

  float bottom[4];
  float top[4];
  for (const int i : IndexRange(4)) {
    bottom[i] = float(dst[i]) / 255.0f;
    top[i] = float(src_top[i]) / 255.0f;
  }

  float result[4];
  copy_v4_v4(result, bottom);
  switch (blend) {
    case CompositeBlend::Mix:
      interp_v4_v4v4(result, bottom, top, fac);
      break;
    case CompositeBlend::Multiply:
      for (const int i : IndexRange(3)) {
        result[i] = bottom[i] * (1.0f - fac) + bottom[i] * top[i] * fac;
      }
      break;
    case CompositeBlend::Overlay:
      for (const int i : IndexRange(3)) {
        result[i] = blend_overlay_channel(bottom[i], top[i], fac);
      }
      break;
    case CompositeBlend::Add:
      for (const int i : IndexRange(3)) {
        result[i] = bottom[i] * (1.0f - fac) + (bottom[i] + top[i]) * fac;
      }
      break;
    case CompositeBlend::NormalCombine: {
      float combined[3];
      blend_normal_combine(bottom, top, combined);
      for (const int i : IndexRange(3)) {
        result[i] = bottom[i] * (1.0f - fac) + combined[i] * fac;
      }
      break;
    }
  }

  for (const int i : IndexRange(4)) {
    dst[i] = uchar(clamp_i(int(result[i] * 255.0f + 0.5f), 0, 255));
  }
}

static bool composite_stack_validate(const PaintMaterialCompositeStack &stack)
{
  if (stack.width <= 0 || stack.height <= 0 || stack.layers.is_empty()) {
    return false;
  }
  bool any_enabled = false;
  for (const PaintMaterialCompositeLayer &layer : stack.layers) {
    if (!layer.enabled) {
      continue;
    }
    if (!composite_ibuf_is_byte_rgba(layer.color_ibuf, stack.width, stack.height)) {
      return false;
    }
    if (layer.mask_ibuf != nullptr &&
        !composite_mask_ibuf_is_valid(layer.mask_ibuf, stack.width, stack.height))
    {
      return false;
    }
    any_enabled = true;
  }
  return any_enabled;
}

bool BKE_paint_material_composite_eval(const PaintMaterialCompositeStack &stack,
                                       ImBuf *composite_ibuf,
                                       const rcti *region,
                                       PaintMaterialCompositeEvalStats *r_stats)
{
  if (!composite_ibuf_is_byte_rgba(composite_ibuf, stack.width, stack.height)) {
    return false;
  }
  if (!composite_stack_validate(stack)) {
    return false;
  }

  rcti area;
  BLI_rcti_init(&area, 0, stack.width, 0, stack.height);
  if (region != nullptr) {
    rcti clipped = *region;
    if (!BLI_rcti_isect(&area, &clipped, &area)) {
      /* Nothing of the tagged region is inside the buffer; the composite is already correct. */
      if (r_stats != nullptr) {
        *r_stats = {};
      }
      return true;
    }
  }

  const double start_time = BLI_time_now_seconds();

  const int64_t row_stride = int64_t(stack.width) * 4;
  const int64_t area_width = BLI_rcti_size_x(&area);
  const IndexRange rows(area.ymin, BLI_rcti_size_y(&area));
  uchar *composite_pixels = composite_ibuf->byte_data_for_write();
  int layers_evaluated = 0;
  bool composite_initialized = false;

  for (const PaintMaterialCompositeLayer &layer : stack.layers) {
    if (!layer.enabled) {
      continue;
    }
    const uchar *layer_pixels = layer.color_ibuf->byte_data();

    if (!composite_initialized) {
      /* A bare bottom is copied rather than blended: it has no Mix node, so it has no blend mode
       * or factor, and blending it over undefined pixels would let them show through wherever it
       * is transparent. A uniform chain's lowest layer has all of those, and blends over the
       * transparency the graph gives it -- so the buffer starts cleared and it is blended like
       * any other layer. */
      threading::parallel_for(rows, 64, [&](const IndexRange range) {
        for (const int64_t y : range) {
          const int64_t offset = y * row_stride + int64_t(area.xmin) * 4;
          if (layer.is_bare_base) {
            memcpy(composite_pixels + offset, layer_pixels + offset, size_t(area_width * 4));
          }
          else {
            memset(composite_pixels + offset, 0, size_t(area_width * 4));
          }
        }
      });
      composite_initialized = true;
      if (layer.is_bare_base) {
        layers_evaluated++;
        continue;
      }
    }

    threading::parallel_for(rows, 64, [&](const IndexRange range) {
      for (const int64_t y : range) {
        const int64_t row_offset = y * row_stride;
        for (const int64_t x : IndexRange(area.xmin, area_width)) {
          const int64_t offset = row_offset + x * 4;
          const float mask_factor = mask_factor_at(
              layer.mask_ibuf, layer.mask_from_alpha, int(x), int(y), layer.mask_influence);
          blend_layer_byte(composite_pixels + offset,
                           layer_pixels + offset,
                           layer.blend,
                           layer.opacity,
                           mask_factor);
        }
      }
    });
    layers_evaluated++;
  }

  if (!composite_initialized) {
    return false;
  }

  if (r_stats != nullptr) {
    r_stats->elapsed_seconds = BLI_time_now_seconds() - start_time;
    r_stats->layers_evaluated = layers_evaluated;
    r_stats->pixels_processed = area_width * BLI_rcti_size_y(&area);
  }
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Image Layer Acquisition
 * \{ */

struct CompositeImageLock {
  Image *image = nullptr;
  ImBuf *ibuf = nullptr;
  void *lock = nullptr;
};

static ImBuf *composite_image_acquire(Image *image,
                                      const ImageUser *iuser,
                                      Vector<CompositeImageLock> &r_locks)
{
  if (image == nullptr) {
    return nullptr;
  }
  /* Acquiring writes to the #ImageUser, and the material's copy is not this code's to mutate. */
  ImageUser iuser_local;
  if (iuser != nullptr) {
    iuser_local = *iuser;
  }
  else {
    BKE_imageuser_default(&iuser_local);
  }

  CompositeImageLock entry;
  entry.image = image;
  entry.ibuf = BKE_image_acquire_ibuf(image, &iuser_local, &entry.lock);
  if (entry.ibuf == nullptr) {
    return nullptr;
  }
  r_locks.append(entry);
  return entry.ibuf;
}

static void composite_images_release(Span<CompositeImageLock> locks)
{
  for (const CompositeImageLock &entry : locks) {
    BKE_image_release_ibuf(entry.image, entry.ibuf, entry.lock);
  }
}

static bool composite_stack_build(Span<PaintMaterialCompositeImageLayer> image_layers,
                                  Vector<CompositeImageLock> &r_locks,
                                  PaintMaterialCompositeStack &r_stack)
{
  if (!BKE_paint_material_composite_stack_dimensions(image_layers, r_stack.width, r_stack.height))
  {
    return false;
  }
  for (const PaintMaterialCompositeImageLayer &image_layer : image_layers) {
    if (!image_layer.enabled || image_layer.color_image == nullptr) {
      continue;
    }
    PaintMaterialCompositeLayer layer;
    layer.color_ibuf = composite_image_acquire(
        image_layer.color_image, image_layer.color_iuser, r_locks);
    if (layer.color_ibuf == nullptr) {
      return false;
    }
    if (image_layer.mask_image != nullptr) {
      layer.mask_ibuf = composite_image_acquire(
          image_layer.mask_image, image_layer.mask_iuser, r_locks);
      if (layer.mask_ibuf == nullptr) {
        return false;
      }
    }
    layer.blend = image_layer.blend;
    layer.opacity = image_layer.opacity;
    layer.mask_influence = image_layer.mask_influence;
    layer.mask_from_alpha = image_layer.mask_from_alpha;
    layer.is_bare_base = image_layer.is_bare_base;
    r_stack.layers.append(layer);
  }
  return !r_stack.layers.is_empty();
}

bool BKE_paint_material_composite_eval_images(Span<PaintMaterialCompositeImageLayer> image_layers,
                                              ImBuf *composite_ibuf,
                                              const rcti *region,
                                              PaintMaterialCompositeEvalStats *r_stats)
{
  Vector<CompositeImageLock> locks;
  PaintMaterialCompositeStack stack;
  bool ok = composite_stack_build(image_layers, locks, stack);
  if (ok) {
    ok = BKE_paint_material_composite_eval(stack, composite_ibuf, region, r_stats);
  }
  composite_images_release(locks);
  return ok;
}

/**
 * Dimensions and byte colorspace of the bottom-most enabled layer.
 *
 * The colorspace is reported alongside the size because the composite has to inherit it rather
 * than take the default: the layers of a Roughness or a Normal channel are Non-Color, and a
 * composite that claimed sRGB instead would be display-transformed on its way to the screen while
 * the very same layer, opened on its own, would not. The two would then disagree about the pixels
 * a stroke is being judged against.
 *
 * \param r_byte_colorspace: name owned by the colour management configuration, so it outlives the
 *                           acquisition it is read from. Null when the layer has no byte buffer,
 *                           which the evaluator rejects anyway.
 */
static bool composite_stack_bottom_layer_info(Span<PaintMaterialCompositeImageLayer> image_layers,
                                              int &r_width,
                                              int &r_height,
                                              const char **r_byte_colorspace)
{
  r_width = 0;
  r_height = 0;
  if (r_byte_colorspace != nullptr) {
    *r_byte_colorspace = nullptr;
  }
  for (const PaintMaterialCompositeImageLayer &layer : image_layers) {
    if (!layer.enabled || layer.color_image == nullptr) {
      continue;
    }
    Vector<CompositeImageLock> locks;
    const ImBuf *ibuf = composite_image_acquire(layer.color_image, layer.color_iuser, locks);
    if (ibuf != nullptr) {
      r_width = ibuf->x;
      r_height = ibuf->y;
      if (r_byte_colorspace != nullptr) {
        *r_byte_colorspace = IMB_colormanagement_get_byte_colorspace(ibuf);
      }
    }
    composite_images_release(locks);
    return r_width > 0 && r_height > 0;
  }
  return false;
}

bool BKE_paint_material_composite_stack_dimensions(
    Span<PaintMaterialCompositeImageLayer> image_layers, int &r_width, int &r_height)
{
  return composite_stack_bottom_layer_info(image_layers, r_width, r_height, nullptr);
}

uint64_t BKE_paint_material_composite_stack_hash(
    Span<PaintMaterialCompositeImageLayer> image_layers)
{
  uint64_t hash = get_default_hash(image_layers.size());
  for (const PaintMaterialCompositeImageLayer &layer : image_layers) {
    /* Session UIDs rather than pointers: a freed image's address can come back as a different
     * one, and the hash is the only thing standing between that and a stale composite. */
    hash = get_default_hash(hash,
                            layer.color_image != nullptr ? layer.color_image->id.session_uid : 0,
                            layer.mask_image != nullptr ? layer.mask_image->id.session_uid : 0);
    hash = get_default_hash(hash,
                            int(layer.blend),
                            layer.enabled,
                            layer.mask_from_alpha,
                            layer.opacity,
                            layer.mask_influence);
    /* Split rather than appended: #get_default_hash mixes a fixed number of values at once. */
    hash = get_default_hash(hash, layer.is_bare_base);
  }
  return hash;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Composite Cache
 *
 * Keyed by the material's #ID.session_uid, like the bake cache and for the same reason: a freed
 * material hands its address to the next one, and a pointer-keyed cache would then serve the old
 * composite for the new material.
 *
 * Main thread only. Everything that reaches this -- the image editor's buffer acquisition, a
 * stroke's region tag, an image edit -- runs there, and unlike the bake there is no worker
 * writing results back.
 * \{ */

struct CompositeCacheKey {
  uint32_t material_session_uid = 0;
  int channel = 0;

  uint64_t hash() const
  {
    return get_default_hash(this->material_session_uid, this->channel);
  }

  friend bool operator==(const CompositeCacheKey &a, const CompositeCacheKey &b)
  {
    return a.material_session_uid == b.material_session_uid && a.channel == b.channel;
  }
};

/**
 * Owning handles for the two resources a cache entry holds.
 *
 * By value rather than as raw pointers freed by hand, because the entry is destroyed from four
 * places -- eviction, a failed evaluation, a per-material drop and the teardown -- and every one
 * of them used to have to remember both. A path added later that forgets is a leak that nothing
 * reports.
 */
struct ImBufDeleter {
  void operator()(ImBuf *ibuf) const
  {
    IMB_freeImBuf(ibuf);
  }
};
using ImBufPtr = std::unique_ptr<ImBuf, ImBufDeleter>;

struct PartialUpdateUserDeleter {
  void operator()(PartialUpdateUser *user) const
  {
    BKE_image_partial_update_free(user);
  }
};
using PartialUpdateUserPtr = std::unique_ptr<PartialUpdateUser, PartialUpdateUserDeleter>;

struct CompositeCacheEntry {
  ImBufPtr ibuf;
  int width = 0;
  int height = 0;
  uint64_t stack_hash = 0;
  /** The whole buffer has to be recomputed. Set on creation, and whenever a region is unknown. */
  bool dirty_full = true;
  /** Bounding rectangle of the pixels tagged since the last evaluation. */
  rcti dirty_region = {0, 0, 0, 0};
  /** Images the composite read, so an edit to one can be reported without knowing the stack. */
  Vector<uint32_t> image_session_uids;
  /**
   * One partial-update subscription per source image, keyed by #ID.session_uid.
   *
   * The image records what changed in it, per tile, whoever caused the change; polling that is
   * what makes this cache independent of anyone remembering to tag it. More to the point, it is
   * what stops a blanket tag from discarding a precise one: painting tags the image ID on every
   * dab, and the depsgraph flush that follows used to reach #image_changed and mark the whole
   * composite dirty -- re-flattening the entire stack for a dab that had already been reported
   * exactly.
   *
   * Never holds an #Image pointer. The images arrive with every `cache_ensure` call, so a poll
   * always has a fresh one, and a cache outliving an ID it pointed at would be a crash rather than
   * a stale pixel.
   */
  Map<uint32_t, PartialUpdateUserPtr> partial_update_users;
  /**
   * Taken from a counter shared by the whole cache, so a consumer holding a copy of the pixels can
   * tell it is old.
   *
   * Shared rather than per entry because a consumer compares one number over time, not per
   * material and channel: the Image Editor switching from one composited pass to another is
   * looking at a different buffer, and two entries counting from one of their own would hand it
   * the same revision for both.
   */
  uint64_t revision = 0;
  /** Monotonic counter used to evict the least recently used entry. */
  int64_t last_use = 0;
};

/** A composite is one buffer per material and channel, so this is a handful of entries at most;
 * the budget only exists to bound a pathological case, not to be managed. */
static constexpr int64_t COMPOSITE_CACHE_BUDGET_BYTES = 256 * 1024 * 1024;

/**
 * The one composite cache of the session.
 *
 * A single object rather than three loose globals, so that the counters cannot drift from the
 * entries they belong to and so that everything the cache owns is reached from one place.
 *
 * Deliberately not stored on #Main or on #Material, which is where derived data normally lives:
 * the dependency this cache exists to answer runs the wrong way. An edit reports "these pixels of
 * this image changed", and the cache is the only thing that knows which materials read that image;
 * per-material storage could not answer it without a walk over every material, from call sites --
 * a paint stroke, an image edit -- that have no #Main to walk. What per-material lifetime would
 * have bought is instead paid for explicitly, by
 * #BKE_paint_material_composite_cache_free_material.
 *
 * Main thread only. Everything that reaches it -- the image editor's buffer acquisition, a
 * stroke's region tag, an image edit -- runs there, and unlike the bake there is no worker writing
 * results back.
 */
struct CompositeCache {
  Map<CompositeCacheKey, CompositeCacheEntry> entries;
  /** Monotonic, and only ever compared: the source of #CompositeCacheEntry.last_use. */
  int64_t use_counter = 0;
  /** Never reset: a consumer compares revisions over time, across entries that come and go. */
  uint64_t revision_counter = 0;
};

static CompositeCache g_cache;

static int64_t composite_entry_size_in_bytes(const CompositeCacheEntry &entry)
{
  return int64_t(entry.width) * entry.height * 4;
}

/**
 * Drop the buffer and the subscriptions of \a entry while keeping the entry itself.
 *
 * Only for a resize, which needs a new buffer of a new size and -- because the subscriptions go
 * with it -- a fresh set of them, whose first poll asks for the full rebuild a resize needs
 * anyway. Removing an entry from the cache needs no call: the handles free themselves.
 */
static void composite_entry_reset(CompositeCacheEntry &entry)
{
  entry.ibuf.reset();
  entry.partial_update_users.clear();
}

/** Evict least recently used entries until the cache fits the budget, never the one just made. */
static void composite_cache_enforce_budget(const CompositeCacheKey &keep)
{
  int64_t total = 0;
  for (const CompositeCacheEntry &entry : g_cache.entries.values()) {
    total += composite_entry_size_in_bytes(entry);
  }
  while (total > COMPOSITE_CACHE_BUDGET_BYTES) {
    const CompositeCacheKey *oldest_key = nullptr;
    int64_t oldest_use = INT64_MAX;
    for (const auto item : g_cache.entries.items()) {
      if (item.key == keep) {
        continue;
      }
      if (item.value.last_use < oldest_use) {
        oldest_use = item.value.last_use;
        oldest_key = &item.key;
      }
    }
    if (oldest_key == nullptr) {
      break;
    }
    const CompositeCacheKey key = *oldest_key;
    total -= composite_entry_size_in_bytes(g_cache.entries.lookup(key));
    g_cache.entries.remove(key);
  }
}

static void composite_entry_image_dependencies_set(
    CompositeCacheEntry &entry, Span<PaintMaterialCompositeImageLayer> image_layers)
{
  entry.image_session_uids.clear();
  for (const PaintMaterialCompositeImageLayer &layer : image_layers) {
    if (layer.color_image != nullptr) {
      entry.image_session_uids.append_non_duplicates(layer.color_image->id.session_uid);
    }
    if (layer.mask_image != nullptr) {
      entry.image_session_uids.append_non_duplicates(layer.mask_image->id.session_uid);
    }
  }

  /* A layer removed from the stack stops being watched, or the entry keeps an allocation and a
   * poll per frame for an image it no longer reads. */
  Vector<uint32_t> stale;
  for (const uint32_t uid : entry.partial_update_users.keys()) {
    if (!entry.image_session_uids.contains(uid)) {
      stale.append(uid);
    }
  }
  for (const uint32_t uid : stale) {
    entry.partial_update_users.remove(uid);
  }
}

ImBuf *BKE_paint_material_composite_cache_ensure(
    const Material &ma,
    const eMaterialPaintChannel channel,
    Span<PaintMaterialCompositeImageLayer> image_layers,
    const uint64_t stack_hash,
    uint64_t *r_revision,
    PaintMaterialCompositeEvalStats *r_stats,
    rcti *r_changed_region)
{
  if (r_changed_region != nullptr) {
    /* Initialized before any early return, so that "nothing was recomputed" is never confused with
     * "the caller forgot to look". */
    BLI_rcti_init(r_changed_region, 0, 0, 0, 0);
  }

  int width = 0;
  int height = 0;
  const char *byte_colorspace = nullptr;
  if (!composite_stack_bottom_layer_info(image_layers, width, height, &byte_colorspace)) {
    return nullptr;
  }

  CompositeCacheKey key;
  key.material_session_uid = ma.id.session_uid;
  key.channel = int(channel);

  CompositeCacheEntry &entry = g_cache.entries.lookup_or_add_default(key);
  entry.last_use = ++g_cache.use_counter;

  const bool size_changed = entry.ibuf == nullptr || entry.width != width ||
                            entry.height != height;
  if (size_changed) {
    composite_entry_reset(entry);
    entry.ibuf.reset(IMB_allocImBuf(uint(width), uint(height), ImBufFlags::ByteData));
    if (entry.ibuf == nullptr) {
      g_cache.entries.remove(key);
      return nullptr;
    }
    entry.ibuf->channels = 4;
    entry.width = width;
    entry.height = height;
  }

  /* What the source images say changed since the last call.
   *
   * Polled rather than reported: a caller that edits pixels no longer has to remember to tell this
   * cache, and -- the reason this exists -- a blanket ID tag from an unrelated subsystem can no
   * longer overwrite a precise report with "everything". Placed after the reallocation above so a
   * resize, which drops the subscriptions with the buffer, is followed by fresh ones whose first
   * poll asks for the full rebuild a resize needs anyway. */
  for (const PaintMaterialCompositeImageLayer &layer : image_layers) {
    for (Image *image : {layer.color_image, layer.mask_image}) {
      if (image == nullptr) {
        continue;
      }
      PartialUpdateUser *user =
          entry.partial_update_users
              .lookup_or_add_cb(
                  image->id.session_uid,
                  [&]() { return PartialUpdateUserPtr(BKE_image_partial_update_create(image)); })
              .get();

      switch (BKE_image_partial_update_collect_changes(image, user)) {
        case ePartialUpdateCollectResult::FullUpdateNeeded:
          /* A brand new subscription lands here too, which is right: nothing of this image has
           * been flattened yet. */
          entry.dirty_full = true;
          break;
        case ePartialUpdateCollectResult::NoChangesDetected:
          break;
        case ePartialUpdateCollectResult::PartialChangesDetected: {
          PartialUpdateRegion change;
          while (BKE_image_partial_update_get_next_change(user, &change) ==
                 ePartialUpdateIterResult::ChangeAvailable)
          {
            /* A layer stack cannot be tiled, so a change reported for any tile but the first would
             * land at the wrong place in a single-tile buffer. Give up precision rather than put
             * pixels somewhere they do not belong. */
            if (change.tile_number != 1001) {
              entry.dirty_full = true;
              break;
            }
            if (BLI_rcti_is_empty(&entry.dirty_region)) {
              entry.dirty_region = change.region;
            }
            else {
              BLI_rcti_union(&entry.dirty_region, &change.region);
            }
          }
          break;
        }
      }
    }
  }

  const bool rebuild_all = size_changed || entry.stack_hash != stack_hash || entry.dirty_full;
  const bool rebuild_region = !rebuild_all && !BLI_rcti_is_empty(&entry.dirty_region);
  if (rebuild_all) {
    /* Only with the stack: the bottom layer decides the colorspace, and a region refresh cannot
     * have changed which layer that is. Non-Color layers -- Roughness, Metallic, a normal map --
     * must not be handed on as sRGB, or the composite is display-transformed on its way to the
     * screen while the layer it is made of is not. */
    BLI_assert(byte_colorspace != nullptr);
    IMB_colormanagement_assign_byte_colorspace(entry.ibuf.get(), byte_colorspace);
  }

  if (rebuild_all || rebuild_region) {
    const rcti *region = rebuild_region ? &entry.dirty_region : nullptr;
    if (!BKE_paint_material_composite_eval_images(image_layers, entry.ibuf.get(), region, r_stats))
    {
      g_cache.entries.remove(key);
      return nullptr;
    }
    if (r_changed_region != nullptr) {
      /* The caller derives its own pixels from these and needs to refresh no more than what really
       * moved; a full rebuild is reported as the whole buffer rather than as "everything", so one
       * rectangle type covers both cases. Read before #dirty_region is reset below. */
      if (rebuild_all) {
        BLI_rcti_init(r_changed_region, 0, entry.ibuf->x, 0, entry.ibuf->y);
      }
      else {
        *r_changed_region = entry.dirty_region;
      }
    }
    entry.stack_hash = stack_hash;
    entry.dirty_full = false;
    entry.revision = ++g_cache.revision_counter;
    BLI_rcti_init(&entry.dirty_region, 0, 0, 0, 0);
    composite_entry_image_dependencies_set(entry, image_layers);
  }

  if (r_revision != nullptr) {
    *r_revision = entry.revision;
  }
  composite_cache_enforce_budget(key);
  return entry.ibuf.get();
}

void BKE_paint_material_composite_cache_invalidate(const Material *ma)
{
  if (ma == nullptr) {
    for (CompositeCacheEntry &entry : g_cache.entries.values()) {
      entry.dirty_full = true;
    }
    return;
  }
  const uint32_t session_uid = ma->id.session_uid;
  for (auto item : g_cache.entries.items()) {
    if (item.key.material_session_uid == session_uid) {
      item.value.dirty_full = true;
    }
  }
}

void BKE_paint_material_composite_cache_free_material(const Material &ma)
{
  if (g_cache.entries.is_empty()) {
    /* This runs from #ID free, so it is on the path of every material in every file ever loaded,
     * almost none of which was ever composited. */
    return;
  }
  const uint32_t session_uid = ma.id.session_uid;
  Vector<CompositeCacheKey> dead_keys;
  for (auto item : g_cache.entries.items()) {
    if (item.key.material_session_uid == session_uid) {
      dead_keys.append(item.key);
    }
  }
  /* Collected first: removing from the map while iterating it would invalidate the iteration. */
  for (const CompositeCacheKey &key : dead_keys) {
    g_cache.entries.remove(key);
  }
}

void BKE_paint_material_composite_cache_free_all()
{
  /* Clearing is the whole teardown: every entry owns its buffer and its subscriptions outright. */
  g_cache.entries.clear();
}

bool BKE_paint_material_composite_cache_contains(const Material &ma,
                                                 const eMaterialPaintChannel channel)
{
  CompositeCacheKey key;
  key.material_session_uid = ma.id.session_uid;
  key.channel = int(channel);
  return g_cache.entries.contains(key);
}

/** \} */

}  // namespace blender
