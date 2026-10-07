/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */


#pragma once

/**
 * Cross-check that the generated node tree expresses the same topology and formulas as the CPU
 * composite.
 *
 * This is *not* the "render == CPU" test: both sides here work in the map's byte space (the
 * interpreter reads p/255, the CPU mixes bytes), so a shader that decodes sRGB first would differ
 * on Base Color at partial coverage. That is the open B-4 item; until a float, shader-space CPU
 * path exists, "render EEVEE == CPU" stays open for Base Color with opacity < 1, Multiply and
 * Overlay.
 *
 * What it does catch is a mis-wired socket in the generator: the generator's own tests only assert
 * the tree's shape, and the composite tests only assert the CPU numbers. A tiny interpreter
 * evaluates the generated group at one pixel for the narrow set of nodes the generator emits --
 * Image Texture, MixRGB (through the same #ramp_blend the CPU uses), Math, SeparateXYZ, Value, RGB,
 * group input/output and the Normal Combine group -- and compares the result with
 * #BKE_paint_layers_composite_channel at the same pixel.
 */

#include "testing/testing.h"

#include "BKE_colorband.hh"
#include "BKE_global.hh"
#include "BKE_gtest_base.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_mesh_maps.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"

#include "paint_layers_generate_intern.hh"
#include "paint_layers_intern.hh"
#include "BKE_paint_layers_composite.hh"
#include "BKE_paint_layers_generate.hh"
#include "BKE_paint_material_composite.hh"
#include "BKE_paint_material_resolve.hh"

#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_math_interp.hh"
#include "BLI_math_vector.h"
#include "BLI_string.h"
#include "BLI_string.h"
#include "BLI_ustring.hh"
#include "BLI_uuid.h"

#include "DNA_colorband_types.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_scene_types.h"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "paint_material_composite_internal.hh"

#include "NOD_socket.hh"

#include <cmath>
#include <functional>

namespace blender::bke::tests {

namespace {

struct RGBA {
  float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
};

/** One channel's bit in #Material::paint_layers_channels. */
static constexpr uint16_t channel_bit(const eMaterialPaintChannel channel)
{
  return uint16_t(1) << int(channel);
}

/**
 * Add \a extra to the material's channel set, keeping the derived default. A Material row holds no
 * channel records, so the base it reads for a channel outside the default set (Specular, Alpha,
 * Emission, ...) has to be opted into explicitly in these fixtures to match the source it is built
 * from. Called after every row exists and before the first regenerate, so the set does not widen
 * the channel records a Paint/Fill row's default-apply would create.
 */
static void channel_set_extend(Material &ma, const uint16_t extra)
{
  BKE_paint_layers_channel_set_mask_set(ma, BKE_paint_layers_channel_set_mask_get(ma) | extra);
}

/** The generated tree's graph, evaluated at one pixel. */
class GraphInterpreter {
 public:
  bNode *instance = nullptr;
  bNodeTree *tree = nullptr;
  int x = 0;
  int y = 0;
  /**
   * The channel's reference grid the pixel `x`/`y` sit on, for reading a MESH_MAP atlas whose
   * Image Texture node is set to Extend. Zero keeps every image read directly at `x`/`y`, which is
   * what painted maps of the same resolution are.
   */
  int ref_width = 0;
  int ref_height = 0;
  /** The interpreter of the tree that holds `instance`; null at the root, whose instance inputs are
   * unlinked. */
  const GraphInterpreter *parent = nullptr;

  /** The value a group-input output socket carries: the instance's matching input socket. */
  RGBA instance_input(const char *identifier) const
  {
    if (instance == nullptr) {
      return {};
    }
    for (bNodeSocket &socket : instance->inputs) {
      if (socket.identifier != nullptr && STREQ(socket.identifier, identifier)) {
        return parent != nullptr ? parent->eval_socket(socket) : socket_default(socket);
      }
    }
    return {};
  }

  static RGBA socket_default(const bNodeSocket &socket)
  {
    RGBA out;
    if (socket.default_value == nullptr) {
      return out;
    }
    switch (socket.type) {
      case SOCK_FLOAT: {
        const float value = static_cast<const bNodeSocketValueFloat *>(socket.default_value)->value;
        out.r = out.g = out.b = out.a = value;
        break;
      }
      case SOCK_RGBA: {
        const float *value = static_cast<const bNodeSocketValueRGBA *>(socket.default_value)->value;
        out.r = value[0];
        out.g = value[1];
        out.b = value[2];
        out.a = value[3];
        break;
      }
      case SOCK_VECTOR: {
        const float *value =
            static_cast<const bNodeSocketValueVector *>(socket.default_value)->value;
        out.r = value[0];
        out.g = value[1];
        out.b = value[2];
        out.a = 1.0f;
        break;
      }
      default:
        break;
    }
    return out;
  }

  RGBA sample_image(Image *image, const char *output) const
  {
    return sample_image_uv(image, -1.0f, -1.0f, output);
  }

