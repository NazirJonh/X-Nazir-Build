/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "BKE_paint_material_composite.hh"

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
#include "BLI_task.hh"
#include "BLI_time.h"
#include "BLI_utildefines.h"
#include "BLI_uuid.h"

#include "BLT_translation.hh"

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_tree_interface_types.h"
#include "DNA_node_types.h"
#include "DNA_scene_types.h"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include <cstring>
#include <iterator>
#include <memory>

#include "paint_material_cache_common.hh"

namespace blender {

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

  /* The normal combine group has to be recognized before the socket walk resolves through it:
   * #BKE_paint_material_source_socket descends into a group and reports the node inside that
   * happens to feed the output, which says nothing about what the group as a whole does. */
  const bNode *shallow_source = composite_source_node_shallow(socket);
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
        out[1] = pixel[1];
        out[2] = pixel[2];
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
 * Render one layer onto tile-sized buffers: its straight colour in \a r_color and the coverage it
 * blends by in \a r_factor, before the layer's own opacity.
 *
 * The coverage is the mask times the layer's own content coverage: a map's alpha for a painted
 * channel map, or full. A mask is decoded like any map, in one tile call.
 *
 * A folder has no map: \a folder_color is the straight colour of its contents and \a folder_coverage
 * the coverage they accumulated, which stand in for the map and for its alpha.
 */
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
  else if (layer.color_ibuf != nullptr) {
    composite_decode_tile(layer.color_ibuf, layer.color_colorspace_name, tile, r_color);
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

  for (int64_t i = 0; i < count; i++) {
    float mask_value = 1.0f;
    if (mask != nullptr) {
      const float *m = mask + i * 4;
      const float value = layer.mask_from_alpha ? m[3] : (m[0] + m[1] + m[2]) / 3.0f;
      mask_value = (1.0f - layer.mask_influence) +
                   layer.mask_influence * clamp_f(value, 0.0f, 1.0f);
    }
    /* A layer whose factor is its own alpha, with no mask map to say otherwise, and a painted map
     * both cover by the map's alpha; a transparent texel shows what is below. */
    const bool own_alpha = (layer.color_alpha_coverage && layer.color_ibuf != nullptr) ||
                           (mask == nullptr && layer.mask_from_alpha);
    float content = own_alpha ? clamp_f(r_color[i * 4 + 3], 0.0f, 1.0f) : 1.0f;
    if (layer.is_folder) {
      /* A folder covers by what its contents covered, not by the alpha of the colour it reads. */
      content = clamp_f(folder_coverage[i], 0.0f, 1.0f);
    }
    r_factor[i] = mask_value * content;
  }

  /* The mask stack lays its coverage over the factor, item by item: `F = mix(F, F * C, A * op)`,
   * the Mix the generator builds for the same item. A transparent texel (A = 0) leaves the row
   * visible, which is what a fresh mask map must do. */
  Vector<float> item_storage;
  for (const PaintMaterialCompositeMaskBuffer &item : layer.mask_items) {
    if (item.opacity <= 0.0f) {
      continue;
    }
    if (item.ibuf == nullptr) {
      const float weight = clamp_f(item.opacity, 0.0f, 1.0f);
      const float gray = clamp_f(item.constant_gray, 0.0f, 1.0f);
      for (int64_t i = 0; i < count; i++) {
        r_factor[i] *= 1.0f - weight * (1.0f - gray);
      }
      continue;
    }
    item_storage.resize(count * 4);
    composite_decode_tile(item.ibuf, item.colorspace_name, tile, item_storage.data());
    for (int64_t i = 0; i < count; i++) {
      const float *c = item_storage.data() + i * 4;
      const float gray = clamp_f((c[0] + c[1] + c[2]) / 3.0f, 0.0f, 1.0f);
      const float weight = clamp_f(c[3], 0.0f, 1.0f) * clamp_f(item.opacity, 0.0f, 1.0f);
      r_factor[i] *= 1.0f - weight * (1.0f - gray);
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
  return has_map || layer.has_constant_color;
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
      blend_row_linear(blended, child_color.data() + i * 4, child.blend, 1.0f);
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
  if (layer.color_ibuf != nullptr) {
    if (!composite_ibuf_is_rgba(layer.color_ibuf, width, height)) {
      return false;
    }
  }
  else if (!composite_layer_has_content(layer, false)) {
    return false;
  }
  if (layer.mask_ibuf != nullptr && !composite_mask_ibuf_is_valid(layer.mask_ibuf, width, height)) {
    return false;
  }
  for (const PaintMaterialCompositeMaskBuffer &item : layer.mask_items) {
    if (item.ibuf != nullptr && !composite_mask_ibuf_is_valid(item.ibuf, width, height)) {
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
  int layers_evaluated = 0;
  Vector<float> tile_storage;
  Vector<float> color_storage;
  Vector<float> factor_storage;

  for (int tile_y = area.ymin; tile_y < area.ymax; tile_y += COMPOSITE_TILE_SIZE) {
    for (int tile_x = area.xmin; tile_x < area.xmax; tile_x += COMPOSITE_TILE_SIZE) {
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

      for (const PaintMaterialCompositeLayer &layer : stack.layers) {
        /* A layer that blends by nothing leaves the tile as it is; do not decode its buffers. */
        if (!layer.enabled || layer.opacity <= 0.0f) {
          continue;
        }
        composite_layer_apply(layer, tile, color_storage.data(), factor_storage.data());
        for (int64_t i = 0; i < count; i++) {
          blend_row_linear(tile_dst + i * 4,
                           color_storage.data() + i * 4,
                           layer.blend,
                           layer.opacity * factor_storage[i]);
        }
        layers_evaluated++;
      }

      /* Copy the tile back into the caller's buffer; only the region is ever touched. */
      for (int row = 0; row < th; row++) {
        float *dst_row = target.pixels.data() +
                         int64_t(tile.ymin - area.ymin + row) * target.row_stride +
                         int64_t(tile.xmin - area.xmin) * 4;
        memcpy(dst_row, tile_dst + int64_t(row) * tw * 4, size_t(tw) * 4 * sizeof(float));
      }
    }
  }

  if (r_stats != nullptr) {
    r_stats->elapsed_seconds = BLI_time_now_seconds() - start_time;
    r_stats->layers_evaluated = layers_evaluated;
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
    BKE_image_release_ibuf(entry.image, entry.ibuf, entry.lock);
  }
}

/**
 * Acquire the buffers of \a image_layers into \a r_layers for the linear evaluator, which takes
 * float buffers and constant layers. A folder is acquired through its children, and left out when
 * nothing it holds paints.
 */
static bool composite_layers_acquire(Span<PaintMaterialCompositeImageLayer> image_layers,
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
    else if (!composite_layer_has_content(image_layer, image_layer.color_image != nullptr)) {
      continue;
    }
    if (image_layer.color_image != nullptr) {
      layer.color_ibuf = composite_image_acquire(
          image_layer.color_image, image_layer.color_iuser, r_locks);
      if (layer.color_ibuf == nullptr) {
        return false;
      }
      /* The buffer's own colorspace, not the Image setting: the two can disagree, and the
       * evaluator has to decode what it is actually handed. */
      layer.color_colorspace_name = composite_buffer_colorspace_name(layer.color_ibuf);
    }
    if (image_layer.mask_image != nullptr) {
      layer.mask_ibuf = composite_image_acquire(
          image_layer.mask_image, image_layer.mask_iuser, r_locks);
      if (layer.mask_ibuf == nullptr) {
        return false;
      }
      layer.mask_colorspace_name = composite_buffer_colorspace_name(layer.mask_ibuf);
    }
    for (const PaintMaterialCompositeMaskItem &item : image_layer.mask_items) {
      PaintMaterialCompositeMaskBuffer buffer;
      buffer.constant_gray = item.constant_gray;
      buffer.opacity = item.opacity;
      if (item.image != nullptr) {
        buffer.ibuf = composite_image_acquire(item.image, item.iuser, r_locks);
        if (buffer.ibuf == nullptr) {
          return false;
        }
        buffer.colorspace_name = composite_buffer_colorspace_name(buffer.ibuf);
      }
      layer.mask_items.append(buffer);
    }
    static_cast<PaintMaterialCompositeRowParams &>(layer) = image_layer;
    r_layers.append(std::move(layer));
  }
  return true;
}

static bool composite_stack_build_linear(Span<PaintMaterialCompositeImageLayer> image_layers,
                                         Vector<CompositeImageLock> &r_locks,
                                         PaintMaterialCompositeStack &r_stack)
{
  if (!BKE_paint_material_composite_stack_dimensions(image_layers, r_stack.width, r_stack.height))
  {
    return false;
  }
  if (!composite_layers_acquire(image_layers, r_locks, r_stack.layers)) {
    return false;
  }
  return !r_stack.layers.is_empty();
}

bool BKE_paint_material_composite_eval_images(Span<PaintMaterialCompositeImageLayer> image_layers,
                                              ImBuf *composite_ibuf,
                                              const rcti *region,
                                              PaintMaterialCompositeEvalStats *r_stats,
                                              const float bottom_color[4])
{
  Vector<CompositeImageLock> locks;
  PaintMaterialCompositeStack stack;
  bool ok = composite_stack_build_linear(image_layers, locks, stack) &&
            composite_ibuf_is_byte_rgba(composite_ibuf, stack.width, stack.height);
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
      /* Scratch the size of the area, never of the frame: a small refresh costs the same on any
       * canvas. */
      Vector<float> linear(int64_t(BLI_rcti_size_x(&area)) * BLI_rcti_size_y(&area) * 4);
      ok = BKE_paint_material_composite_eval_linear(
          stack, PaintMaterialCompositeLinearTarget::packed(linear, area), region, r_stats);
      if (ok) {
        BKE_paint_material_composite_linear_to_byte(linear, area, *composite_ibuf);
      }
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
    const float bottom_color[4])
{
  Vector<CompositeImageLock> locks;
  PaintMaterialCompositeStack stack;
  bool ok = composite_stack_build_linear(image_layers, locks, stack);
  if (ok && bottom_color != nullptr) {
    copy_v4_v4(stack.bottom_color, bottom_color);
    stack.has_bottom_color = true;
  }
  if (ok) {
    ok = BKE_paint_material_composite_eval_linear(stack, target, region, r_stats);
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
    if (layer.color_image == nullptr) {
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
      hash = get_default_hash(hash, item.constant_gray);
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

static void composite_cache_enforce_budget(const CompositeCacheKey &keep)
{
  bke::paint_material_cache::enforce_budget(
      g_cache.entries, keep, COMPOSITE_CACHE_BUDGET_BYTES, composite_entry_size_in_bytes);
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
    for (const PaintMaterialCompositeMaskItem &item : layer.mask_items) {
      r_images.append(item.image);
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

ImBuf *BKE_paint_material_composite_cache_ensure(
    const Material &ma,
    const int channel,
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
  key.channel = channel;

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
  Vector<Image *> source_images;
  composite_stack_images_gather(image_layers, source_images);
  /* The composite is stored in the canvas's own texels, so a reported rectangle needs no mapping.
   */
  const bke::paint_material_cache::PollResult poll =
      bke::paint_material_cache::subscriptions_poll(entry.partial_update_users, source_images);
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
    if (!BKE_paint_material_composite_eval_images(image_layers,
                                                  entry.ibuf.get(),
                                                  region,
                                                  r_stats,
                                                  has_bottom ? bottom_color : nullptr))
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

bool BKE_paint_material_composite_cache_contains(const Material &ma, const int channel)
{
  CompositeCacheKey key;
  key.material_session_uid = ma.id.session_uid;
  key.channel = channel;
  return g_cache.entries.contains(key);
}

/** \} */

}  // namespace blender
