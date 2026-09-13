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
#include "paint_material_layer_idprops.hh"

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

/** Name of the shared Normal Combine group data-block. NOTE: do not translate. */
static const char *NORMAL_COMBINE_TREE_NAME = "PBR Normal Combine";

bool BKE_paint_material_is_layer_group(const bNode &node)
{
  if (!node.is_group() || node.id == nullptr || GS(node.id->name) != ID_NT) {
    return false;
  }
  /* The same marker property the Normal Combine group uses, with a different value: one place to
   * look to know whether a group node is one of ours, and which. */
  const char *marker = bke::paint_layer::node_tree_group_marker_get(*node.id);
  return marker != nullptr && STREQ(marker, bke::paint_layer::LAYER_GROUP_MARKER_VALUE);
}

Material *BKE_paint_material_layer_group_material_get(const ID &group_tree_id)
{
  return bke::paint_layer::group_material_get(group_tree_id);
}

void BKE_paint_material_layer_group_material_set(ID &group_tree_id, Material *material)
{
  bke::paint_layer::group_material_set(group_tree_id, material);
}

bool BKE_paint_material_is_normal_combine_group(const bNode &node)
{
  if (!node.is_group() || node.id == nullptr || GS(node.id->name) != ID_NT) {
    return false;
  }
  const char *marker = bke::paint_layer::node_tree_group_marker_get(*node.id);
  return marker != nullptr && STREQ(marker, bke::paint_layer::NORMAL_COMBINE_MARKER_VALUE);
}

