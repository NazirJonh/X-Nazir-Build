/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "BKE_paint_material_composite.hh"

#include "BKE_colorband.hh"
#include "BKE_idprop.hh"
#include "BKE_image.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_gradient.hh"
#include "BKE_paint_perf_debug.hh"
#include "BKE_paint_material_resolve.hh"

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
#include "BLI_enumerable_thread_specific.hh"
#include "BLI_task.hh"
#include "BLI_time.h"
#include "BLI_utildefines.h"
#include "BLI_uuid.h"

#include "BLT_translation.hh"

#include "paint_layers/paint_layers_kind.hh"

#include "paint_material_composite_internal.hh"

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_tree_interface_types.h"
#include "DNA_node_types.h"
#include "DNA_scene_types.h"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <iterator>
#include <memory>

#include "paint_material_cache_common.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Node group markers
 * \{ */

/** The key shared by every group this module marks; only the value says which group it is. */
static constexpr const char *NODE_GROUP_MARKER_PROP = "pbr_paint_node_group";

/** The marker value on \a tree_id, or null when unmarked. */
static const char *node_group_marker_get(const ID &tree_id)
{
  const IDProperty *properties = IDP_GetProperties(const_cast<ID *>(&tree_id));
  if (properties == nullptr) {
    return nullptr;
  }
  const IDProperty *marker = IDP_GetPropertyTypeFromGroup(
      properties, NODE_GROUP_MARKER_PROP, IDP_STRING);
  return (marker != nullptr) ? IDP_string_get(marker) : nullptr;
}

bool paint_material_group_has_marker(const bNode &node, const char *marker_value)
{
  if (!node.is_group() || node.id == nullptr || GS(node.id->name) != ID_NT) {
    return false;
  }
  const char *marker = node_group_marker_get(*node.id);
  return marker != nullptr && STREQ(marker, marker_value);
}

bNodeTree *paint_material_group_find(Main &bmain, const char *marker_value)
{
  for (bNodeTree &ntree : bmain.nodetrees) {
    const char *marker = node_group_marker_get(ntree.id);
    if (marker != nullptr && STREQ(marker, marker_value)) {
      return &ntree;
    }
  }
  return nullptr;
}

void paint_material_group_marker_set(bNodeTree &group, const char *marker_value)
{
  IDProperty *properties = IDP_EnsureProperties(&group.id);
  IDPropertyTemplate value = {0};
  value.string.str = const_cast<char *>(marker_value);
  value.string.len = int(strlen(marker_value)) + 1;
  value.string.subtype = IDP_STRING_SUB_UTF8;
  IDP_AddToGroup(properties, IDP_New(IDP_STRING, &value, NODE_GROUP_MARKER_PROP));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Stack Derivation
 *
 * Recognizes the one graph shape this module can reproduce: image layers stacked with Mix nodes.
 * Everything here only reads the tree, so it is as cheap as the resolver and may run on a redraw.
 * \{ */

/** The subset of Mix blend modes that the legacy graph-as-truth reader reproduces exactly. The
 * description path (a layered material) is not limited by this: it reads the row's blend through
 * #BKE_paint_layers_blend_to_ramp and #blend_row_linear handles every Mix mode. */
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
      /* The legacy reader only takes the modes it always did; a chain using any other falls to the
       * bake, which evaluates it properly, rather than being read as a stack that the old byte path
       * would approximate. */
      return false;
  }
}

/** The inputs of \a node when it is a colour Mix node, in either the legacy or the current form.
 */
struct CompositeMixNode {
  const bNodeSocket *factor = nullptr;
  /** What is below in the stack. */
  const bNodeSocket *bottom = nullptr;
  /** The layer itself. */
  const bNodeSocket *top = nullptr;
  CompositeBlend blend = CompositeBlend::Mix;
};

static bool composite_mix_node_read(const bNode &node, CompositeMixNode &r_mix)
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
  if (BKE_paint_material_is_blend_group(node)) {
    /* The mode is a value on the group rather than a node type: the group exists so that changing
     * a layer's blend mode is one integer edit instead of a rewire. */
    const bNodeSocket *mode = bke::node_find_socket(
        node, SOCK_IN, UString::from_ptr_noinline(BLEND_GROUP_ID_MODE));
    if (mode == nullptr || mode->type != SOCK_INT || mode->default_value == nullptr) {
      return false;
    }
    /* Strictly the int socket's own storage. A blend mode is a `MA_RAMP_*` code that indexes the
     * group's branches, so reading it through any other value type -- a float socket that happens
     * to sit in its place -- would silently round it to a neighbouring mode. */
    const int mode_value = static_cast<const bNodeSocketValueInt *>(mode->default_value)->value;
    if (!composite_blend_from_ramp_blend(mode_value, r_mix.blend)) {
      return false;
    }
    r_mix.factor = bke::node_find_socket(
        node, SOCK_IN, UString::from_ptr_noinline(BLEND_GROUP_ID_FACTOR));
    r_mix.bottom = bke::node_find_socket(
        node, SOCK_IN, UString::from_ptr_noinline(BLEND_GROUP_ID_A));
    r_mix.top = bke::node_find_socket(
        node, SOCK_IN, UString::from_ptr_noinline(BLEND_GROUP_ID_B));
    return r_mix.factor != nullptr && r_mix.bottom != nullptr && r_mix.top != nullptr;
  }
  if (node.type_legacy == SH_NODE_MIX_RGB_LEGACY) {
    if (!composite_blend_from_ramp_blend(node.custom1, r_mix.blend)) {
      return false;
    }
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
    if (!composite_blend_from_ramp_blend(storage->blend_type, r_mix.blend)) {
      return false;
    }
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
static bool composite_image_from_socket(const bNodeSocket &socket,
                                        Image *&r_image,
                                        const ImageUser *&r_iuser,
                                        bool *r_from_alpha = nullptr)
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
static const bNode *composite_source_node_shallow(const bNodeSocket &socket)
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
                                    const int depth)
{
  /* A malformed tree can cycle, and a very deep chain is not worth compositing anyway. */
  if (depth > 64) {
    return false;
  }

  /* A group that is itself an operation -- the normal combine, the PBR blend -- has to be
   * recognized before the socket walk resolves through it: #BKE_paint_material_source_socket
   * descends into a group and reports the node inside that happens to feed the output, which says
   * nothing about what the group as a whole does. Both groups are therefore read at the shallow
   * node. A blend mode this reader does not reproduce makes #composite_mix_node_read fail, and the
   * caller keeps its bake fallback: the group evaluates every mode correctly in the shader, so
   * widening the fast path mode by mode would only duplicate that arithmetic and let the two
   * disagree about a layer's blend. */
  const bNode *shallow_source = composite_source_node_shallow(socket);
  const bool is_operation_group =
      shallow_source != nullptr &&
      (BKE_paint_material_is_normal_combine_group(*shallow_source) ||
       BKE_paint_material_is_blend_group(*shallow_source));

  if (!is_operation_group) {
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
      r_layers.append(layer);
      return true;
    }
    shallow_source = &node;
  }

  CompositeMixNode mix;
  if (!composite_mix_node_read(*shallow_source, mix)) {
    return false;
  }
  if (!composite_stack_collect(*mix.bottom, r_layers, depth + 1)) {
    return false;
  }

  PaintMaterialCompositeImageLayer layer;
  layer.blend = mix.blend;
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

/**
 * The images that carry a paint layer tag at all, which is the whole of what a map lookup can
 * match.
 *
 * Gathered once for a stack rather than re-scanned per layer. The lookup below is asked once per
 * layer per channel, and it used to walk every image in the file each time: a resolve that a
 * redraw performs is then O(layers x images), on a path whose entire purpose is to stay
 * proportional to what the user just painted. Almost no image in a file is tagged, so the filtered
 * list is short and the scan happens once.
 */
static void composite_layer_map_candidates_gather(const Main &bmain, Vector<Image *> &r_images)
{
  r_images.clear();
  for (Image &image : const_cast<Main &>(bmain).images) {
    if (image.paint_layer_channel == PAINT_LAYER_MAP_NONE) {
      continue;
    }
    if (BLI_uuid_is_nil(image.paint_layer_id)) {
      /* The tag is only meaningful together with the layer it names. */
      continue;
    }
    r_images.append(&image);
  }
}

/** The image tagged as \a channel of the paint layer \a layer_id, or null. */
static Image *composite_layer_map_find(Span<Image *> candidates,
                                       const bUUID &layer_id,
                                       const int channel)
{
  for (Image *image : candidates) {
    if (image->paint_layer_channel != channel) {
      continue;
    }
    if (BLI_uuid_equal(image->paint_layer_id, layer_id)) {
      return image;
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
  Vector<Image *> candidates;
  composite_layer_map_candidates_gather(bmain, candidates);
  for (const PaintMaterialCompositeImageLayer &reference : reference_layers) {
    if (reference.color_image == nullptr || BLI_uuid_is_nil(reference.color_image->paint_layer_id))
    {
      continue;
    }
    Image *map = composite_layer_map_find(
        candidates, reference.color_image->paint_layer_id, channel);
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
    Vector<PaintMaterialCompositeImageLayer> &r_layers,
    const bool allow_layer_map_fallback)
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
  if (!allow_layer_map_fallback) {
    /* Everything below walks the graph again, once per channel until one resolves, and then reads
     * the file's images. A caller that opted out is in a loop over materials and cannot afford it.
     */
    r_layers.clear();
    return false;
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

float BKE_paint_material_coverage_match(const int16_t map_coverage,
                                        const float rgb[3],
                                        const float tolerance,
                                        const float softness,
                                        const Span<PaintMaterialCompositeIdColor> colors)
{
  if (map_coverage == MA_PAINT_LAYER_FILTER_UV_ISLAND) {
    /* The island mask is a scalar packed in the red channel of a data map. */
    return clamp_f(rgb[0], 0.0f, 1.0f);
  }
  if (map_coverage != MA_PAINT_LAYER_FILTER_COLOR_ID) {
    return 0.0f;
  }
  const float tol = max_ff(0.0f, tolerance);
  const float soft = max_ff(0.0f, softness);
  float match = 0.0f;
  for (const PaintMaterialCompositeIdColor &entry : colors) {
    if (!entry.enabled) {
      continue;
    }
    /* The max-channel distance, like the picker's tolerance: a per-channel mean would let a colour
     * differ wildly in one channel and still read as near. */
    const float dist = max_ff(max_ff(fabsf(rgb[0] - entry.color[0]), fabsf(rgb[1] - entry.color[1])),
                              fabsf(rgb[2] - entry.color[2]));
    /* Advanced mode gives each colour its own tolerance and drops the shared softness: the edge is
     * hard, as there is one tolerance per colour and no per-colour softness. */
    const float entry_tol = entry.own_tolerance ? max_ff(0.0f, entry.tolerance) : tol;
    const float entry_soft = entry.own_tolerance ? 0.0f : soft;
    /* `1 - smoothstep(tol, tol + soft, dist)`: one inside the tolerance, zero past the falloff. A
     * zero softness is a hard edge, which #smoothstep already gives for equal edges. */
    const float entry_match = 1.0f - smoothstep(entry_tol, entry_tol + entry_soft, dist);
    match = max_ff(match, entry_match);
  }
  return match;
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

bool BKE_paint_material_composite_area_get(
    const int width, const int height, const rcti *region, rcti &r_area)
{
  BLI_rcti_init(&r_area, 0, width, 0, height);
  if (region == nullptr) {
    return true;
  }
  rcti clipped = *region;
  return BLI_rcti_isect(&r_area, &clipped, &r_area);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Linear Evaluation
 *
 * The scene-linear float pipeline: decodes every source into scene linear first and mixes the way
 * the shader does, so a CPU composite and the render agree beyond a rounding step.
 * \{ */

/** Whether \a ibuf is a byte or float RGBA buffer of the stack's dimensions. */
static bool composite_ibuf_is_rgba(const ImBuf *ibuf, const int width, const int height)
{
  if (ibuf == nullptr || ibuf->x != width || ibuf->y != height) {
    return false;
  }
  if (!ELEM(ibuf->channels, 0, 4)) {
    return false;
  }
  return ibuf->byte_buffer.data != nullptr || ibuf->float_buffer.data != nullptr;
}

/** The colorspace name of \a ibuf's own buffer, or null when its buffer names none. */
static const char *composite_buffer_colorspace_name(const ImBuf *ibuf)
{
  const ColorSpace *colorspace = (ibuf->byte_buffer.data != nullptr) ?
                                     ibuf->byte_buffer.colorspace :
                                     ibuf->float_buffer.colorspace;
  if (colorspace == nullptr) {
    return nullptr;
  }
  return IMB_colormanagement_colorspace_get_name(colorspace);
}

/**
 * Decode one tile of \a ibuf into \a dst as straight scene-linear RGBA: `tw * th * 4` floats.
 *
 * A byte buffer is straight already; a float one is premultiplied and is straightened here, before
 * the colorspace conversion, and the conversion is one buffer call per tile rather than a per-pixel
 * one -- what keeps a 4K map cheap. A data or scene-linear space is left as it is.
 */
static void composite_decode_tile(const ImBuf *ibuf,
                                  const char *colorspace_name,
                                  const rcti &tile,
                                  float *dst)
{
  const int tw = BLI_rcti_size_x(&tile);
  const int th = BLI_rcti_size_y(&tile);
  const bool is_float = ibuf->byte_buffer.data == nullptr && ibuf->float_buffer.data != nullptr;
  const int channels = ibuf->channels == 0 ? 4 : ibuf->channels;
  const bool four = channels == 4;
  const bool grey = channels == 1;
  for (int ty = 0; ty < th; ty++) {
    const int y = tile.ymin + ty;
    for (int tx = 0; tx < tw; tx++) {
      const int x = tile.xmin + tx;
      float *out = dst + (int64_t(ty) * tw + tx) * 4;
      if (!is_float) {
        const uchar *pixel = ibuf->byte_data() + (int64_t(y) * ibuf->x + x) * 4;
        out[0] = float(pixel[0]) / 255.0f;
        out[1] = float(pixel[1]) / 255.0f;
        out[2] = float(pixel[2]) / 255.0f;
        out[3] = float(pixel[3]) / 255.0f;
      }
      else {
        const float *pixel = ibuf->float_buffer.data + (int64_t(y) * ibuf->x + x) * channels;
        out[0] = pixel[0];
        /* A one-channel buffer is a grey: the other channels read its only one, not the next texel. */
        out[1] = grey ? pixel[0] : pixel[1];
        out[2] = grey ? pixel[0] : pixel[2];
        out[3] = four ? pixel[3] : 1.0f;
      }
    }
  }
  if (is_float) {
    /* A float buffer is premultiplied: straighten it before anything reads the colour. */
    for (int64_t i = 0; i < int64_t(tw) * th; i++) {
      const float alpha = dst[i * 4 + 3];
      if (alpha > 0.0f) {
        const float inv = 1.0f / alpha;
        dst[i * 4 + 0] *= inv;
        dst[i * 4 + 1] *= inv;
        dst[i * 4 + 2] *= inv;
      }
    }
  }
  /* The name is resolved by the acquisition path from the buffer it hands over; a stack built by
   * hand (a test) leaves it null, and the buffer's own colorspace is then the right answer. */
  const ColorSpace *colorspace = (colorspace_name != nullptr) ?
                                     IMB_colormanagement_space_get_named(colorspace_name) :
                                     nullptr;
  if (colorspace == nullptr) {
    colorspace = (ibuf->byte_buffer.data != nullptr) ? ibuf->byte_buffer.colorspace :
                                                       ibuf->float_buffer.colorspace;
  }
  if (BKE_paint_material_composite_colorspace_is_converted(colorspace)) {
    IMB_colormanagement_colorspace_to_scene_linear(dst, tw, th, 4, colorspace, false);
  }
}

/**
 * Blend one scene-linear pixel: the shader's own `ramp_blend` for every colour mode, and the
 * whiteout for #CompositeBlend::NormalCombine, which no Mix node expresses.
 *
 * \a top carries the row's coverage in its alpha: Mix mixes all four components like the node does,
 * the other modes keep the destination's alpha.
 * Nothing is clamped beyond the factor: the encode step is where a byte output is finally clamped.
 */
static void blend_row_linear(float dst[4],
                             const float top[4],
                             const CompositeBlend blend,
                             const float factor)
{
  const float fac = clamp_f(factor, 0.0f, 1.0f);
  if (blend == CompositeBlend::NormalCombine) {
    float combined[3];
    blend_normal_combine(dst, top, combined);
    for (const int i : IndexRange(3)) {
      dst[i] = dst[i] * (1.0f - fac) + combined[i] * fac;
    }
    return;
  }
  /* Every other mode is a Mix mode, and CompositeBlend holds the very `MA_RAMP_*` code the node
   * stores, so this is the same call the shader makes. */
  ramp_blend(int(blend), dst, fac, top);
}

/**
 * The Height record's own blend: the delta-space formula every height mode works in. The red
 * component is the height (the graph reads the same channel through a Separate Color), the
 * distance from the neutral level of the record is scaled by \a scale and the whole step is
 * weighted by \a factor exactly the way #blend_row_linear weights its modes:
 *
 *   d_below = below - neutral,  d_layer = (c - neutral) * scale
 *   Mix:       d_out = d_below * (1 - f) + d_layer * f
 *   Add:       d_out = d_below + f * d_layer
 *   Subtract:  d_out = d_below - f * d_layer
 * Nothing clamps: the encode step is where a byte output is finally clamped, and a float buffer
 * keeps the overshoot.
 *
 * The fourth component is not the height's: the generated chain writes only the scalar and the
 * row's content alpha is accumulated apart, by the channel's own blend (#blend_row_linear's Mix
 * rule). \a channel_is_mix carries that answer, so the alpha cannot start following the height
 * mode instead of the channel's blend.
 */
static void blend_row_height(float dst[4],
                             const float top[4],
                             const int16_t height_blend,
                             const float scale,
                             const float factor,
                             const bool channel_is_mix)
{
  const float fac = clamp_f(factor, 0.0f, 1.0f);
  const float out = height_blend_step(height_blend, dst[0], top[0], scale, fac);
  /* The height is a scalar the whole colour carries; the fourth component follows the Mix model
   * the ordinary blends read (a Mix row lays its alpha by its factor, the rest keeps what is
   * below), indexed by the channel's blend rather than by the height mode. */
  dst[0] = dst[1] = dst[2] = out;
  if (channel_is_mix) {
    dst[3] = dst[3] * (1.0f - fac) + top[3] * fac;
  }
}

float height_blend_step(const int height_blend,
                        const float below,
                        const float layer,
                        const float scale,
                        const float factor)
{
  const float fac = clamp_f(factor, 0.0f, 1.0f);
  const float d_below = below - PAINT_HEIGHT_NEUTRAL;
  const float d_layer = (layer - PAINT_HEIGHT_NEUTRAL) * scale;
  float d_out;
  switch (height_blend) {
    case MA_PAINT_LAYER_HEIGHT_BLEND_MIX:
      d_out = d_below * (1.0f - fac) + d_layer * fac;
      break;
    case MA_PAINT_LAYER_HEIGHT_BLEND_ADD:
      d_out = d_below + fac * d_layer;
      break;
    case MA_PAINT_LAYER_HEIGHT_BLEND_SUBTRACT:
      d_out = d_below - fac * d_layer;
      break;
    default:
      /* The caller picks the ordinary blend for an unset mode (#composite_row_blend); anything
       * else is a corrupt description, and a plain mix keeps the buffer sane rather than laying
       * garbage. */
      d_out = d_below * (1.0f - fac) + d_layer * fac;
      break;
  }
  return d_out + PAINT_HEIGHT_NEUTRAL;
}

/**
 * Lay \a top over \a dst by what the row describes: the Height record's own delta-space step when
 * it has a mode (#blend_row_height), the ordinary blend otherwise (#blend_row_linear). The one
 * place the choice is made, so the top-level stack and a folder's isolated accumulation cannot
 * pick differently for the same row.
 */
static void composite_row_blend(float dst[4],
                                const float top[4],
                                const CompositeBlend blend,
                                const int16_t height_blend,
                                const float height_scale,
                                const float factor)
{
  if (height_blend != MA_PAINT_LAYER_HEIGHT_BLEND_INHERIT) {
    blend_row_height(dst, top, height_blend, height_scale, factor, blend == CompositeBlend::Mix);
    return;
  }
  blend_row_linear(dst, top, blend, factor);
}

/**
 * Render one layer onto tile-sized buffers: its straight colour in \a r_color and the coverage it
 * blends by in \a r_factor, before the layer's own opacity.
 *
 * The coverage is the mask times the layer's own content coverage: a map's alpha for a painted
 * channel map, or full. A mask is decoded like any map, in one tile call.
 *
 * A folder has no map: \a folder_color is the straight colour of its contents and \a folder_coverage
 * the coverage they accumulated, which stand in for the map and for its alpha.
 */
/**
 * The result of filter \a filter on \a rgb at full strength, written back into it. The one place the
 * CPU knows the filters, for a correction (which then mixes it in by its opacity) and for an
 * adjustment (whose row lays it by its own factor). An unknown filter leaves the colour alone.
 */
static void composite_filter_color(const int16_t filter, const float *params, float rgb[3])
{
  switch (filter) {
    case MA_PAINT_LAYER_FILTER_INVERT:
      for (const int k : IndexRange(3)) {
        rgb[k] = fabsf(rgb[k] - 1.0f);
      }
      break;
    case MA_PAINT_LAYER_FILTER_HSL:
      paint_layer_hsl_apply(rgb, params);
      break;
    case MA_PAINT_LAYER_FILTER_LEVELS:
      for (const int k : IndexRange(3)) {
        rgb[k] = paint_layer_levels_apply(rgb[k], params);
      }
      break;
    default:
      break;
  }
}

float BKE_paint_material_mask_factor_eval(const Span<PaintMaterialCompositeMaskBuffer> items,
                                          const Span<const float *> item_samples,
                                          const Span<const float *> coverage_samples,
                                          const int64_t pixel_index,
                                          const PaintMaterialCompositeTexel &texel)
{
  float mask_factor = 1.0f;
  for (const int item_i : items.index_range()) {
    const PaintMaterialCompositeMaskBuffer &item = items[item_i];
    if (item.opacity <= 0.0f) {
      continue;
    }
    const float opacity = clamp_f(item.opacity, 0.0f, 1.0f);
    if (item.map_coverage != 0) {
      /* A map-driven coverage operator lays `match` from its own map: `M = M * lerp(1, match,
       * opacity)`, so opacity zero is a pass-through and a full match leaves M alone. A missing map
       * samples as `match = 0`, which is not a pass-through but a full restriction; the operator
       * takes part whenever its element is enabled. */
      float match = 0.0f;
      if (coverage_samples[item_i] != nullptr) {
        match = BKE_paint_material_coverage_match(item.map_coverage,
                                                  coverage_samples[item_i] + pixel_index * 4,
                                                  item.coverage_tolerance,
                                                  item.coverage_softness,
                                                  item.coverage_colors.as_span());
      }
      if (item.invert) {
        match = 1.0f - match;
      }
      /* White/Black carry one here; Gradient carries the item's standard value. */
      match *= item.coverage_output;
      mask_factor *= 1.0f + (match - 1.0f) * opacity;
      continue;
    }
    if (item.filter != 0) {
      /* A filter works on the factor so far as a grey, `M = lerp(M, f(M), opacity)`; its function is
       * the one a correction applies to a colour. */
      float grey[3] = {mask_factor, mask_factor, mask_factor};
      composite_filter_color(item.filter, item.filter_params, grey);
      mask_factor += (grey[0] - mask_factor) * opacity;
      continue;
    }
    const float *sample = item_samples[item_i];
    /* A Gradient has no map: its ramp is computed at the texel, so no canvas-sized buffer of it
     * exists. A texel outside the surface's islands samples as zero, which covers nothing. */
    float gradient_rgba[4];
    const float *c = nullptr;
    if (item.gradient.enabled) {
      BKE_paint_material_gradient_sample(item.gradient,
                                         texel.x,
                                         texel.y,
                                         texel.width > 0 ? texel.width : item.gradient.width,
                                         texel.height > 0 ? texel.height : item.gradient.height,
                                         gradient_rgba);
      c = gradient_rgba;
    }
    else if (sample != nullptr) {
      c = sample + pixel_index * 4;
    }
    float gray = clamp_f(item.constant_gray, 0.0f, 1.0f);
    float weight = opacity;
    if (c != nullptr) {
      gray = clamp_f((c[0] + c[1] + c[2]) / 3.0f, 0.0f, 1.0f);
      weight = clamp_f(c[3], 0.0f, 1.0f) * opacity;
    }
    /* The grey only: a transparent texel is still one the item does not cover. */
    if (item.invert) {
      gray = 1.0f - gray;
    }
    /* The factor is a grey colour to the blend, as it is in the graph's Mix node. */
    float value[4] = {mask_factor, mask_factor, mask_factor, mask_factor};
    const float top[4] = {gray, gray, gray, gray};
    blend_row_linear(value, top, item.blend, weight);
    mask_factor = value[0];
  }
  return mask_factor;
}

bool BKE_paint_material_gradient_sample(const PaintMaterialCompositeGradient &gradient,
                                        const int x,
                                        const int y,
                                        float r_rgba[4])
{
  return BKE_paint_material_gradient_sample(
      gradient, x, y, gradient.width, gradient.height, r_rgba);
}

bool BKE_paint_material_gradient_sample(const PaintMaterialCompositeGradient &gradient,
                                        const int x,
                                        const int y,
                                        const int grid_width,
                                        const int grid_height,
                                        float r_rgba[4])
{
  const int width = std::max(grid_width, 1);
  const int height = std::max(grid_height, 1);
  const float u = (float(x) + 0.5f) / float(width);
  const float v = (float(y) + 0.5f) / float(height);
  float3 coordinate;
  if (gradient.approximate || gradient.surface == nullptr) {
    /* No surface: the texel's own UV is the coordinate. A UV row always reads this way. */
    coordinate = float3(u, v, 0.0f);
  }
  else if (gradient.payload.space == MA_PAINT_LAYER_GRADIENT_SPACE_GENERATED) {
    /* Generated reads the texel's orco, which is not the object-space position once a deformer
     * moved it; #sample_generated prefers the stored CD_ORCO and only falls back to the bbox
     * normalization. */
    if (!gradient.surface->sample_generated(float2(u, v), coordinate)) {
      zero_v4(r_rgba);
      return false;
    }
  }
  else if (!gradient.surface->sample(float2(u, v), coordinate)) {
    zero_v4(r_rgba);
    return false;
  }
  const float t = BKE_paint_layers_gradient_factor(gradient.payload, coordinate);
  if (gradient.color_channel) {
    BKE_paint_layers_gradient_color(gradient.payload, t, r_rgba);
  }
  else if (gradient.scalar_ramp_valid) {
    /* The channel's grey value ramp: red carries the value, the alpha stays one. */
    float rgba[4];
    BKE_colorband_evaluate(&gradient.scalar_ramp, t, rgba);
    r_rgba[0] = r_rgba[1] = r_rgba[2] = rgba[0];
    r_rgba[3] = 1.0f;
  }
  else {
    const float value = gradient.scalar_start + (gradient.scalar_end - gradient.scalar_start) * t;
    r_rgba[0] = r_rgba[1] = r_rgba[2] = value;
    r_rgba[3] = 1.0f;
  }
  return true;
}

static void composite_layer_render(const PaintMaterialCompositeLayer &layer,
                                   const rcti &tile,
                                   const float *folder_color,
                                   const float *folder_coverage,
                                   float *r_color,
                                   float *r_factor)
{
  const int64_t count = int64_t(BLI_rcti_size_x(&tile)) * BLI_rcti_size_y(&tile);

  if (layer.is_folder) {
    BLI_assert(folder_color != nullptr && folder_coverage != nullptr);
    memcpy(r_color, folder_color, size_t(count) * 4 * sizeof(float));
  }
  else if (layer.adjustment_filter != 0) {
    /* The colour is the filtered stack below, which only the caller holds: it filled r_color. */
  }
  else if (layer.gradient.enabled) {
    /* A Gradient row has no map: each texel evaluates the same line and ramp the shader reads. The
     * fourth component carries the ramp's alpha for a colour channel and one for a scalar. */
    for (int ty = 0; ty < BLI_rcti_size_y(&tile); ty++) {
      for (int tx = 0; tx < BLI_rcti_size_x(&tile); tx++) {
        float *p = r_color + (int64_t(ty) * BLI_rcti_size_x(&tile) + tx) * 4;
        BKE_paint_material_gradient_sample(layer.gradient, tile.xmin + tx, tile.ymin + ty, p);
      }
    }
  }
  else if (layer.color_ibuf != nullptr) {
    composite_decode_tile(layer.color_ibuf, layer.color_colorspace_name, tile, r_color);
    if (layer.normal_directx) {
      for (int64_t i = 0; i < count; i++) {
        r_color[i * 4 + 1] = 1.0f - r_color[i * 4 + 1];
      }
    }
  }
  else {
    /* The stack was validated: a layer without a map is a constant. */
    BLI_assert(layer.has_constant_color);
    for (int64_t i = 0; i < count; i++) {
      copy_v4_v4(r_color + i * 4, layer.constant_color);
    }
  }

  Vector<float> mask_storage;
  const float *mask = nullptr;
  if (layer.mask_ibuf != nullptr) {
    mask_storage.resize(count * 4);
    composite_decode_tile(layer.mask_ibuf, layer.mask_colorspace_name, tile, mask_storage.data());
    mask = mask_storage.data();
  }

  /* The content coverage: what the row's own content covers, before the mask. A layer whose factor
   * is its own alpha, with no mask map to say otherwise, and a painted map both cover by the map's
   * alpha; a transparent texel shows what is below. */
  for (int64_t i = 0; i < count; i++) {
    const bool own_alpha = (layer.color_alpha_coverage && layer.color_ibuf != nullptr) ||
                           (mask == nullptr && layer.mask_from_alpha);
    float content = own_alpha ? clamp_f(r_color[i * 4 + 3], 0.0f, 1.0f) : 1.0f;
    if (layer.gradient.enabled) {
      /* A gradient covers by the alpha the evaluation left: the ramp's alpha for a colour channel,
       * one for a valid scalar texel and zero outside the UV islands. */
      content = clamp_f(r_color[i * 4 + 3], 0.0f, 1.0f);
    }
    else if (layer.is_folder) {
      /* A folder covers by what its contents covered, not by the alpha of the colour it reads. */
      content = clamp_f(folder_coverage[i], 0.0f, 1.0f);
    }
    else if (layer.empty_base) {
      /* An unpainted row kept for its corrections covers nothing until they bring coverage in. */
      content = 0.0f;
    }
    r_factor[i] = content;
  }

  /* An Anchor Point slice carries its coverage beside the colour: the content the slice covers, not
   * the alpha of its colour buffer (which is the content alpha). */
  Vector<float> anchor_coverage_storage;
  if (layer.anchor_point_coverage != nullptr) {
    anchor_coverage_storage.resize(count * 4);
    composite_decode_tile(
        layer.anchor_point_coverage.get(), nullptr, tile, anchor_coverage_storage.data());
    for (int64_t i = 0; i < count; i++) {
      const float *c = anchor_coverage_storage.data() + i * 4;
      r_factor[i] = clamp_f((c[0] + c[1] + c[2]) / 3.0f, 0.0f, 1.0f);
    }
  }

  /* The content corrections adjust the colour the row paints with, and the coverage it lays with.
   * With `a` the coverage so far and `f = opacity * alpha(map)`:
   *   c_eff = lerp(c, blend(S, c), a)   (where the row covers nothing, the correction is its colour)
   *   a'    = a + f * (1 - a)
   *   S'    = lerp(S, c_eff, f / a')
   * which is the plain blend by `f` where `a` is one, and what the generator builds node by node. */
  Vector<float> correction_storage;
  for (const PaintMaterialCompositeCorrectionBuffer &correction : layer.content_corrections) {
    if (correction.filter != 0) {
      /* A filter changes the colour and leaves the coverage and the content alpha as they are:
       * `c = lerp(c, f(c), opacity)`, the Mix the generator builds. */
      const float weight = clamp_f(correction.opacity, 0.0f, 1.0f);
      if (weight <= 0.0f) {
        continue;
      }
      for (int64_t i = 0; i < count; i++) {
        float *s = r_color + i * 4;
        float filtered[3] = {s[0], s[1], s[2]};
        composite_filter_color(correction.filter, correction.filter_params, filtered);
        for (const int k : IndexRange(3)) {
          s[k] += (filtered[k] - s[k]) * weight;
        }
      }
      continue;
    }
    if ((correction.ibuf == nullptr && !correction.has_constant_color &&
         correction.anchor_point_slice == nullptr && !correction.gradient.enabled) ||
        correction.opacity <= 0.0f)
    {
      continue;
    }
    Vector<float> anchor_slice_storage;
    Vector<float> anchor_coverage_tile;
    const float *anchor_slice = nullptr;
    const float *anchor_coverage = nullptr;
    if (correction.anchor_point_slice != nullptr) {
      anchor_slice_storage.resize(count * 4);
      composite_decode_tile(
          correction.anchor_point_slice, nullptr, tile, anchor_slice_storage.data());
      anchor_slice = anchor_slice_storage.data();
      if (correction.anchor_point_coverage != nullptr) {
        anchor_coverage_tile.resize(count * 4);
        composite_decode_tile(
            correction.anchor_point_coverage, nullptr, tile, anchor_coverage_tile.data());
        anchor_coverage = anchor_coverage_tile.data();
      }
    }
    if (correction.anchor_point_coverage_ibuf != nullptr) {
      anchor_coverage_tile.resize(count * 4);
      composite_decode_tile(correction.anchor_point_coverage_ibuf,
                            correction.anchor_point_coverage_colorspace_name,
                            tile,
                            anchor_coverage_tile.data());
      anchor_coverage = anchor_coverage_tile.data();
    }
    if (correction.ibuf != nullptr) {
      correction_storage.resize(count * 4);
      composite_decode_tile(
          correction.ibuf, correction.colorspace_name, tile, correction_storage.data());
    }
    const int tile_width = BLI_rcti_size_x(&tile);
    for (int64_t i = 0; i < count; i++) {
      /* A flat colour covers by its opacity alone: its alpha is content alpha, which a Fill row's
       * is too, and is no part of the factor. A Gradient covers by its ramp's alpha, as a row's
       * does. */
      float gradient_color[4];
      const float *c = nullptr;
      if (correction.gradient.enabled) {
        const int tx = int(i % tile_width);
        const int ty = int(i / tile_width);
        if (!BKE_paint_material_gradient_sample(
                correction.gradient, tile.xmin + tx, tile.ymin + ty, gradient_color))
        {
          /* Outside the UV islands the correction, like a row, lays nothing. */
          continue;
        }
        c = gradient_color;
      }
      else if (anchor_slice != nullptr) {
        c = anchor_slice + i * 4;
      }
      else {
        c = (correction.ibuf != nullptr) ? correction_storage.data() + i * 4 :
                                           correction.constant_color;
      }
      float *s = r_color + i * 4;
      float correction_factor = 1.0f;
      if (anchor_coverage != nullptr) {
        /* A reader's slice covers by the coverage it was rendered with, whether the colour comes
         * live or from its bake; the colour map's own alpha is content alpha, not coverage. */
        const float *cov = anchor_coverage + i * 4;
        correction_factor = (cov[0] + cov[1] + cov[2]) / 3.0f;
      }
      else if (correction.ibuf != nullptr || correction.gradient.enabled) {
        correction_factor = c[3];
      }
      const float f = clamp_f(correction.opacity * correction_factor, 0.0f, 1.0f);
      const float a = clamp_f(r_factor[i], 0.0f, 1.0f);

      float blended[4] = {s[0], s[1], s[2], s[3]};
      if (correction.blend == CompositeBlend::NormalCombine) {
        /* Where the row has no content its colour is the map's empty black, which decodes to a
         * tangent of `(-1, -1, -1)`: the flat normal stands in by the coverage, as in the graph. */
        blended[0] = 0.5f + (s[0] - 0.5f) * a;
        blended[1] = 0.5f + (s[1] - 0.5f) * a;
        blended[2] = 1.0f + (s[2] - 1.0f) * a;
      }
      blend_row_linear(blended, c, correction.blend, 1.0f);

      const float a_new = a + f * (1.0f - a);
      const float weight = (a_new > 0.0f) ? f / a_new : 0.0f;
      for (const int k : IndexRange(3)) {
        const float c_eff = c[k] + (blended[k] - c[k]) * a;
        s[k] += (c_eff - s[k]) * weight;
      }
      /* The fourth component follows the over model on its own where it is a content alpha. */
      const float alpha = clamp_f(s[3], 0.0f, 1.0f);
      s[3] = layer.tracks_content_alpha ? alpha + f * (1.0f - alpha) : a_new;
      r_factor[i] = a_new;
    }
  }

  /* The mask clips the layer, corrections included. */
  for (int64_t i = 0; i < count; i++) {
    float mask_value = 1.0f;
    if (mask != nullptr) {
      const float *m = mask + i * 4;
      const float value = layer.mask_from_alpha ? m[3] : (m[0] + m[1] + m[2]) / 3.0f;
      mask_value = (1.0f - layer.mask_influence) +
                   layer.mask_influence * clamp_f(value, 0.0f, 1.0f);
    }
    r_factor[i] *= mask_value;
  }

  /* The mask stack lays its coverage over a factor `M` that starts at one, item by item, the last
   * on top: `M = blend(M, C, A * op)`, the Mix the generator builds for the same item, so a
   * correction can bring back what the base hid but never conjure content the row does not have
   * (the row's coverage is scaled by `M` afterwards). The base is the Multiply item, which over one
   * is `1 - A * op * (1 - C)`. A transparent texel (A = 0) leaves the factor alone, which is what a
   * fresh mask map must do. */
  if (!layer.mask_items.is_empty()) {
    /* Decode every item's map once for the tile, then reduce per texel through the one arithmetic
     * the strokes share (#BKE_paint_material_mask_factor_eval), so the canvas a stroke paints and
     * the canvas the render shows cannot disagree about what a mask hides. */
    const int item_count = layer.mask_items.size();
    Vector<Vector<float>> item_storage(item_count);
    Vector<Vector<float>> coverage_storage(item_count);
    Vector<const float *> item_samples(item_count, nullptr);
    Vector<const float *> coverage_samples(item_count, nullptr);
    for (const int item_i : IndexRange(item_count)) {
      const PaintMaterialCompositeMaskBuffer &item = layer.mask_items[item_i];
      if (item.ibuf != nullptr) {
        item_storage[item_i].resize(count * 4);
        composite_decode_tile(item.ibuf, item.colorspace_name, tile, item_storage[item_i].data());
        item_samples[item_i] = item_storage[item_i].data();
      }
      if (item.coverage_ibuf != nullptr) {
        coverage_storage[item_i].resize(count * 4);
        composite_decode_tile(item.coverage_ibuf,
                              item.coverage_colorspace_name,
                              tile,
                              coverage_storage[item_i].data());
        coverage_samples[item_i] = coverage_storage[item_i].data();
      }
    }
    const int tile_width = BLI_rcti_size_x(&tile);
    for (int64_t i = 0; i < count; i++) {
      const PaintMaterialCompositeTexel texel{
          tile.xmin + int(i % tile_width), tile.ymin + int(i / tile_width)};
      r_factor[i] *= BKE_paint_material_mask_factor_eval(
          layer.mask_items.as_span(), item_samples, coverage_samples, i, texel);
    }
  }
}

/**
 * Whether a layer paints anything: it has a map, or a flat colour. The one rule behind both the
 * validation of a hand-built stack (which refuses an empty layer) and the building of a stack from
 * image layers (which leaves one out).
 */
template<typename LayerT>
static bool composite_layer_has_content(const LayerT &layer, const bool has_map)
{
  return has_map || layer.has_constant_color || layer.adjustment_filter != 0 ||
         layer.gradient.enabled;
}

static void composite_folder_accumulate(const PaintMaterialCompositeLayer &folder,
                                        const rcti &tile,
                                        float *r_straight,
                                        float *r_coverage);

/**
 * Render \a layer to tile-sized buffers: its straight colour and the coverage it blends by, before
 * its own opacity. A folder is rendered through its isolated contents.
 */
static void composite_layer_apply(const PaintMaterialCompositeLayer &layer,
                                  const rcti &tile,
                                  float *r_color,
                                  float *r_factor)
{
  if (!layer.is_folder) {
    composite_layer_render(layer, tile, nullptr, nullptr, r_color, r_factor);
    return;
  }
  const int64_t count = int64_t(BLI_rcti_size_x(&tile)) * BLI_rcti_size_y(&tile);
  Vector<float> sub_color(count * 4);
  Vector<float> sub_coverage(count);
  composite_folder_accumulate(layer, tile, sub_color.data(), sub_coverage.data());
  composite_layer_render(
      layer, tile, sub_color.data(), sub_coverage.data(), r_color, r_factor);
}

/**
 * Accumulate a folder's contents in isolation: the pre-multiplied colour `P` and the coverage `a`
 * start at zero, and each child lays itself over them. With `S = P / a` the straight colour so far
 * and `f` the child's factor:
 *   c_eff = lerp(c, blend(S, c), a)   (where nothing is below yet a child is its own colour)
 *   P'    = lerp(P, c_eff, f)
 *   a'    = a + f * (1 - a)
 * The result is `S` and `a`, which the folder row then reads like a map and its alpha. The
 * generator builds the same nodes per row, so the two cannot disagree.
 */
static void composite_folder_accumulate(const PaintMaterialCompositeLayer &folder,
                                        const rcti &tile,
                                        float *r_straight,
                                        float *r_coverage)
{
  const int64_t count = int64_t(BLI_rcti_size_x(&tile)) * BLI_rcti_size_y(&tile);

  Vector<float> premul(count * 4, 0.0f);
  Vector<float> coverage(count, 0.0f);
  Vector<float> child_color(count * 4);
  Vector<float> child_factor(count);

  for (const PaintMaterialCompositeLayer &child : folder.children) {
    if (!child.enabled) {
      continue;
    }
    composite_layer_apply(child, tile, child_color.data(), child_factor.data());

    for (int64_t i = 0; i < count; i++) {
      const float f = clamp_f(child.opacity * child_factor[i], 0.0f, 1.0f);
      const float a = coverage[i];
      float straight[4] = {0.0f, 0.0f, 0.0f, 0.0f};
      if (a > 0.0f) {
        for (const int k : IndexRange(4)) {
          straight[k] = premul[i * 4 + k] / a;
        }
      }
      /* The row's blend at full factor, then the mix towards its own colour by what is there. */
      float blended[4] = {straight[0], straight[1], straight[2], straight[3]};
      composite_row_blend(blended,
                          child_color.data() + i * 4,
                          child.blend,
                          child.height_blend,
                          child.height_scale,
                          1.0f);
      const float *c = child_color.data() + i * 4;
      for (const int k : IndexRange(4)) {
        const float c_eff = c[k] + (blended[k] - c[k]) * a;
        premul[i * 4 + k] = premul[i * 4 + k] * (1.0f - f) + c_eff * f;
      }
      coverage[i] = a + f * (1.0f - a);
    }
  }

  for (int64_t i = 0; i < count; i++) {
    const float a = coverage[i];
    if (a > 0.0f) {
      for (const int k : IndexRange(4)) {
        r_straight[i * 4 + k] = premul[i * 4 + k] / a;
      }
    }
    else {
      zero_v4(r_straight + i * 4);
    }
    r_coverage[i] = a;
  }
}

static bool composite_layers_validate_linear(Span<PaintMaterialCompositeLayer> layers,
                                             int width,
                                             int height);

/** Whether the buffers of \a layer -- and, for a folder, of everything it holds -- are usable. */
static bool composite_layer_validate_linear(const PaintMaterialCompositeLayer &layer,
                                            const int width,
                                            const int height)
{
  if (layer.is_folder) {
    /* A folder carries no buffers of its own; its contents are what has to be usable. Validated in
     * place: a stack built from copies would deep-copy every nested layer per call. */
    return composite_layers_validate_linear(
        Span<PaintMaterialCompositeLayer>(layer.children.data(), int64_t(layer.children.size())),
        width,
        height);
  }
  if (layer.gradient.enabled && (layer.gradient.width <= 0 || layer.gradient.height <= 0)) {
    /* Without a map the size has to come from the description; a zero would read every texel at
     * the same UV. */
    return false;
  }
  if (layer.color_ibuf != nullptr) {
    if (!composite_ibuf_is_rgba(layer.color_ibuf, width, height)) {
      return false;
    }
  }
  else if (!composite_layer_has_content(layer, false)) {
    return false;
  }
  if (layer.anchor_point_coverage != nullptr &&
      !composite_ibuf_is_rgba(layer.anchor_point_coverage.get(), width, height))
  {
    return false;
  }
  if (layer.mask_ibuf != nullptr &&
      !composite_mask_ibuf_is_valid(layer.mask_ibuf, width, height))
  {
    return false;
  }
  for (const PaintMaterialCompositeMaskBuffer &item : layer.mask_items) {
    if (item.ibuf != nullptr && !composite_mask_ibuf_is_valid(item.ibuf, width, height)) {
      return false;
    }
    /* A coverage operator's own map: absent is fine (it samples as `match = 0`), present has to be
     * a readable buffer of the stack's dimensions like any mask. */
    if (item.coverage_ibuf != nullptr &&
        !composite_mask_ibuf_is_valid(item.coverage_ibuf, width, height))
    {
      return false;
    }
    if (item.anchor_point_slice != nullptr &&
        !composite_mask_ibuf_is_valid(item.anchor_point_slice, width, height))
    {
      return false;
    }
  }
  for (const PaintMaterialCompositeCorrectionBuffer &correction : layer.content_corrections) {
    if (correction.ibuf != nullptr && !composite_ibuf_is_rgba(correction.ibuf, width, height)) {
      return false;
    }
    if (correction.anchor_point_slice != nullptr &&
        !composite_ibuf_is_rgba(correction.anchor_point_slice, width, height))
    {
      return false;
    }
    if (correction.anchor_point_coverage != nullptr &&
        !composite_ibuf_is_rgba(correction.anchor_point_coverage, width, height))
    {
      return false;
    }
    /* A baked reader's coverage map is read tile by tile like the colour map: a bake of another
     * size or format would be read past its end. */
    if (correction.anchor_point_coverage_ibuf != nullptr &&
        !composite_ibuf_is_rgba(correction.anchor_point_coverage_ibuf, width, height))
    {
      return false;
    }
  }
  return true;
}

static bool composite_layers_validate_linear(const Span<PaintMaterialCompositeLayer> layers,
                                             const int width,
                                             const int height)
{
  bool any_enabled = false;
  for (const PaintMaterialCompositeLayer &layer : layers) {
    if (!layer.enabled) {
      continue;
    }
    if (!composite_layer_validate_linear(layer, width, height)) {
      return false;
    }
    any_enabled = true;
  }
  return any_enabled;
}

static bool composite_stack_validate_linear(const PaintMaterialCompositeStack &stack)
{
  if (stack.width <= 0 || stack.height <= 0 || stack.layers.is_empty()) {
    return false;
  }
  return composite_layers_validate_linear(stack.layers, stack.width, stack.height);
}

/** One 256x256 tile: the scratch a layer is decoded and mixed in, never a full-size copy. */
static constexpr int COMPOSITE_TILE_SIZE = 256;

PaintMaterialCompositeLinearTarget PaintMaterialCompositeLinearTarget::frame(
    const MutableSpan<float> storage, const int width)
{
  PaintMaterialCompositeLinearTarget target;
  target.pixels = storage;
  target.row_stride = int64_t(width) * 4;
  return target;
}

PaintMaterialCompositeLinearTarget PaintMaterialCompositeLinearTarget::packed(
    const MutableSpan<float> storage, const rcti &area)
{
  PaintMaterialCompositeLinearTarget target;
  target.pixels = storage;
  target.row_stride = int64_t(BLI_rcti_size_x(&area)) * 4;
  return target;
}

PaintMaterialCompositeLinearTarget PaintMaterialCompositeLinearTarget::in_frame(
    const MutableSpan<float> storage, const int width, const rcti &area)
{
  PaintMaterialCompositeLinearTarget target;
  const int64_t offset = (int64_t(area.ymin) * width + area.xmin) * 4;
  if (offset <= storage.size()) {
    target.pixels = storage.drop_front(offset);
  }
  target.row_stride = int64_t(width) * 4;
  return target;
}

bool BKE_paint_material_composite_eval_linear(const PaintMaterialCompositeStack &stack,
                                              const PaintMaterialCompositeLinearTarget &target,
                                              const rcti *region,
                                              PaintMaterialCompositeEvalStats *r_stats)
{
  if (!composite_stack_validate_linear(stack)) {
    return false;
  }
  rcti area;
  if (!BKE_paint_material_composite_area_get(stack.width, stack.height, region, area)) {
    /* Nothing of the tagged region is inside the buffer; the composite is already correct. */
    if (r_stats != nullptr) {
      *r_stats = {};
    }
    return true;
  }

  /* Refused, never written out of bounds: a target that does not describe the area would
   * otherwise be a silent overrun. */
  const int64_t area_width = BLI_rcti_size_x(&area);
  const int64_t area_height = BLI_rcti_size_y(&area);
  if (target.row_stride < area_width * 4 ||
      target.pixels.size() < (area_height - 1) * target.row_stride + area_width * 4)
  {
    return false;
  }

  const double start_time = BLI_time_now_seconds();
  struct TileScratch {
    Vector<float> tile_storage;
    Vector<float> color_storage;
    Vector<float> factor_storage;
  };
  threading::EnumerableThreadSpecific<TileScratch> scratch_per_thread;
  std::atomic<int> layers_evaluated = 0;

  /* Tiles share nothing: each reads the layers' buffers and writes its own part of the target, so
   * they run in parallel. One tile is already enough work to be a task of its own. */
  const int tiles_x = (int(area_width) + COMPOSITE_TILE_SIZE - 1) / COMPOSITE_TILE_SIZE;
  const int tiles_y = (int(area_height) + COMPOSITE_TILE_SIZE - 1) / COMPOSITE_TILE_SIZE;
  threading::parallel_for(IndexRange(int64_t(tiles_x) * tiles_y), 1, [&](const IndexRange range) {
    TileScratch &scratch = scratch_per_thread.local();
    Vector<float> &tile_storage = scratch.tile_storage;
    Vector<float> &color_storage = scratch.color_storage;
    Vector<float> &factor_storage = scratch.factor_storage;

    for (const int64_t tile_index : range) {
      const int tile_x = area.xmin + int(tile_index % tiles_x) * COMPOSITE_TILE_SIZE;
      const int tile_y = area.ymin + int(tile_index / tiles_x) * COMPOSITE_TILE_SIZE;
      rcti tile;
      BLI_rcti_init(&tile,
                    tile_x,
                    min_ii(tile_x + COMPOSITE_TILE_SIZE, area.xmax),
                    tile_y,
                    min_ii(tile_y + COMPOSITE_TILE_SIZE, area.ymax));
      const int tw = BLI_rcti_size_x(&tile);
      const int th = BLI_rcti_size_y(&tile);
      const int64_t count = int64_t(tw) * th;
      tile_storage.resize(count * 4);
      color_storage.resize(count * 4);
      factor_storage.resize(count);
      float *tile_dst = tile_storage.data();

      /* The tile starts from the shared bottom colour and every layer, the lowest included, is
       * blended over it like any other. */
      for (int64_t i = 0; i < count; i++) {
        if (stack.has_bottom_color) {
          copy_v4_v4(tile_dst + i * 4, stack.bottom_color);
        }
        else {
          zero_v4(tile_dst + i * 4);
        }
      }

      int tile_layers_evaluated = 0;
      for (const PaintMaterialCompositeLayer &layer : stack.layers) {
        /* A layer that blends by nothing leaves the tile as it is; do not decode its buffers. */
        if (!layer.enabled || layer.opacity <= 0.0f) {
          continue;
        }
        if (layer.adjustment_filter != 0) {
          /* The adjustment's colour is the stack so far, filtered; its row then lays it as any
           * other, by its blend, opacity and mask. */
          for (int64_t i = 0; i < count; i++) {
            float *c = color_storage.data() + i * 4;
            copy_v4_v4(c, tile_dst + i * 4);
            composite_filter_color(layer.adjustment_filter, layer.adjustment_params, c);
          }
        }
        composite_layer_apply(layer, tile, color_storage.data(), factor_storage.data());
        for (int64_t i = 0; i < count; i++) {
          composite_row_blend(tile_dst + i * 4,
                              color_storage.data() + i * 4,
                              layer.blend,
                              layer.height_blend,
                              layer.height_scale,
                              layer.opacity * factor_storage[i]);
        }
        tile_layers_evaluated++;
      }
      layers_evaluated += tile_layers_evaluated;

      /* Copy the tile back into the caller's buffer; only the region is ever touched. */
      for (int row = 0; row < th; row++) {
        float *dst_row = target.pixels.data() +
                         int64_t(tile.ymin - area.ymin + row) * target.row_stride +
                         int64_t(tile.xmin - area.xmin) * 4;
        memcpy(dst_row, tile_dst + int64_t(row) * tw * 4, size_t(tw) * 4 * sizeof(float));
      }
    }
  });

  if (r_stats != nullptr) {
    r_stats->elapsed_seconds = BLI_time_now_seconds() - start_time;
    r_stats->layers_evaluated = layers_evaluated.load();
    r_stats->pixels_processed = int64_t(BLI_rcti_size_x(&area)) * BLI_rcti_size_y(&area);
  }
  return true;
}

bool BKE_paint_material_composite_colorspace_is_converted(const ColorSpace *colorspace)
{
  return colorspace != nullptr && !IMB_colormanagement_space_is_data(colorspace) &&
         !IMB_colormanagement_space_is_scene_linear(colorspace);
}

void BKE_paint_material_composite_linear_to_byte(MutableSpan<float> linear,
                                                 const rcti &area,
                                                 ImBuf &r_dst)
{
  /* The color encoding the byte output is written in: the buffer's own colorspace, so the preview
   * and a saved map are the same pixels. Null (a plain ImBuf) leaves the scene-linear values as
   * they are, which is what a test comparing one raw pixel wants. */
  const char *colorspace_name = composite_buffer_colorspace_name(&r_dst);
  const ColorSpace *colorspace = (colorspace_name != nullptr) ?
                                     IMB_colormanagement_space_get_named(colorspace_name) :
                                     nullptr;
  const bool encode = BKE_paint_material_composite_colorspace_is_converted(colorspace);

  const int64_t area_width = BLI_rcti_size_x(&area);
  uchar *pixels = r_dst.byte_data_for_write();
  for (int y = area.ymin; y < area.ymax; y++) {
    float *row = linear.data() + int64_t(y - area.ymin) * area_width * 4;
    if (encode) {
      IMB_colormanagement_scene_linear_to_colorspace(row, int(area_width), 1, 4, colorspace);
    }
    uchar *out = pixels + (int64_t(y) * r_dst.x + area.xmin) * 4;
    for (int64_t i = 0; i < area_width * 4; i++) {
      out[i] = uchar(clamp_i(int(row[i] * 255.0f + 0.5f), 0, 255));
    }
  }
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
    if (entry.image == nullptr) {
      /* An owned buffer (a resample), not an acquisition. */
      IMB_freeImBuf(entry.ibuf);
    }
    else {
      BKE_image_release_ibuf(entry.image, entry.ibuf, entry.lock);
    }
  }
}

/** Hand \a ibuf, a buffer this evaluation made, to \a r_locks to be freed with the acquisitions. */
static void composite_image_own(ImBuf *ibuf, Vector<CompositeImageLock> &r_locks)
{
  CompositeImageLock entry;
  entry.ibuf = ibuf;
  r_locks.append(entry);
}

/**
 * The canvas-UV transform of a row's mapping, the one both CPU passes (the resample of a map and
 * the clip of a Material row's coverage) read through, so they cannot disagree on a texel: rotate
 * around Z, then scale along the canvas axes, then offset. An axis that repeats wraps with floor;
 * one that clips is range-tested.
 */
struct CompositeMappingTransform {
  float scale_x;
  float scale_y;
  float cos_rot;
  float sin_rot;
  float offset_u;
  float offset_v;
  bool repeat_u = true;
  bool repeat_v = true;

  explicit CompositeMappingTransform(const PaintMaterialCompositeMapping &mapping)
      : scale_x(BKE_paint_layers_mapping_scale_normalize(mapping.scale[0])),
        scale_y(BKE_paint_layers_mapping_scale_normalize(mapping.scale[1])),
        cos_rot(cosf(mapping.rotation)),
        sin_rot(sinf(mapping.rotation)),
        offset_u(mapping.offset[0]),
        offset_v(mapping.offset[1])
  {
    BKE_paint_layers_mapping_wrap_axes(mapping.wrap, repeat_u, repeat_v);
  }

  /** Whether any axis clips, so a texel can fall outside the unit square. */
  bool clips() const
  {
    return !(repeat_u && repeat_v);
  }

  /** The mapped coordinate of the canvas point (\a u, \a v). \return false outside the square on
   * an axis that clips. Only a clipping axis is range-tested: a wrapped one can round to 1. */
  bool apply(const float u, const float v, float &r_u, float &r_v) const
  {
    const float ru = u * cos_rot - v * sin_rot;
    const float rv = u * sin_rot + v * cos_rot;
    r_u = ru * scale_x + offset_u;
    r_v = rv * scale_y + offset_v;
    if (repeat_u) {
      r_u -= floorf(r_u);
    }
    if (repeat_v) {
      r_v -= floorf(r_v);
    }
    return !((!repeat_u && (r_u < 0.0f || r_u >= 1.0f)) ||
             (!repeat_v && (r_v < 0.0f || r_v >= 1.0f)));
  }
};

/**
 * Resample \a src onto a \a ref_width x \a ref_height grid through a row's UV mapping, as a float
 * RGBA premultiplied scene-linear ImBuf the caller owns.
 *
 * Each reference texel centre `((x + 0.5) / W, (y + 0.5) / H)` is transformed exactly like the
 * generated Mapping node chain does -- rotate around Z, then scale, then offset -- and the source
 * is read bilinearly at `u * W_src - 0.5` with Repeat edges, the texel-centre convention of the
 * Image Texture node. The taps are decoded to linear and filtered premultiplied, the way that node
 * filters before it un-premultiplies; the evaluator straightens the result on read, like every
 * float map. The grey a mask reduces (mean) is therefore read after the mapping.
 */
static ImBuf *composite_resample_mapped(const ImBuf *src,
                                        const int ref_width,
                                        const int ref_height,
                                        const PaintMaterialCompositeMapping &mapping)
{
  if (src == nullptr || ref_width <= 0 || ref_height <= 0 || src->x <= 0 || src->y <= 0) {
    return nullptr;
  }
  bke::paint_perf::ScopedTimer perf_timer("composite_resample_mapped");
  perf_timer.note("src=%dx%d -> %dx%d wrap=%d", src->x, src->y, ref_width, ref_height, mapping.wrap);
  const bool src_is_float = src->byte_buffer.data == nullptr && src->float_buffer.data != nullptr;
  if (!src_is_float && src->byte_buffer.data == nullptr) {
    return nullptr;
  }
  const int src_channels = src->channels == 0 ? 4 : src->channels;
  ImBuf *dst = IMB_allocImBuf(uint(ref_width), uint(ref_height), ImBufFlags::FloatData);
  if (dst == nullptr || dst->float_data() == nullptr) {
    if (dst != nullptr) {
      IMB_freeImBuf(dst);
    }
    return nullptr;
  }
  dst->channels = 4;
  float *out = dst->float_data_for_write();
  /* The buffer's own colorspace, like the evaluator reads: a data buffer filters as stored. */
  const ColorSpace *colorspace = src_is_float ? src->float_buffer.colorspace :
                                                src->byte_buffer.colorspace;
  const bool decode = BKE_paint_material_composite_colorspace_is_converted(colorspace);
  /* A byte sRGB map is decoded by a table: the colour-management call per tap costs about as much
   * as everything else a tap does together, and a 4K map has 67 million of them. Any other
   * converted space keeps the call. */
  static const std::array<float, 256> srgb_byte_to_linear = []() {
    std::array<float, 256> table;
    for (const int i : IndexRange(256)) {
      table[i] = srgb_to_linearrgb(float(i) * (1.0f / 255.0f));
    }
    return table;
  }();
  const bool decode_by_table = decode && !src_is_float && colorspace != nullptr &&
                               IMB_colormanagement_space_name_is_srgb(
                                   IMB_colormanagement_colorspace_get_name(colorspace));
  const CompositeMappingTransform transform(mapping);
  /* The direction remap of a tangent normal below needs the parts of the transform. */
  const float scale_x = transform.scale_x;
  const float scale_y = transform.scale_y;
  const float cos_rot = transform.cos_rot;
  const float sin_rot = transform.sin_rot;
  /* One straight-linear tap at texel (tx, ty), re-premultiplied for the filter below: byte buffers
   * are straight-stored, float buffers premultiplied-stored, the convention the tile decode
   * assumes. */
  auto tap_straight = [&](const int tx, const int ty, float r_tap[4]) {
    if (!src_is_float) {
      const uchar *pixel = src->byte_data() + (int64_t(ty) * src->x + tx) * 4;
      if (decode_by_table) {
        r_tap[0] = srgb_byte_to_linear[pixel[0]];
        r_tap[1] = srgb_byte_to_linear[pixel[1]];
        r_tap[2] = srgb_byte_to_linear[pixel[2]];
      }
      else {
        r_tap[0] = float(pixel[0]) / 255.0f;
        r_tap[1] = float(pixel[1]) / 255.0f;
        r_tap[2] = float(pixel[2]) / 255.0f;
      }
      r_tap[3] = float(pixel[3]) / 255.0f;
    }
    else {
      const float *pixel = src->float_buffer.data + (int64_t(ty) * src->x + tx) * src_channels;
      r_tap[0] = pixel[0];
      /* A one-channel float buffer is a grey, the convention the tile decode reads it by: the
       * resample has to keep that, or a mask resampled onto another grid would read a third of
       * its value. */
      r_tap[1] = src_channels == 1 ? pixel[0] : (src_channels > 1 ? pixel[1] : 0.0f);
      r_tap[2] = src_channels == 1 ? pixel[0] : (src_channels > 2 ? pixel[2] : 0.0f);
      r_tap[3] = src_channels == 4 ? pixel[3] : 1.0f;
      if (r_tap[3] > 0.0f) {
        const float inv = 1.0f / r_tap[3];
        r_tap[0] *= inv;
        r_tap[1] *= inv;
        r_tap[2] *= inv;
      }
    }
    if (decode && !decode_by_table) {
      IMB_colormanagement_colorspace_to_scene_linear_v4(r_tap, false, colorspace);
    }
    r_tap[0] *= r_tap[3];
    r_tap[1] *= r_tap[3];
    r_tap[2] *= r_tap[3];
  };
  auto wrap_texel = [](const int i, const int n) {
    const int r = i % n;
    return r < 0 ? r + n : r;
  };
  const auto resample_row = [&](const int y) {
    const float v = (float(y) + 0.5f) / float(ref_height);
    for (int x = 0; x < ref_width; x++) {
      const float u = (float(x) + 0.5f) / float(ref_width);
      /* Rotate first, then scale along the canvas axes: the transform the gizmo's rectangle shows,
       * which keeps a non-uniform scale from shearing the pattern. An axis that clips reads
       * transparent outside [0, 1); the row covers by the map's own alpha, so the texel leaves the
       * coverage there, which is what the generated graph's clip factor does to the map's Alpha. */
      float uu;
      float vv;
      const bool inside = transform.apply(u, v, uu, vv);
      float *p = out + (int64_t(y) * ref_width + x) * 4;
      if (!inside) {
        p[0] = p[1] = p[2] = p[3] = 0.0f;
        continue;
      }
      const float su = uu * float(src->x) - 0.5f;
      const float sv = vv * float(src->y) - 0.5f;
      const int x0 = int(floorf(su));
      const int y0 = int(floorf(sv));
      const float a = su - float(x0);
      const float b = sv - float(y0);
      float t00[4], t10[4], t01[4], t11[4];
      tap_straight(wrap_texel(x0, src->x), wrap_texel(y0, src->y), t00);
      tap_straight(wrap_texel(x0 + 1, src->x), wrap_texel(y0, src->y), t10);
      tap_straight(wrap_texel(x0, src->x), wrap_texel(y0 + 1, src->y), t01);
      tap_straight(wrap_texel(x0 + 1, src->x), wrap_texel(y0 + 1, src->y), t11);
      for (const int k : IndexRange(4)) {
        p[k] = (1.0f - a) * (1.0f - b) * t00[k] + a * (1.0f - b) * t10[k] +
               (1.0f - a) * b * t01[k] + a * b * t11[k];
      }
      if (mapping.tangent_normal && p[3] > 0.0f) {
        /* The mapping moved only the read point; the tangent-space vectors follow it. `R^T * S`
         * (flipped by the scale's sign, then rotated back) is the covariant counterpart of the
         * UV transform `S * R`; the magnitude of the scale does not turn a direction.
         * Un-premultiply first so the alpha does not skew the decode. */
        const float inv_alpha = 1.0f / p[3];
        const float nx = 2.0f * p[0] * inv_alpha - 1.0f;
        const float ny = 2.0f * p[1] * inv_alpha - 1.0f;
        const float nz = 2.0f * p[2] * inv_alpha - 1.0f;
        const float rx = scale_x < 0.0f ? -1.0f : 1.0f;
        const float ry = scale_y < 0.0f ? -1.0f : 1.0f;
        const float fx = rx * nx;
        const float fy = ry * ny;
        p[0] = (0.5f * (cos_rot * fx + sin_rot * fy) + 0.5f) * p[3];
        p[1] = (0.5f * (-sin_rot * fx + cos_rot * fy) + 0.5f) * p[3];
        p[2] = (0.5f * nz + 0.5f) * p[3];
      }
    }
  };
  /* The source is only read and every row writes its own output texels, so rows run unsynchronized;
   * a slider drag rebuilds the composite, and this is the cost of each step. */
  threading::parallel_for(IndexRange(ref_height), 8, [&](const IndexRange rows) {
    for (const int y : rows) {
      resample_row(y);
    }
  });
  /* Premultiplied scene-linear, like every float map the evaluator straightens on read. A fresh
   * ImBuf's float colorspace is null, so it is assigned rather than written through. */
  IMB_colormanagement_assign_float_colorspace(
      dst, IMB_colormanagement_role_colorspace_name_get(COLOR_ROLE_SCENE_LINEAR));
  return dst;
}

/**
 * Replace an acquired map buffer by its UV-mapped resample when the row remaps it. A null resample
 * keeps the acquired buffer, so a mapping never fails the stack, it just reads unmapped. One owned
 * buffer per mapped map, reused for every pixel and tile of the evaluation; nothing is allocated
 * per pixel.
 */
static void composite_mapped_remap(ImBuf *&r_ibuf,
                                   const PaintMaterialCompositeMapping &mapping,
                                   const int ref_width,
                                   const int ref_height,
                                   Vector<CompositeImageLock> &r_locks)
{
  if (!mapping.enabled || r_ibuf == nullptr) {
    return;
  }
  if (ImBuf *mapped = composite_resample_mapped(r_ibuf, ref_width, ref_height, mapping)) {
    /* The source buffer stays locked; the owned resample is what the evaluator reads. */
    composite_image_own(mapped, r_locks);
    r_ibuf = mapped;
  }
}

ImBuf *BKE_paint_material_mask_map_acquire(Image *image,
                                           const int tile_number,
                                           const PaintMaterialCompositeMapping *mapping,
                                           const int ref_width,
                                           const int ref_height)
{
  if (image == nullptr) {
    return nullptr;
  }
  /* The UDIM tile is what a mask map is sampled by, so the acquisition reads the same tile the
   * painted map of the stroke does. */
  ImageUser iuser;
  BKE_imageuser_default(&iuser);
  iuser.tile = tile_number;
  void *lock = nullptr;
  ImBuf *src = BKE_image_acquire_ibuf(image, &iuser, &lock);
  if (src == nullptr) {
    return nullptr;
  }

  /* A mapping resample first, exactly as the composite acquires a mapped item; the decode below
   * then straightens whatever buffer the mapping left, the same way the evaluator reads it. */
  const ImBuf *read = src;
  ImBuf *mapped = nullptr;
  if (mapping != nullptr && mapping->enabled) {
    mapped = composite_resample_mapped(src, ref_width, ref_height, *mapping);
    if (mapped != nullptr) {
      read = mapped;
    }
  }

  ImBuf *out = nullptr;
  if (read->byte_buffer.data != nullptr || read->float_buffer.data != nullptr) {
    out = IMB_allocImBuf(uint(read->x), uint(read->y), ImBufFlags::FloatData);
    if (out != nullptr && out->float_data() != nullptr) {
      out->channels = 4;
      rcti whole;
      BLI_rcti_init(&whole, 0, read->x, 0, read->y);
      composite_decode_tile(
          read, composite_buffer_colorspace_name(read), whole, out->float_data_for_write());
    }
    else if (out != nullptr) {
      IMB_freeImBuf(out);
      out = nullptr;
    }
  }

  if (mapped != nullptr) {
    IMB_freeImBuf(mapped);
  }
  BKE_image_release_ibuf(image, src, lock);
  return out;
}

/**
 * Fold a clipping UV wrap into a row's coverage.
 *
 * A map texel outside the unit square after the mapping is transparent, and a Fill or painted row
 * drops it from the coverage through the map's own alpha. A Material row covers by its source's
 * transparency instead, which the map's alpha does not carry, so the clip has to be multiplied into
 * the coverage on its own -- the 0/1 factor the generated graph multiplies in.
 *
 * The row's existing coverage factor at each texel is kept inside the square and zeroed outside:
 * the already mapped coverage map when there is one (read grey or alpha, scaled by the row's mask
 * influence), one when the row covers fully, and one when it has no map at all. The result is one
 * owned buffer read as a grey with full influence, which the evaluator multiplies exactly like any
 * coverage map. The row's mask items still apply on top, in their usual order.
 *
 * The transform is the one #composite_resample_mapped applies: rotate around Z first, then scale,
 * then offset; an axis that repeats wraps with floor, a clipping one range-tests.
 */
static void composite_clip_coverage_fold(PaintMaterialCompositeLayer &r_layer,
                                         const PaintMaterialCompositeMapping &mapping,
                                         const int ref_width,
                                         const int ref_height,
                                         Vector<CompositeImageLock> &r_locks)
{
  const CompositeMappingTransform transform(mapping);
  if (ref_width <= 0 || ref_height <= 0 || !transform.clips()) {
    return;
  }
  const bool has_map = r_layer.mask_ibuf != nullptr;
  if (has_map && !composite_mask_ibuf_is_valid(r_layer.mask_ibuf, ref_width, ref_height)) {
    /* A coverage of another size is rejected later with the rest; do not guess its texels. */
    return;
  }
  /* One float per texel: the coverage is a grey, and the decode reads a one-channel buffer as one.
   * At 4K this is 64 MB where four channels would hold 256 MB. Every texel is written below. */
  ImBuf *clip = IMB_allocImBuf(uint(ref_width), uint(ref_height), ImBufFlags::Zero);
  if (clip == nullptr) {
    return;
  }
  if (!IMB_alloc_float_pixels(clip, 1, false) || clip->float_data() == nullptr) {
    IMB_freeImBuf(clip);
    return;
  }
  float *out = clip->float_data_for_write();
  const PaintMaterialCompositeLayer &layer = r_layer;
  /* Strips of rows, each decoding its own piece of the coverage so far the way the evaluator
   * reads it (a float buffer is straightened here like it is there): the inside of the square is
   * unchanged, and no second full-size buffer is held. */
  threading::parallel_for(IndexRange(ref_height), 16, [&](const IndexRange rows) {
    Vector<float> strip;
    if (has_map) {
      strip.resize(int64_t(ref_width) * rows.size() * 4);
      rcti tile;
      BLI_rcti_init(&tile, 0, ref_width, int(rows.first()), int(rows.one_after_last()));
      composite_decode_tile(layer.mask_ibuf, layer.mask_colorspace_name, tile, strip.data());
    }
    for (const int64_t y : rows) {
      const float v = (float(y) + 0.5f) / float(ref_height);
      for (int x = 0; x < ref_width; x++) {
        const float u = (float(x) + 0.5f) / float(ref_width);
        float uu;
        float vv;
        const bool inside = transform.apply(u, v, uu, vv);
        float value = 0.0f;
        if (inside) {
          value = 1.0f;
          if (has_map) {
            const float *m = strip.data() + ((y - rows.first()) * ref_width + x) * 4;
            const float raw = layer.mask_from_alpha ? m[3] : (m[0] + m[1] + m[2]) / 3.0f;
            value = (1.0f - layer.mask_influence) + layer.mask_influence * clamp_f(raw, 0.0f, 1.0f);
          }
        }
        out[y * ref_width + x] = value;
      }
    }
  });
  IMB_colormanagement_assign_float_colorspace(
      clip, IMB_colormanagement_role_colorspace_name_get(COLOR_ROLE_SCENE_LINEAR));
  composite_image_own(clip, r_locks);
  r_layer.mask_ibuf = clip;
  r_layer.mask_colorspace_name = composite_buffer_colorspace_name(clip);
  /* Read as a grey with full influence: the value above already holds the mask's own. */
  r_layer.mask_from_alpha = false;
  r_layer.mask_influence = 1.0f;
}

/**
 * Bring an acquired buffer onto the evaluation grid.
 *
 * A full evaluation reads every map at the stack's own size, so this only ever moves something for
 * a reduced one (#BKE_paint_material_composite_eval_images_linear's eval dims): the resample is
 * bilinear in the map's own UV, which at a power-of-two octave is the preview-grade twin of what
 * the full evaluation reads texel by texel. A borrowed buffer -- an Anchor Point slice owned by
 * the description -- is never written to; the owned resample replaces it for this evaluation alone.
 * A resample that cannot be made keeps the original, which the size validation refuses, exactly as
 * it refuses a stack of disagreeing sizes.
 */
static ImBuf *composite_buffer_to_eval_grid(ImBuf *ibuf,
                                            const int eval_width,
                                            const int eval_height,
                                            Vector<CompositeImageLock> &r_locks)
{
  if (ibuf == nullptr || eval_width <= 0 || eval_height <= 0 ||
      (ibuf->x == eval_width && ibuf->y == eval_height))
  {
    return ibuf;
  }
  const PaintMaterialCompositeMapping identity;
  ImBuf *resampled = composite_resample_mapped(ibuf, eval_width, eval_height, identity);
  if (resampled == nullptr) {
    return ibuf;
  }
  composite_image_own(resampled, r_locks);
  return resampled;
}

/**
 * Acquire the buffers of \a image_layers into \a r_layers for the linear evaluator, which takes
 * float buffers and constant layers. A folder is acquired through its children, and left out when
 * nothing it holds paints.
 */
static bool composite_layers_acquire(Span<PaintMaterialCompositeImageLayer> image_layers,
                                     const int ref_width,
                                     const int ref_height,
                                     Vector<CompositeImageLock> &r_locks,
                                     Vector<PaintMaterialCompositeLayer> &r_layers)
{
  for (const PaintMaterialCompositeImageLayer &image_layer : image_layers) {
    if (!image_layer.enabled) {
      continue;
    }
    PaintMaterialCompositeLayer layer;
    if (image_layer.is_folder) {
      Vector<PaintMaterialCompositeLayer> children;
      if (!composite_layers_acquire(
              Span<PaintMaterialCompositeImageLayer>(image_layer.children.data(),
                                                     int64_t(image_layer.children.size())),
              ref_width,
              ref_height,
              r_locks,
              children))
      {
        return false;
      }
      if (children.is_empty()) {
        continue;
      }
      layer.children.assign(std::make_move_iterator(children.begin()),
                            std::make_move_iterator(children.end()));
    }
    else if (!composite_layer_has_content(image_layer, image_layer.color_image != nullptr) &&
             image_layer.anchor_point_color == nullptr)
    {
      continue;
    }
    if (image_layer.color_image != nullptr) {
      layer.color_ibuf = composite_image_acquire(
          image_layer.color_image, image_layer.color_iuser, r_locks);
      if (layer.color_ibuf == nullptr) {
        return false;
      }
      composite_mapped_remap(layer.color_ibuf, image_layer.mapping, ref_width, ref_height, r_locks);
      layer.color_ibuf = composite_buffer_to_eval_grid(
          layer.color_ibuf, ref_width, ref_height, r_locks);
      /* The buffer's own colorspace, not the Image setting: the two can disagree, and the
       * evaluator has to decode what it is actually handed. */
      layer.color_colorspace_name = composite_buffer_colorspace_name(layer.color_ibuf);
    }
    else if (image_layer.anchor_point_color != nullptr) {
      /* A slice is already in canvas space and scene linear: read it as it is, with no mapping and
       * no colorspace conversion. The image-layer description owns the buffer; the evaluator's
       * layer only borrows it for this call. A reduced grid reads an owned resample of it instead,
       * the slice itself staying untouched. */
      layer.color_ibuf = composite_buffer_to_eval_grid(
          image_layer.anchor_point_color.get(), ref_width, ref_height, r_locks);
      layer.color_colorspace_name = composite_buffer_colorspace_name(layer.color_ibuf);
    }
    if (image_layer.mask_image != nullptr) {
      layer.mask_ibuf = composite_image_acquire(
          image_layer.mask_image, image_layer.mask_iuser, r_locks);
      if (layer.mask_ibuf == nullptr) {
        return false;
      }
      /* A source coverage map reads through the row's mapping too, the same transform the
       * generated graph applies to it. It is resampled on its own even when it shares the colour
       * image: the colour pass may carry the covariant Normal remap, which would corrupt a
       * coverage scalar. */
      if (image_layer.mapping.enabled) {
        PaintMaterialCompositeMapping coverage_mapping = image_layer.mapping;
        coverage_mapping.tangent_normal = false;
        composite_mapped_remap(
            layer.mask_ibuf, coverage_mapping, ref_width, ref_height, r_locks);
      }
      layer.mask_ibuf = composite_buffer_to_eval_grid(
          layer.mask_ibuf, ref_width, ref_height, r_locks);
      layer.mask_colorspace_name = composite_buffer_colorspace_name(layer.mask_ibuf);
    }
    for (const PaintMaterialCompositeMaskItem &item : image_layer.mask_items) {
      PaintMaterialCompositeMaskBuffer buffer;
      buffer.constant_gray = item.constant_gray;
      buffer.opacity = item.opacity;
      buffer.blend = item.blend;
      buffer.invert = item.invert;
      buffer.filter = item.filter;
      std::copy(std::begin(item.filter_params),
                std::end(item.filter_params),
                std::begin(buffer.filter_params));
      if (item.image != nullptr) {
        buffer.ibuf = composite_image_acquire(item.image, item.iuser, r_locks);
        if (buffer.ibuf == nullptr) {
          return false;
        }
        /* Resampled before the grey is reduced, so the grey is read after the mapping. */
        composite_mapped_remap(buffer.ibuf, item.mapping, ref_width, ref_height, r_locks);
        buffer.ibuf = composite_buffer_to_eval_grid(buffer.ibuf, ref_width, ref_height, r_locks);
        buffer.colorspace_name = composite_buffer_colorspace_name(buffer.ibuf);
      }
      else if (item.anchor_point_slice != nullptr) {
        /* A slice is already in canvas space and scene linear; the description owns the buffer. A
         * reduced grid reads an owned resample of it, the slice itself staying untouched. */
        buffer.ibuf = composite_buffer_to_eval_grid(
            item.anchor_point_slice.get(), ref_width, ref_height, r_locks);
        buffer.colorspace_name = composite_buffer_colorspace_name(buffer.ibuf);
      }
      buffer.map_coverage = item.map_coverage;
      buffer.coverage_tolerance = item.coverage_tolerance;
      buffer.coverage_softness = item.coverage_softness;
      buffer.coverage_output = item.coverage_output;
      buffer.coverage_colors = item.coverage_colors;
      if (item.coverage_image != nullptr) {
        buffer.coverage_ibuf = composite_image_acquire(
            item.coverage_image, item.coverage_iuser, r_locks);
        if (buffer.coverage_ibuf != nullptr) {
          composite_mapped_remap(
              buffer.coverage_ibuf, item.coverage_mapping, ref_width, ref_height, r_locks);
          buffer.coverage_ibuf = composite_buffer_to_eval_grid(
              buffer.coverage_ibuf, ref_width, ref_height, r_locks);
          buffer.coverage_colorspace_name = composite_buffer_colorspace_name(
              buffer.coverage_ibuf);
        }
        /* A map that cannot be read is left as absent, so it samples as `match = 0` like a missing
         * one rather than failing the whole composite; the operator still takes part. */
      }
      buffer.gradient = item.gradient;
      if (buffer.gradient.enabled) {
        /* The gradient has no map: its grid is the composite's, not the description's size. */
        buffer.gradient.width = ref_width;
        buffer.gradient.height = ref_height;
      }
      layer.mask_items.append(buffer);
    }
    for (const PaintMaterialCompositeCorrection &correction : image_layer.content_corrections) {
      if (correction.image == nullptr && !correction.has_constant_color && correction.filter == 0 &&
          correction.anchor_point_slice == nullptr && !correction.gradient.enabled)
      {
        continue;
      }
      PaintMaterialCompositeCorrectionBuffer buffer;
      buffer.filter = correction.filter;
      std::copy(std::begin(correction.filter_params),
                std::end(correction.filter_params),
                std::begin(buffer.filter_params));
      buffer.blend = correction.blend;
      buffer.opacity = correction.opacity;
      if (correction.anchor_point_slice != nullptr) {
        /* A slice is already in canvas space and scene linear; the description owns both buffers.
         * A reduced grid reads owned resamples of them, the slices staying untouched. */
        buffer.anchor_point_slice = composite_buffer_to_eval_grid(
            correction.anchor_point_slice.get(), ref_width, ref_height, r_locks);
        buffer.anchor_point_coverage = composite_buffer_to_eval_grid(
            correction.anchor_point_coverage.get(), ref_width, ref_height, r_locks);
      }
      else if (correction.image != nullptr) {
        buffer.ibuf = composite_image_acquire(correction.image, correction.iuser, r_locks);
        if (buffer.ibuf == nullptr) {
          return false;
        }
        composite_mapped_remap(
            buffer.ibuf, correction.mapping, ref_width, ref_height, r_locks);
        buffer.ibuf = composite_buffer_to_eval_grid(buffer.ibuf, ref_width, ref_height, r_locks);
        buffer.colorspace_name = composite_buffer_colorspace_name(buffer.ibuf);
      }
      if (correction.anchor_point_coverage_image != nullptr) {
        buffer.anchor_point_coverage_ibuf = composite_image_acquire(
            correction.anchor_point_coverage_image,
            correction.anchor_point_coverage_iuser,
            r_locks);
        if (buffer.anchor_point_coverage_ibuf != nullptr) {
          buffer.anchor_point_coverage_ibuf = composite_buffer_to_eval_grid(
              buffer.anchor_point_coverage_ibuf, ref_width, ref_height, r_locks);
          buffer.anchor_point_coverage_colorspace_name = composite_buffer_colorspace_name(
              buffer.anchor_point_coverage_ibuf);
        }
      }
      /* The flat colour belongs to the chain above: a slice, a map or a gradient has none. */
      if (correction.anchor_point_slice == nullptr && correction.image == nullptr &&
          !correction.gradient.enabled) {
        buffer.has_constant_color = true;
        copy_v4_v4(buffer.constant_color, correction.constant_color);
      }
      buffer.gradient = correction.gradient;
      if (buffer.gradient.enabled) {
        /* The gradient has no map: its grid is the composite's, not the description's size. */
        buffer.gradient.width = ref_width;
        buffer.gradient.height = ref_height;
      }
      layer.content_corrections.append(buffer);
    }
    static_cast<PaintMaterialCompositeRowParams &>(layer) = image_layer;
    layer.gradient = image_layer.gradient;
    if (layer.gradient.enabled) {
      /* The gradient has no map of its own: its UV grid is the composite's, not the size the
       * description was created at. A stack whose bottom map is smaller than the material would
       * otherwise read only part of the ramp. */
      layer.gradient.width = ref_width;
      layer.gradient.height = ref_height;
    }
    /* A clipping wrap of a Material row reaches its coverage as a 0/1 factor, since the row does
     * not cover through its map's alpha. Folded after the row's own coverage is acquired and
     * before the mask items are read, which still multiply on top in the evaluator. */
    if (image_layer.mapping_clips_coverage) {
      composite_clip_coverage_fold(
          layer, image_layer.mapping, ref_width, ref_height, r_locks);
    }
    r_layers.append(std::move(layer));
  }
  return true;
}

static bool composite_stack_build_linear(Span<PaintMaterialCompositeImageLayer> image_layers,
                                         Vector<CompositeImageLock> &r_locks,
                                         PaintMaterialCompositeStack &r_stack,
                                         const int eval_width,
                                         const int eval_height)
{
  if (!BKE_paint_material_composite_stack_dimensions(image_layers, r_stack.width, r_stack.height))
  {
    return false;
  }
  /* A reduced grid replaces the stack's own dimensions: every buffer is brought onto it by
   * #composite_layers_acquire, and a Gradient reads the grid's own UVs. */
  if (eval_width > 0 && eval_height > 0) {
    r_stack.width = eval_width;
    r_stack.height = eval_height;
  }
  if (!composite_layers_acquire(
          image_layers, r_stack.width, r_stack.height, r_locks, r_stack.layers))
  {
    return false;
  }
  return !r_stack.layers.is_empty();
}

/** How many Gradient payloads \a image_layers lays: a row, a correction and a mask item each
 * sample its line and ramp per texel, so their count is the part of an evaluation a UV-line drag
 * pays for again on every step. */
static int composite_stack_gradient_count(Span<PaintMaterialCompositeImageLayer> image_layers)
{
  int count = 0;
  for (const PaintMaterialCompositeImageLayer &layer : image_layers) {
    if (layer.gradient.enabled) {
      count++;
    }
    for (const PaintMaterialCompositeCorrection &correction : layer.content_corrections) {
      if (correction.gradient.enabled) {
        count++;
      }
    }
    for (const PaintMaterialCompositeMaskItem &item : layer.mask_items) {
      if (item.gradient.enabled) {
        count++;
      }
    }
    if (layer.is_folder) {
      count += composite_stack_gradient_count(
          Span<PaintMaterialCompositeImageLayer>(layer.children.data(),
                                                 int64_t(layer.children.size())));
    }
  }
  return count;
}

bool BKE_paint_material_composite_eval_images(Span<PaintMaterialCompositeImageLayer> image_layers,
                                              ImBuf *composite_ibuf,
                                              const rcti *region,
                                              PaintMaterialCompositeEvalStats *r_stats,
                                              const float bottom_color[4],
                                              const int eval_width,
                                              const int eval_height)
{
  Vector<CompositeImageLock> locks;
  PaintMaterialCompositeStack stack;
  bool ok = composite_stack_build_linear(image_layers, locks, stack, eval_width, eval_height) &&
            composite_ibuf_is_byte_rgba(composite_ibuf, stack.width, stack.height);
  /* The target is the caller's buffer and its size is part of the contract; a mismatch is caught
   * by the check above, so this only states which of the two the evaluation writes. */
  BLI_assert(!ok || (composite_ibuf != nullptr && composite_ibuf->x == stack.width &&
                     composite_ibuf->y == stack.height));
  int64_t area_texels = 0;
  if (ok) {
    if (bottom_color != nullptr) {
      copy_v4_v4(stack.bottom_color, bottom_color);
      stack.has_bottom_color = true;
    }
    rcti area;
    if (!BKE_paint_material_composite_area_get(stack.width, stack.height, region, area)) {
      /* Nothing of the tagged region is inside the buffer; the composite is already correct. */
      if (r_stats != nullptr) {
        *r_stats = {};
      }
    }
    else {
      area_texels = int64_t(BLI_rcti_size_x(&area)) * BLI_rcti_size_y(&area);
      /* Scratch the size of the area, never of the frame: a small refresh costs the same on any
       * canvas. */
      Vector<float> linear(area_texels * 4);
      ok = BKE_paint_material_composite_eval_linear(
          stack, PaintMaterialCompositeLinearTarget::packed(linear, area), region, r_stats);
      if (ok) {
        BKE_paint_material_composite_linear_to_byte(linear, area, *composite_ibuf);
      }
    }
    if (r_stats != nullptr) {
      r_stats->gradient_items = composite_stack_gradient_count(image_layers);
      r_stats->gradient_samples = int64_t(r_stats->gradient_items) * area_texels;
    }
  }
  composite_images_release(locks);
  return ok;
}

bool BKE_paint_material_composite_eval_images_linear(
    Span<PaintMaterialCompositeImageLayer> image_layers,
    const PaintMaterialCompositeLinearTarget &target,
    const rcti *region,
    PaintMaterialCompositeEvalStats *r_stats,
    const float bottom_color[4],
    const int eval_width,
    const int eval_height)
{
  Vector<CompositeImageLock> locks;
  PaintMaterialCompositeStack stack;
  bool ok = composite_stack_build_linear(image_layers, locks, stack, eval_width, eval_height);
  if (ok && bottom_color != nullptr) {
    copy_v4_v4(stack.bottom_color, bottom_color);
    stack.has_bottom_color = true;
  }
  if (ok) {
    ok = BKE_paint_material_composite_eval_linear(stack, target, region, r_stats);
  }
  if (r_stats != nullptr) {
    r_stats->gradient_items = composite_stack_gradient_count(image_layers);
    if (ok) {
      rcti area;
      if (BKE_paint_material_composite_area_get(stack.width, stack.height, region, area)) {
        r_stats->gradient_samples = int64_t(r_stats->gradient_items) *
                                    int64_t(BLI_rcti_size_x(&area)) * BLI_rcti_size_y(&area);
      }
    }
  }
  composite_images_release(locks);
  return ok;
}

bool BKE_paint_material_composite_eval_row_content(const PaintMaterialCompositeImageLayer &row,
                                                   float *r_color_rgba,
                                                   float *r_coverage_gray,
                                                   const rcti *region,
                                                   const int channel)
{
  if (r_color_rgba == nullptr || r_coverage_gray == nullptr) {
    return false;
  }
  Vector<CompositeImageLock> locks;
  PaintMaterialCompositeStack stack;
  /* Always the stack's own size: a bake renders at the canvas, never at a preview octave. */
  const bool built = composite_stack_build_linear(
      Span<PaintMaterialCompositeImageLayer>(&row, 1), locks, stack, 0, 0);
  bool ok = false;
  rcti area;
  if (built && stack.layers.size() == 1 && composite_stack_validate_linear(stack) &&
      BKE_paint_material_composite_area_get(stack.width, stack.height, region, area))
  {
    const PaintMaterialCompositeLayer &layer = stack.layers.first();
    const int64_t count = int64_t(BLI_rcti_size_x(&area)) * BLI_rcti_size_y(&area);
    Vector<float> color(count * 4);
    Vector<float> factor(count);
    composite_layer_apply(layer, area, color.data(), factor.data());
    /* Normal keeps its own map alpha: its content coverage has nowhere else to live, and a
     * substituted row reads it back as `common x content`. */
    const bool keep_alpha = layer.tracks_content_alpha || channel == PAINT_MATERIAL_CHANNEL_NORMAL;
    for (int64_t i = 0; i < count; i++) {
      r_color_rgba[i * 4 + 0] = color[i * 4 + 0];
      r_color_rgba[i * 4 + 1] = color[i * 4 + 1];
      r_color_rgba[i * 4 + 2] = color[i * 4 + 2];
      /* The colour's alpha is the row's content alpha, so the generated chain can read it back after
       * substitution. A channel that tracks none stays opaque; its transparency is the coverage
       * alone. */
      r_color_rgba[i * 4 + 3] = keep_alpha ? color[i * 4 + 3] : 1.0f;
      r_coverage_gray[i] = clamp_f(layer.opacity * factor[i], 0.0f, 1.0f);
    }
    ok = true;
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
    if (!layer.enabled) {
      continue;
    }
    if (layer.is_folder) {
      /* A folder is as big as what it holds: the lowest map in it decides, at the folder's place. */
      if (composite_stack_bottom_layer_info(
              Span<PaintMaterialCompositeImageLayer>(layer.children.data(),
                                                     int64_t(layer.children.size())),
              r_width,
              r_height,
              r_byte_colorspace))
      {
        return true;
      }
      continue;
    }
    /* A Gradient has no map: the description carries the material's map size. */
    if (layer.gradient.enabled) {
      if (layer.gradient.width <= 0 || layer.gradient.height <= 0) {
        continue;
      }
      r_width = layer.gradient.width;
      r_height = layer.gradient.height;
      return true;
    }
    /* A row kept only for its corrections is as big as the first map it has. */
    Image *size_image = layer.color_image;
    const ImageUser *size_iuser = layer.color_iuser;
    if (size_image == nullptr) {
      for (const PaintMaterialCompositeCorrection &correction : layer.content_corrections) {
        if (correction.image != nullptr) {
          size_image = correction.image;
          size_iuser = correction.iuser;
          break;
        }
      }
    }
    if (size_image == nullptr) {
      if (layer.empty_base && layer.empty_base_size > 0) {
        r_width = r_height = layer.empty_base_size;
        return true;
      }
      /* A row kept for its corrections and carrying a Gradient has no map, but the gradient's grid
       * still decides the stack's size, as a Gradient row's does. */
      for (const PaintMaterialCompositeCorrection &correction : layer.content_corrections) {
        if (!correction.gradient.enabled) {
          continue;
        }
        r_width = correction.gradient.width;
        r_height = correction.gradient.height;
        if (r_width > 0 && r_height > 0) {
          return true;
        }
      }
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

bool BKE_paint_material_channel_tracks_content_alpha(const eMaterialPaintChannel channel)
{
  if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM) {
    return false;
  }
  if (channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
    return false;
  }
  /* A layered material paints a channel its row owns even without a Principled backend
   * (#supports_layer_paint, Height): its map's alpha is the row's coverage, so the CPU composite
   * and the generated graph must read it the same way. */
  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(channel);
  return info.stores_image();
}

bool BKE_paint_material_composite_stack_dimensions(
    Span<PaintMaterialCompositeImageLayer> image_layers, int &r_width, int &r_height)
{
  return composite_stack_bottom_layer_info(image_layers, r_width, r_height, nullptr);
}

/**
 * Fold \a mapping into \a hash. A mapping slider moves pixels without changing any image, so the
 * normalized values have to move the hash, or an edit would keep serving the cached composite.
 */
static uint64_t composite_mapping_hash(uint64_t hash, const PaintMaterialCompositeMapping &mapping)
{
  if (!mapping.enabled) {
    return hash;
  }
  hash = get_default_hash(hash, mapping.offset[0], mapping.offset[1], mapping.rotation);
  hash = get_default_hash(hash, mapping.wrap);
  return get_default_hash(hash,
                          BKE_paint_layers_mapping_scale_normalize(mapping.scale[0]),
                          BKE_paint_layers_mapping_scale_normalize(mapping.scale[1]),
                          mapping.tangent_normal);
}

/** Fold a Gradient payload into \a hash: every parameter moves the composited pixels. */
static uint64_t composite_gradient_hash(uint64_t hash, const PaintMaterialCompositeGradient &gradient)
{
  if (!gradient.enabled) {
    return hash;
  }
  hash = get_default_hash(hash,
                          gradient.color_channel,
                          int(gradient.payload.space),
                          int(gradient.payload.type),
                          int(gradient.payload.repeat));
  hash = get_default_hash(hash,
                          gradient.payload.start[0],
                          gradient.payload.start[1],
                          gradient.payload.start[2],
                          gradient.payload.end[0]);
  hash = get_default_hash(hash,
                          gradient.payload.end[1],
                          gradient.payload.end[2],
                          BKE_paint_layers_gradient_midpoint_effective(gradient.payload));
  /* The approximate and exact variants must not share a cache entry. The content revision, unlike
   * the identity key, changes when a notifier forces the same object/UV/size surface to rebuild. */
  hash = get_default_hash(hash,
                          gradient.surface_key,
                          gradient.approximate,
                          gradient.surface ? gradient.surface->revision : 0);
  hash = get_default_hash(hash,
                          gradient.scalar_start,
                          gradient.scalar_end,
                          gradient.width,
                          gradient.height);
  hash = get_default_hash(hash, gradient.scalar_ramp_valid);
  if (gradient.scalar_ramp_valid) {
    hash = get_default_hash(hash, BKE_paint_layers_gradient_ramp_hash(gradient.scalar_ramp));
  }
  return get_default_hash(hash, BKE_paint_layers_gradient_ramp_hash(gradient.payload.ramp));
}

uint64_t BKE_paint_material_composite_stack_hash(
    Span<PaintMaterialCompositeImageLayer> image_layers)
{
  uint64_t hash = get_default_hash(image_layers.size());
  for (const PaintMaterialCompositeImageLayer &layer : image_layers) {
    /* Mix the running hash multiplicatively before each layer: #get_default_hash folds its
     * arguments with XOR, which is order-independent, so two layers swapped would otherwise hash
     * the same and a reorder would not invalidate the composite. */
    hash *= 0x100000001b3ULL;
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
    /* The height modes are another formula: they have to move the hash or an edit would keep
     * serving the cached composite. */
    hash = get_default_hash(hash, int(layer.height_blend), layer.height_scale);
    /* A flat colour is not backed by an image, so its value has to be hashed explicitly or the
     * cached composite would not follow an edit of it. Split rather than appended:
     * #get_default_hash mixes a fixed number of values at once. */
    hash = get_default_hash(hash,
                            layer.has_constant_color,
                            layer.constant_color[0],
                            layer.constant_color[1],
                            layer.constant_color[2],
                            layer.constant_color[3]);
    hash = get_default_hash(hash, layer.color_alpha_coverage, layer.is_folder);
    hash = get_default_hash(hash, layer.empty_base, layer.tracks_content_alpha);
    /* A slice has no Image to name its contents: the description hash keeps a stale composite from
     * surviving an edit above the marker. */
    hash = get_default_hash(hash, layer.anchor_point_hash);
    hash = composite_gradient_hash(hash, layer.gradient);
    /* An adjustment is its filter and what it was told. */
    hash = get_default_hash(hash, int(layer.adjustment_filter));
    for (const float param : layer.adjustment_params) {
      hash = get_default_hash(hash, param);
    }
    hash = composite_mapping_hash(get_default_hash(hash, layer.mapping.enabled), layer.mapping);
    /* A correction changes the colour and the coverage: its map (by session uid), its blend and its
     * opacity all have to move the hash, or an edit would keep serving the cached composite. */
    for (const PaintMaterialCompositeCorrection &correction : layer.content_corrections) {
      hash *= 0x100000001b3ULL;
      hash = get_default_hash(hash,
                              correction.image != nullptr ? correction.image->id.session_uid : 0,
                              correction.anchor_point_coverage_image != nullptr ?
                                  correction.anchor_point_coverage_image->id.session_uid :
                                  0,
                              int(correction.blend),
                              correction.opacity);
      /* Which filter it is, or none, and what it was told. */
      hash = get_default_hash(hash, int(correction.filter));
      for (const float param : correction.filter_params) {
        hash = get_default_hash(hash, param);
      }
      hash = get_default_hash(hash, correction.anchor_point_hash);
      hash = composite_mapping_hash(get_default_hash(hash, correction.mapping.enabled),
                                    correction.mapping);
      hash = composite_gradient_hash(hash, correction.gradient);
      /* A flat colour is not backed by an image, so its value is hashed like a row's. */
      hash = get_default_hash(hash,
                              correction.has_constant_color,
                              correction.constant_color[0],
                              correction.constant_color[1],
                              correction.constant_color[2]);
    }
    if (layer.is_folder) {
      /* The children are part of the folder's identity: the stack hash of the folder's own stack,
       * folded in as one value. */
      hash = get_default_hash(hash,
                              BKE_paint_material_composite_stack_hash(
                                  Span<PaintMaterialCompositeImageLayer>(
                                      layer.children.data(), int64_t(layer.children.size()))));
    }
    /* A mask item changes the factor: its map (by session uid, like the layer's), its constant and
     * its opacity all have to move the hash, or an edit would keep serving the cached composite. */
    for (const PaintMaterialCompositeMaskItem &item : layer.mask_items) {
      hash *= 0x100000001b3ULL;
      hash = get_default_hash(
          hash, item.image != nullptr ? item.image->id.session_uid : 0, item.opacity);
      hash = get_default_hash(hash, item.constant_gray, int(item.blend), item.invert);
      hash = get_default_hash(hash, item.anchor_point_hash);
      hash = get_default_hash(hash, int(item.filter));
      for (const float param : item.filter_params) {
        hash = get_default_hash(hash, param);
      }
      hash = composite_mapping_hash(get_default_hash(hash, item.mapping.enabled), item.mapping);
      hash = composite_gradient_hash(hash, item.gradient);
      /* A map-coverage operator changes the factor from its own map and its colour list, none of
       * which is the laid item's map: all of it has to move the hash. */
      hash = get_default_hash(hash,
                              int(item.map_coverage),
                              item.coverage_image != nullptr ? item.coverage_image->id.session_uid : 0,
                              item.coverage_tolerance,
                              item.coverage_softness,
                              item.coverage_output);
      for (const PaintMaterialCompositeIdColor &color : item.coverage_colors) {
        hash = get_default_hash(
            hash, color.color[0], color.color[1], color.color[2], color.enabled);
        hash = get_default_hash(hash, color.tolerance, color.own_tolerance);
      }
      hash = composite_mapping_hash(get_default_hash(hash, item.coverage_mapping.enabled),
                                    item.coverage_mapping);
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

using bke::paint_material_cache::ImBufPtr;
using bke::paint_material_cache::SubscriptionMap;

struct CompositeCacheEntry {
  ImBufPtr ibuf;
  int width = 0;
  int height = 0;
  /**
   * The canvas #width and #height are a fraction of: an interactive Gradient drag keeps the
   * composite at a power-of-two octave of it, the same scheme the Combined preview shades at.
   * Zero only before the first evaluation; a change of the canvas itself is a size change.
   */
  int canvas_width = 0;
  int canvas_height = 0;
  uint64_t stack_hash = 0;
  /** The whole buffer has to be recomputed. Set on creation, and whenever a region is unknown. */
  bool dirty_full = true;
  /** Bounding rectangle of the pixels tagged since the last evaluation. */
  rcti dirty_region = {0, 0, 0, 0};
  /**
   * One partial-update subscription per source image, maintained by
   * #bke::paint_material_cache::subscriptions_poll, which is also where the reasoning for polling
   * rather than being told lives.
   */
  SubscriptionMap partial_update_users;
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

/**
 * The floor is #COMPOSITE_CACHE_BUDGET_BYTES. Above it, the budget grows to hold one composite per
 * channel at the size being gathered: a larger material's whole set would otherwise evict itself on
 * every redraw, since the gather needs all of its channels resident at once.
 */
static void composite_cache_enforce_budget(const CompositeCacheKey &keep,
                                           const int width,
                                           const int height)
{
  const int64_t one_set_bytes = int64_t(width) * height * 4 * PAINT_MATERIAL_CHANNEL_NUM;
  const int64_t budget_bytes = std::max(COMPOSITE_CACHE_BUDGET_BYTES, one_set_bytes);
  bke::paint_material_cache::enforce_budget(
      g_cache.entries, keep, budget_bytes, composite_entry_size_in_bytes);
}

/** Every image the stack reads, in the order the layers are composited. */
static void composite_stack_images_gather(Span<PaintMaterialCompositeImageLayer> image_layers,
                                          Vector<Image *> &r_images)
{
  r_images.clear();
  r_images.reserve(image_layers.size() * 2);
  for (const PaintMaterialCompositeImageLayer &layer : image_layers) {
    /* Nulls and duplicates are the poll's business, not this function's. */
    r_images.append(layer.color_image);
    r_images.append(layer.mask_image);
    /* An Anchor Point slice carries no pixels in the hash; a stroke on its source reaches the cache
     * through the source's maps. */
    r_images.extend(layer.anchor_point_images);
    for (const PaintMaterialCompositeMaskItem &item : layer.mask_items) {
      r_images.append(item.image);
      r_images.extend(item.anchor_point_images);
      /* A coverage operator reads its own map, so an edit to it must be seen by the cache too. */
      r_images.append(item.coverage_image);
    }
    for (const PaintMaterialCompositeCorrection &correction : layer.content_corrections) {
      r_images.append(correction.image);
      r_images.append(correction.anchor_point_coverage_image);
      r_images.extend(correction.anchor_point_images);
    }
    if (layer.is_folder) {
      /* The images a folder holds are read through it, so a stroke deep inside still reaches the
       * cache. */
      Vector<Image *> nested;
      composite_stack_images_gather(Span<PaintMaterialCompositeImageLayer>(
                                        layer.children.data(), int64_t(layer.children.size())),
                                    nested);
      r_images.extend(nested);
    }
  }
}

/**
 * The output size to evaluate at: \a output_size when the caller decided one, the canvas otherwise.
 *
 * Nothing is derived here. The octave of an interactive preview is decided once, by the gather that
 * fills every channel's composite, because the preview reads them together: an octave this cache
 * settled on per entry would differ from the next channel's whenever their histories did, and the
 * preview would drop the channel that disagreed.
 *
 * A size that is not a power-of-two fraction of the canvas is not an octave of it and is refused:
 * the buffer covers the whole canvas, and texels that do not land on that grid would sit somewhere
 * the UV mapping does not expect them. Refusing rather than resampling keeps the size mismatch
 * visible instead of silently blurring it away.
 */
static void composite_output_size_for(const int canvas_width,
                                      const int canvas_height,
                                      const int2 &output_size,
                                      int &r_width,
                                      int &r_height)
{
  r_width = canvas_width;
  r_height = canvas_height;
  if (output_size.x <= 0 || output_size.y <= 0 || output_size.x > canvas_width ||
      output_size.y > canvas_height || canvas_width % output_size.x != 0 ||
      canvas_height % output_size.y != 0 ||
      !is_power_of_2_i(canvas_width / output_size.x) ||
      !is_power_of_2_i(canvas_height / output_size.y))
  {
    return;
  }
  r_width = output_size.x;
  r_height = output_size.y;
}

ImBuf *BKE_paint_material_composite_cache_ensure(
    const Material &ma,
    const int channel,
    Span<PaintMaterialCompositeImageLayer> image_layers,
    const uint64_t stack_hash,
    uint64_t *r_revision,
    PaintMaterialCompositeEvalStats *r_stats,
    rcti *r_changed_region,
    FunctionRef<bool(Vector<PaintMaterialCompositeImageLayer> &)> full_layers,
    const int2 &output_size)
{
  bke::paint_perf::ScopedTimer perf_timer("composite_cache_ensure");
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
  if (byte_colorspace == nullptr) {
    /* A Gradient-only stack has no image to take the encoding from: use the channel's role, the
     * same one a composited map gets. */
    const bool is_color = BKE_paint_material_channel_info(eMaterialPaintChannel(channel)).is_color;
    byte_colorspace = IMB_colormanagement_role_colorspace_name_get(
        is_color ? COLOR_ROLE_DEFAULT_BYTE : COLOR_ROLE_DATA);
  }

  /* The output size: the canvas, or the octave the caller asked for. The canvas is recorded
   * separately because the buffer may be a fraction of it. */
  int out_width = width;
  int out_height = height;

  CompositeCacheKey key;
  key.material_session_uid = ma.id.session_uid;
  key.channel = channel;

  CompositeCacheEntry &entry = g_cache.entries.lookup_or_add_default(key);
  entry.last_use = ++g_cache.use_counter;

  composite_output_size_for(width, height, output_size, out_width, out_height);

  const bool size_changed = entry.ibuf == nullptr || entry.width != out_width ||
                            entry.height != out_height || entry.canvas_width != width ||
                            entry.canvas_height != height;
  /* Recorded unconditionally, like the Combined cache records its canvas: an octave that absorbs
   * a canvas resize -- 4096 to 2048 while the display keeps the output at 1024 -- would otherwise
   * go on reporting the old one. */
  entry.canvas_width = width;
  entry.canvas_height = height;
  if (size_changed) {
    composite_entry_reset(entry);
    entry.ibuf.reset(IMB_allocImBuf(uint(out_width), uint(out_height), ImBufFlags::ByteData));
    if (entry.ibuf == nullptr) {
      g_cache.entries.remove(key);
      return nullptr;
    }
    entry.ibuf->channels = 4;
    entry.width = out_width;
    entry.height = out_height;
  }

  /* What the source images say changed since the last call.
   *
   * Polled rather than reported: a caller that edits pixels no longer has to remember to tell this
   * cache, and -- the reason this exists -- a blanket ID tag from an unrelated subsystem can no
   * longer overwrite a precise report with "everything". Placed after the reallocation above so a
   * resize, which drops the subscriptions with the buffer, is followed by fresh ones whose first
   * poll asks for the full rebuild a resize needs anyway. */
  Vector<Image *> source_images;
  composite_stack_images_gather(image_layers, source_images);
  /* A reported rectangle arrives in the image's own texels, which are the canvas's. The composite
   * stores those texels when it is at the canvas, and a fraction of them at an octave: rounded
   * outwards, so a dab never loses its edge and leaves a stale fringe. */
  const bool entry_scaled = entry.width != width || entry.height != height;
  const auto poll_region_to_entry = [&](const rcti &region) {
    return bke::paint_material_cache::rect_scale(
        region, int2(width, height), int2(entry.width, entry.height));
  };
  const bke::paint_material_cache::PollResult poll = bke::paint_material_cache::subscriptions_poll(
      entry.partial_update_users,
      source_images,
      entry_scaled ? FunctionRef<rcti(const rcti &)>(poll_region_to_entry) :
                     FunctionRef<rcti(const rcti &)>());
  if (poll.needs_full_update) {
    entry.dirty_full = true;
  }
  if (!BLI_rcti_is_empty(&poll.changed_region)) {
    if (BLI_rcti_is_empty(&entry.dirty_region)) {
      entry.dirty_region = poll.changed_region;
    }
    else {
      BLI_rcti_union(&entry.dirty_region, &poll.changed_region);
    }
  }

  const bool rebuild_all = size_changed || entry.stack_hash != stack_hash || entry.dirty_full;
  const bool rebuild_region = !rebuild_all && !BLI_rcti_is_empty(&entry.dirty_region);
  perf_timer.note("channel=%d %dx%d out=%dx%d layers=%d rebuild=%s (size=%d hash=%d dirty_full=%d) region=%dx%d",
                  channel,
                  width,
                  height,
                  out_width,
                  out_height,
                  int(image_layers.size()),
                  rebuild_all ? "ALL" : (rebuild_region ? "region" : "none"),
                  int(size_changed),
                  int(entry.stack_hash != stack_hash),
                  int(entry.dirty_full),
                  BLI_rcti_size_x(&entry.dirty_region),
                  BLI_rcti_size_y(&entry.dirty_region));
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
    /* A layered material's rows blend over the channel's own neutral value, so a partially
     * covered row fades towards what the material shows where nothing covers it rather than
     * towards zero; the same start the description's own composite uses. */
    float bottom_color[4];
    const bool has_bottom = BKE_paint_layers_is_layered(ma) && channel >= 0 &&
                            channel < PAINT_MATERIAL_CHANNEL_NUM;
    if (has_bottom) {
      BKE_paint_layers_channel_bottom_color(eMaterialPaintChannel(channel), bottom_color);
    }
    /* The slices are rendered here and nowhere earlier: the description above was enough to find
     * out that nothing changed, which is what almost every call is. */
    Vector<PaintMaterialCompositeImageLayer> evaluated_layers;
    Span<PaintMaterialCompositeImageLayer> eval_layers = image_layers;
    if (full_layers) {
      if (!full_layers(evaluated_layers)) {
        g_cache.entries.remove(key);
        return nullptr;
      }
      eval_layers = evaluated_layers;
    }
    bool eval_ok;
    {
      bke::paint_perf::ScopedTimer perf_eval("composite_eval_images");
      eval_ok = BKE_paint_material_composite_eval_images(eval_layers,
                                                         entry.ibuf.get(),
                                                         region,
                                                         r_stats,
                                                         has_bottom ? bottom_color : nullptr,
                                                         out_width,
                                                         out_height);
      perf_eval.note("channel=%d region=%s gradient_items=%d gradient_samples=%lld",
                     channel,
                     region != nullptr ? "partial" : "full",
                     r_stats != nullptr ? r_stats->gradient_items : 0,
                     r_stats != nullptr ? static_cast<long long>(r_stats->gradient_samples) : 0LL);
    }
    if (!eval_ok) {
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
  }

  if (r_revision != nullptr) {
    *r_revision = entry.revision;
  }
  composite_cache_enforce_budget(key, width, height);
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

bool BKE_paint_material_composite_cache_contains(const Material &ma, const int channel)
{
  CompositeCacheKey key;
  key.material_session_uid = ma.id.session_uid;
  key.channel = channel;
  return g_cache.entries.contains(key);
}

ImBuf *BKE_paint_material_composite_cache_peek(const Material &ma,
                                               const int channel,
                                               uint64_t *r_revision)
{
  CompositeCacheKey key;
  key.material_session_uid = ma.id.session_uid;
  key.channel = channel;
  CompositeCacheEntry *entry = g_cache.entries.lookup_ptr(key);
  if (entry == nullptr || entry->ibuf == nullptr) {
    return nullptr;
  }
  /* As #BKE_paint_material_combined_cache_peek: a displayed buffer is a use, but not a recompute. */
  entry->last_use = ++g_cache.use_counter;
  if (r_revision != nullptr) {
    *r_revision = entry->revision;
  }
  return entry->ibuf.get();
}

bool BKE_paint_material_composite_cache_sizes_get(const Material &ma,
                                                 const int channel,
                                                 int2 &r_canvas,
                                                 int2 &r_output)
{
  CompositeCacheKey key;
  key.material_session_uid = ma.id.session_uid;
  key.channel = channel;
  const CompositeCacheEntry *entry = g_cache.entries.lookup_ptr(key);
  if (entry == nullptr || entry->ibuf == nullptr) {
    return false;
  }
  /* Deliberately not a use: a size query must not keep an entry alive against the budget. */
  r_canvas = int2(entry->canvas_width, entry->canvas_height);
  r_output = int2(entry->width, entry->height);
  return true;
}

/** \} */

}  // namespace blender