  /**
   * Read the Image Texture at the UV \a u/\a v, or -- when either is negative -- at the current
   * reference texel, the way the plain node reads it. A mapped chain (UV Map -> Mapping -> Image)
   * supplies the vector, so the read follows the mapping: bilinear, with Repeat edges, exactly the
   * wrap the CPU resample and the shader's REPEAT textures use. The colorspace, premultiply and
   * alpha stages are the sample's own, shared with the direct read.
   */
  RGBA sample_image_uv(Image *image, const float u_in, const float v_in, const char *output) const
  {
    RGBA out;
    if (image == nullptr) {
      return out;
    }
    ImageUser iuser;
    BKE_imageuser_default(&iuser);
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(image, &iuser, &lock);
    if (ibuf == nullptr) {
      return out;
    }
    const float u = (u_in >= 0.0f) ? u_in : (float(x) + 0.5f) / float(ibuf->x);
    const float v = (v_in >= 0.0f) ? v_in : (float(y) + 0.5f) / float(ibuf->y);
    /* Repeat edges: a UV outside [0, 1] wraps, then a bilinear read follows. */
    const float uu = u - floorf(u);
    const float vv = v - floorf(v);
    const float su = uu * float(ibuf->x) - 0.5f;
    const float sv = vv * float(ibuf->y) - 0.5f;
    const int channels = ibuf->channels == 0 ? 4 : ibuf->channels;
    auto wrap_texel = [&](int i, int n) {
      const int r = i % n;
      return r < 0 ? r + n : r;
    };
    auto read_texel = [&](const int tx, const int ty, float r_pixel[4]) {
      const int64_t offset = (int64_t(ty) * ibuf->x + tx) * channels;
      if (ibuf->byte_buffer.data != nullptr) {
        const uchar *p = ibuf->byte_data() + offset;
        r_pixel[0] = float(p[0]) / 255.0f;
        r_pixel[1] = float(p[1]) / 255.0f;
        r_pixel[2] = float(p[2]) / 255.0f;
        r_pixel[3] = channels == 4 ? float(p[3]) / 255.0f : 1.0f;
        if (ibuf->byte_buffer.colorspace != nullptr) {
          IMB_colormanagement_colorspace_to_scene_linear_v4(
              r_pixel, false, ibuf->byte_buffer.colorspace);
        }
      }
      else {
        const float *p = ibuf->float_buffer.data + offset;
        r_pixel[0] = p[0];
        r_pixel[1] = p[1];
        r_pixel[2] = p[2];
        r_pixel[3] = channels == 4 ? p[3] : 1.0f;
        if (r_pixel[3] > 0.0f) {
          const float inv = 1.0f / r_pixel[3];
          r_pixel[0] *= inv;
          r_pixel[1] *= inv;
          r_pixel[2] *= inv;
        }
        if (ibuf->float_buffer.colorspace != nullptr) {
          IMB_colormanagement_colorspace_to_scene_linear_v4(
              r_pixel, false, ibuf->float_buffer.colorspace);
        }
      }
    };
    float pixel[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    if (u_in >= 0.0f || v_in >= 0.0f) {
      /* A mapped read: bilinear over the wrapped coordinates. */
      const float a = su - floorf(su);
      const float b = sv - floorf(sv);
      const int x0 = int(floorf(su));
      const int y0 = int(floorf(sv));
      float t00[4], t10[4], t01[4], t11[4];
      read_texel(wrap_texel(x0, ibuf->x), wrap_texel(y0, ibuf->y), t00);
      read_texel(wrap_texel(x0 + 1, ibuf->x), wrap_texel(y0, ibuf->y), t10);
      read_texel(wrap_texel(x0, ibuf->x), wrap_texel(y0 + 1, ibuf->y), t01);
      read_texel(wrap_texel(x0 + 1, ibuf->x), wrap_texel(y0 + 1, ibuf->y), t11);
      for (const int k : IndexRange(4)) {
        pixel[k] = (1.0f - a) * (1.0f - b) * t00[k] + a * (1.0f - b) * t10[k] +
                   (1.0f - a) * b * t01[k] + a * b * t11[k];
      }
    }
    else {
      /* The direct read of the current reference texel; out of range stays empty, as the plain
       * Image Texture branch always did. */
      if (x >= ibuf->x || y >= ibuf->y) {
        BKE_image_release_ibuf(image, ibuf, lock);
        return out;
      }
      read_texel(x, y, pixel);
    }
    /* Straight alpha, as the generator's layer and mask maps are created (#IMA_ALPHA_STRAIGHT): the
     * Image Texture Alpha output is then exactly the stored alpha, like the CPU reads it. */
    const bool gpu_premul = (image->flag & IMA_GPU_LINEAR_PREMUL) != 0;
    if (gpu_premul && ibuf->byte_buffer.data != nullptr) {
      pixel[0] *= pixel[3];
      pixel[1] *= pixel[3];
      pixel[2] *= pixel[3];
    }
    BKE_image_release_ibuf(image, ibuf, lock);
    if (STREQ(output, "Alpha")) {
      /* The upload and the node leave alpha alone; only the stored alpha is read. */
      out.r = out.g = out.b = out.a = pixel[3];
      return out;
    }
    /* The Image Texture node's own alpha handling (node_shader_tex_image.cc): a data or
     * alpha-packed image is read as-is, while a premultiplied texture is un-premultiplied because
     * the chain also reads the Alpha output. A data buffer is never touched here. */
    const bool data_space =
        IMB_colormanagement_space_name_is_data(image->colorspace_settings.name) ||
        ELEM(image->alpha_mode, IMA_ALPHA_IGNORE, IMA_ALPHA_CHANNEL_PACKED);
    if (!data_space && (image->alpha_mode == IMA_ALPHA_PREMUL || gpu_premul) && pixel[3] > 0.0f &&
        pixel[3] != 1.0f)
    {
      const float inv = 1.0f / pixel[3];
      pixel[0] *= inv;
      pixel[1] *= inv;
      pixel[2] *= inv;
    }
    out.r = pixel[0];
    out.g = pixel[1];
    out.b = pixel[2];
    out.a = pixel[3];
    return out;
  }

  /**
   * Read an Extend-interpolated atlas Image Texture at the reference texel `x`/`y`: the same
   * bilinear, edge-clamped lookup the CPU's resample performs (the reference centre `(x + 0.5) /
   * W_ref` sampled at `u * W_atlas - 0.5`), so an atlas of any resolution agrees between the graph
   * and the CPU, and at equal sizes the read is the direct texel.
   */
  RGBA sample_image_extend(Image *image, const char *output) const
  {
    RGBA out;
    if (image == nullptr) {
      return out;
    }
    ImageUser iuser;
    BKE_imageuser_default(&iuser);
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(image, &iuser, &lock);
    if (ibuf == nullptr) {
      return out;
    }
    const float u = (float(x) + 0.5f) / float(ref_width);
    const float v = (float(y) + 0.5f) / float(ref_height);
    const float su = u * float(ibuf->x) - 0.5f;
    const float sv = v * float(ibuf->y) - 0.5f;
    float pixel[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    bool decoded = false;
    if (ibuf->byte_buffer.data != nullptr && ibuf->channels == 4) {
      const uchar4 c = math::interpolate_bilinear_byte(ibuf->byte_data(), ibuf->x, ibuf->y, su, sv);
      pixel[0] = float(c.x) / 255.0f;
      pixel[1] = float(c.y) / 255.0f;
      pixel[2] = float(c.z) / 255.0f;
      pixel[3] = float(c.w) / 255.0f;
      decoded = true;
      if (ibuf->byte_buffer.colorspace != nullptr) {
        IMB_colormanagement_colorspace_to_scene_linear_v4(
            pixel, false, ibuf->byte_buffer.colorspace);
      }
    }
    else if (ibuf->float_buffer.data != nullptr && ibuf->channels == 4) {
      const float4 c = math::interpolate_bilinear_fl(
          ibuf->float_buffer.data, ibuf->x, ibuf->y, su, sv);
      pixel[0] = c.x;
      pixel[1] = c.y;
      pixel[2] = c.z;
      pixel[3] = c.w;
      decoded = true;
      /* A float buffer is premultiplied: straighten it, then decode its colorspace. */
      if (pixel[3] > 0.0f) {
        const float inv = 1.0f / pixel[3];
        pixel[0] *= inv;
        pixel[1] *= inv;
        pixel[2] *= inv;
      }
      if (ibuf->float_buffer.colorspace != nullptr) {
        IMB_colormanagement_colorspace_to_scene_linear_v4(
            pixel, false, ibuf->float_buffer.colorspace);
      }
    }
    BKE_image_release_ibuf(image, ibuf, lock);
    if (!decoded) {
      return out;
    }
    if (STREQ(output, "Alpha")) {
      out.r = out.g = out.b = out.a = pixel[3];
      return out;
    }
    out.r = pixel[0];
    out.g = pixel[1];
    out.b = pixel[2];
    out.a = pixel[3];
    return out;
  }

  RGBA eval_socket(const bNodeSocket &socket) const
  {
    const Span<const bNodeLink *> links = socket.directly_linked_links();
    if (!links.is_empty() && links[0]->is_available()) {
      const bNodeLink &link = *links[0];
      return eval_output(*link.fromnode, link.fromsock->identifier);
    }
    const bNode &node = socket.owner_node();
    if (node.is_group_input() && socket.identifier != nullptr) {
      return instance_input(socket.identifier);
    }
    return socket_default(socket);
  }

  RGBA eval_output(const bNode &node, const char *out_identifier) const
  {
    if (node.is_group_input()) {
      return instance_input(out_identifier);
    }
    if (node.is_group()) {
      if (is_normal_combine(node)) {
        return eval_normal_combine(node);
      }
      return eval_group(node, out_identifier);
    }
    switch (node.type_legacy) {
      case SH_NODE_TEX_IMAGE: {
        /* The generator sets Extend only on a MESH_MAP atlas' node; sampled bilinearly through the
         * reference grid, like the CPU resample reads it. Painted maps keep the direct read.
         * A mapped chain (UV Map -> Mapping -> Image) feeds the Vector input: the read follows the
         * mapping, with the Repeat wrap the graph and the CPU agree on. */
        const NodeTexImage *storage = static_cast<const NodeTexImage *>(node.storage);
        if (storage != nullptr && storage->extension == SHD_IMAGE_EXTENSION_EXTEND &&
            ref_width > 0 && ref_height > 0)
        {
          return sample_image_extend(id_cast<Image *>(node.id), out_identifier);
        }
        bNode &mutable_node = const_cast<bNode &>(node);
        const bNodeSocket *vector = bke::node_find_socket(mutable_node, SOCK_IN, "Vector"_ustr);
        /* Only a Mapping in front moves the read; a bare UV Map feed is the reference texel, which
         * the direct read below already is (the UV Map node has no grid in some tests). */
        if (vector != nullptr && !vector->directly_linked_links().is_empty() &&
            vector->directly_linked_links()[0]->fromnode->type_legacy == SH_NODE_MAPPING)
        {
          const RGBA uv = eval_socket(*vector);
          return sample_image_uv(id_cast<Image *>(node.id), uv.r, uv.g, out_identifier);
        }
        return sample_image(id_cast<Image *>(node.id), out_identifier);
      }
      case SH_NODE_MAPPING: {
        /* Point only, the one type the generated chain builds: `R * (uv * scale) + offset`, the
         * rotation around Z. The output is a Vector; its fourth component stays 1. */
        bNode &mutable_node = const_cast<bNode &>(node);
        const RGBA uv = eval_socket(
            *bke::node_find_socket(mutable_node, SOCK_IN, "Vector"_ustr));
        const RGBA scale = eval_socket(
            *bke::node_find_socket(mutable_node, SOCK_IN, "Scale"_ustr));
        const RGBA location = eval_socket(
            *bke::node_find_socket(mutable_node, SOCK_IN, "Location"_ustr));
        const RGBA rotation = eval_socket(
            *bke::node_find_socket(mutable_node, SOCK_IN, "Rotation"_ustr));
        const float angle = rotation.b;
        const float cos_rot = cosf(angle);
        const float sin_rot = sinf(angle);
        if (node.custom1 == NODE_MAPPING_TYPE_VECTOR) {
          /* `R * (v * scale)`, no location; the rotation is around Z only (the generator never
           * sets X/Y). */
          const float vx = uv.r * scale.r;
          const float vy = uv.g * scale.g;
          return {vx * cos_rot - vy * sin_rot, vx * sin_rot + vy * cos_rot, uv.b * scale.b, 1.0f};
        }
        if (node.custom1 == NODE_MAPPING_TYPE_TEXTURE) {
          /* `(R^-1 * (v - location)) / scale`, the divide being the shader's safe divide. */
          const float vx = uv.r - location.r;
          const float vy = uv.g - location.g;
          auto safe = [](const float a, const float b) { return b != 0.0f ? a / b : 0.0f; };
          return {safe(vx * cos_rot + vy * sin_rot, scale.r),
                  safe(-vx * sin_rot + vy * cos_rot, scale.g),
                  safe(uv.b - location.b, scale.b),
                  1.0f};
        }
        if (node.custom1 != NODE_MAPPING_TYPE_POINT) {
          return uv;
        }
        const float sx = uv.r * scale.r;
        const float sy = uv.g * scale.g;
        return {sx * cos_rot - sy * sin_rot + location.r,
                sx * sin_rot + sy * cos_rot + location.g,
                uv.b * scale.b + location.b,
                1.0f};
      }
      case SH_NODE_UVMAP: {
        /* The generated stack reads one named UV layer. The tests run with a single UV layer and
         * sample the reference texel, so the layer's coordinates are the reference centre; the node
         * is otherwise a pass-through the Image Texture branch does not consult. */
        if (ref_width > 0 && ref_height > 0) {
          return {float(x + 0.5f) / float(ref_width),
                  float(y + 0.5f) / float(ref_height),
                  0.0f,
                  1.0f};
        }
        return {0.0f, 0.0f, 0.0f, 1.0f};
      }
      case SH_NODE_RGB:
      case SH_NODE_VALUE:
        if (const bNodeSocket *out = bke::node_find_socket(
                const_cast<bNode &>(node), SOCK_OUT, UString::from_ptr_noinline(out_identifier)))
        {
          return socket_default(*out);
        }
        return {};
      case NODE_REROUTE: {
        bNode &mutable_node = const_cast<bNode &>(node);
        bNodeSocket *input = bke::node_find_socket(mutable_node, SOCK_IN, "Input"_ustr);
        return (input != nullptr) ? eval_socket(*input) : RGBA{};
      }
      case SH_NODE_RGBTOBW: {
        /* Real Blender's own GPU implementation (node_shader_rgb_to_bw.cc) is exactly
         * #IMB_colormanagement_get_luminance_coefficients applied through GPU_stack_link -- the
         * same coefficients #IMB_colormanagement_get_luminance uses, so calling it here is not an
         * approximation but the node's own defining formula. */
        bNode &mutable_node = const_cast<bNode &>(node);
        const RGBA color = eval_socket(
            *bke::node_find_socket(mutable_node, SOCK_IN, "Color"_ustr));
        float rgb[3] = {color.r, color.g, color.b};
        const float value = IMB_colormanagement_get_luminance(rgb);
        return {value, value, value, 1.0f};
      }
      case SH_NODE_MIX: {
        bNode &mutable_node = const_cast<bNode &>(node);
        const NodeShaderMix &storage = *static_cast<const NodeShaderMix *>(node.storage);
        const RGBA bottom = eval_socket(
            *bke::node_find_socket(mutable_node, SOCK_IN, "A_Color"_ustr));
        const RGBA top = eval_socket(
            *bke::node_find_socket(mutable_node, SOCK_IN, "B_Color"_ustr));
        const float fac =
            eval_socket(*bke::node_find_socket(mutable_node, SOCK_IN, "Factor_Float"_ustr)).r;
        float result[4] = {bottom.r, bottom.g, bottom.b, bottom.a};
        const float top_rgba[4] = {top.r, top.g, top.b, top.a};
        /* The node clamps its factor and leaves the result alone, like the CPU. */
        ramp_blend(storage.blend_type, result, clamp_f(fac, 0.0f, 1.0f), top_rgba);
        return {result[0], result[1], result[2], result[3]};
      }
      case SH_NODE_MATH: {
        bNode &mutable_node = const_cast<bNode &>(node);
        const float a =
            eval_socket(*bke::node_find_socket(mutable_node, SOCK_IN, "Value"_ustr)).r;
        const float b =
            eval_socket(*bke::node_find_socket(mutable_node, SOCK_IN, "Value_001"_ustr)).r;
        float value = 0.0f;
        switch (node.custom1) {
          case NODE_MATH_ADD:
            value = a + b;
            break;
          case NODE_MATH_MULTIPLY:
            value = a * b;
            break;
          case NODE_MATH_DIVIDE:
            value = (b != 0.0f) ? a / b : 0.0f;
            break;
          case NODE_MATH_SUBTRACT:
            value = a - b;
            break;
          default:
            value = a;
            break;
        }
        return {value, value, value, value};
      }
      case SH_NODE_COMPOSE_COLOR_ALPHA: {
        bNode &mutable_node = const_cast<bNode &>(node);
        const RGBA color = eval_socket(
            *bke::node_find_socket(mutable_node, SOCK_IN, "Color"_ustr));
        const float alpha =
            eval_socket(*bke::node_find_socket(mutable_node, SOCK_IN, "Alpha"_ustr)).r;
        return {color.r, color.g, color.b, alpha};
      }
      case SH_NODE_COMBXYZ: {
        bNode &mutable_node = const_cast<bNode &>(node);
        const float x = eval_socket(*bke::node_find_socket(mutable_node, SOCK_IN, "X"_ustr)).r;
        const float y = eval_socket(*bke::node_find_socket(mutable_node, SOCK_IN, "Y"_ustr)).r;
        const float z = eval_socket(*bke::node_find_socket(mutable_node, SOCK_IN, "Z"_ustr)).r;
        return {x, y, z, 1.0f};
      }
      case SH_NODE_SEPXYZ: {
        bNode &mutable_node = const_cast<bNode &>(node);
        const RGBA vector = eval_socket(
            *bke::node_find_socket(mutable_node, SOCK_IN, "Vector"_ustr));
        float value = 0.0f;
        if (STREQ(out_identifier, "X")) {
          value = vector.r;
        }
        else if (STREQ(out_identifier, "Y")) {
          value = vector.g;
        }
        else if (STREQ(out_identifier, "Z")) {
          value = vector.b;
        }
        return {value, value, value, value};
      }
      case SH_NODE_VECTOR_MATH: {
        bNode &mutable_node = const_cast<bNode &>(node);
        const RGBA vector = eval_socket(
            *bke::node_find_socket(mutable_node, SOCK_IN, "Vector"_ustr));
        /* A VectorMath output is a Vector (three components). Read through a Color socket its
         * fourth component is 1.0, never the alpha of the value that was fed in; modelling it as
         * the input's alpha would hide a chain that drops a data map's alpha. */
        if (node.custom1 == NODE_VECTOR_MATH_NORMALIZE) {
          float normalized[3] = {vector.r, vector.g, vector.b};
          normalize_v3(normalized);
          return {normalized[0], normalized[1], normalized[2], 1.0f};
        }
        if (node.custom1 == NODE_VECTOR_MATH_SIGN) {
          auto sign = [](const float a) { return a > 0.0f ? 1.0f : (a < 0.0f ? -1.0f : 0.0f); };
          return {sign(vector.r), sign(vector.g), sign(vector.b), 1.0f};
        }
        if (node.custom1 == NODE_VECTOR_MATH_MULTIPLY_ADD) {
          const RGBA scale = eval_socket(
              *bke::node_find_socket(mutable_node, SOCK_IN, "Vector_001"_ustr));
          const RGBA offset = eval_socket(
              *bke::node_find_socket(mutable_node, SOCK_IN, "Vector_002"_ustr));
          return {vector.r * scale.r + offset.r,
                  vector.g * scale.g + offset.g,
                  vector.b * scale.b + offset.b,
                  1.0f};
        }
        if (node.custom1 == NODE_VECTOR_MATH_DIVIDE) {
          const RGBA divisor = eval_socket(
              *bke::node_find_socket(mutable_node, SOCK_IN, "Vector_001"_ustr));
          /* Blender's vector divide is a safe divide: zero where the divisor is zero. */
          auto safe = [](const float a, const float b) { return b != 0.0f ? a / b : 0.0f; };
          return {safe(vector.r, divisor.r),
                  safe(vector.g, divisor.g),
                  safe(vector.b, divisor.b),
                  1.0f};
        }
        return {vector.r, vector.g, vector.b, 1.0f};
      }
      case SH_NODE_VALTORGB: {
        /* A Color Ramp: the CPU composite never evaluates a source graph, so this only serves the
         * generated wrapper. #BKE_colorband_evaluate is exactly what the shader side runs. */
        bNode &mutable_node = const_cast<bNode &>(node);
        const ColorBand *band = static_cast<const ColorBand *>(node.storage);
        if (band == nullptr) {
          return {};
        }
        const float fac =
            eval_socket(*bke::node_find_socket(mutable_node, SOCK_IN, "Fac"_ustr)).r;
        float result[4];
        BKE_colorband_evaluate(band, fac, result);
        if (STREQ(out_identifier, "Alpha")) {
          return {result[3], result[3], result[3], result[3]};
        }
        return {result[0], result[1], result[2], result[3]};
      }
      case SH_NODE_NORMAL_MAP: {
        /* A Normal Map decodes its encoded Color input into a tangent-space vector. The generated
         * chain only reaches this through the SourceGroup wrapper's `COLOR:Normal` output. */
        bNode &mutable_node = const_cast<bNode &>(node);
        const auto &storage = *static_cast<const NodeShaderNormalMap *>(node.storage);
        const RGBA encoded = eval_socket(
            *bke::node_find_socket(mutable_node, SOCK_IN, "Color"_ustr));
        const float strength =
            eval_socket(*bke::node_find_socket(mutable_node, SOCK_IN, "Strength"_ustr)).r;
        float vector[3] = {encoded.r * 2.0f - 1.0f,
                           encoded.g * 2.0f - 1.0f,
                           encoded.b * 2.0f - 1.0f};
        if (storage.convention == SHD_NORMAL_MAP_CONVENTION_DIRECTX) {
          vector[1] = -vector[1];
        }
        vector[0] *= strength;
        vector[1] *= strength;
        normalize_v3(vector);
        return {vector[0], vector[1], vector[2], 1.0f};
      }
      default:
        return {};
    }
  }

  /** True for the Normal Combine group: told apart by its marker, not by an input name that a
   * layer group can also take. */
  static bool is_normal_combine(const bNode &node)
  {
    return BKE_paint_material_is_normal_combine_group(node);
  }

  /** The active Group Output node of \a tree, or the first one when none is flagged active. */
  static const bNode *active_group_output(const bNodeTree &tree)
  {
    const bNode *first = nullptr;
    for (const bNode &node : tree.nodes) {
      if (!node.is_group_output()) {
        continue;
      }
      if (first == nullptr) {
        first = &node;
      }
      if (node.flag & NODE_DO_OUTPUT) {
        return &node;
      }
    }
    return first;
  }

  /** Evaluate a nested group instance: its layers live in child trees, so descend into the group. */
  RGBA eval_group(const bNode &node, const char *out_identifier) const
  {
    if (node.id == nullptr || GS(node.id->name) != ID_NT) {
      return {};
    }
    bNodeTree *group = reinterpret_cast<bNodeTree *>(node.id);
    GraphInterpreter child;
    child.instance = const_cast<bNode *>(&node);
    child.tree = group;
    child.x = x;
    child.y = y;
    child.ref_width = ref_width;
    child.ref_height = ref_height;
    child.parent = this;
    group->ensure_topology_cache();
    group->ensure_interface_cache();
    const bNode *output = active_group_output(*group);
    if (output == nullptr) {
      return {};
    }
    if (const bNodeSocket *socket = bke::node_find_socket(
            const_cast<bNode &>(*output), SOCK_IN, UString::from_ptr_noinline(out_identifier)))
    {
      return child.eval_socket(*socket);
    }
    return {};
  }

  RGBA eval_normal_combine(const bNode &node) const
  {
    bNode &mutable_node = const_cast<bNode &>(node);
    const RGBA a = eval_socket(*bke::node_find_socket(
        mutable_node, SOCK_IN, UString::from_ptr_noinline(NORMAL_COMBINE_ID_A)));
    const RGBA b = eval_socket(*bke::node_find_socket(
        mutable_node, SOCK_IN, UString::from_ptr_noinline(NORMAL_COMBINE_ID_B)));
    const float fac = eval_socket(*bke::node_find_socket(
                                      mutable_node,
                                      SOCK_IN,
                                      UString::from_ptr_noinline(NORMAL_COMBINE_ID_FACTOR)))
                          .r;
    float base[3] = {a.r * 2.0f - 1.0f, a.g * 2.0f - 1.0f, a.b * 2.0f - 1.0f};
    float detail[3] = {b.r * 2.0f - 1.0f, b.g * 2.0f - 1.0f, b.b * 2.0f - 1.0f};
    float combined[3] = {
        base[0] + detail[0], base[1] + detail[1], base[2] * detail[2]};
    normalize_v3(combined);
    const float encoded[3] = {combined[0] * 0.5f + 0.5f, combined[1] * 0.5f + 0.5f,
                              combined[2] * 0.5f + 0.5f};
    RGBA out;
    out.r = a.r * (1.0f - fac) + encoded[0] * fac;
    out.g = a.g * (1.0f - fac) + encoded[1] * fac;
    out.b = a.b * (1.0f - fac) + encoded[2] * fac;
    out.a = a.a;
    return out;
  }

  /** The value of the channel wired to the group output named \a result_name. */
  RGBA eval_result(const char *result_name) const
  {
    if (tree == nullptr) {
      return {};
    }
    tree->ensure_interface_cache();
    const bNode *output = active_group_output(*tree);
    if (output == nullptr) {
      return {};
    }
    for (bNodeTreeInterfaceSocket *iface : tree->interface_outputs()) {
      if (iface->name == nullptr || !STREQ(iface->name, result_name) ||
          iface->identifier == nullptr)
      {
        continue;
      }
      if (const bNodeSocket *input = bke::node_find_socket(
              const_cast<bNode &>(*output), SOCK_IN, UString::from_ptr_noinline(iface->identifier)))
      {
        return eval_socket(*input);
      }
    }
    return {};
  }
};

}  // namespace

class PaintLayersGraphEvalTest : public bke::BlenderGTestBase {
 public:
  Main *bmain = nullptr;
  Material *ma = nullptr;