/** The group already in \a bmain, or null. Found by its marker, so a rename does not lose it. */
static bNodeTree *normal_combine_group_find(Main &bmain)
{
  for (bNodeTree &ntree : bmain.nodetrees) {
    const char *marker = bke::paint_layer::node_tree_group_marker_get(ntree.id);
    if (marker != nullptr && STREQ(marker, bke::paint_layer::NORMAL_COMBINE_MARKER_VALUE)) {
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
  bke::paint_layer::normal_combine_tree_marker_set(*group);

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

  /* The Mix node's `data_type` and the Vector Math nodes' `operation` were set on the storage
   * after the nodes were created, so their socket declarations are still the defaults. Tag the
   * whole tree and run the update now: without this the group instantiates into a shader with
   * the wrong sockets and the node inliner asserts when EEVEE compiles a material using it. */
  BKE_ntree_update_tag_all(group);
  BKE_ntree_update_after_single_tree_change(bmain, *group);
  return group;
}

/** \} */

/**
 * Whether \a r_mix.factor is linked through a Math node in Multiply mode that combines a
 * per-pixel coverage source with a plain constant, and if so, fills in #factor_opacity and
 * #factor_coverage.
 *
 * Coverage and opacity coexist this way: the layer's own image still drives per-pixel coverage
 * through the Multiply's linked input, and the other, unlinked input is a constant the user can
 * still edit -- unlike a bare link straight into the Mix's Factor, which leaves nothing to edit.
 * Neither input linked is the shape of a switched-off channel (coverage first, opacity second).
 */
static void composite_mix_factor_opacity_detect(CompositeMixNode &r_mix)
{
  if (r_mix.factor == nullptr) {
    return;
  }
  const bNode *source = composite_source_node_shallow(*r_mix.factor);
  if (source == nullptr || source->type_legacy != SH_NODE_MATH ||
      NodeMathOperation(source->custom1) != NODE_MATH_MULTIPLY)
  {
    return;
  }
  const bNodeSocket *value_a = static_cast<const bNodeSocket *>(
      BLI_findlink(&source->inputs, 0));
  const bNodeSocket *value_b = static_cast<const bNodeSocket *>(
      BLI_findlink(&source->inputs, 1));
  if (value_a == nullptr || value_b == nullptr) {
    return;
  }
  const bool a_linked = !value_a->directly_linked_links().is_empty();
  const bool b_linked = !value_b->directly_linked_links().is_empty();
  if (a_linked && b_linked) {
    /* Two sources and no constant: not this shape, the legacy reading takes over. */
    return;
  }
  if (!a_linked && !b_linked) {
    /* A channel the layer has switched off or never had: coverage is unlinked and zero, and the
     * opacity keeps its own input. #layer_factor_coverage_link always puts coverage first, so the
     * order is what tells the two apart when neither is linked. */
    r_mix.factor_coverage = value_a;
    r_mix.factor_opacity = value_b;
    return;
  }
  r_mix.factor_coverage = a_linked ? value_a : value_b;
  r_mix.factor_opacity = a_linked ? value_b : value_a;
}

bool composite_mix_node_read(const bNode &node, CompositeMixNode &r_mix)
{
  if (BKE_paint_material_is_normal_combine_group(node)) {
    /* A group instance inherits the interface's `Socket_N` identifiers, not its display names;
     * #node_find_socket matches identifiers. See #NORMAL_COMBINE_ID_*. */
    r_mix.factor = bke::node_find_socket(
        node, SOCK_IN, UString::from_ptr_noinline(NORMAL_COMBINE_ID_FACTOR));
    r_mix.bottom = bke::node_find_socket(
        node, SOCK_IN, UString::from_ptr_noinline(NORMAL_COMBINE_ID_A));
    r_mix.top = bke::node_find_socket(
        node, SOCK_IN, UString::from_ptr_noinline(NORMAL_COMBINE_ID_B));
    r_mix.blend = CompositeBlend::NormalCombine;
    composite_mix_factor_opacity_detect(r_mix);
    return r_mix.factor != nullptr && r_mix.bottom != nullptr && r_mix.top != nullptr;
  }
  if (node.type_legacy == SH_NODE_MIX_RGB_LEGACY) {
    r_mix.blend_supported = composite_blend_from_ramp_blend(node.custom1, r_mix.blend);
    r_mix.factor = bke::node_find_socket(node, SOCK_IN, "Fac"_ustr);
    r_mix.bottom = bke::node_find_socket(node, SOCK_IN, "Color1"_ustr);
    r_mix.top = bke::node_find_socket(node, SOCK_IN, "Color2"_ustr);
    composite_mix_factor_opacity_detect(r_mix);
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
    composite_mix_factor_opacity_detect(r_mix);
    return r_mix.factor != nullptr && r_mix.bottom != nullptr && r_mix.top != nullptr;
  }
  return false;
}

const bNode *composite_mix_map_node(const CompositeMixNode &mix)
{
  const Span<const bNodeLink *> links = mix.top->directly_linked_links();
  if (links.size() != 1 || links[0]->fromnode->type_legacy != SH_NODE_TEX_IMAGE) {
    return nullptr;
  }
  return links[0]->fromnode;
}

bool composite_mix_coverage_off(const CompositeMixNode &mix)
{
  return mix.factor_opacity != nullptr && mix.factor_coverage != nullptr &&
         mix.factor_coverage->directly_linked_links().is_empty();
}

bool composite_mix_channel_state_get(const CompositeMixNode &mix,
                                     PaintMaterialLayerChannelState &r_state)
{
  if (mix.factor_opacity == nullptr || mix.factor_coverage == nullptr) {
    return false;
  }
  if (mix.top->directly_linked_links().is_empty()) {
    r_state = PaintMaterialLayerChannelState::Absent;
    return true;
  }
  /* A correction hanging on the map input owns the shape between the layer and its map. The
   * coverage input it relinks is the over chain's, so the unlinked-coverage test below can no
   * longer see a switched-off channel: the state is the base map's, read under the corrections
   * (spec 18 I1'). */
  const Span<const bNodeLink *> top_links = mix.top->directly_linked_links();
  if (top_links.size() == 1 && top_links[0]->is_available() && !top_links[0]->is_muted() &&
      bke::paint_layer::node_is_correction(*top_links[0]->fromnode))
  {
    return composite_layer_base_state_get(mix.top->owner_node(), r_state);
  }
  /* Mask corrections own the coverage input the same way: it stays linked to their chain in every
   * state, so the muted map is again what tells a switched-off channel apart. */
  const Span<const bNodeLink *> coverage_links = mix.factor_coverage->directly_linked_links();
  if (coverage_links.size() == 1 && coverage_links[0]->is_available() &&
      !coverage_links[0]->is_muted() &&
      bke::paint_layer::node_is_correction(*coverage_links[0]->fromnode))
  {
    return composite_layer_base_state_get(mix.top->owner_node(), r_state);
  }
  if (composite_mix_map_node(mix) == nullptr) {
    return false;
  }
  r_state = composite_mix_coverage_off(mix) ? PaintMaterialLayerChannelState::Disabled :
                                              PaintMaterialLayerChannelState::Enabled;
  return true;
}

bool composite_layer_base_state_get(const bNode &layer_mix,
                                    PaintMaterialLayerChannelState &r_state)
{
  CompositeMixNode mix;
  if (!composite_mix_node_read(layer_mix, mix) || mix.top == nullptr) {
    return false;
  }
  /* Descend the content stack by its links, corrections included. A muted node must not stop the
   * walk: a muted map is exactly the Disabled state this has to report, and a muted correction
   * still hangs where it hangs. Reroutes and anything else this shape does not build are not one
   * of the three states. */
  const bNodeSocket *socket = mix.top;
  for (int step = 0; step < 64; step++) {
    if (socket->directly_linked_links().is_empty()) {
      /* No map below the corrections: whatever they hold, the layer is Absent here. */
      r_state = PaintMaterialLayerChannelState::Absent;
      return true;
    }
    const bNodeLink *link = socket->directly_linked_links()[0];
    if (!link->is_available() || link->is_muted()) {
      return false;
    }
    const bNode &from = *link->fromnode;
    if (from.type_legacy == SH_NODE_TEX_IMAGE) {
      /* The layer's own map. Under corrections the coverage input is fed by the over chain, so
       * the muted map is what tells a switched-off channel from a live one. */
      r_state = from.is_muted() ? PaintMaterialLayerChannelState::Disabled :
                                  PaintMaterialLayerChannelState::Enabled;
      return true;
    }
    if (!bke::paint_layer::node_is_correction(from) ||
        bke::paint_layer::correction_section_get(from) !=
            PaintMaterialCorrectionSection::Content)
    {
      return false;
    }
    CompositeMixNode below;
    if (!composite_mix_node_read(from, below) || below.bottom == nullptr) {
      return false;
    }
    socket = below.bottom;
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

/**
 * The corrections hanging on \a socket's input, walked down by their links to what feeds them.
 *
 * The walk follows the links, not #composite_source_node_shallow: a muted node must not stop the
 * reading of the shape, because a muted correction still hangs where it hangs -- it is collected
 * switched off instead. Reroutes and muted links are the shapes the chain reader refuses, and so
 * does this. On return \a r_base is the socket the corrections sit on -- the layer's map, a mask
 * image, the "over" pair's output, or nothing, which the caller tells apart -- and
 * \a r_any_contributes says whether any correction brings pixels of its own, the test that keeps
 * a switched-off channel from swallowing a row that its corrections still paint (spec 18 I2').
 *
 * \return false when the chain's shape is not one this module reproduces: the whole channel goes
 * to the bake rather than being composited wrong.
 */
static bool composite_corrections_walk(const bNodeSocket &socket,
                                       const PaintMaterialCorrectionSection section,
                                       Vector<PaintMaterialCompositeCorrection> &r_corrections,
                                       const bNodeSocket *&r_base,
                                       bool &r_any_contributes)
{
  r_corrections.clear();
  r_any_contributes = false;
  const bNodeSocket *current = &socket;
  /* A malformed tree can in principle cycle; bound the walk rather than trust the data. */
  for (int step = 0; step < 64; step++) {
    const Span<const bNodeLink *> links = current->directly_linked_links();
    if (links.is_empty()) {
      /* Nothing under the corrections: the base is absent, the layer's own shape. */
      r_base = current;
      return true;
    }
    const bNodeLink *link = links[0];
    const bNode &from = *link->fromnode;
    if (!bke::paint_layer::node_is_correction(from)) {
      /* The map, a mask image, the "over" pair, or something the caller will refuse -- the
       * corrections end here either way. */
      r_base = current;
      return true;
    }
    /* A correction only hangs here in the shape the engine builds, which is one available link. */
    if (links.size() != 1 || !link->is_available()) {
      return false;
    }
    if (bke::paint_layer::correction_section_get(from) != section) {
      /* A correction of the other section does not belong on this path. */
      r_base = current;
      return true;
    }
    CompositeMixNode corr;
    if (!composite_mix_node_read(from, corr) || corr.top == nullptr || corr.bottom == nullptr) {
      return false;
    }
    if (!corr.blend_supported) {
      /* As with a layer's own blend: the bake reproduces it, a byte composite cannot. */
      return false;
    }

    PaintMaterialCompositeCorrection correction;
    correction.blend = corr.blend;
    correction.marker = bke::paint_layer::marker_get(from);
    /* Switched off twice over: a muted node, or the unlinked-coverage form (invariant I1) whose
     * factor is zero by construction. The buffer model reads a correction's coverage from its own
     * alpha, so the latter must be carried as off rather than applied at full alpha. */
    correction.enabled = !from.is_muted() && !composite_mix_coverage_off(corr);
    correction.row_enabled = !from.is_muted();
    /* The same reading of the opacity a layer row gets: the coverage Multiply's constant, or the
     * bare Factor when the per-channel shape is not there. */
    if (corr.factor_opacity != nullptr) {
      correction.opacity =
          static_cast<const bNodeSocketValueFloat *>(corr.factor_opacity->default_value)->value;
    }
    else if (corr.factor != nullptr &&
             BKE_paint_material_source_socket(*corr.factor) == nullptr)
    {
      correction.opacity =
          static_cast<const bNodeSocketValueFloat *>(corr.factor->default_value)->value;
    }
    /* The correction's own map: the one Image Texture its top input reads, null when it is
     * Absent in this channel. */
    const bNode *map_node = composite_mix_map_node(corr);
    if (map_node != nullptr) {
      if (map_node->id == nullptr || GS(map_node->id->name) != ID_IM) {
        return false;
      }
      Image *image = id_cast<Image *>(map_node->id);
      if (image->source == IMA_SRC_TILED) {
        /* A tiled image has no single buffer to composite, the same rule as everywhere here. */
        return false;
      }
      correction.image = image;
    }
    else if (!corr.top->directly_linked_links().is_empty()) {
      /* Fed, but not by the one Image Texture a correction map is. */
      return false;
    }
    r_any_contributes = r_any_contributes ||
                        (correction.enabled && (correction.image != nullptr ||
                                                (corr.factor_coverage != nullptr &&
                                                 !corr.factor_coverage->directly_linked_links()
                                                      .is_empty())));
    r_corrections.append(correction);
    current = corr.bottom;
  }
  return false;
}

/**
 * Append the layer \a mix builds, carrying the corrections walked off its inputs.
 *
 * A layer whose corrections contribute is collected even when its own coverage is switched off
 * (the unlinked-coverage shape): the corrections are what the row still paints with (spec 18
 * I2'). Its base map may be absent entirely, which is why #color_image may be left null -- the
 * buffer model composites the corrections over transparency then.
 *
 * The coverage the layer blends by comes from whatever fed the coverage input under the mask
 * corrections: a mask image stays a mask image, while the map's own alpha and the content
 * corrections' accumulated "over" coverage both read as the layer masking itself by its own
 * alpha -- the buffer model's way of saying the factor is that accumulated alpha.
 *
 * \param base_on: the row's own map is on in this channel. A Disabled map stays linked but is
 * muted, which shader localization reads as no map at all -- so it composites as an absent base.
 */
static bool composite_collect_layer_with_corrections(
    const CompositeMixNode &mix,
    const Vector<PaintMaterialCompositeCorrection> &content_corrections,
    const Vector<PaintMaterialCompositeCorrection> &mask_corrections,
    const bNodeSocket &content_base,
    const bNodeSocket *coverage_base,
    const bool base_on,
    PaintMaterialCompositeImageLayer &r_layer,
    Vector<PaintMaterialCompositeImageLayer> &r_layers)
{
  r_layer.content_corrections = content_corrections;
  r_layer.mask_corrections = mask_corrections;

  /* The base under the content corrections: one image, or nothing -- Absent in this channel. */
  if (base_on && !content_base.directly_linked_links().is_empty()) {
    if (!composite_image_from_socket(content_base, r_layer.color_image, r_layer.color_iuser)) {
      return false;
    }
  }

  /* The layer's own opacity, kept editable by the coverage Multiply the corrections leave in
   * place; a bare factor keeps its own constant. A factor linked without a Multiply has already
   * been consumed as the coverage above, and leaves nothing editable. */
  if (mix.factor_opacity != nullptr) {
    r_layer.opacity =
        static_cast<const bNodeSocketValueFloat *>(mix.factor_opacity->default_value)->value;
  }
  else if (mix.factor != nullptr &&
           BKE_paint_material_source_socket(*mix.factor) == nullptr)
  {
    r_layer.opacity =
        static_cast<const bNodeSocketValueFloat *>(mix.factor->default_value)->value;
  }

  /* Self-masking by default: the map's own alpha, or -- with an absent base -- the corrections'
   * accumulated coverage, which an absent map reads as zero. */
  r_layer.mask_from_alpha = true;
  r_layer.mask_image = r_layer.color_image;
  r_layer.mask_iuser = r_layer.color_iuser;
  if (coverage_base != nullptr && !coverage_base->directly_linked_links().is_empty()) {
    bool coverage_from_alpha = false;
    Image *coverage_image = nullptr;
    const ImageUser *coverage_iuser = nullptr;
    if (composite_image_from_socket(
            *coverage_base, coverage_image, coverage_iuser, &coverage_from_alpha))
    {
      if (!coverage_from_alpha) {
        /* A mask image of its own, rather than the map's alpha. */
        r_layer.mask_image = coverage_image;
        r_layer.mask_iuser = coverage_iuser;
        r_layer.mask_from_alpha = false;
      }
    }
    else if (content_corrections.is_empty()) {
      /* Not an image and nothing of the layer's own that could have accumulated there: a shape
       * this module does not build. */
      return false;
    }
  }
  r_layers.append(r_layer);
  return true;
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

  /* The corrections hanging on the layer's own inputs, walked down to whatever feeds them: the
   * content stack off the map input, the mask chain off the coverage input. */
  Vector<PaintMaterialCompositeCorrection> content_corrections;
  bool content_contributes = false;
  const bNodeSocket *content_base = mix.top;
  if (!composite_corrections_walk(*mix.top,
                                  PaintMaterialCorrectionSection::Content,
                                  content_corrections,
                                  content_base,
                                  content_contributes))
  {
    return false;
  }
  Vector<PaintMaterialCompositeCorrection> mask_corrections;
  bool mask_contributes = false;
  const bNodeSocket *coverage_base = nullptr;
  {
    const bNodeSocket *coverage_socket = (mix.factor_opacity != nullptr) ? mix.factor_coverage :
                                                                          mix.factor;
    if (coverage_socket != nullptr &&
        !composite_corrections_walk(*coverage_socket,
                                    PaintMaterialCorrectionSection::Mask,
                                    mask_corrections,
                                    coverage_base,
                                    mask_contributes))
    {
      return false;
    }
  }
  const bool has_corrections = !content_corrections.is_empty() || !mask_corrections.is_empty();

  PaintMaterialCompositeImageLayer layer;
  layer.blend = mix.blend;

  const bNode *top_source = composite_source_node_shallow(*mix.top);
  if (top_source != nullptr && BKE_paint_material_is_layer_group(*top_source)) {
    if (has_corrections) {
      /* A mask correction on a folder limits the folder's result as a whole (spec 18 §4.3), but
       * flattening lists the layers it holds one by one: there is no combined folder buffer for
       * the correction to shape, and applying it to each layer's own coverage would composite
       * something else. Not a shape this module reproduces -- the channel goes to the bake,
       * which evaluates the graph properly, rather than one that quietly differs. */
      return false;
    }
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

  if (has_corrections) {
    /* What the row puts into this channel (spec 18 I2'): its own map switched on, or a content
     * correction painting. A mask correction brings no pixels of its own -- where the row puts
     * nothing in, its mask chain is zeroed in the graph (#layer_mask_corrections_sync) -- so it
     * never keeps a row. A base the reader does not recognize keeps the old reading: on. */
    PaintMaterialLayerChannelState base_state = PaintMaterialLayerChannelState::Enabled;
    const bool base_on = !composite_layer_base_state_get(*shallow_source, base_state) ||
                         base_state == PaintMaterialLayerChannelState::Enabled;
    if (!base_on && !content_contributes) {
      /* Absent or Disabled in this channel, and nothing the corrections hold brings pixels back:
       * it contributes nothing and the rows below composite exactly as if it were not there. */
      return true;
    }
    return composite_collect_layer_with_corrections(mix,
                                                    content_corrections,
                                                    mask_corrections,
                                                    *content_base,
                                                    coverage_base,
                                                    base_on,
                                                    layer,
                                                    r_layers);
  }

  if (composite_mix_coverage_off(mix)) {
    /* Absent or Disabled in this channel: its Factor is zero by construction, so it contributes
     * nothing and the rows below it composite exactly as if it were not there. */
    return true;
  }
  if (!composite_image_from_socket(*mix.top, layer.color_image, layer.color_iuser)) {
    return false;
  }
  if (mix.factor_opacity != nullptr) {
    /* Coverage and the layer's own opacity coexist: the mask comes from the Multiply's other
     * input, not from #mix.factor itself. */
    if (!composite_image_from_socket(
            *mix.factor_coverage, layer.mask_image, layer.mask_iuser, &layer.mask_from_alpha))
    {
      return false;
    }
    layer.opacity =
        static_cast<const bNodeSocketValueFloat *>(mix.factor_opacity->default_value)->value;
  }
  else if (BKE_paint_material_source_socket(*mix.factor) != nullptr) {
    /* A linked factor with nothing to separate an opacity from is a mask alone, and only an image
     * one can be sampled per pixel. */
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

/** The image tagged as the map of the correction \a marker in \a channel, or null. */
static Image *composite_correction_map_find(const Main &bmain,
                                            const bUUID &marker,
                                            const int channel)
{
  if (BLI_uuid_is_nil(marker)) {
    return nullptr;
  }
  return composite_layer_map_find(bmain, marker, channel);
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
 * whole stack: a user who baked AO for one layer only should still see that layer's AO. The same
 * holds for a layer's corrections, whose maps are tagged by the correction's marker -- one without
 * a map here is kept, Absent, since the correction still exists in the channel the row was built
 * from.
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
    /* The reference's corrections arrive pointing at the reference channel's maps; this channel's
     * own are found by the correction's marker, the way the layer's own map was. */
    layer.content_corrections.clear();
    for (const PaintMaterialCompositeCorrection &correction : reference.content_corrections) {
      PaintMaterialCompositeCorrection remapped = correction;
      remapped.image = composite_correction_map_find(bmain, correction.marker, channel);
      remapped.iuser = nullptr;
      /* The reference channel's coverage says whether *its* map is on; this channel's map is on
       * exactly when it exists, as long as the row itself is. */
      remapped.enabled = correction.row_enabled && remapped.image != nullptr;
      layer.content_corrections.append(remapped);
    }
    layer.mask_corrections.clear();
    for (const PaintMaterialCompositeCorrection &correction : reference.mask_corrections) {
      PaintMaterialCompositeCorrection remapped = correction;
      /* A mask has one map for every channel (spec 18 §4.3), tagged with the mask role rather
       * than with a channel. */
      remapped.image = composite_correction_map_find(bmain, correction.marker, PAINT_LAYER_MAP_MASK);
      remapped.iuser = nullptr;
      layer.mask_corrections.append(remapped);
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
 * The mean colour and the alpha of a mask-correction pixel, read the way #mask_factor_at reads a
 * mask.
 *
 * A mask correction blends a coverage factor, not a colour, so its buffer is sampled as the
 * scalar the graph's color-to-value conversion would produce -- the mean of its RGB -- while its
 * own alpha is the coverage the correction applies with.
 */
static void correction_mask_values_at(const ImBuf *ibuf,
                                      const int x,
                                      const int y,
                                      float &r_gray,
                                      float &r_alpha)
{
  const int channels = ibuf->channels == 0 ? 4 : ibuf->channels;
  const int64_t offset = (int64_t(y) * ibuf->x + x) * channels;
  if (ibuf->byte_buffer.data != nullptr) {
    const uchar *pixel = ibuf->byte_data() + offset;
    r_gray = (float(pixel[0]) + float(pixel[1]) + float(pixel[2])) / (3.0f * 255.0f);
    r_alpha = channels == 4 ? float(pixel[3]) / 255.0f : 1.0f;
  }
  else {
    const float *pixel = ibuf->float_buffer.data + offset;
    r_gray = (pixel[0] + pixel[1] + pixel[2]) / 3.0f;
    r_alpha = channels == 4 ? pixel[3] : 1.0f;
  }
  r_gray = clamp_f(r_gray, 0.0f, 1.0f);
  r_alpha = clamp_f(r_alpha, 0.0f, 1.0f);
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

/**
 * One scalar component of #blend_layer_byte, for the mask corrections that blend a coverage
 * factor rather than a colour (spec 18 §5.3).
 *
 * The formulas are the per-component bodies of #blend_layer_byte on a single channel.
 * NormalCombine has no scalar form -- whiteout needs a direction -- so it is evaluated on an
 * isotropic pair, all channels equal, and averaged: a shape no graph this module builds puts on a
 * mask, kept total rather than special-cased away.
 */
static float blend_value(const float bottom,
                         const float top,
                         const CompositeBlend blend,
                         const float fac)
{
  const float clamped = clamp_f(fac, 0.0f, 1.0f);
  if (clamped == 0.0f) {
    return bottom;
  }
  switch (blend) {
    case CompositeBlend::Mix:
      return bottom + (top - bottom) * clamped;
    case CompositeBlend::Multiply:
      return bottom * (1.0f - clamped) + bottom * top * clamped;
    case CompositeBlend::Overlay:
      return blend_overlay_channel(bottom, top, clamped);
    case CompositeBlend::Add:
      return bottom * (1.0f - clamped) + (bottom + top) * clamped;
    case CompositeBlend::NormalCombine: {
      const float bottom_rgba[4] = {bottom, bottom, bottom, 1.0f};
      const float top_rgba[4] = {top, top, top, 1.0f};
      float combined[3];
      blend_normal_combine(bottom_rgba, top_rgba, combined);
      return (combined[0] + combined[1] + combined[2]) / 3.0f;
    }
  }
  return bottom;
}

/**
 * Composite one pixel of a layer that carries corrections (spec 18 §5.3).
 *
 * The colour starts at the layer's own map -- or at nothing, for a layer Absent in this channel
 * whose content corrections are what it paints with. Each content correction blends by its own
 * alpha, the way the graph routes a correction's map alpha into its coverage Multiply, and the
 * accumulated coverage grows the way the engine's "over" pair does:
 * `a = a + f * (1 - a)`.
 *
 * What the layer finally blends by is the mask image when it has one -- the graph leaves the
 * corrections' accumulated coverage unwired there -- and the accumulated coverage everywhere else.
 * Mask corrections blend onto that factor after #mask_influence, which the base coverage already
 * carries: the mask image establishes the coverage, the corrections sit on top of it.
 *
 * \param offset: the pixel's offset into every layer buffer, which all match the stack dimensions.
 */
static void composite_correction_pixel_apply(const PaintMaterialCompositeLayer &layer,
                                             const int64_t offset,
                                             const int x,
                                             const int y,
                                             uchar *r_dst)
{
  uchar top[4] = {0, 0, 0, 0};
  if (layer.color_ibuf != nullptr) {
    const uchar *base = layer.color_ibuf->byte_data() + offset;
    top[0] = base[0];
    top[1] = base[1];
    top[2] = base[2];
  }

  /* The coverage the layer starts from: its own mask, its own alpha, or full. The mask cases read
   * exactly as the no-correction path reads them; with no mask at all, an alpha-driven layer
   * covers by its own alpha -- where an absent base has none, and the corrections are what bring
   * coverage in -- and a plain one covers fully, like the null-mask factor does. */
  float alpha;
  if (layer.mask_ibuf != nullptr) {
    alpha = mask_factor_at(layer.mask_ibuf, layer.mask_from_alpha, x, y, layer.mask_influence);
  }
  else if (layer.mask_from_alpha) {
    alpha = (layer.color_ibuf != nullptr) ?
                float(layer.color_ibuf->byte_data()[offset + 3]) / 255.0f :
                0.0f;
  }
  else {
    alpha = 1.0f;
  }
  /* A mask image owns the coverage: what the corrections accumulate goes to the layer's alpha
   * alone, not to the factor the layer blends by. */
  const float mask_base = alpha;

  for (const PaintMaterialCompositeCorrectionBuffer &correction : layer.content_corrections) {
    if (!correction.enabled || correction.ibuf == nullptr) {
      continue;
    }
    const uchar *corr_pixel = correction.ibuf->byte_data() + offset;
    const float corr_alpha = float(corr_pixel[3]) / 255.0f;
    /* The correction's colour and its coverage ride the same factor, as the graph's own coverage
     * Multiply does for a layer. */
    blend_layer_byte(top, corr_pixel, correction.blend, correction.opacity, corr_alpha);
    const float fac = clamp_f(correction.opacity * corr_alpha, 0.0f, 1.0f);
    alpha = alpha + fac * (1.0f - alpha);
  }

  float mask_factor = (layer.mask_ibuf != nullptr && !layer.mask_from_alpha) ? mask_base : alpha;
  for (const PaintMaterialCompositeCorrectionBuffer &correction : layer.mask_corrections) {
    if (!correction.enabled || correction.ibuf == nullptr) {
      continue;
    }
    float gray = 0.0f;
    float corr_alpha = 0.0f;
    correction_mask_values_at(correction.ibuf, x, y, gray, corr_alpha);
    mask_factor = blend_value(
        mask_factor, gray, correction.blend, correction.opacity * corr_alpha);
  }

  top[3] = uchar(clamp_i(int(alpha * 255.0f + 0.5f), 0, 255));
  blend_layer_byte(r_dst, top, layer.blend, layer.opacity, mask_factor);
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
    const bool has_corrections = !layer.content_corrections.is_empty() ||
                                 !layer.mask_corrections.is_empty();
    if (layer.color_ibuf != nullptr) {
      if (!composite_ibuf_is_byte_rgba(layer.color_ibuf, stack.width, stack.height)) {
        return false;
      }
    }
    else if (!has_corrections) {
      /* A layer without a map has nothing to composite unless its content corrections carry it
       * (spec 18 §5.3); refusing it here is what sends such a stack to the bake. */
      return false;
    }
    if (layer.mask_ibuf != nullptr &&
        !composite_mask_ibuf_is_valid(layer.mask_ibuf, stack.width, stack.height))
    {
      return false;
    }
    for (const PaintMaterialCompositeCorrectionBuffer &correction : layer.content_corrections) {
      if (correction.ibuf != nullptr &&
          !composite_ibuf_is_byte_rgba(correction.ibuf, stack.width, stack.height))
      {
        return false;
      }
    }
    for (const PaintMaterialCompositeCorrectionBuffer &correction : layer.mask_corrections) {
      if (correction.ibuf != nullptr &&
          !composite_mask_ibuf_is_valid(correction.ibuf, stack.width, stack.height))
      {
        return false;
      }
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
    const bool has_corrections = !layer.content_corrections.is_empty() ||
                                 !layer.mask_corrections.is_empty();
    const uchar *layer_pixels = (layer.color_ibuf != nullptr) ? layer.color_ibuf->byte_data() :
                                                               nullptr;

    if (!composite_initialized) {
      /* A bare bottom is copied rather than blended: it has no Mix node, so it has no blend mode
       * or factor, and blending it over undefined pixels would let them show through wherever it
       * is transparent. A uniform chain's lowest layer has all of those, and blends over the
       * transparency the graph gives it -- so the buffer starts cleared and it is blended like
       * any other layer. */
      threading::parallel_for(rows, 64, [&](const IndexRange range) {
        for (const int64_t y : range) {
          const int64_t offset = y * row_stride + int64_t(area.xmin) * 4;
          if (layer.is_bare_base && layer_pixels != nullptr) {
            memcpy(composite_pixels + offset, layer_pixels + offset, size_t(area_width * 4));
          }
          else {
            /* Including a corrections layer with no base of its own: its bottom is transparency,
             * and its corrections bring what coverage there is (spec 18 §5.3). */
            memset(composite_pixels + offset, 0, size_t(area_width * 4));
          }
        }
      });
      composite_initialized = true;
      if (layer.is_bare_base && !has_corrections) {
        layers_evaluated++;
        continue;
      }
    }

    if (has_corrections) {
      /* The corrections path: per pixel, the layer's colour and coverage rebuilt under its
       * corrections before it blends onto the composite. Same row layout as the plain path, so a
       * region refresh covers the same rectangle whichever path a layer takes. */
      threading::parallel_for(rows, 64, [&](const IndexRange range) {
        for (const int64_t y : range) {
          const int64_t row_offset = y * row_stride;
          for (const int64_t x : IndexRange(area.xmin, area_width)) {
            composite_correction_pixel_apply(layer,
                                             row_offset + x * 4,
                                             int(x),
                                             int(y),
                                             composite_pixels + row_offset + x * 4);
          }
        }
      });
      layers_evaluated++;
      continue;
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
    if (!image_layer.enabled) {
      continue;
    }
    const bool has_corrections = !image_layer.content_corrections.is_empty() ||
                                 !image_layer.mask_corrections.is_empty();
    if (image_layer.color_image == nullptr && !has_corrections) {
      continue;
    }
    PaintMaterialCompositeLayer layer;
    if (image_layer.color_image != nullptr) {
      layer.color_ibuf = composite_image_acquire(
          image_layer.color_image, image_layer.color_iuser, r_locks);
      if (layer.color_ibuf == nullptr) {
        return false;
      }
    }
    if (image_layer.mask_image != nullptr) {
      layer.mask_ibuf = composite_image_acquire(
          image_layer.mask_image, image_layer.mask_iuser, r_locks);
      if (layer.mask_ibuf == nullptr) {
        return false;
      }
    }
    for (const PaintMaterialCompositeCorrection &correction : image_layer.content_corrections) {
      PaintMaterialCompositeCorrectionBuffer buffer;
      buffer.ibuf = composite_image_acquire(correction.image, correction.iuser, r_locks);
      if (buffer.ibuf == nullptr && correction.image != nullptr) {
        return false;
      }
      buffer.blend = correction.blend;
      buffer.opacity = correction.opacity;
      buffer.enabled = correction.enabled;
      layer.content_corrections.append(buffer);
    }
    for (const PaintMaterialCompositeCorrection &correction : image_layer.mask_corrections) {
      PaintMaterialCompositeCorrectionBuffer buffer;
      buffer.ibuf = composite_image_acquire(correction.image, correction.iuser, r_locks);
      if (buffer.ibuf == nullptr && correction.image != nullptr) {
        return false;
      }
      buffer.blend = correction.blend;
      buffer.opacity = correction.opacity;
      buffer.enabled = correction.enabled;
      layer.mask_corrections.append(buffer);
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
 * The image whose buffer answers the bottom layer's size and colorspace: the layer's own map, or
 * -- when the layer is Absent here and its corrections carry it -- the first correction map.
 */
static Image *composite_bottom_layer_size_image(const PaintMaterialCompositeImageLayer &layer,
                                                const ImageUser *&r_iuser)
{
  if (layer.color_image != nullptr) {
    r_iuser = layer.color_iuser;
    return layer.color_image;
  }
  for (const PaintMaterialCompositeCorrection &correction : layer.content_corrections) {
    if (correction.image != nullptr) {
      r_iuser = correction.iuser;
      return correction.image;
    }
  }
  for (const PaintMaterialCompositeCorrection &correction : layer.mask_corrections) {
    if (correction.image != nullptr) {
      r_iuser = correction.iuser;
      return correction.image;
    }
  }
  return nullptr;
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
    if (!layer.enabled) {
      continue;
    }
    const ImageUser *size_iuser = nullptr;
    Image *size_image = composite_bottom_layer_size_image(layer, size_iuser);
    if (size_image == nullptr) {
      continue;
    }
    Vector<CompositeImageLock> locks;
    const ImBuf *ibuf = composite_image_acquire(size_image, size_iuser, locks);
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

/** Extend \a hash with everything about one correction that changes the composited pixels. */
static uint64_t composite_correction_hash(uint64_t hash,
                                          const PaintMaterialCompositeCorrection &correction)
{
  /* The marker hashed field by field: it is the correction's identity, so a map re-tagged to a
   * different correction must not keep serving the old composite. */
  const bUUID &marker = correction.marker;
  uint64_t node_bytes = 0;
  for (const int i : IndexRange(6)) {
    node_bytes |= uint64_t(marker.node[i]) << (8 * (5 - i));
  }
  hash = get_default_hash(hash,
                          marker.time_low,
                          uint64_t(marker.time_mid) << 16 | marker.time_hi_and_version,
                          uint64_t(marker.clock_seq_hi_and_reserved) << 8 |
                              marker.clock_seq_low,
                          node_bytes);
  /* Session UID rather than a pointer, like the layers' own maps: a freed image's address can
   * come back as a different one. */
  return get_default_hash(hash,
                          correction.image != nullptr ? correction.image->id.session_uid : 0,
                          int(correction.blend),
                          correction.enabled,
                          correction.opacity);
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
    /* A correction changes the composite like any other layer input, and so belongs in the hash
     * that decides whether the whole stack has to be re-flattened. */
    for (const PaintMaterialCompositeCorrection &correction : layer.content_corrections) {
      hash = composite_correction_hash(hash, correction);
    }
    for (const PaintMaterialCompositeCorrection &correction : layer.mask_corrections) {
      hash = composite_correction_hash(hash, correction);
    }
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

/**
 * The images one layer is read from: its maps, then its corrections'.
 *
 * The cache subscribes to each image's partial-update log through this one list, so a correction's
 * own map reports its edits exactly the way a layer's map does. Deduplicated, since a layer that
 * masks itself by its own map names the same image twice and one subscription per image is all a
 * poll can use.
 */
static Vector<Image *> composite_layer_images(const PaintMaterialCompositeImageLayer &layer)
{
  Vector<Image *> images;
  if (layer.color_image != nullptr) {
    images.append_non_duplicates(layer.color_image);
  }
  if (layer.mask_image != nullptr) {
    images.append_non_duplicates(layer.mask_image);
  }
  for (const PaintMaterialCompositeCorrection &correction : layer.content_corrections) {
    if (correction.image != nullptr) {
      images.append_non_duplicates(correction.image);
    }
  }
  for (const PaintMaterialCompositeCorrection &correction : layer.mask_corrections) {
    if (correction.image != nullptr) {
      images.append_non_duplicates(correction.image);
    }
  }
  return images;
}

static void composite_entry_image_dependencies_set(
    CompositeCacheEntry &entry, Span<PaintMaterialCompositeImageLayer> image_layers)
{
  entry.image_session_uids.clear();
  for (const PaintMaterialCompositeImageLayer &layer : image_layers) {
    for (Image *image : composite_layer_images(layer)) {
      entry.image_session_uids.append_non_duplicates(image->id.session_uid);
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
    for (Image *image : composite_layer_images(layer)) {
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
