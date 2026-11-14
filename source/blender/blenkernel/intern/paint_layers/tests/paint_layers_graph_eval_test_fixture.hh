/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/**
 * Cross-check that the generated node tree expresses the same formulas as the CPU composite.
 *
 * This is *not* the "render == CPU" test: a tiny interpreter evaluates the generated group at one
 * pixel for the narrow set of nodes the generator emits -- Image Texture (read at the pixel, or
 * through a Mapping), Texture Coordinate, Mapping, Mix (through the same #ramp_blend the CPU uses),
 * Math (with Floor and Compare, the UV wrap's clip factor), Vector Math, Value, RGB, Group Input,
 * Hue/Saturation/Value, Gamma, Brightness/Contrast, group input/output and the Normal Combine
 * group -- and the result
 * is compared with #BKE_paint_layers_composite_image_layers
 * evaluated at the same pixel.
 *
 * What it catches is a mis-wired socket in the generator: the generator's own tests only assert the
 * tree's shape, and the composite tests only assert the CPU numbers.
 */

#include "testing/testing.h"

#include "BKE_global.hh"
#include "BKE_gtest_base.hh"
#include "BKE_colorband.hh"
#include "BKE_colortools.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_composite.hh"
#include "BKE_paint_layers_generate.hh"
#include "BKE_paint_layers_generator.hh"
#include "BKE_paint_material_composite.hh"

#include "BLI_hash.h"
#include "BLI_index_range.hh"
#include <algorithm>

#include "BLI_math_base.h"
#include "BLI_math_color.h"
#include "BLI_math_vector.h"
#include "BLI_noise.hh"
#include "BLI_procedural_texture.hh"
#include "BLI_string.h"
#include "BLI_ustring.hh"
#include "BLI_vector.hh"

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_scene_types.h"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "../../paint_material_composite_internal.hh"
#include "../paint_layers_intern.hh"
#include "../paint_material_generator_group_ids.hh"