  void SetUp() override
  {
    bmain = BKE_main_new();
    G_MAIN = bmain;
  }

  void TearDown() override
  {
    BKE_main_free(bmain);
    G_MAIN = nullptr;
  }

  Image *add_solid_image(const char *name,
                         const int size,
                         const uchar r,
                         const uchar g,
                         const uchar b,
                         const uchar a)
  {
    const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    Image *image = BKE_image_add_generated(
        bmain, size, size, name, 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
    image->alpha_mode = IMA_ALPHA_STRAIGHT;
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
    uchar *pixels = ibuf->byte_data_for_write();
    for (const int64_t i : IndexRange(int64_t(size) * size)) {
      pixels[i * 4 + 0] = r;
      pixels[i * 4 + 1] = g;
      pixels[i * 4 + 2] = b;
      pixels[i * 4 + 3] = a;
    }
    BKE_image_release_ibuf(image, ibuf, lock);
    return image;
  }

  /** Mark \a image as data, so neither side decodes it as a colour. The image's own colorspace is
   * set too: the Image Texture node reads that, not the ImBuf's, when it decides whether to
   * un-premultiply. */
  static void make_image_data(Image *image)
  {
    const char *data_name = IMB_colormanagement_role_colorspace_name_get(COLOR_ROLE_DATA);
    BLI_strncpy(image->colorspace_settings.name, data_name, sizeof(image->colorspace_settings.name));
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
    if (ibuf != nullptr) {
      IMB_colormanagement_assign_byte_colorspace(ibuf, data_name);
      BKE_image_release_ibuf(image, ibuf, lock);
    }
  }

  MaterialPaintLayer *add_layer(const char *name,
                                const eMaterialPaintLayerSource source,
                                Image *image,
                                const eMaterialPaintChannel channel =
                                    PAINT_MATERIAL_CHANNEL_BASE_COLOR)
  {
    MaterialPaintLayer *layer = BKE_paint_layers_add(
        *ma, source, name, nullptr, PaintLayerPlace::Above);
    EXPECT_NE(layer, nullptr);
    MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(*ma, layer, channel);
    EXPECT_NE(record, nullptr);
    record->image = image;
    record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
    return layer;
  }

  /**
   * Add a Layer-role row directly into \a anchor's own #children -- a Stack correction/mask item
   * (#BKE_paint_layers_add accepts \a place Into there since it is a folder in every sense
   * #BKE_paint_layers_is_folder cares about, regardless of \a anchor's role) or an ordinary folder.
   * Mirrors #add_layer, which always anchors at the top level instead.
   */
  MaterialPaintLayer *add_layer_into(MaterialPaintLayer *anchor,
                                     const char *name,
                                     const eMaterialPaintLayerSource source,
                                     Image *image,
                                     const eMaterialPaintChannel channel =
                                         PAINT_MATERIAL_CHANNEL_BASE_COLOR)
  {
    MaterialPaintLayer *layer = BKE_paint_layers_add(
        *ma, source, name, anchor, PaintLayerPlace::Into);
    EXPECT_NE(layer, nullptr);
    MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(*ma, layer, channel);
    EXPECT_NE(record, nullptr);
    record->image = image;
    record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
    return layer;
  }

  /** Give \a layer a mask item whose map is \a image, as a Mask-mode stroke would leave it. */
  MaterialPaintLayer *set_mask_image(MaterialPaintLayer &layer, Image *image)
  {
    MaterialPaintLayer *item = BKE_paint_layers_mask_add(*ma, &layer, 1.0f);
    EXPECT_NE(item, nullptr);
    MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
        *ma, item, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
    EXPECT_NE(record, nullptr);
    /* Every map the generator reaches through a correction is GPU-linear: the texture upload
     * pre-multiplies its straight bytes, and the chain's Divide undoes that. */
    image->alpha_mode = IMA_ALPHA_STRAIGHT;
    image->flag |= IMA_GPU_LINEAR_PREMUL;
    record->image = image;
    record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
    BKE_paint_layers_correction_source_set(*ma, item, MA_PAINT_LAYER_SOURCE_IMAGE);
    return item;
  }

  static const char *result_name(const int channel)
  {
    return (channel == PAINT_MATERIAL_CHANNEL_NORMAL) ? "Result Normal" : "Result Base Color";
  }

  bNode *find_instance()
  {
    if (ma->nodetree == nullptr || ma->paint_layers_tree == nullptr) {
      return nullptr;
    }
    for (bNode &node : ma->nodetree->nodes) {
      if (node.id == &ma->paint_layers_tree->id) {
        return &node;
      }
    }
    return nullptr;
  }

  /** CPU composite of the channel at pixel (1, 1), in scene-linear float, as the shader sees it. */
  RGBA cpu_pixel(const int channel)
  {
    return cpu_pixel_at(channel, 1, 1);
  }

  /** CPU composite of the channel at (x, y), in scene-linear float, as the shader sees it. */
  RGBA cpu_pixel_at(const int channel, const int px, const int py)
  {
    Vector<PaintMaterialCompositeImageLayer> layers;
    EXPECT_TRUE(BKE_paint_layers_composite_image_layers(*ma, channel, layers));
    int width = 0, height = 0;
    EXPECT_TRUE(BKE_paint_material_composite_stack_dimensions(layers, width, height));
    Vector<float> linear(int64_t(width) * height * 4);
    float bottom[4];
    BKE_paint_layers_channel_bottom_color(eMaterialPaintChannel(channel), bottom);
    EXPECT_TRUE(BKE_paint_material_composite_eval_images_linear(
        layers, linear.data(), nullptr, nullptr, bottom));
    float *p = linear.data() + (int64_t(py) * width + px) * 4;
    RGBA out = {p[0], p[1], p[2], p[3]};
    if (channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
      /* The generated chain ends with the same decode-normalize-encode; the CPU applies it after
       * the last row, which the raw linear evaluation does not know about. */
      float normal[3] = {out.r * 2.0f - 1.0f, out.g * 2.0f - 1.0f, out.b * 2.0f - 1.0f};
      normalize_v3(normal);
      out.r = normal[0] * 0.5f + 0.5f;
      out.g = normal[1] * 0.5f + 0.5f;
      out.b = normal[2] * 0.5f + 0.5f;
    }
    return out;
  }

  /** A float Non-Color atlas of \a width x \a height whose R is a known gradient along x (G, B
   * stay zero, the way a scalar geometry map writes only R) and whose alpha is one. */
  Image *add_gradient_atlas(const char *name, const int width, const int height)
  {
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    Image *image = BKE_image_add_generated(
        bmain, width, height, name, 32, /*floatbuf=*/true, IMA_GENTYPE_BLANK, black, false, true, false);
    EXPECT_NE(image, nullptr);
    image->alpha_mode = IMA_ALPHA_STRAIGHT;
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
    EXPECT_NE(ibuf, nullptr);
    if (ibuf != nullptr) {
      if (ibuf->float_data() != nullptr) {
        float *pixels = ibuf->float_data_for_write();
        for (const int y : IndexRange(height)) {
          for (const int x : IndexRange(width)) {
            float *p = pixels + (int64_t(y) * width + x) * 4;
            p[0] = float(x) / float(width - 1);
            p[1] = 0.0f;
            p[2] = 0.0f;
            p[3] = 1.0f;
          }
        }
      }
      BKE_image_release_ibuf(image, ibuf, lock);
    }
    return image;
  }

  /** A MESH_MAP row painting the material's shared atlas of \a type in Base Color, with \a opacity
   * set like a user's row. The slot image is assigned by the caller. */
  MaterialPaintLayer *add_mesh_map_layer(const char *name,
                                         const eMaterialMeshMapType type,
                                         const float opacity = 1.0f)
  {
    MaterialPaintLayer *layer = BKE_paint_layers_add(
        *ma, MA_PAINT_LAYER_SOURCE_MESH_MAP, name, nullptr, PaintLayerPlace::Above);
    EXPECT_NE(layer, nullptr);
    EXPECT_NE(BKE_paint_layers_channel_add(*ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
              nullptr);
    EXPECT_TRUE(BKE_paint_layers_mesh_map_type_set(*ma, layer, type));
    if (opacity != 1.0f) {
      EXPECT_TRUE(BKE_paint_layers_set_opacity(*ma, layer, opacity));
    }
    return layer;
  }

  /** Defined after the shared test helpers, which this method builds on. */
  void check_iso_folder_two_map_channel(eMaterialPaintChannel channel,
                                        const char *material_name,
                                        const char *folder_name,
                                        uchar bottom_r,
                                        uchar bottom_g,
                                        uchar bottom_b,
                                        float folder_opacity);
};

/** \name Reusable Material sources for the multi-row stack tests
 *
 * A and C put the Principled inside a nested group and drive it through the group interface, a
 * Reroute, an Image Texture and a Color Ramp, so every wired channel resolves to a graph and the
 * row must use the SourceGroup wrapper. B keeps the Principled at the top level with constant
 * sockets, a live Alpha and a Normal Map over a flat data texture: every channel is a constant or
 * a trivial map, so the row uses Hybrid.
 * \{ */

static RGBA eval_channel_result(const GraphInterpreter &interpreter,
                                eMaterialPaintChannel channel);
static Image *make_bake_data_map(Main *bmain, const char *name, const int size, const float rgba[4]);

/** The construction of a grouped source: a constant Base Color mixed with a 2x2 data map, a Color
 * Ramp over the Roughness input, constant Specular and an instance-default Metallic. */
struct GroupSourceSpec {
  const float base_color[3];
  /** Row-major 2x2 map pixels (r, g, b) in data space. */
  const float map_pixels[4][3];
  const float rough_in;
  /** The Color Ramp's alpha at positions 0 and 1. */
  const float ramp_alpha0;
  const float ramp_alpha1;
  const float specular_in;
  const float metallic_default;
};

/** The construction of a top-level source: constants on every scalar/colour channel, a live Alpha
 * and a flat Normal Map over a 2x2 data texture. */
struct HybridSourceSpec {
  const float base_color[3];
  const float metallic;
  const float roughness;
  const float specular;
  const float alpha;
  const float emission[3];
};

static const GroupSourceSpec source_spec_a = {
    {0.2f, 0.5f, 0.9f},
    {{0.1f, 0.2f, 0.3f}, {0.4f, 0.5f, 0.6f}, {0.7f, 0.8f, 0.9f}, {0.9f, 0.1f, 0.4f}},
    0.3f,
    0.2f,
    0.9f,
    0.25f,
    0.7f,
};

static const GroupSourceSpec source_spec_c = {
    {0.3f, 0.1f, 0.7f},
    {{0.9f, 0.8f, 0.1f}, {0.6f, 0.2f, 0.5f}, {0.1f, 0.6f, 0.2f}, {0.4f, 0.9f, 0.3f}},
    0.6f,
    0.7f,
    0.1f,
    0.5f,
    0.2f,
};

static const HybridSourceSpec source_spec_b = {
    {0.15f, 0.30f, 0.60f},
    0.80f,
    0.42f,
    0.20f,
    0.60f,
    {0.05f, 0.10f, 0.40f},
};

}  // namespace blender::bke::tests
