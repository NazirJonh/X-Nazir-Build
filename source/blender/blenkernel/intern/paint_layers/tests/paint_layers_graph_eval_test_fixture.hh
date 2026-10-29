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
 * Math (with Floor and Compare, the UV wrap's clip factor), Vector Math, Value, RGB, group
 * input/output and the Normal Combine group -- and the result
 * is compared with #BKE_paint_layers_composite_image_layers
 * evaluated at the same pixel.
 *
 * What it catches is a mis-wired socket in the generator: the generator's own tests only assert the
 * tree's shape, and the composite tests only assert the CPU numbers.
 */

#include "testing/testing.h"

#include "BKE_global.hh"
#include "BKE_gtest_base.hh"
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
#include "BKE_paint_material_composite.hh"

#include "BLI_index_range.hh"
#include <algorithm>

#include "BLI_math_base.h"
#include "BLI_math_color.h"
#include "BLI_math_vector.h"
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
    if (x >= ibuf->x || y >= ibuf->y || ibuf->byte_buffer.data == nullptr) {
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
        /* The reference texel's centre in UV space. */
        return {(float(x) + 0.5f) / kGridSize, (float(y) + 0.5f) / kGridSize, 0.0f, 1.0f};
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
        ramp_blend(node.custom1, result, clamp_f(fac, 0.0f, 1.0f), top_rgba);
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
            value = powf(a, b);
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
          default:
            break;
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
    EXPECT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
    GraphInterpreter interpreter;
    interpreter.tree = ma->paint_layers_tree;
    interpreter.x = 1;
    interpreter.y = 1;
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