namespace blender::bke::tests {

struct RGBA {
  float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
};

/** The generated tree's graph, evaluated at one pixel. */
class GraphInterpreter {
 public:
  bNode *instance = nullptr;
  bNodeTree *tree = nullptr;
  int x = 0;
  int y = 0;
  /** The interpreter of the tree that holds `instance`; null at the root, whose instance inputs are
   * unlinked. */
  const GraphInterpreter *parent = nullptr;
  /** What an Attribute node reading the object's mask record returns, the shader's per-object
   * value: 0 for a missing record, which is "nothing hidden". */
  float object_mask_value = 0.0f;

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
      case SOCK_INT: {
        /* The blend group's Mode: an integer value the compare nodes read as a float. */
        const int value = static_cast<const bNodeSocketValueInt *>(socket.default_value)->value;
        out.r = out.g = out.b = out.a = float(value);
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

  /** The Image Texture read at the reference texel `x`/`y`, decoded to scene linear. */
  RGBA sample_image(Image *image, const char *output) const
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
    if (x >= ibuf->x || y >= ibuf->y) {
      BKE_image_release_ibuf(image, ibuf, lock);
      return out;
    }
    if (ibuf->byte_buffer.data == nullptr && ibuf->float_buffer.data != nullptr) {
      /* A float map is stored premultiplied; the node hands out the straight colour. A data map
       * (the Height one) is not converted, and neither is a scene-linear one. */
      const int channels = ibuf->channels == 0 ? 4 : ibuf->channels;
      const float *p = ibuf->float_buffer.data + (int64_t(y) * ibuf->x + x) * channels;
      const float alpha = channels == 4 ? p[3] : 1.0f;
      const float inv = alpha > 0.0f ? 1.0f / alpha : 0.0f;
      BKE_image_release_ibuf(image, ibuf, lock);
      if (STREQ(output, "Alpha")) {
        out.r = out.g = out.b = out.a = alpha;
        return out;
      }
      out.r = p[0] * inv;
      out.g = (channels > 1 ? p[1] : p[0]) * inv;
      out.b = (channels > 2 ? p[2] : p[0]) * inv;
      out.a = alpha;
      return out;
    }
    if (ibuf->byte_buffer.data == nullptr) {
      BKE_image_release_ibuf(image, ibuf, lock);
      return out;
    }
    const int channels = ibuf->channels == 0 ? 4 : ibuf->channels;
    const uchar *p = ibuf->byte_data() + (int64_t(y) * ibuf->x + x) * channels;
    float pixel[4] = {float(p[0]) / 255.0f,
                      float(p[1]) / 255.0f,
                      float(p[2]) / 255.0f,
                      channels == 4 ? float(p[3]) / 255.0f : 1.0f};
    if (ibuf->byte_buffer.colorspace != nullptr) {
      IMB_colormanagement_colorspace_to_scene_linear_v4(
          pixel, false, ibuf->byte_buffer.colorspace);
    }
    BKE_image_release_ibuf(image, ibuf, lock);
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

  /**
   * The Image Texture read bilinearly at `(u, v)` with Repeat edges and the texel-centre
   * convention, the way the node samples a mapped coordinate: the taps are decoded to scene
   * linear and filtered premultiplied, and the colour is straightened again on the way out.
   */
  RGBA sample_image_at(Image *image, float u, float v, const char *output) const
  {
    RGBA out;
    if (image == nullptr) {
      return out;
    }
    ImageUser iuser;
    BKE_imageuser_default(&iuser);
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(image, &iuser, &lock);
    if (ibuf == nullptr || ibuf->byte_buffer.data == nullptr) {
      BKE_image_release_ibuf(image, ibuf, lock);
      return out;
    }
    u -= floorf(u);
    v -= floorf(v);
    const float su = u * float(ibuf->x) - 0.5f;
    const float sv = v * float(ibuf->y) - 0.5f;
    const int x0 = int(floorf(su));
    const int y0 = int(floorf(sv));
    const float fx = su - float(x0);
    const float fy = sv - float(y0);
    const auto wrap = [](const int i, const int n) { return ((i % n) + n) % n; };
    float sum[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    for (const int dy : {0, 1}) {
      for (const int dx : {0, 1}) {
        const int tx = wrap(x0 + dx, ibuf->x);
        const int ty = wrap(y0 + dy, ibuf->y);
        const uchar *p = ibuf->byte_data() + (int64_t(ty) * ibuf->x + tx) * 4;
        float tap[4] = {float(p[0]) / 255.0f,
                        float(p[1]) / 255.0f,
                        float(p[2]) / 255.0f,
                        float(p[3]) / 255.0f};
        if (ibuf->byte_buffer.colorspace != nullptr) {
          IMB_colormanagement_colorspace_to_scene_linear_v4(
              tap, false, ibuf->byte_buffer.colorspace);
        }
        const float weight = (dx != 0 ? fx : 1.0f - fx) * (dy != 0 ? fy : 1.0f - fy);
        for (const int k : IndexRange(3)) {
          sum[k] += tap[k] * tap[3] * weight;
        }
        sum[3] += tap[3] * weight;
      }
    }
    BKE_image_release_ibuf(image, ibuf, lock);
    if (STREQ(output, "Alpha")) {
      out.r = out.g = out.b = out.a = sum[3];
      return out;
    }
    const float inv = sum[3] > 0.0f ? 1.0f / sum[3] : 0.0f;
    out.r = sum[0] * inv;
    out.g = sum[1] * inv;
    out.b = sum[2] * inv;
    out.a = sum[3];
    return out;
  }

  /** The reference grid is 4x4, the size of the fixture's solid images. */
  static constexpr float kGridSize = 4.0f;

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
    if (node.is_type("NodeImplicitConversion"_ustr)) {
      /* The tree update inserts one when the blend group's integer Mode feeds a float Math input;
       * the conversion is the value itself in every type this interpreter reads. */
      const Span<const bNodeSocket *> inputs = node.input_sockets();
      if (!inputs.is_empty()) {
        return eval_socket(*inputs[0]);
      }
      return {};
    }
    if (node.is_group_input()) {
      return instance_input(out_identifier);
    }
    if (node.is_group()) {
      if (BKE_paint_material_is_normal_combine_group(node)) {
        return eval_normal_combine(node);
      }
      return eval_group(node, out_identifier);
    }
    bNode &mutable_node = const_cast<bNode &>(node);
    switch (node.type_legacy) {
      case SH_NODE_TEX_IMAGE: {
        const bNodeSocket *vector = bke::node_find_socket(mutable_node, SOCK_IN, "Vector"_ustr);
        if (vector != nullptr && !vector->directly_linked_links().is_empty()) {
          const RGBA uv = eval_socket(*vector);
          return sample_image_at(id_cast<Image *>(node.id), uv.r, uv.g, out_identifier);
        }
        return sample_image(id_cast<Image *>(node.id), out_identifier);
      }
      case SH_NODE_TEX_COORD:
        /* The reference texel's centre in UV space. The projection chains read this as the Object
         * output too: the reference surface is the flat grid whose object XY is its UV. */
        return {(float(x) + 0.5f) / kGridSize, (float(y) + 0.5f) / kGridSize, 0.0f, 1.0f};
      case SH_NODE_TEX_WHITE_NOISE: {
        /* The deband's hash family at the reference texel, the W carrying which pass reads: a
         * noise stable for the texel, of the same shape the CPU composite lays. The shader itself
         * hashes the UV, so the parity between the graph and the CPU stays a tolerance and no
         * closer; the exact pair is the CPU's own to answer. */
        const RGBA vector = eval_socket(
            *bke::node_find_socket(mutable_node, SOCK_IN, "Vector"_ustr));
        const float w = eval_socket(*bke::node_find_socket(mutable_node, SOCK_IN, "W"_ustr)).r;
        const uint32_t noise = BLI_hash_int_2d(
            uint32_t(x) + uint32_t(int(w) + 1) * 7919u,
            uint32_t(y) + uint32_t(int(vector.r * kGridSize)) * 6151u);
        const float value = float(noise) / 4294967296.0f;
        return {value, value, value, 1.0f};
      }
      case SH_NODE_NEW_GEOMETRY:
        /* The reference surface's normal: the flat +Z grid the facing and the Box/Triplanar
         * weights read. */
        if (STREQ(out_identifier, "Normal")) {
          return {0.0f, 0.0f, 1.0f, 1.0f};
        }
        return {};
      case SH_NODE_VECT_TRANSFORM:
        /* One space in the interpreter's convention: the world and the object coincide, so every
         * transform the generated chains build is the identity. */
        return eval_socket(*bke::node_find_socket(mutable_node, SOCK_IN, "Vector"_ustr));
      case SH_NODE_COMBXYZ: {
        const RGBA x = eval_socket(*bke::node_find_socket(mutable_node, SOCK_IN, "X"_ustr));
        const RGBA y = eval_socket(*bke::node_find_socket(mutable_node, SOCK_IN, "Y"_ustr));
        const RGBA z = eval_socket(*bke::node_find_socket(mutable_node, SOCK_IN, "Z"_ustr));
        return {x.r, y.r, z.r, 1.0f};
      }
      case SH_NODE_MAPPING: {
        const RGBA vector = eval_socket(
            *bke::node_find_socket(mutable_node, SOCK_IN, "Vector"_ustr));
        const RGBA location = eval_socket(
            *bke::node_find_socket(mutable_node, SOCK_IN, "Location"_ustr));
        const RGBA rotation = eval_socket(
            *bke::node_find_socket(mutable_node, SOCK_IN, "Rotation"_ustr));
        const RGBA scale = eval_socket(*bke::node_find_socket(mutable_node, SOCK_IN, "Scale"_ustr));
        const float cos_z = cosf(rotation.b);
        const float sin_z = sinf(rotation.b);
        if (node.custom1 == NODE_MAPPING_TYPE_TEXTURE) {
          /* `(R^-1 * (v - location)) / scale`. */
          const float vx = vector.r - location.r;
          const float vy = vector.g - location.g;
          const float rx = cos_z * vx + sin_z * vy;
          const float ry = -sin_z * vx + cos_z * vy;
          return {scale.r != 0.0f ? rx / scale.r : 0.0f,
                  scale.g != 0.0f ? ry / scale.g : 0.0f,
                  scale.b != 0.0f ? (vector.b - location.b) / scale.b : 0.0f,
                  1.0f};
        }
        /* Point: `v * scale + location`. The generated chain turns the vector in a Vector Rotate
         * in front of this node, so the node's own Rotation stays at zero. */
        return {vector.r * scale.r + location.r,
                vector.g * scale.g + location.g,
                vector.b * scale.b + location.b,
                1.0f};
      }
      case SH_NODE_VECTOR_ROTATE: {
        /* Euler Z only, the one the generated chain uses: `R(angle) * v`, or its inverse. */
        const RGBA vector = eval_socket(
            *bke::node_find_socket(mutable_node, SOCK_IN, "Vector"_ustr));
        const RGBA rotation = eval_socket(
            *bke::node_find_socket(mutable_node, SOCK_IN, "Rotation"_ustr));
        const float angle = node.custom2 ? -rotation.b : rotation.b;
        const float c = cosf(angle);
        const float s = sinf(angle);
        return {c * vector.r - s * vector.g, s * vector.r + c * vector.g, vector.b, 1.0f};
      }
      case SH_NODE_ATTRIBUTE:
        return {object_mask_value, object_mask_value, object_mask_value, object_mask_value};
      case SH_NODE_SEPARATE_COLOR: {
        /* The height modes' colour-to-scalar read: the red, the value every component of a grey
         * height map carries. */
        const RGBA color = eval_socket(
            *bke::node_find_socket(mutable_node, SOCK_IN, "Color"_ustr));
        const float value = STREQ(out_identifier, "Green") ? color.g :
                            (STREQ(out_identifier, "Blue") ? color.b :
                             (STREQ(out_identifier, "Alpha") ? color.a : color.r));
        return {value, value, value, value};
      }
      case SH_NODE_RGB:
      case SH_NODE_VALUE:
        if (const bNodeSocket *out = bke::node_find_socket(
                mutable_node, SOCK_OUT, UString::from_ptr_noinline(out_identifier)))
        {
          return socket_default(*out);
        }
        return {};
      case SH_NODE_MIX_RGB_LEGACY: {
        /* The runtime blend group's selector: a plain Mix of the branch chosen so far and the next
         * one, exactly what the node's own GPU function does. */
        const float fac =
            eval_socket(*bke::node_find_socket(mutable_node, SOCK_IN, "Fac"_ustr)).r;
        const RGBA color1 = eval_socket(
            *bke::node_find_socket(mutable_node, SOCK_IN, "Color1"_ustr));
        const RGBA color2 = eval_socket(
            *bke::node_find_socket(mutable_node, SOCK_IN, "Color2"_ustr));
        float result[4] = {color1.r, color1.g, color1.b, color1.a};
        const float top_rgba[4] = {color2.r, color2.g, color2.b, color2.a};
        BKE_paint_material_blend_apply(node.custom1, result, clamp_f(fac, 0.0f, 1.0f), top_rgba);
        return {result[0], result[1], result[2], result[3]};
      }
      case SH_NODE_MIX: {
        const NodeShaderMix &storage = *static_cast<const NodeShaderMix *>(node.storage);
        if (storage.data_type == SOCK_FLOAT) {
          /* The mask's factor: a plain interpolation of two scalars by a clamped factor. */
          const float a =
              eval_socket(*bke::node_find_socket(mutable_node, SOCK_IN, "A_Float"_ustr)).r;
          const float b =
              eval_socket(*bke::node_find_socket(mutable_node, SOCK_IN, "B_Float"_ustr)).r;
          const float fac = clamp_f(
              eval_socket(*bke::node_find_socket(mutable_node, SOCK_IN, "Factor_Float"_ustr)).r,
              0.0f,
              1.0f);
          const float value = a * (1.0f - fac) + b * fac;
          return {value, value, value, value};
        }
        if (storage.data_type == SOCK_VECTOR) {
          /* The node's Vector mode is a plain interpolation, per component when the factor is
           * non-uniform: the blend group's per-channel choice of two branches. */
          const RGBA a = eval_socket(*bke::node_find_socket(mutable_node, SOCK_IN, "A_Vector"_ustr));
          const RGBA b = eval_socket(*bke::node_find_socket(mutable_node, SOCK_IN, "B_Vector"_ustr));
          RGBA fac;
          if (storage.factor_mode == NODE_MIX_MODE_NON_UNIFORM) {
            fac = eval_socket(*bke::node_find_socket(mutable_node, SOCK_IN, "Factor_Vector"_ustr));
          }
          else {
            const float value =
                eval_socket(*bke::node_find_socket(mutable_node, SOCK_IN, "Factor_Float"_ustr)).r;
            fac = {value, value, value, value};
          }
          if (storage.clamp_factor) {
            fac.r = clamp_f(fac.r, 0.0f, 1.0f);
            fac.g = clamp_f(fac.g, 0.0f, 1.0f);
            fac.b = clamp_f(fac.b, 0.0f, 1.0f);
          }
          return {a.r * (1.0f - fac.r) + b.r * fac.r,
                  a.g * (1.0f - fac.g) + b.g * fac.g,
                  a.b * (1.0f - fac.b) + b.b * fac.b,
                  1.0f};
        }
        const RGBA bottom = eval_socket(
            *bke::node_find_socket(mutable_node, SOCK_IN, "A_Color"_ustr));
        const RGBA top = eval_socket(
            *bke::node_find_socket(mutable_node, SOCK_IN, "B_Color"_ustr));
        const float fac =
            eval_socket(*bke::node_find_socket(mutable_node, SOCK_IN, "Factor_Float"_ustr)).r;
        float result[4] = {bottom.r, bottom.g, bottom.b, bottom.a};
        const float top_rgba[4] = {top.r, top.g, top.b, top.a};
        /* The node clamps its factor and leaves the result alone, like the CPU. */
        BKE_paint_material_blend_apply(
            storage.blend_type, result, clamp_f(fac, 0.0f, 1.0f), top_rgba);
        return {result[0], result[1], result[2], result[3]};
      }
      case SH_NODE_MATH: {
        const float a =
            eval_socket(*bke::node_find_socket(mutable_node, SOCK_IN, "Value"_ustr)).r;
        const float b =
            eval_socket(*bke::node_find_socket(mutable_node, SOCK_IN, "Value_001"_ustr)).r;
        float value = a;
        switch (node.custom1) {
          case NODE_MATH_ADD:
            value = a + b;
            break;
          case NODE_MATH_MULTIPLY:
            value = a * b;
            break;
          case NODE_MATH_SUBTRACT:
            value = a - b;
            break;
          case NODE_MATH_DIVIDE:
            value = (b != 0.0f) ? a / b : 0.0f;
            break;
          case NODE_MATH_FLOOR:
            value = floorf(a);
            break;
          case NODE_MATH_POWER:
            /* The node's own rule (its CPU eval reads #safe_powf): a negative base only carries an
             * integer exponent; written out here so the CPU filter is not its own witness. */
            value = (a < 0.0f && b != int(b)) ? 0.0f : powf(a, b);
            break;
          case NODE_MATH_LOGARITHM:
            /* The node's own rule (its CPU eval reads #safe_logf): a non-positive base or argument
             * reads zero, and so does a base of one, whose logarithm divides by zero. */
            value = (a > 0.0f && b > 0.0f && logf(b) != 0.0f) ? logf(a) / logf(b) : 0.0f;
            break;
          case NODE_MATH_SQRT:
            value = sqrtf(a);
            break;
          case NODE_MATH_MINIMUM:
            value = std::min(a, b);
            break;
          case NODE_MATH_MAXIMUM:
            value = std::max(a, b);
            break;
          case NODE_MATH_LESS_THAN:
            value = (a < b) ? 1.0f : 0.0f;
            break;
          case NODE_MATH_GREATER_THAN:
            value = (a > b) ? 1.0f : 0.0f;
            break;
          case NODE_MATH_FLOORED_MODULO:
            value = (b != 0.0f) ? a - floorf(a / b) * b : 0.0f;
            break;
          case NODE_MATH_COMPARE: {
            /* One when the two differ by no more than the epsilon, the node's own rule. */
            const float epsilon =
                eval_socket(*bke::node_find_socket(mutable_node, SOCK_IN, "Value_002"_ustr)).r;
            value = (fabsf(a - b) <= std::max(epsilon, 1e-5f)) ? 1.0f : 0.0f;
            break;
          }
          case NODE_MATH_MULTIPLY_ADD: {
            /* The affine the frame chains build: `a * b + c`. */
            const float c = eval_socket(
                *bke::node_find_socket(mutable_node, SOCK_IN, "Value_002"_ustr)).r;
            value = a * b + c;
            break;
          }
          case NODE_MATH_ABSOLUTE:
            value = fabsf(a);
            break;
          case NODE_MATH_ARCTAN2:
            value = (a == 0.0f && b == 0.0f) ? 0.0f : atan2f(a, b);
            break;
          case NODE_MATH_ARCCOSINE:
            value = acosf(clamp_f(a, -1.0f, 1.0f));
            break;
          case NODE_MATH_FRACTION:
            value = a - floorf(a);
            break;
          case NODE_MATH_PINGPONG: {
            /* #pingpongf: the triangle wave of \a a over the scale \a b. */
            if (b == 0.0f) {
              value = 0.0f;
            }
            else {
              const float span = b * 2.0f;
              const float phase = (a - b) / span - floorf((a - b) / span);
              value = fabsf(phase * span - b);
            }
            break;
          }
          default:
            break;
        }
        /* Use Clamp folds the result into [0, 1], the node's own flag. */
        if ((node.custom2 & SHD_MATH_CLAMP) != 0) {
          value = clamp_f(value, 0.0f, 1.0f);
        }
        return {value, value, value, value};
      }
      case SH_NODE_COMPOSE_COLOR_ALPHA: {
        /* The content alpha composed over the colour: the colour's own alpha is replaced. */
        const RGBA color = eval_socket(
            *bke::node_find_socket(mutable_node, SOCK_IN, "Color"_ustr));
        const RGBA alpha = eval_socket(
            *bke::node_find_socket(mutable_node, SOCK_IN, "Alpha"_ustr));
        return {color.r, color.g, color.b, alpha.r};
      }
      case SH_NODE_SEPXYZ: {
        /* The mask's grey is the mean of a map's colour, split into its three components. */
        const RGBA vector = eval_socket(
            *bke::node_find_socket(mutable_node, SOCK_IN, "Vector"_ustr));
        const float value = STREQ(out_identifier, "X") ? vector.r :
                            STREQ(out_identifier, "Y") ? vector.g :
                                                         vector.b;
        return {value, value, value, value};
      }
      case SH_NODE_VECTOR_MATH: {
        const RGBA vector = eval_socket(
            *bke::node_find_socket(mutable_node, SOCK_IN, "Vector"_ustr));
        if (node.custom1 == NODE_VECTOR_MATH_NORMALIZE) {
          float normalized[3] = {vector.r, vector.g, vector.b};
          normalize_v3(normalized);
          return {normalized[0], normalized[1], normalized[2], 1.0f};
        }
        if (node.custom1 == NODE_VECTOR_MATH_SIGN) {
          const auto sign = [](const float value) {
            return value > 0.0f ? 1.0f : (value < 0.0f ? -1.0f : 0.0f);
          };
          return {sign(vector.r), sign(vector.g), sign(vector.b), 1.0f};
        }
        if (node.custom1 == NODE_VECTOR_MATH_DOT_PRODUCT) {
          /* The scalar output of the node, carried in every component. */
          const RGBA other = eval_socket(
              *bke::node_find_socket(mutable_node, SOCK_IN, "Vector_001"_ustr));
          const float dot = vector.r * other.r + vector.g * other.g + vector.b * other.b;
          return {dot, dot, dot, dot};
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
        if (node.custom1 == NODE_VECTOR_MATH_MULTIPLY) {
          const RGBA other = eval_socket(
              *bke::node_find_socket(mutable_node, SOCK_IN, "Vector_001"_ustr));
          return {vector.r * other.r, vector.g * other.g, vector.b * other.b, 1.0f};
        }
        if (node.custom1 == NODE_VECTOR_MATH_ADD) {
          const RGBA other = eval_socket(
              *bke::node_find_socket(mutable_node, SOCK_IN, "Vector_001"_ustr));
          return {vector.r + other.r, vector.g + other.g, vector.b + other.b, 1.0f};
        }
        if (node.custom1 == NODE_VECTOR_MATH_SUBTRACT) {
          const RGBA other = eval_socket(
              *bke::node_find_socket(mutable_node, SOCK_IN, "Vector_001"_ustr));
          return {vector.r - other.r, vector.g - other.g, vector.b - other.b, 1.0f};
        }
        if (node.custom1 == NODE_VECTOR_MATH_ABSOLUTE) {
          /* The Mirror fold reads |uv - center|; left out, the fold would silently be the identity. */
          return {fabsf(vector.r), fabsf(vector.g), fabsf(vector.b), 1.0f};
        }
        if (node.custom1 == NODE_VECTOR_MATH_FLOOR) {
          return {floorf(vector.r), floorf(vector.g), floorf(vector.b), 1.0f};
        }
        if (node.custom1 == NODE_VECTOR_MATH_SCALE) {
          const RGBA scale = eval_socket(
              *bke::node_find_socket(mutable_node, SOCK_IN, "Scale"_ustr));
          return {vector.r * scale.r, vector.g * scale.g, vector.b * scale.b, 1.0f};
        }
        if (node.custom1 == NODE_VECTOR_MATH_DIVIDE) {
          /* Per component, zero where the divisor is zero: what an empty folder relies on. */
          const RGBA divisor = eval_socket(
              *bke::node_find_socket(mutable_node, SOCK_IN, "Vector_001"_ustr));
          return {divisor.r != 0.0f ? vector.r / divisor.r : 0.0f,
                  divisor.g != 0.0f ? vector.g / divisor.g : 0.0f,
                  divisor.b != 0.0f ? vector.b / divisor.b : 0.0f,
                  1.0f};
        }
        if (ELEM(node.custom1, NODE_VECTOR_MATH_MINIMUM, NODE_VECTOR_MATH_MAXIMUM)) {
          const RGBA other = eval_socket(
              *bke::node_find_socket(mutable_node, SOCK_IN, "Vector_001"_ustr));
          const bool smaller = node.custom1 == NODE_VECTOR_MATH_MINIMUM;
          return {smaller ? std::min(vector.r, other.r) : std::max(vector.r, other.r),
                  smaller ? std::min(vector.g, other.g) : std::max(vector.g, other.g),
                  smaller ? std::min(vector.b, other.b) : std::max(vector.b, other.b),
                  1.0f};
        }
        if (ELEM(node.custom1, NODE_VECTOR_MATH_ADD, NODE_VECTOR_MATH_SUBTRACT)) {
          const RGBA other = eval_socket(
              *bke::node_find_socket(mutable_node, SOCK_IN, "Vector_001"_ustr));
          const bool plus = node.custom1 == NODE_VECTOR_MATH_ADD;
          return {plus ? vector.r + other.r : vector.r - other.r,
                  plus ? vector.g + other.g : vector.g - other.g,
                  plus ? vector.b + other.b : vector.b - other.b,
                  1.0f};
        }
        if (node.custom1 == NODE_VECTOR_MATH_DOT_PRODUCT) {
          const RGBA other = eval_socket(
              *bke::node_find_socket(mutable_node, SOCK_IN, "Vector_001"_ustr));
          const float dot = vector.r * other.r + vector.g * other.g + vector.b * other.b;
          return {dot, dot, dot, 1.0f};
        }
        if (node.custom1 == NODE_VECTOR_MATH_LENGTH) {
          const float length = sqrtf(vector.r * vector.r + vector.g * vector.g +
                                     vector.b * vector.b);
          return {length, length, length, 1.0f};
        }
        return {vector.r, vector.g, vector.b, 1.0f};
      }
      case SH_NODE_HUE_SAT: {
        /* The node's own function, written out here so the CPU filter is not its own witness. */
        const RGBA color = eval_socket(*bke::node_find_socket(mutable_node, SOCK_IN, "Color"_ustr));
        const float hue = eval_socket(*bke::node_find_socket(mutable_node, SOCK_IN, "Hue"_ustr)).r;
        const float saturation =
            eval_socket(*bke::node_find_socket(mutable_node, SOCK_IN, "Saturation"_ustr)).r;
        const float value =
            eval_socket(*bke::node_find_socket(mutable_node, SOCK_IN, "Value"_ustr)).r;
        const float fac = clamp_f(
            eval_socket(*bke::node_find_socket(mutable_node, SOCK_IN, "Fac"_ustr)).r, 0.0f, 1.0f);
        const float in[3] = {color.r, color.g, color.b};
        float hsv[3];
        rgb_to_hsv_v(in, hsv);
        const float turned = hsv[0] + hue + 0.5f;
        hsv[0] = turned - floorf(turned);
        hsv[1] = clamp_f(hsv[1] * saturation, 0.0f, 1.0f);
        hsv[2] *= value;
        float rgb[3];
        hsv_to_rgb_v(hsv, rgb);
        auto mixed = [&](const int i) { return in[i] + (std::max(rgb[i], 0.0f) - in[i]) * fac; };
        return {mixed(0), mixed(1), mixed(2), color.a};
      }
      case SH_NODE_GAMMA: {
        /* A positive component is raised to the exponent, any other is left as it is. */
        const RGBA color = eval_socket(*bke::node_find_socket(mutable_node, SOCK_IN, "Color"_ustr));
        const float gamma =
            eval_socket(*bke::node_find_socket(mutable_node, SOCK_IN, "Gamma"_ustr)).r;
        const auto raise = [&](const float value) {
          return value > 0.0f ? powf(value, gamma) : value;
        };
        return {raise(color.r), raise(color.g), raise(color.b), color.a};
      }
      case SH_NODE_BRIGHTCONTRAST: {
        /* The node's own function, written out here so the CPU filter is not its own witness. */
        const RGBA color = eval_socket(*bke::node_find_socket(mutable_node, SOCK_IN, "Color"_ustr));
        const float brightness =
            eval_socket(*bke::node_find_socket(mutable_node, SOCK_IN, "Bright"_ustr)).r;
        const float contrast =
            eval_socket(*bke::node_find_socket(mutable_node, SOCK_IN, "Contrast"_ustr)).r;
        const float a = 1.0f + contrast;
        const float b = brightness - contrast * 0.5f;
        return {std::max(a * color.r + b, 0.0f),
                std::max(a * color.g + b, 0.0f),
                std::max(a * color.b + b, 0.0f),
                color.a};
      }
      case SH_NODE_RGBTOBW: {
        /* The node's scene-linear Rec.709 luminance, the same coefficients the CPU ramp reads. */
        const RGBA color = eval_socket(*bke::node_find_socket(mutable_node, SOCK_IN, "Color"_ustr));
        const float value =
            0.2126f * color.r + 0.7152f * color.g + 0.0722f * color.b;
        return {value, value, value, value};
      }
      case SH_NODE_VALTORGB: {
        const RGBA fac = eval_socket(*bke::node_find_socket(mutable_node, SOCK_IN, "Fac"_ustr));
        const ColorBand *band = static_cast<const ColorBand *>(node.storage);
        float ramp_color[4];
        if (band != nullptr &&
            BKE_colorband_evaluate(band, std::clamp(fac.r, 0.0f, 1.0f), ramp_color))
        {
          /* The Alpha output is a float: a consumer reads its `.r`. */
          if (STREQ(out_identifier, "Alpha")) {
            return {ramp_color[3], ramp_color[3], ramp_color[3], ramp_color[3]};
          }
          return {ramp_color[0], ramp_color[1], ramp_color[2], ramp_color[3]};
        }
        return {};
      }
      case SH_NODE_CURVE_RGB: {
        /* The node's own function, the one the CPU filter calls: each channel through its curve,
         * no white balancing, then the node's mix by its Fac (one, unless the tree says other). */
        const RGBA color = eval_socket(*bke::node_find_socket(mutable_node, SOCK_IN, "Color"_ustr));
        const float fac = eval_socket(*bke::node_find_socket(mutable_node, SOCK_IN, "Fac"_ustr)).r;
        const CurveMapping *cumap = static_cast<const CurveMapping *>(node.storage);
        if (cumap == nullptr) {
          return {};
        }
        const float in[3] = {color.r, color.g, color.b};
        float out[3];
        BKE_curvemapping_evaluateRGBF(cumap, out, in);
        const float clamped = clamp_f(fac, 0.0f, 1.0f);
        return {in[0] + (out[0] - in[0]) * clamped,
                in[1] + (out[1] - in[1]) * clamped,
                in[2] + (out[2] - in[2]) * clamped,
                color.a};
      }
      case SH_NODE_TEX_NOISE: {
        /* The node's own body, through the one #BLI_noise.hh entry the node's multi-function and
         * the CPU composite both read: the position is the vector scaled, the detail clamped and
         * the roughness floored the way the node does, and the two conditional inputs ride only
         * for the types that read them. A generator row is 2D or 3D, which are the two branches
         * this keeps. */
        const NodeTexNoise &tex = *static_cast<const NodeTexNoise *>(node.storage);
        /* A socket the node's signature leaves out reads as its default of zero, the way an
         * unavailable input stands in the multi-function. */
        const auto input_float = [&](const char *identifier) {
          if (const bNodeSocket *socket = bke::node_find_socket(
                  mutable_node, SOCK_IN, UString::from_ptr_noinline(identifier)))
          {
            return eval_socket(*socket).r;
          }
          return 0.0f;
        };
        const float scale = input_float("Scale");
        const float detail = clamp_f(input_float("Detail"), 0.0f, 15.0f);
        const float roughness = max_ff(input_float("Roughness"), 0.0f);
        const float lacunarity = input_float("Lacunarity");
        const float offset = ELEM(tex.type,
                                  SHD_NOISE_RIDGED_MULTIFRACTAL,
                                  SHD_NOISE_HYBRID_MULTIFRACTAL,
                                  SHD_NOISE_HETERO_TERRAIN) ?
                                 input_float("Offset") :
                                 0.0f;
        const float gain = ELEM(tex.type,
                                SHD_NOISE_RIDGED_MULTIFRACTAL,
                                SHD_NOISE_HYBRID_MULTIFRACTAL) ?
                               input_float("Gain") :
                               0.0f;
        const float distortion = input_float("Distortion");
        if (tex.dimensions == 2) {
          const RGBA vector = eval_socket(
              *bke::node_find_socket(mutable_node, SOCK_IN, "Vector"_ustr));
          if (STREQ(out_identifier, "Color")) {
            const float3 c = noise::perlin_float3_fractal_distorted(
                float2(vector.r * scale, vector.g * scale),
                detail,
                roughness,
                lacunarity,
                offset,
                gain,
                distortion,
                int(tex.type),
                tex.normalize != 0);
            return {c.x, c.y, c.z, 1.0f};
          }
          const float fac = noise::perlin_fractal_distorted(
              float2(vector.r * scale, vector.g * scale),
              detail,
              roughness,
              lacunarity,
              offset,
              gain,
              distortion,
              int(tex.type),
              tex.normalize != 0);
          return {fac, fac, fac, 1.0f};
        }
        const RGBA vector = eval_socket(
            *bke::node_find_socket(mutable_node, SOCK_IN, "Vector"_ustr));
        const float3 position{vector.r * scale, vector.g * scale, vector.b * scale};
        if (STREQ(out_identifier, "Color")) {
          const float3 c = noise::perlin_float3_fractal_distorted(position,
                                                                  detail,
                                                                  roughness,
                                                                  lacunarity,
                                                                  offset,
                                                                  gain,
                                                                  distortion,
                                                                  int(tex.type),
                                                                  tex.normalize != 0);
          return {c.x, c.y, c.z, 1.0f};
        }
        const float fac = noise::perlin_fractal_distorted(position,
                                                          detail,
                                                          roughness,
                                                          lacunarity,
                                                          offset,
                                                          gain,
                                                          distortion,
                                                          int(tex.type),
                                                          tex.normalize != 0);
        return {fac, fac, fac, 1.0f};
      }
      case SH_NODE_TEX_VORONOI: {
        /* The node's own body, per feature: the params block filled the way the multi-function
         * fills it, the reach the feature's own sockets set, and the output the socket names. */
        const NodeTexVoronoi &tex = *static_cast<const NodeTexVoronoi *>(node.storage);
        /* A socket the node's signature leaves out for the feature chosen now reads as zero. */
        const auto input_float = [&](const char *identifier) {
          if (const bNodeSocket *socket = bke::node_find_socket(
                  mutable_node, SOCK_IN, UString::from_ptr_noinline(identifier)))
          {
            return eval_socket(*socket).r;
          }
          return 0.0f;
        };
        noise::VoronoiParams params;
        params.feature = tex.feature;
        params.metric = tex.distance;
        params.normalize = tex.normalize != 0;
        params.scale = input_float("Scale");
        params.detail = input_float("Detail");
        params.roughness = input_float("Roughness");
        params.lacunarity = input_float("Lacunarity");
        params.smoothness = (tex.feature == SHD_VORONOI_SMOOTH_F1) ?
                                clamp_f(input_float("Smoothness") / 2.0f, 0.0f, 0.5f) :
                                0.0f;
        params.exponent = (tex.distance == SHD_VORONOI_MINKOWSKI &&
                           tex.feature != SHD_VORONOI_DISTANCE_TO_EDGE) ?
                              input_float("Exponent") :
                              0.0f;
        params.randomness = clamp_f(input_float("Randomness"), 0.0f, 1.0f);

        const RGBA vector = eval_socket(
            *bke::node_find_socket(mutable_node, SOCK_IN, "Vector"_ustr));
        const bool is_2d = tex.dimensions == 2;
        const float2 v2{vector.r * params.scale, vector.g * params.scale};
        const float3 v3{vector.r * params.scale, vector.g * params.scale, vector.b * params.scale};

        if (tex.feature == SHD_VORONOI_N_SPHERE_RADIUS) {
          const float radius = is_2d ? noise::voronoi_n_sphere_radius(params, v2) :
                                       noise::voronoi_n_sphere_radius(params, v3);
          return {radius, radius, radius, 1.0f};
        }
        if (tex.feature == SHD_VORONOI_DISTANCE_TO_EDGE) {
          params.max_distance = 0.5f + 0.5f * params.randomness;
          const float distance = is_2d ?
                                     noise::fractal_voronoi_distance_to_edge<float2>(params, v2) :
                                     noise::fractal_voronoi_distance_to_edge<float3>(params, v3);
          return {distance, distance, distance, 1.0f};
        }
        const bool calc_color = STREQ(out_identifier, "Color");
        const float f2_scale = (tex.feature == SHD_VORONOI_F2) ? 2.0f : 1.0f;
        const float reach = 0.5f + 0.5f * params.randomness;
        params.max_distance = is_2d ?
                                  noise::voronoi_distance(float2{0.0f, 0.0f},
                                                          float2(reach, reach),
                                                          params) *
                                      f2_scale :
                                  noise::voronoi_distance(float3{0.0f, 0.0f, 0.0f},
                                                          float3(reach, reach, reach),
                                                          params) *
                                      f2_scale;
        const noise::VoronoiOutput output = is_2d ?
                                                noise::fractal_voronoi_x_fx<float2>(
                                                    params, v2, calc_color) :
                                                noise::fractal_voronoi_x_fx<float3>(
                                                    params, v3, calc_color);
        if (calc_color) {
          return {output.color.x, output.color.y, output.color.z, 1.0f};
        }
        return {output.distance, output.distance, output.distance, 1.0f};
      }
      case SH_NODE_TEX_CHECKER: {
        const RGBA vector = eval_socket(
            *bke::node_find_socket(mutable_node, SOCK_IN, "Vector"_ustr));
        const float scale = eval_socket(
                                *bke::node_find_socket(mutable_node, SOCK_IN, "Scale"_ustr))
                                .r;
        const float fac = procedural_checker_factor(float3(vector.r, vector.g, vector.b), scale);
        if (STREQ(out_identifier, "Color")) {
          const RGBA color1 = eval_socket(
              *bke::node_find_socket(mutable_node, SOCK_IN, "Color1"_ustr));
          const RGBA color2 = eval_socket(
              *bke::node_find_socket(mutable_node, SOCK_IN, "Color2"_ustr));
          const RGBA &picked = (fac == 1.0f) ? color1 : color2;
          return picked;
        }
        return {fac, fac, fac, 1.0f};
      }
      case SH_NODE_TEX_BRICK: {
        const NodeTexBrick &tex = *static_cast<const NodeTexBrick *>(node.storage);
        const auto input_float = [&](const char *identifier) {
          if (const bNodeSocket *socket = bke::node_find_socket(
                  mutable_node, SOCK_IN, UString::from_ptr_noinline(identifier)))
          {
            return eval_socket(*socket).r;
          }
          return 0.0f;
        };
        const RGBA vector = eval_socket(
            *bke::node_find_socket(mutable_node, SOCK_IN, "Vector"_ustr));
        const float2 f2 = procedural_brick(
            float3(vector.r, vector.g, vector.b) * input_float("Scale"),
            input_float("Mortar Size"),
            input_float("Mortar Smooth"),
            input_float("Bias"),
            input_float("Brick Width"),
            input_float("Row Height"),
            tex.offset,
            tex.offset_freq,
            tex.squash,
            tex.squash_freq);
        const float tint = f2.x;
        const float f = f2.y;
        if (STREQ(out_identifier, "Fac")) {
          return {f, f, f, 1.0f};
        }
        /* The node's colour: a mix of the two bricks by the tint, then the mortar over it by how
         * much of the texel it covers. */
        const RGBA color1 = eval_socket(
            *bke::node_find_socket(mutable_node, SOCK_IN, "Color1"_ustr));
        const RGBA color2 = eval_socket(
            *bke::node_find_socket(mutable_node, SOCK_IN, "Color2"_ustr));
        const RGBA mortar = eval_socket(
            *bke::node_find_socket(mutable_node, SOCK_IN, "Mortar"_ustr));
        RGBA color_data = color1;
        if (f != 1.0f) {
          const float facm = 1.0f - tint;
          color_data.r = color1.r * facm + color2.r * tint;
          color_data.g = color1.g * facm + color2.g * tint;
          color_data.b = color1.b * facm + color2.b * tint;
          color_data.a = color1.a * facm + color2.a * tint;
        }
        color_data.r = color_data.r * (1.0f - f) + mortar.r * f;
        color_data.g = color_data.g * (1.0f - f) + mortar.g * f;
        color_data.b = color_data.b * (1.0f - f) + mortar.b * f;
        color_data.a = color_data.a * (1.0f - f) + mortar.a * f;
        return color_data;
      }
      case SH_NODE_TEX_WAVE: {
        const NodeTexWave &tex = *static_cast<const NodeTexWave *>(node.storage);
        const auto input_float = [&](const char *identifier) {
          if (const bNodeSocket *socket = bke::node_find_socket(
                  mutable_node, SOCK_IN, UString::from_ptr_noinline(identifier)))
          {
            return eval_socket(*socket).r;
          }
          return 0.0f;
        };
        const RGBA vector = eval_socket(
            *bke::node_find_socket(mutable_node, SOCK_IN, "Vector"_ustr));
        const float fac = procedural_wave_factor(float3(vector.r, vector.g, vector.b),
                                                 input_float("Scale"),
                                                 input_float("Distortion"),
                                                 input_float("Detail"),
                                                 input_float("Detail Scale"),
                                                 input_float("Detail Roughness"),
                                                 input_float("Phase Offset"),
                                                 eProceduralWaveType(tex.wave_type),
                                                 tex.bands_direction,
                                                 tex.rings_direction,
                                                 eProceduralWaveProfile(tex.wave_profile));
        if (STREQ(out_identifier, "Color")) {
          return {fac, fac, fac, 1.0f};
        }
        return {fac, fac, fac, 1.0f};
      }
      default:
        return {};
    }
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
    child.parent = this;
    child.object_mask_value = object_mask_value;
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

  /** The Normal Combine group's own formula: whiteout, renormalized, faded by the factor. */
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
    float combined[3] = {base[0] + detail[0], base[1] + detail[1], base[2] * detail[2]};
    normalize_v3(combined);
    const float encoded[3] = {
        combined[0] * 0.5f + 0.5f, combined[1] * 0.5f + 0.5f, combined[2] * 0.5f + 0.5f};
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
    tree->ensure_topology_cache();
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

class PaintLayersGraphEvalTest : public bke::BlenderGTestBase {
 public:
  Main *bmain = nullptr;
  Material *ma = nullptr;
  /** The cache budget the test found, restored on teardown so a test that shrank it cannot hide a
   * slice in a later one. */
  size_t slice_cache_budget_before_ = 0;
  /** The value an object-mask Attribute node reads in #graph_pixel, see
   * #GraphInterpreter::object_mask_value. */
  float object_mask_value = 0.0f;

  void SetUp() override
  {
    /* The tier records are file-static; start each graph-evaluation test from the static tier. */
    BKE_paint_layers_blend_tier_clear();
    /* The slice cache is process-wide: a slice a previous test rendered must not be found here. */
    paint_layer_anchor_point_slice_cache_clear();
    slice_cache_budget_before_ = paint_layer_anchor_point_slice_cache_budget();
    bmain = BKE_main_new();
    G_MAIN = bmain;
    ma = BKE_material_add(bmain, "Layered");
    ma->paint_layers_flag |= MA_PAINT_LAYERED;
    /* The channel set the New Layered Material dialog starts from, Height included: a channel the
     * set leaves out takes part nowhere, and the height tests read that channel. */
    BKE_paint_layers_channel_set_mask_set(*ma, BKE_paint_layers_new_material_channel_set());
  }

  void TearDown() override
  {
    BKE_paint_layers_blend_tier_clear();
    paint_layer_anchor_point_slice_cache_clear();
    paint_layer_anchor_point_slice_cache_set_budget(slice_cache_budget_before_);
    BKE_main_free(bmain);
    G_MAIN = nullptr;
  }

  /** A byte image of \a size x \a size filled with \a rgba. */
  Image *add_image(const char *name, const int size, const uchar rgba[4])
  {
    const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    Image *image = BKE_image_add_generated(
        bmain, size, size, name, 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
    image->alpha_mode = IMA_ALPHA_STRAIGHT;
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
    uchar *pixels = ibuf->byte_data_for_write();
    for (const int64_t i : IndexRange(int64_t(size) * size)) {
      for (const int c : IndexRange(4)) {
        pixels[i * 4 + c] = rgba[c];
      }
    }
    BKE_image_release_ibuf(image, ibuf, lock);
    return image;
  }

  /** A byte image of \a size x \a size whose left half is \a left and its right half \a right: a
   * map with two levels in one image, which a comparison needs to pick a side by. */
  Image *add_split_image(const char *name,
                         const int size,
                         const uchar left[4],
                         const uchar right[4])
  {
    const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    Image *image = BKE_image_add_generated(
        bmain, size, size, name, 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
    image->alpha_mode = IMA_ALPHA_STRAIGHT;
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
    uchar *pixels = ibuf->byte_data_for_write();
    for (const int y : IndexRange(size)) {
      for (const int x : IndexRange(size)) {
        const uchar *rgba = x < size / 2 ? left : right;
        for (const int c : IndexRange(4)) {
          pixels[(int64_t(y) * size + x) * 4 + c] = rgba[c];
        }
      }
    }
    BKE_image_release_ibuf(image, ibuf, lock);
    return image;
  }

  /** A byte image of \a size x \a size whose first rows (y < size / 2) are \a upper and the rest
   * \a lower: the row-wise counterpart of #add_split_image, which a comparison that varies by row
   * needs. */
  Image *add_row_split_image(const char *name,
                             const int size,
                             const uchar upper[4],
                             const uchar lower[4])
  {
    const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    Image *image = BKE_image_add_generated(
        bmain, size, size, name, 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
    image->alpha_mode = IMA_ALPHA_STRAIGHT;
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
    uchar *pixels = ibuf->byte_data_for_write();
    for (const int y : IndexRange(size)) {
      for (const int x : IndexRange(size)) {
        const uchar *rgba = y < size / 2 ? upper : lower;
        for (const int c : IndexRange(4)) {
          pixels[(int64_t(y) * size + x) * 4 + c] = rgba[c];
        }
      }
    }
    BKE_image_release_ibuf(image, ibuf, lock);
    return image;
  }

  /** A solid 4x4 byte image. */
  Image *add_solid_image(
      const char *name, const uchar r, const uchar g, const uchar b, const uchar a)
  {
    const uchar rgba[4] = {r, g, b, a};
    return add_image(name, 4, rgba);
  }

  /** A solid 4x4 float data image, stored premultiplied: \a rgb is the straight value and is
   * multiplied by \a a on the way in, the association a float map holds (see
   * #composite_decode_tile). Height is the channel such a map belongs to. */
  Image *add_solid_float_image(
      const char *name, const float r, const float g, const float b, const float a)
  {
    const float color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    Image *image = BKE_image_add_generated(
        bmain, 4, 4, name, 32, true, IMA_GENTYPE_BLANK, color, false, /*is_data=*/true, false);
    image->alpha_mode = IMA_ALPHA_STRAIGHT;
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
    float *pixels = ibuf->float_data_for_write();
    for (const int64_t i : IndexRange(16)) {
      pixels[i * 4 + 0] = r * a;
      pixels[i * 4 + 1] = g * a;
      pixels[i * 4 + 2] = b * a;
      pixels[i * 4 + 3] = a;
    }
    BKE_image_release_ibuf(image, ibuf, lock);
    return image;
  }

  /** Mark \a image as data, so neither side decodes it as a colour. The image's own colorspace is
   * set too: the Image Texture node reads that, not the ImBuf's. */
  static void make_image_data(Image *image)
  {
    const char *data_name = IMB_colormanagement_role_colorspace_name_get(COLOR_ROLE_DATA);
    BLI_strncpy(
        image->colorspace_settings.name, data_name, sizeof(image->colorspace_settings.name));
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
    if (ibuf != nullptr) {
      IMB_colormanagement_assign_byte_colorspace(ibuf, data_name);
      BKE_image_release_ibuf(image, ibuf, lock);
    }
  }

  /** A row on top of the stack with \a image on its enabled \a channel. */
  MaterialPaintLayer *add_layer(const char *name,
                                Image *image,
                                const eMaterialPaintChannel channel =
                                    PAINT_MATERIAL_CHANNEL_BASE_COLOR)
  {
    MaterialPaintLayer *layer = BKE_paint_layers_add(*ma, name, nullptr, PaintLayerPlace::Above);
    EXPECT_NE(layer, nullptr);
    EXPECT_NE(BKE_paint_layers_channel_add(*ma, layer, channel), nullptr);
    EXPECT_TRUE(BKE_paint_layers_channel_set_image(*ma, layer, channel, image));
    return layer;
  }

  /** A row on top of the stack with the flat \a value on its enabled \a channel. */
  MaterialPaintLayer *add_constant_layer(const char *name,
                                         const float value[4],
                                         const eMaterialPaintChannel channel =
                                             PAINT_MATERIAL_CHANNEL_BASE_COLOR)
  {
    MaterialPaintLayer *layer = BKE_paint_layers_add(*ma, name, nullptr, PaintLayerPlace::Above);
    EXPECT_NE(layer, nullptr);
    EXPECT_NE(BKE_paint_layers_channel_add(*ma, layer, channel), nullptr);
    EXPECT_TRUE(BKE_paint_layers_channel_set_value(*ma, layer, channel, value));
    return layer;
  }

  /**
   * A generator row of \a generator on top of the stack, taking part in \a channel. The Add starts
   * the row in the set's first channel; another channel replaces that record the way a Fill's
   * channel is switched, with the scalar ends seeded from the channel's own range.
   */
  MaterialPaintLayer *add_generator_layer(const char *name,
                                          const eMaterialPaintLayerGenerator generator,
                                          const eMaterialPaintChannel channel =
                                              PAINT_MATERIAL_CHANNEL_BASE_COLOR)
  {
    MaterialPaintLayer *layer = BKE_paint_layers_generator_add(
        *ma, generator, name, nullptr, PaintLayerPlace::Above);
    EXPECT_NE(layer, nullptr);
    if (layer == nullptr) {
      return nullptr;
    }
    if (channel != PAINT_MATERIAL_CHANNEL_BASE_COLOR) {
      EXPECT_TRUE(BKE_paint_layers_channel_remove(*ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
      EXPECT_NE(BKE_paint_layers_channel_add(*ma, layer, channel), nullptr);
      const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(channel);
      if (!info.is_color) {
        EXPECT_TRUE(BKE_paint_layers_generator_set_channel_scalars(
            *ma, *layer, channel, info.value_min, info.value_max));
      }
    }
    return layer;
  }

  /** Set \a row's parameter \a index to \a value: the schema stores it in the row itself. */
  static void set_param(MaterialPaintLayer &row, const int index, const float value)
  {
    EXPECT_TRUE(paint_layer_kind_param_set(row, index, value));
  }

  static void set_param_int(MaterialPaintLayer &row, const int index, const int value)
  {
    EXPECT_TRUE(paint_layer_kind_param_int_set(row, index, value));
  }

  static const char *result_name(const eMaterialPaintChannel channel)
  {
    /* The generator names the output "Result <ui name of the channel>"; building the name from the
     * same source keeps every channel (a switch with a default silently read Base Color for any
     * channel it did not list). The buffer is per thread, valid until the next call. */
    static thread_local char name[96];
    SNPRINTF(name, "Result %s", BKE_paint_material_channel_info(channel).ui_name);
    return name;
  }

  /** The generated graph of \a channel evaluated at pixel (1, 1), after a regeneration. */
  RGBA graph_pixel(const eMaterialPaintChannel channel)
  {
    return this->graph_pixel(channel, 1, 1);
  }

  /** The generated graph of \a channel evaluated at pixel (\a x, \a y), after a regeneration. */
  RGBA graph_pixel(const eMaterialPaintChannel channel, const int x, const int y)
  {
    EXPECT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
    GraphInterpreter interpreter;
    interpreter.tree = ma->paint_layers_tree;
    interpreter.x = x;
    interpreter.y = y;
    interpreter.object_mask_value = object_mask_value;
    return interpreter.eval_result(result_name(channel));
  }

  /** CPU composite of \a channel at (1, 1), in scene-linear float, as the shader sees it. */
  RGBA cpu_pixel(const eMaterialPaintChannel channel)
  {
    Vector<PaintMaterialCompositeImageLayer> layers;
    EXPECT_TRUE(BKE_paint_layers_composite_image_layers(*ma, channel, layers));
    int width = 0, height = 0;
    if (!BKE_paint_material_composite_stack_dimensions(layers, width, height) || width <= 1 ||
        height <= 1)
    {
      /* A stack made only of constants has no size: there is no CPU pixel to compare with, and
       * reading one would run past the empty buffer. */
      ADD_FAILURE() << "channel " << int(channel) << " has no stack dimensions to compare";
      return {};
    }
    Vector<float> linear(int64_t(width) * height * 4);
    float bottom[4];
    BKE_paint_layers_channel_bottom_color(channel, bottom);
    EXPECT_TRUE(BKE_paint_material_composite_eval_images_linear(
        layers,
        PaintMaterialCompositeLinearTarget::frame(linear, width),
        nullptr,
        nullptr,
        bottom));
    const float *p = linear.data() + (int64_t(1) * width + 1) * 4;
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

  /** The graph and the CPU composite of \a channel agree at the pixel, colour channels only. */
  void expect_graph_matches_cpu(const eMaterialPaintChannel channel, const float tolerance = 2e-3f)
  {
    const RGBA graph = graph_pixel(channel);
    const RGBA cpu = cpu_pixel(channel);
    EXPECT_NEAR(graph.r, cpu.r, tolerance);
    EXPECT_NEAR(graph.g, cpu.g, tolerance);
    EXPECT_NEAR(graph.b, cpu.b, tolerance);
  }
};

}  // namespace blender::bke::tests
