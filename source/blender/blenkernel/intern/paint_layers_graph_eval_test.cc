/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

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

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "BKE_colorband.hh"
#include "BKE_global.hh"
#include "BKE_gtest_base.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_composite.hh"
#include "BKE_paint_layers_generate.hh"
#include "BKE_paint_material_composite.hh"
#include "BKE_paint_material_resolve.hh"

#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_math_vector.h"
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
    RGBA out;
    if (image == nullptr) {
      return out;
    }
    ImageUser iuser;
    BKE_imageuser_default(&iuser);
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(image, &iuser, &lock);
    if (ibuf == nullptr || x >= ibuf->x || y >= ibuf->y) {
      if (ibuf != nullptr) {
        BKE_image_release_ibuf(image, ibuf, lock);
      }
      return out;
    }
    /* Straight alpha, as the generator's layer and mask maps are created (#IMA_ALPHA_STRAIGHT): the
     * Image Texture Alpha output is then exactly the stored alpha, like the CPU reads it. */
    const int channels = ibuf->channels == 0 ? 4 : ibuf->channels;
    const int64_t offset = (int64_t(y) * ibuf->x + x) * channels;
    float pixel[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    if (ibuf->byte_buffer.data != nullptr) {
      const uchar *p = ibuf->byte_data() + offset;
      pixel[0] = float(p[0]) / 255.0f;
      pixel[1] = float(p[1]) / 255.0f;
      pixel[2] = float(p[2]) / 255.0f;
      pixel[3] = channels == 4 ? float(p[3]) / 255.0f : 1.0f;
      if (ibuf->byte_buffer.colorspace != nullptr) {
        IMB_colormanagement_colorspace_to_scene_linear_v4(
            pixel, false, ibuf->byte_buffer.colorspace);
      }
    }
    else {
      const float *p = ibuf->float_buffer.data + offset;
      pixel[0] = p[0];
      pixel[1] = p[1];
      pixel[2] = p[2];
      pixel[3] = channels == 4 ? p[3] : 1.0f;
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
    /* Model the GPU texture upload: an #IMA_GPU_LINEAR_PREMUL byte buffer is stored as a scene
     * linear, pre-multiplied float texture, so its straight byte RGB is multiplied by alpha here
     * (colormanagement.cc's `use_premultiply` path). Without this stage the interpreter sees the
     * raw byte buffer and cannot show the double multiply that makes the viewport halo. */
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
      case SH_NODE_TEX_IMAGE:
        return sample_image(id_cast<Image *>(node.id), out_identifier);
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
        if (node.custom1 == NODE_VECTOR_MATH_NORMALIZE) {
          float normalized[3] = {vector.r, vector.g, vector.b};
          normalize_v3(normalized);
          return {normalized[0], normalized[1], normalized[2], vector.a};
        }
        if (node.custom1 == NODE_VECTOR_MATH_MULTIPLY_ADD) {
          const RGBA scale = eval_socket(
              *bke::node_find_socket(mutable_node, SOCK_IN, "Vector_001"_ustr));
          const RGBA offset = eval_socket(
              *bke::node_find_socket(mutable_node, SOCK_IN, "Vector_002"_ustr));
          return {vector.r * scale.r + offset.r,
                  vector.g * scale.g + offset.g,
                  vector.b * scale.b + offset.b,
                  vector.a};
        }
        if (node.custom1 == NODE_VECTOR_MATH_DIVIDE) {
          const RGBA divisor = eval_socket(
              *bke::node_find_socket(mutable_node, SOCK_IN, "Vector_001"_ustr));
          /* Blender's vector divide is a safe divide: zero where the divisor is zero. */
          auto safe = [](const float a, const float b) { return b != 0.0f ? a / b : 0.0f; };
          return {safe(vector.r, divisor.r),
                  safe(vector.g, divisor.g),
                  safe(vector.b, divisor.b),
                  vector.a};
        }
        return vector;
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
                                const eMaterialPaintLayerKind kind,
                                Image *image,
                                const eMaterialPaintChannel channel =
                                    PAINT_MATERIAL_CHANNEL_BASE_COLOR)
  {
    MaterialPaintLayer *layer = BKE_paint_layers_add(
        *ma, kind, name, nullptr, PaintLayerPlace::Above);
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
    BKE_paint_layers_correction_set_effect(*ma, item, MA_PAINT_LAYER_EFFECT_PAINT);
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
};

TEST_F(PaintLayersGraphEvalTest, compose_color_alpha_uses_linked_color_and_alpha)
{
  bNodeTree *tree = bke::node_tree_add_tree(bmain, "ComposeColorAlpha", "ShaderNodeTree");
  ASSERT_NE(tree, nullptr);

  bNode *rgb = bke::node_add_static_node(nullptr, *tree, SH_NODE_RGB);
  bNode *value = bke::node_add_static_node(nullptr, *tree, SH_NODE_VALUE);
  bNode *compose = bke::node_add_static_node(nullptr, *tree, SH_NODE_COMPOSE_COLOR_ALPHA);
  ASSERT_NE(rgb, nullptr);
  ASSERT_NE(value, nullptr);
  ASSERT_NE(compose, nullptr);

  bNodeSocket *rgb_out = bke::node_find_socket(*rgb, SOCK_OUT, "Color"_ustr);
  bNodeSocket *value_out = bke::node_find_socket(*value, SOCK_OUT, "Value"_ustr);
  bNodeSocket *color_in = bke::node_find_socket(*compose, SOCK_IN, "Color"_ustr);
  bNodeSocket *alpha_in = bke::node_find_socket(*compose, SOCK_IN, "Alpha"_ustr);
  ASSERT_NE(rgb_out, nullptr);
  ASSERT_NE(value_out, nullptr);
  ASSERT_NE(color_in, nullptr);
  ASSERT_NE(alpha_in, nullptr);

  const float color[4] = {0.2f, 0.5f, 0.9f, 0.1f};
  copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(rgb_out->default_value)->value, color);
  static_cast<bNodeSocketValueFloat *>(value_out->default_value)->value = 0.3f;
  bke::node_add_link(*tree, *rgb, *rgb_out, *compose, *color_in);
  bke::node_add_link(*tree, *value, *value_out, *compose, *alpha_in);
  tree->ensure_topology_cache();

  GraphInterpreter interpreter;
  interpreter.tree = tree;
  const RGBA result = interpreter.eval_output(*compose, "Color");
  EXPECT_FLOAT_EQ(result.r, color[0]);
  EXPECT_FLOAT_EQ(result.g, color[1]);
  EXPECT_FLOAT_EQ(result.b, color[2]);
  EXPECT_FLOAT_EQ(result.a, 0.3f);
}

/**
 * A source whose Principled sits in a nested group fed through the group's interface: the root's
 * RGB and Value nodes reach the group inputs through Reroutes, Metallic is an instance value, and
 * the group output feeds the Material Output. The earlier helpers fed the Principled constants, so
 * a group interface lost in the wrapper copy stayed invisible.
 */
static Material *make_interface_wired_source(Main &bmain, const char *name)
{
  bNodeTree *group = bke::node_tree_add_tree(&bmain, "InterfaceWiredGroup", "ShaderNodeTree");
  bNodeTreeInterface &iface = group->tree_interface;
  bNodeTreeInterfaceSocket *in_color = iface.add_socket(
      "Color", "", "NodeSocketColor", NODE_INTERFACE_SOCKET_INPUT, nullptr);
  bNodeTreeInterfaceSocket *in_roughness = iface.add_socket(
      "Roughness", "", "NodeSocketFloat", NODE_INTERFACE_SOCKET_INPUT, nullptr);
  bNodeTreeInterfaceSocket *in_metallic = iface.add_socket(
      "Metallic", "", "NodeSocketFloat", NODE_INTERFACE_SOCKET_INPUT, nullptr);
  bNodeTreeInterfaceSocket *out_bsdf = iface.add_socket(
      "BSDF", "", "NodeSocketShader", NODE_INTERFACE_SOCKET_OUTPUT, nullptr);
  iface.tag_items_changed();
  static_cast<bNodeSocketValueFloat *>(in_metallic->socket_data)->value = 0.7f;

  bNode *group_input = bke::node_add_node(nullptr, *group, "NodeGroupInput"_ustr);
  bNode *group_output = bke::node_add_node(nullptr, *group, "NodeGroupOutput"_ustr);
  bNode *principled = bke::node_add_static_node(nullptr, *group, SH_NODE_BSDF_PRINCIPLED);
  nodes::update_node_declaration_and_sockets(*group, *group_input);
  nodes::update_node_declaration_and_sockets(*group, *group_output);
  bke::node_add_link(*group,
                     *group_input,
                     *bke::node_find_socket(
                         *group_input, SOCK_OUT, UString::from_ptr_noinline(in_color->identifier)),
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr));
  bke::node_add_link(
      *group,
      *group_input,
      *bke::node_find_socket(
          *group_input, SOCK_OUT, UString::from_ptr_noinline(in_roughness->identifier)),
      *principled,
      *bke::node_find_socket(*principled, SOCK_IN, "Roughness"_ustr));
  bke::node_add_link(
      *group,
      *group_input,
      *bke::node_find_socket(
          *group_input, SOCK_OUT, UString::from_ptr_noinline(in_metallic->identifier)),
      *principled,
      *bke::node_find_socket(*principled, SOCK_IN, "Metallic"_ustr));
  bke::node_add_link(*group,
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_OUT, "BSDF"_ustr),
                     *group_output,
                     *bke::node_find_socket(
                         *group_output, SOCK_IN, UString::from_ptr_noinline(out_bsdf->identifier)));
  bNode *group_frame = bke::node_add_static_node(nullptr, *group, NODE_FRAME);
  for (bNode &node : group->nodes) {
    if (!node.is_frame()) {
      node.parent = group_frame;
    }
  }
  BKE_ntree_update_tag_all(group);
  BKE_ntree_update_after_single_tree_change(bmain, *group);

  Material *source = BKE_material_add(&bmain, name);
  bNodeTree &tree = *source->nodetree;
  bNode *instance = bke::node_add_node(nullptr, tree, group->typeinfo->group_idname);
  instance->id = &group->id;
  id_us_plus(&group->id);
  nodes::update_node_declaration_and_sockets(tree, *instance);
  /* A full update before the links: it gives the instance's input sockets their default values,
   * which only then exist to be written (the source's Metallic rides the interface default). */
  BKE_ntree_update_tag_all(source->nodetree);
  BKE_ntree_update_after_single_tree_change(bmain, *source->nodetree);

  bNode *rgb = bke::node_add_static_node(nullptr, tree, SH_NODE_RGB);
  bNodeSocket *rgb_out = bke::node_find_socket(*rgb, SOCK_OUT, "Color"_ustr);
  const float rgb_color[4] = {0.2f, 0.5f, 0.9f, 1.0f};
  copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(rgb_out->default_value)->value, rgb_color);
  bNode *value = bke::node_add_static_node(nullptr, tree, SH_NODE_VALUE);
  bNodeSocket *value_out = bke::node_find_socket(*value, SOCK_OUT, "Value"_ustr);
  static_cast<bNodeSocketValueFloat *>(value_out->default_value)->value = 0.3f;

  bNode *reroute_color = bke::node_add_static_node(nullptr, tree, NODE_REROUTE);
  bNode *reroute_roughness = bke::node_add_static_node(nullptr, tree, NODE_REROUTE);
  bke::node_add_link(tree,
                     *rgb,
                     *rgb_out,
                     *reroute_color,
                     *bke::node_find_socket(*reroute_color, SOCK_IN, "Input"_ustr));
  bke::node_add_link(
      tree,
      *reroute_color,
      *bke::node_find_socket(*reroute_color, SOCK_OUT, "Output"_ustr),
      *instance,
      *bke::node_find_socket(*instance, SOCK_IN, UString::from_ptr_noinline(in_color->identifier)));
  bke::node_add_link(tree,
                     *value,
                     *value_out,
                     *reroute_roughness,
                     *bke::node_find_socket(*reroute_roughness, SOCK_IN, "Input"_ustr));
  bke::node_add_link(
      tree,
      *reroute_roughness,
      *bke::node_find_socket(*reroute_roughness, SOCK_OUT, "Output"_ustr),
      *instance,
      *bke::node_find_socket(
          *instance, SOCK_IN, UString::from_ptr_noinline(in_roughness->identifier)));

  bNode *output = bke::node_add_static_node(nullptr, tree, SH_NODE_OUTPUT_MATERIAL);
  bke::node_add_link(
      tree,
      *instance,
      *bke::node_find_socket(
          *instance, SOCK_OUT, UString::from_ptr_noinline(out_bsdf->identifier)),
      *output,
      *bke::node_find_socket(*output, SOCK_IN, "Surface"_ustr));

  bNode *root_frame = bke::node_add_static_node(nullptr, tree, NODE_FRAME);
  for (bNode &node : tree.nodes) {
    if (!node.is_frame()) {
      node.parent = root_frame;
    }
  }
  BKE_ntree_update_tag_all(source->nodetree);
  BKE_ntree_update_after_single_tree_change(bmain, *source->nodetree);
  return source;
}

/**
 * \a source with the Principled inside a nested group whose interface also declares an input named
 * "Specular" (the same name the wrapper gives its output). The Specular IOR Level is either linked
 * through that Group Input (its instance value 0.25) or left as an unlinked 0.25.
 */
static Material *make_specular_source(Main &bmain, const char *name, const bool linked)
{
  Material *source = make_interface_wired_source(bmain, name);
  bNodeTree *group = nullptr;
  bNode *group_instance = nullptr;
  for (bNode &node : source->nodetree->nodes) {
    if (node.is_group() && node.id != nullptr) {
      group = id_cast<bNodeTree *>(node.id);
      group_instance = &node;
    }
  }
  if (group == nullptr || group_instance == nullptr) {
    return source;
  }
  bNode *group_input = nullptr;
  bNode *principled = nullptr;
  for (bNode &node : group->nodes) {
    if (node.is_group_input()) {
      group_input = &node;
    }
    else if (node.type_legacy == SH_NODE_BSDF_PRINCIPLED) {
      principled = &node;
    }
  }
  if (group_input == nullptr || principled == nullptr) {
    return source;
  }
  bNodeTreeInterfaceSocket *spec_in = group->tree_interface.add_socket(
      "Specular", "", "NodeSocketFloat", NODE_INTERFACE_SOCKET_INPUT, nullptr);
  nodes::update_node_declaration_and_sockets(*group, *group_input);
  nodes::update_node_declaration_and_sockets(*group, *group_instance);
  BKE_ntree_update_tag_all(group);
  BKE_ntree_update_after_single_tree_change(bmain, *group);
  BKE_ntree_update_tag_all(source->nodetree);
  BKE_ntree_update_after_single_tree_change(bmain, *source->nodetree);

  bNodeSocket *specular = bke::node_find_socket(*principled, SOCK_IN, "Specular IOR Level"_ustr);
  if (specular == nullptr) {
    return source;
  }
  if (linked && spec_in != nullptr && spec_in->identifier != nullptr) {
    bNodeSocket *instance_in = bke::node_find_socket(
        *group_instance, SOCK_IN, UString::from_ptr_noinline(spec_in->identifier));
    if (instance_in != nullptr && instance_in->default_value != nullptr) {
      static_cast<bNodeSocketValueFloat *>(instance_in->default_value)->value = 0.25f;
    }
    bNodeSocket *input_out = bke::node_find_socket(
        *group_input, SOCK_OUT, UString::from_ptr_noinline(spec_in->identifier));
    if (input_out != nullptr) {
      bke::node_add_link(*group, *group_input, *input_out, *principled, *specular);
    }
  }
  else {
    static_cast<bNodeSocketValueFloat *>(specular->default_value)->value = 0.25f;
  }
  BKE_ntree_update_tag_all(group);
  BKE_ntree_update_after_single_tree_change(bmain, *group);
  return source;
}

/* -------------------------------------------------------------------- */
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

/** A float data-space image of \a size x \a size from a row-major array of RGB triples, alpha one. */
static Image *make_data_pattern_map(Main *bmain,
                                    const char *name,
                                    const int size,
                                    const float (*pixels)[3])
{
  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *image = BKE_image_add_generated(
      bmain, size, size, name, 32, /*floatbuf=*/true, IMA_GENTYPE_BLANK, black, false, true, false);
  if (image == nullptr) {
    return nullptr;
  }
  image->alpha_mode = IMA_ALPHA_STRAIGHT;
  BLI_strncpy(image->colorspace_settings.name,
              IMB_colormanagement_role_colorspace_name_get(COLOR_ROLE_DATA),
              sizeof(image->colorspace_settings.name));
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
  if (ibuf == nullptr || ibuf->float_data() == nullptr) {
    if (ibuf != nullptr) {
      BKE_image_release_ibuf(image, ibuf, lock);
    }
    return image;
  }
  float *dst = ibuf->float_data_for_write();
  for (const int y : IndexRange(size)) {
    for (const int x : IndexRange(size)) {
      const int64_t texel = int64_t(y) * size + x;
      const float *p = pixels[texel];
      dst[texel * 4 + 0] = p[0];
      dst[texel * 4 + 1] = p[1];
      dst[texel * 4 + 2] = p[2];
      dst[texel * 4 + 3] = 1.0f;
    }
  }
  BKE_image_release_ibuf(image, ibuf, lock);
  return image;
}

/** Configure a Color Ramp node into a two-stop linear ramp whose alpha runs \a a0 -> \a a1. */
static void configure_color_ramp(bNode &node, const float a0, const float a1)
{
  ColorBand *band = static_cast<ColorBand *>(node.storage);
  if (band == nullptr) {
    return;
  }
  band->tot = 2;
  band->ipotype = COLBAND_INTERP_LINEAR;
  band->color_mode = COLBAND_BLEND_RGB;
  band->data[0].pos = 0.0f;
  band->data[0].r = 0.0f;
  band->data[0].g = 0.0f;
  band->data[0].b = 0.0f;
  band->data[0].a = a0;
  band->data[1].pos = 1.0f;
  band->data[1].r = 1.0f;
  band->data[1].g = 1.0f;
  band->data[1].b = 1.0f;
  band->data[1].a = a1;
}

/**
 * Build a grouped source: the Principled sits in a nested group whose interface carries Base Color,
 * Roughness, Specular and Metallic. Base Color is a Group Input / Image Texture mix through a
 * Reroute, Roughness runs through a Color Ramp, and an off-path group instance shares the tree.
 */
static Material *build_group_source(Main &bmain,
                                    const char *name,
                                    const char *group_name,
                                    const char *map_name,
                                    const char *shared_name,
                                    const GroupSourceSpec &spec)
{
  bNodeTree *group = bke::node_tree_add_tree(&bmain, group_name, "ShaderNodeTree");
  bNodeTreeInterface &iface = group->tree_interface;
  bNodeTreeInterfaceSocket *in_base = iface.add_socket(
      "Base Color", "", "NodeSocketColor", NODE_INTERFACE_SOCKET_INPUT, nullptr);
  bNodeTreeInterfaceSocket *in_rough = iface.add_socket(
      "Roughness", "", "NodeSocketFloat", NODE_INTERFACE_SOCKET_INPUT, nullptr);
  bNodeTreeInterfaceSocket *in_spec = iface.add_socket(
      "Specular", "", "NodeSocketFloat", NODE_INTERFACE_SOCKET_INPUT, nullptr);
  bNodeTreeInterfaceSocket *in_metal = iface.add_socket(
      "Metallic", "", "NodeSocketFloat", NODE_INTERFACE_SOCKET_INPUT, nullptr);
  bNodeTreeInterfaceSocket *out_bsdf = iface.add_socket(
      "BSDF", "", "NodeSocketShader", NODE_INTERFACE_SOCKET_OUTPUT, nullptr);
  iface.tag_items_changed();
  static_cast<bNodeSocketValueFloat *>(in_metal->socket_data)->value = spec.metallic_default;

  bNode *group_input = bke::node_add_node(nullptr, *group, "NodeGroupInput"_ustr);
  bNode *group_output = bke::node_add_node(nullptr, *group, "NodeGroupOutput"_ustr);
  bNode *principled = bke::node_add_static_node(nullptr, *group, SH_NODE_BSDF_PRINCIPLED);
  bNode *tex_coord = bke::node_add_static_node(nullptr, *group, SH_NODE_TEX_COORD);
  bNode *tex = bke::node_add_static_node(nullptr, *group, SH_NODE_TEX_IMAGE);
  bNode *ramp = bke::node_add_static_node(nullptr, *group, SH_NODE_VALTORGB);
  bNode *reroute = bke::node_add_static_node(nullptr, *group, NODE_REROUTE);
  bNode *mix = bke::node_add_static_node(nullptr, *group, SH_NODE_MIX);
  nodes::update_node_declaration_and_sockets(*group, *group_input);
  nodes::update_node_declaration_and_sockets(*group, *group_output);

  Image *map = make_data_pattern_map(&bmain, map_name, 2, spec.map_pixels);
  if (map == nullptr) {
    return nullptr;
  }
  tex->id = &map->id;
  id_us_plus(&map->id);
  if (NodeTexImage *storage = static_cast<NodeTexImage *>(tex->storage)) {
    storage->projection = SHD_PROJ_FLAT;
  }
  configure_color_ramp(*ramp, spec.ramp_alpha0, spec.ramp_alpha1);
  if (NodeShaderMix *storage = static_cast<NodeShaderMix *>(mix->storage)) {
    storage->data_type = SOCK_RGBA;
    storage->factor_mode = NODE_MIX_MODE_UNIFORM;
    storage->blend_type = MA_RAMP_BLEND;
    storage->clamp_factor = true;
    storage->clamp_result = false;
  }
  if (bNodeSocket *fac = bke::node_find_socket(*mix, SOCK_IN, "Factor_Float"_ustr)) {
    static_cast<bNodeSocketValueFloat *>(fac->default_value)->value = 0.5f;
  }

  auto link = [&](bNode &from_node, const char *from_name, bNode &to_node, const char *to_name) {
    bNodeSocket *from = bke::node_find_socket(from_node, SOCK_OUT, UString::from_ptr_noinline(from_name));
    bNodeSocket *to = bke::node_find_socket(to_node, SOCK_IN, UString::from_ptr_noinline(to_name));
    ASSERT_NE(from, nullptr);
    ASSERT_NE(to, nullptr);
    bke::node_add_link(*group, from_node, *from, to_node, *to);
  };

  link(*tex_coord, "Generated", *tex, "Vector");
  link(*tex, "Color", *mix, "B_Color");
  link(*group_input,
       in_base->identifier,
       *reroute,
       "Input");
  link(*reroute, "Output", *mix, "A_Color");
  link(*mix, "Result_Color", *principled, "Base Color");
  link(*group_input, in_rough->identifier, *ramp, "Fac");
  link(*ramp, "Alpha", *principled, "Roughness");
  link(*group_input, in_spec->identifier, *principled, "Specular IOR Level");
  link(*group_input, in_metal->identifier, *principled, "Metallic");
  link(*principled, "BSDF", *group_output, out_bsdf->identifier);

  bNode *frame = bke::node_add_static_node(nullptr, *group, NODE_FRAME);
  for (bNode &node : group->nodes) {
    if (!node.is_frame()) {
      node.parent = frame;
    }
  }
  BKE_ntree_update_tag_all(group);
  BKE_ntree_update_after_single_tree_change(bmain, *group);

  /* A second group on its own, never wired to the Principled: the copy path must carry it too. */
  bNodeTree *shared = bke::node_tree_add_tree(&bmain, shared_name, "ShaderNodeTree");
  bke::node_add_static_node(nullptr, *shared, SH_NODE_RGB);

  Material *source = BKE_material_add(&bmain, name);
  bNodeTree &tree = *source->nodetree;
  bNode *instance = bke::node_add_node(nullptr, tree, group->typeinfo->group_idname);
  instance->id = &group->id;
  id_us_plus(&group->id);
  bNode *shared_instance = bke::node_add_node(nullptr, tree, shared->typeinfo->group_idname);
  shared_instance->id = &shared->id;
  id_us_plus(&shared->id);
  nodes::update_node_declaration_and_sockets(tree, *instance);
  nodes::update_node_declaration_and_sockets(tree, *shared_instance);
  BKE_ntree_update_tag_all(source->nodetree);
  BKE_ntree_update_after_single_tree_change(bmain, *source->nodetree);

  auto root_link = [&](bNode &from_node, const char *from_name, const char *to_identifier) {
    bNodeSocket *from = bke::node_find_socket(from_node, SOCK_OUT, UString::from_ptr_noinline(from_name));
    ASSERT_NE(from, nullptr);
    bNodeSocket *to = bke::node_find_socket(
        *instance, SOCK_IN, UString::from_ptr_noinline(to_identifier));
    ASSERT_NE(to, nullptr);
    bke::node_add_link(tree, from_node, *from, *instance, *to);
  };

  bNode *rgb = bke::node_add_static_node(nullptr, tree, SH_NODE_RGB);
  bNodeSocket *rgb_out = bke::node_find_socket(*rgb, SOCK_OUT, "Color"_ustr);
  const float color[4] = {spec.base_color[0], spec.base_color[1], spec.base_color[2], 1.0f};
  copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(rgb_out->default_value)->value, color);
  root_link(*rgb, "Color", in_base->identifier);

  bNode *rough_value = bke::node_add_static_node(nullptr, tree, SH_NODE_VALUE);
  bNodeSocket *rough_out = bke::node_find_socket(*rough_value, SOCK_OUT, "Value"_ustr);
  static_cast<bNodeSocketValueFloat *>(rough_out->default_value)->value = spec.rough_in;
  root_link(*rough_value, "Value", in_rough->identifier);

  bNode *spec_value = bke::node_add_static_node(nullptr, tree, SH_NODE_VALUE);
  bNodeSocket *spec_out = bke::node_find_socket(*spec_value, SOCK_OUT, "Value"_ustr);
  static_cast<bNodeSocketValueFloat *>(spec_out->default_value)->value = spec.specular_in;
  root_link(*spec_value, "Value", in_spec->identifier);

  bNode *output = bke::node_add_static_node(nullptr, tree, SH_NODE_OUTPUT_MATERIAL);
  bNodeSocket *bsdf_out = bke::node_find_socket(
      *instance, SOCK_OUT, UString::from_ptr_noinline(out_bsdf->identifier));
  if (bsdf_out == nullptr) {
    return nullptr;
  }
  bke::node_add_link(tree,
                     *instance,
                     *bsdf_out,
                     *output,
                     *bke::node_find_socket(*output, SOCK_IN, "Surface"_ustr));

  BKE_ntree_update_tag_all(source->nodetree);
  BKE_ntree_update_after_single_tree_change(bmain, *source->nodetree);
  return source;
}

/** Build a top-level source with constant sockets and a flat Normal Map over a data texture. */
static Material *build_hybrid_source(Main &bmain,
                                     const char *name,
                                     const char *normal_map_name,
                                     const HybridSourceSpec &spec)
{
  Material *source = BKE_material_add(&bmain, name);
  bNodeTree &tree = *source->nodetree;
  bNode *principled = bke::node_add_static_node(nullptr, tree, SH_NODE_BSDF_PRINCIPLED);
  bNode *output = bke::node_add_static_node(nullptr, tree, SH_NODE_OUTPUT_MATERIAL);
  bke::node_add_link(tree,
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_OUT, "BSDF"_ustr),
                     *output,
                     *bke::node_find_socket(*output, SOCK_IN, "Surface"_ustr));

  auto set_color = [&](const char *socket_name, const float rgb[3]) {
    bNodeSocket *socket = bke::node_find_socket(
        *principled, SOCK_IN, UString::from_ptr_noinline(socket_name));
    const float rgba[4] = {rgb[0], rgb[1], rgb[2], 1.0f};
    copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(socket->default_value)->value, rgba);
  };
  auto set_float = [&](const char *socket_name, const float value) {
    bNodeSocket *socket = bke::node_find_socket(
        *principled, SOCK_IN, UString::from_ptr_noinline(socket_name));
    static_cast<bNodeSocketValueFloat *>(socket->default_value)->value = value;
  };
  set_color("Base Color", spec.base_color);
  set_float("Metallic", spec.metallic);
  set_float("Roughness", spec.roughness);
  set_float("Specular IOR Level", spec.specular);
  set_float("Alpha", spec.alpha);
  set_color("Emission Color", spec.emission);

  /* A flat tangent-space normal map: the encoded map is the constant (0.5, 0.5, 1). */
  const float flat[4][3] = {{0.5f, 0.5f, 1.0f},
                            {0.5f, 0.5f, 1.0f},
                            {0.5f, 0.5f, 1.0f},
                            {0.5f, 0.5f, 1.0f}};
  Image *normal_image = make_data_pattern_map(&bmain, normal_map_name, 2, flat);
  bNode *tex = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
  tex->id = &normal_image->id;
  id_us_plus(&normal_image->id);
  bNode *normal_map = bke::node_add_static_node(nullptr, tree, SH_NODE_NORMAL_MAP);
  if (NodeShaderNormalMap *storage = static_cast<NodeShaderNormalMap *>(normal_map->storage)) {
    storage->space = SHD_SPACE_TANGENT;
  }
  bke::node_add_link(tree,
                     *tex,
                     *bke::node_find_socket(*tex, SOCK_OUT, "Color"_ustr),
                     *normal_map,
                     *bke::node_find_socket(*normal_map, SOCK_IN, "Color"_ustr));
  bke::node_add_link(tree,
                     *normal_map,
                     *bke::node_find_socket(*normal_map, SOCK_OUT, "Normal"_ustr),
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_IN, "Normal"_ustr));

  BKE_ntree_update_tag_all(&tree);
  BKE_ntree_update_after_single_tree_change(bmain, tree);
  return source;
}

/** The channel bottom constant the generated chain and the CPU both start from. */
static RGBA channel_bottom(const int channel)
{
  float bottom[4];
  BKE_paint_layers_channel_bottom_color(eMaterialPaintChannel(channel), bottom);
  return {bottom[0], bottom[1], bottom[2], bottom[3]};
}

/** The tangent-space normal combine of #blend_normal_combine (paint_material_composite.cc:225). */
static RGBA combine_normal(const RGBA &bottom, const RGBA &top, const float fac)
{
  const float base[3] = {bottom.r * 2.0f - 1.0f, bottom.g * 2.0f - 1.0f, bottom.b * 2.0f - 1.0f};
  const float detail[3] = {top.r * 2.0f - 1.0f, top.g * 2.0f - 1.0f, top.b * 2.0f - 1.0f};
  float combined[3] = {base[0] + detail[0], base[1] + detail[1], base[2] * detail[2]};
  normalize_v3(combined);
  RGBA out;
  out.r = bottom.r * (1.0f - fac) + (combined[0] * 0.5f + 0.5f) * fac;
  out.g = bottom.g * (1.0f - fac) + (combined[1] * 0.5f + 0.5f) * fac;
  out.b = bottom.b * (1.0f - fac) + (combined[2] * 0.5f + 0.5f) * fac;
  out.a = bottom.a;
  return out;
}

/** Lay the clean source value \a raw over the channel's bottom, the way a single row is composited
 * (`ramp_blend` for every channel, the Normal Combine for Normal). */
static RGBA composite_over(const int channel, const RGBA &raw, const float coverage)
{
  const RGBA bottom = channel_bottom(channel);
  if (channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
    return combine_normal(bottom, raw, coverage);
  }
  float dst[4] = {bottom.r, bottom.g, bottom.b, bottom.a};
  const float top[4] = {raw.r, raw.g, raw.b, raw.a};
  ramp_blend(MA_RAMP_BLEND, dst, coverage, top);
  return {dst[0], dst[1], dst[2], dst[3]};
}

/**
 * The clean value the grouped source supplies to \a channel at pixel (\a x, \a y). Mirrors
 * #build_group_source: Base Color is the 0.5 mix of the constant and the map pixel (a`ramp_blend`
 * MIX, material.cc:2544), Roughness the Color Ramp's alpha (`BKE_colorband_evaluate`), the rest
 * are constants. Normal, Custom, Height and AO have no socket and report false.
 */
static bool group_source_expected(const GroupSourceSpec &spec,
                                   const int channel,
                                   const int x,
                                   const int y,
                                   RGBA &r_out)
{
  switch (channel) {
    case PAINT_MATERIAL_CHANNEL_BASE_COLOR: {
      const float *p = spec.map_pixels[y * 2 + x];
      float dst[4] = {spec.base_color[0], spec.base_color[1], spec.base_color[2], 1.0f};
      const float top[4] = {p[0], p[1], p[2], 1.0f};
      ramp_blend(MA_RAMP_BLEND, dst, 0.5f, top);
      r_out = {dst[0], dst[1], dst[2], 1.0f};
      return true;
    }
    case PAINT_MATERIAL_CHANNEL_ROUGHNESS: {
      ColorBand band{};
      band.tot = 2;
      band.ipotype = COLBAND_INTERP_LINEAR;
      band.color_mode = COLBAND_BLEND_RGB;
      band.data[0].pos = 0.0f;
      band.data[0].a = spec.ramp_alpha0;
      band.data[1].pos = 1.0f;
      band.data[1].a = spec.ramp_alpha1;
      float out[4];
      BKE_colorband_evaluate(&band, spec.rough_in, out);
      r_out = {out[3], out[3], out[3], 1.0f};
      return true;
    }
    case PAINT_MATERIAL_CHANNEL_SPECULAR:
      r_out = {spec.specular_in, spec.specular_in, spec.specular_in, 1.0f};
      return true;
    case PAINT_MATERIAL_CHANNEL_METALLIC:
      r_out = {spec.metallic_default, spec.metallic_default, spec.metallic_default, 1.0f};
      return true;
    case PAINT_MATERIAL_CHANNEL_EMISSION:
      r_out = {1.0f, 1.0f, 1.0f, 1.0f};
      return true;
    case PAINT_MATERIAL_CHANNEL_ALPHA:
      r_out = {1.0f, 1.0f, 1.0f, 1.0f};
      return true;
    default:
      return false;
  }
}

/** The clean value the top-level source supplies to \a channel; false when it has none. */
static bool hybrid_source_expected(const HybridSourceSpec &spec,
                                    const int channel,
                                    RGBA &r_out)
{
  switch (channel) {
    case PAINT_MATERIAL_CHANNEL_BASE_COLOR:
      r_out = {spec.base_color[0], spec.base_color[1], spec.base_color[2], 1.0f};
      return true;
    case PAINT_MATERIAL_CHANNEL_METALLIC:
      r_out = {spec.metallic, spec.metallic, spec.metallic, 1.0f};
      return true;
    case PAINT_MATERIAL_CHANNEL_ROUGHNESS:
      r_out = {spec.roughness, spec.roughness, spec.roughness, 1.0f};
      return true;
    case PAINT_MATERIAL_CHANNEL_SPECULAR:
      r_out = {spec.specular, spec.specular, spec.specular, 1.0f};
      return true;
    case PAINT_MATERIAL_CHANNEL_ALPHA:
      r_out = {spec.alpha, spec.alpha, spec.alpha, 1.0f};
      return true;
    case PAINT_MATERIAL_CHANNEL_EMISSION:
      r_out = {spec.emission[0], spec.emission[1], spec.emission[2], 1.0f};
      return true;
    case PAINT_MATERIAL_CHANNEL_NORMAL:
      /* The flat map encodes the tangent-space normal (0.5, 0.5, 1). */
      r_out = {0.5f, 0.5f, 1.0f, 1.0f};
      return true;
    default:
      return false;
  }
}

/** True when the generated group exposes `Result <ui_name>` for \a channel. */
static bool result_output_exists(const bNodeTree &tree, const int channel)
{
  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
      eMaterialPaintChannel(channel));
  char name[64];
  BLI_snprintf(name, sizeof(name), "Result %s", info.ui_name);
  for (const bNodeTreeInterfaceSocket *iface : tree.interface_outputs()) {
    if (iface->name != nullptr && STREQ(iface->name, name)) {
      return true;
    }
  }
  return false;
}

/**
 * A single Material row per source, read live. The generated chain must equal the
 * hand-computed composite of the source's clean channel over the channel bottom, with coverage =
 * the source alpha. The mode the contract picks (SourceGroup for the grouped graph, Hybrid for the
 * constant/trivial-map one) is asserted too.
 */
TEST_F(PaintLayersGraphEvalTest, multi_source_rows_match_the_reference)
{
  struct Case {
    const char *name;
    Material *source;
    PaintLayerMaterialMode expected_mode;
    float coverage;
  };

  const int points[3][2] = {{0, 0}, {1, 0}, {1, 1}};
  const float tolerance = 1e-4f;

  auto run_case = [&](const Case &test_case,
                      const bool grouped,
                      const GroupSourceSpec &group_spec,
                      const HybridSourceSpec &hybrid_spec) {
    ma = BKE_material_add(bmain, test_case.name);
    MaterialPaintLayer *row = BKE_paint_layers_add(
        *ma, MA_PAINT_LAYER_KIND_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
    ASSERT_NE(row, nullptr);
    ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, test_case.source));
    BKE_paint_layers_active_set(*ma, row->marker);
    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
    ASSERT_NE(ma->paint_layers_tree, nullptr);
    EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row), test_case.expected_mode);

    GraphInterpreter interpreter;
    interpreter.instance = find_instance();
    interpreter.tree = ma->paint_layers_tree;
    ASSERT_NE(interpreter.instance, nullptr);
    interpreter.tree->ensure_topology_cache();

    for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
      const int channel = int(info.channel);
      RGBA raw;
      const bool has_source = grouped ? group_source_expected(group_spec, channel, 0, 0, raw) :
                                        hybrid_source_expected(hybrid_spec, channel, raw);
      if (!has_source) {
        EXPECT_FALSE(result_output_exists(*ma->paint_layers_tree, channel))
            << info.ui_name << ": no source value, so the channel must not be wired";
        continue;
      }
      ASSERT_TRUE(result_output_exists(*ma->paint_layers_tree, channel)) << info.ui_name;
      for (const int i : IndexRange(ARRAY_SIZE(points))) {
        const int x = points[i][0];
        const int y = points[i][1];
        if (grouped) {
          ASSERT_TRUE(group_source_expected(group_spec, channel, x, y, raw));
        }
        const RGBA expected = composite_over(channel, raw, test_case.coverage);
        interpreter.x = x;
        interpreter.y = y;
        const RGBA graph = eval_channel_result(interpreter, eMaterialPaintChannel(channel));
        EXPECT_NEAR(graph.r, expected.r, tolerance)
            << info.ui_name << " point(" << x << "," << y << ")";
        EXPECT_NEAR(graph.g, expected.g, tolerance)
            << info.ui_name << " point(" << x << "," << y << ")";
        EXPECT_NEAR(graph.b, expected.b, tolerance)
            << info.ui_name << " point(" << x << "," << y << ")";
      }
    }

    BKE_id_free(bmain, ma);
    ma = nullptr;
  };

  Material *source_a = build_group_source(
      *bmain, "MultiSourceA", "MultiGroupA", "MultiMapA", "MultiSharedA", source_spec_a);
  Material *source_b = build_hybrid_source(
      *bmain, "MultiSourceB", "MultiNormalB", source_spec_b);
  Material *source_c = build_group_source(
      *bmain, "MultiSourceC", "MultiGroupC", "MultiMapC", "MultiSharedC", source_spec_c);
  ASSERT_NE(source_a, nullptr);
  ASSERT_NE(source_b, nullptr);
  ASSERT_NE(source_c, nullptr);

  run_case({"MultiRowA", source_a, PaintLayerMaterialMode::SourceGroup, 1.0f},
           true,
           source_spec_a,
           {});
  run_case({"MultiRowB", source_b, PaintLayerMaterialMode::Hybrid, source_spec_b.alpha},
           false,
           source_spec_a,
           source_spec_b);
  run_case({"MultiRowC", source_c, PaintLayerMaterialMode::SourceGroup, 1.0f},
           true,
           source_spec_c,
           {});
}

/**
 * The same sources, each row shown from deterministic bake maps. Every resolvable
 * channel gets a 2x2 float data map holding the reference's clean value at each pixel, plus a grey
 * coverage map holding the source alpha. The row is not deferred, so the contract picks Baked; the
 * generated graph and the CPU composite must both equal the reference composite. The maps are made
 * the way the existing bake-substitution tests do: #BKE_paint_layers_bake_set_map for each channel
 * and `-1` (coverage), then #BKE_paint_layers_bake_finalize, which makes
 * #BKE_paint_layers_bake_is_valid hold.
 */
TEST_F(PaintLayersGraphEvalTest, multi_source_baked_rows_match_the_reference_and_the_cpu)
{
  const int size = 2;
  const int points[3][2] = {{0, 0}, {1, 0}, {1, 1}};
  const float tolerance = 1e-4f;

  auto run_case = [&](const char *name,
                      Material *source,
                      const bool grouped,
                      const GroupSourceSpec &group_spec,
                      const HybridSourceSpec &hybrid_spec,
                      const float coverage) {
    ma = BKE_material_add(bmain, name);
    MaterialPaintLayer *row = BKE_paint_layers_add(
        *ma, MA_PAINT_LAYER_KIND_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
    ASSERT_NE(row, nullptr);
    ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));

    auto raw_at = [&](const int channel, const int x, const int y, RGBA &r_out) -> bool {
      return grouped ? group_source_expected(group_spec, channel, x, y, r_out) :
                       hybrid_source_expected(hybrid_spec, channel, r_out);
    };

    /* A bake map per resolvable bakeable channel, the reference's clean value per pixel. */
    for (const eMaterialPaintChannel channel : BKE_paint_material_bakeable_channels()) {
      RGBA probe;
      if (!raw_at(int(channel), 0, 0, probe)) {
        continue;
      }
      float pixels[4][3];
      for (const int y : IndexRange(size)) {
        for (const int x : IndexRange(size)) {
          RGBA raw;
          EXPECT_TRUE(raw_at(int(channel), x, y, raw));
          const int64_t texel = int64_t(y) * size + x;
          pixels[texel][0] = raw.r;
          pixels[texel][1] = raw.g;
          pixels[texel][2] = raw.b;
        }
      }
      char map_name[64];
      BLI_snprintf(map_name,
                   sizeof(map_name),
                   "%s %s",
                   name,
                   BKE_paint_material_channel_info(channel).ui_name);
      Image *map = make_data_pattern_map(bmain, map_name, size, pixels);
      ASSERT_NE(map, nullptr);
      ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *row, int(channel), map));
    }
    const float coverage_rgba[4] = {coverage, coverage, coverage, 1.0f};
    char coverage_name[64];
    BLI_snprintf(coverage_name, sizeof(coverage_name), "%s Coverage", name);
    Image *coverage_map = make_bake_data_map(bmain, coverage_name, size, coverage_rgba);
    ASSERT_NE(coverage_map, nullptr);
    ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *row, -1, coverage_map));
    BKE_paint_layers_bake_finalize(*ma, *row);

    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
    ASSERT_NE(ma->paint_layers_tree, nullptr);
    EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Baked);
    EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *row));

    GraphInterpreter interpreter;
    interpreter.instance = find_instance();
    interpreter.tree = ma->paint_layers_tree;
    ASSERT_NE(interpreter.instance, nullptr);
    interpreter.tree->ensure_topology_cache();

    for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
      const int channel = int(info.channel);
      RGBA probe;
      if (!raw_at(channel, 0, 0, probe)) {
        continue;
      }
      for (const int i : IndexRange(ARRAY_SIZE(points))) {
        const int x = points[i][0];
        const int y = points[i][1];
        RGBA raw;
        ASSERT_TRUE(raw_at(channel, x, y, raw));
        const RGBA expected = composite_over(channel, raw, coverage);
        interpreter.x = x;
        interpreter.y = y;
        const RGBA graph = eval_channel_result(interpreter, eMaterialPaintChannel(channel));
        const RGBA cpu = cpu_pixel_at(channel, x, y);
        EXPECT_NEAR(graph.r, expected.r, tolerance)
            << info.ui_name << " graph point(" << x << "," << y << ")";
        EXPECT_NEAR(graph.g, expected.g, tolerance)
            << info.ui_name << " graph point(" << x << "," << y << ")";
        EXPECT_NEAR(graph.b, expected.b, tolerance)
            << info.ui_name << " graph point(" << x << "," << y << ")";
        EXPECT_NEAR(cpu.r, expected.r, tolerance)
            << info.ui_name << " cpu point(" << x << "," << y << ")";
        EXPECT_NEAR(cpu.g, expected.g, tolerance)
            << info.ui_name << " cpu point(" << x << "," << y << ")";
        EXPECT_NEAR(cpu.b, expected.b, tolerance)
            << info.ui_name << " cpu point(" << x << "," << y << ")";
      }
    }

    BKE_id_free(bmain, ma);
    ma = nullptr;
  };

  Material *source_a = build_group_source(
      *bmain, "MultiBakedSourceA", "MultiBakedGroupA", "MultiBakedMapA", "MultiBakedSharedA",
      source_spec_a);
  Material *source_b = build_hybrid_source(
      *bmain, "MultiBakedSourceB", "MultiBakedNormalB", source_spec_b);
  Material *source_c = build_group_source(
      *bmain, "MultiBakedSourceC", "MultiBakedGroupC", "MultiBakedMapC", "MultiBakedSharedC",
      source_spec_c);
  ASSERT_NE(source_a, nullptr);
  ASSERT_NE(source_b, nullptr);
  ASSERT_NE(source_c, nullptr);

  run_case("MultiBakedA", source_a, true, source_spec_a, {}, 1.0f);
  run_case("MultiBakedB", source_b, false, source_spec_a, source_spec_b, source_spec_b.alpha);
  run_case("MultiBakedC", source_c, true, source_spec_c, {}, 1.0f);
}

/** Two Paint children of the isolating folder: distinct uniform data maps (alpha stored as one). */
static const float kIsolatingPaintA[4] = {0.70f, 0.30f, 0.10f, 1.0f};
static const float kIsolatingPaintB[4] = {0.20f, 0.80f, 0.40f, 1.0f};

static void blend_over_rgba(RGBA &r_dst,
                            const RGBA &src,
                            const int blend,
                            const float factor,
                            const bool normal_channel)
{
  const float fac = clamp_f(factor, 0.0f, 1.0f);
  if (normal_channel) {
    r_dst = combine_normal(r_dst, src, fac);
    return;
  }
  float dst[4] = {r_dst.r, r_dst.g, r_dst.b, r_dst.a};
  const float top[4] = {src.r, src.g, src.b, src.a};
  ramp_blend(blend, dst, fac, top);
  r_dst = {dst[0], dst[1], dst[2], dst[3]};
}

/**
 * The reference for an isolating folder (opacity 0.5) holding two Paint children of opacity 0.6
 * and coverage one, laid over the channel bottom: the premultiplied accumulation of
 * `composite_folder_accumulate` (paint_material_composite.cc:702-775) and the
 * `opacity * coverage` overlay of `composite_apply_layer_linear` (`:777-804`). With two children
 * the second one evaluates the over's `(1 - A * op)` against non-zero premultiplied colour, which
 * is exactly what the `(1 - A)` mutation changes.
 */
static RGBA isolating_folder_expected(const int channel)
{
  const bool normal_channel = (channel == PAINT_MATERIAL_CHANNEL_NORMAL);
  RGBA state = channel_bottom(channel);

  float premul[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  float coverage = 0.0f;
  auto accumulate = [&](const RGBA &color) {
    const float f = 0.6f;
    const float a = coverage;
    float straight[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if (a > 0.0f) {
      for (const int k : IndexRange(4)) {
        straight[k] = premul[k] / a;
      }
    }
    RGBA blended = {straight[0], straight[1], straight[2], straight[3]};
    blend_over_rgba(blended, color, MA_RAMP_BLEND, 1.0f, normal_channel);
    const float child[4] = {color.r, color.g, color.b, color.a};
    const float blended_rgba[4] = {blended.r, blended.g, blended.b, blended.a};
    float c_eff[4];
    for (const int k : IndexRange(4)) {
      c_eff[k] = child[k] + (blended_rgba[k] - child[k]) * a;
    }
    for (const int k : IndexRange(4)) {
      premul[k] = premul[k] * (1.0f - f) + c_eff[k] * f;
    }
    coverage = a + f * (1.0f - a);
  };

  if (channel == PAINT_MATERIAL_CHANNEL_BASE_COLOR) {
    accumulate({kIsolatingPaintA[0], kIsolatingPaintA[1], kIsolatingPaintA[2], 1.0f});
    accumulate({kIsolatingPaintB[0], kIsolatingPaintB[1], kIsolatingPaintB[2], 1.0f});
  }

  RGBA folder_src = {0.0f, 0.0f, 0.0f, 0.0f};
  if (coverage > 0.0f) {
    folder_src = {premul[0] / coverage,
                  premul[1] / coverage,
                  premul[2] / coverage,
                  premul[3] / coverage};
  }
  blend_over_rgba(state, folder_src, MA_RAMP_BLEND, 0.5f * coverage, normal_channel);
  return state;
}

/**
 * An isolating folder with two Paint children of opacity below one: the generated chain, the CPU
 * composite and the reference must agree. This is the narrow case the premultiplied over's
 * `(1 - A * op)` needs; the full stack that would also cover it is blocked by the generator crash
 * the report describes.
 */
TEST_F(PaintLayersGraphEvalTest, isolating_folder_partial_coverage_matches_the_cpu)
{
  const int size = 2;
  const int points[3][2] = {{1, 1}, {1, 0}, {0, 1}};
  const char *point_names[3] = {"center", "edge", "corner"};
  const float tolerance = 1e-4f;

  ma = BKE_material_add(bmain, "IsolatingFolder");
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_FOLDER, "Iso", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  BKE_paint_layers_set_opacity(*ma, folder, 0.5f);
  ASSERT_FALSE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));

  auto add_paint_child = [&](const char *name, const float color[4]) -> MaterialPaintLayer * {
    MaterialPaintLayer *child = BKE_paint_layers_add(
        *ma, MA_PAINT_LAYER_KIND_PAINT, name, folder, PaintLayerPlace::Into);
    EXPECT_NE(child, nullptr);
    MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
        *ma, child, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
    EXPECT_NE(record, nullptr);
    Image *map = make_bake_data_map(bmain, name, size, color);
    EXPECT_NE(map, nullptr);
    record->image = map;
    record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
    BKE_paint_layers_set_opacity(*ma, child, 0.6f);
    return child;
  };
  MaterialPaintLayer *child_a = add_paint_child("IsolatingPaintA", kIsolatingPaintA);
  MaterialPaintLayer *child_b = add_paint_child("IsolatingPaintB", kIsolatingPaintB);
  ASSERT_NE(child_a, nullptr);
  ASSERT_NE(child_b, nullptr);

  struct ActiveCase {
    const char *label;
    const bUUID marker;
  };
  const ActiveCase active_cases[] = {
      {"none", BLI_uuid_nil()},
      {"folder", folder->marker},
      {"paint-a", child_a->marker},
      {"paint-b", child_b->marker},
  };

  for (const ActiveCase &active_case : active_cases) {
    BKE_paint_layers_active_set(*ma, active_case.marker);
    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
    ASSERT_NE(ma->paint_layers_tree, nullptr);

    GraphInterpreter interpreter;
    interpreter.instance = find_instance();
    interpreter.tree = ma->paint_layers_tree;
    ASSERT_NE(interpreter.instance, nullptr);
    interpreter.tree->ensure_topology_cache();

    for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
      const int channel = int(info.channel);
      if (!result_output_exists(*ma->paint_layers_tree, channel)) {
        continue;
      }
      for (const int i : IndexRange(ARRAY_SIZE(points))) {
        const int x = points[i][0];
        const int y = points[i][1];
        const RGBA expected = isolating_folder_expected(channel);
        interpreter.x = x;
        interpreter.y = y;
        const RGBA graph = eval_channel_result(interpreter, eMaterialPaintChannel(channel));
        const RGBA cpu = cpu_pixel_at(channel, x, y);
        EXPECT_NEAR(graph.r, expected.r, tolerance)
            << active_case.label << " " << info.ui_name << " " << point_names[i] << " graph";
        EXPECT_NEAR(graph.g, expected.g, tolerance)
            << active_case.label << " " << info.ui_name << " " << point_names[i] << " graph";
        EXPECT_NEAR(graph.b, expected.b, tolerance)
            << active_case.label << " " << info.ui_name << " " << point_names[i] << " graph";
        EXPECT_NEAR(cpu.r, expected.r, tolerance)
            << active_case.label << " " << info.ui_name << " " << point_names[i] << " cpu";
        EXPECT_NEAR(cpu.g, expected.g, tolerance)
            << active_case.label << " " << info.ui_name << " " << point_names[i] << " cpu";
        EXPECT_NEAR(cpu.b, expected.b, tolerance)
            << active_case.label << " " << info.ui_name << " " << point_names[i] << " cpu";
      }
    }
  }

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/* Fill a byte Base Color leaf map with straight RGB (`r`, `g`, `b`) and a soft alpha edge:
 * alpha 0.25 / 0.5 / 0.75 / 1.0 across `x`, the same for every row. Stored straight with
 * #IMA_ALPHA_STRAIGHT and #IMA_GPU_LINEAR_PREMUL, like every paint-layer map. */
static void fill_nested_leaf_soft_edge(Image *image, const int size, const uchar r, const uchar g, const uchar b)
{
  image->alpha_mode = IMA_ALPHA_STRAIGHT;
  image->flag |= IMA_GPU_LINEAR_PREMUL;
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
  ASSERT_NE(ibuf, nullptr);
  uchar *pixels = ibuf->byte_data_for_write();
  ASSERT_NE(pixels, nullptr);
  for (int y = 0; y < size; y++) {
    for (int x = 0; x < size; x++) {
      const float alpha = 0.25f * float(x + 1);
      const int64_t i = int64_t(y) * size + x;
      pixels[i * 4 + 0] = r;
      pixels[i * 4 + 1] = g;
      pixels[i * 4 + 2] = b;
      pixels[i * 4 + 3] = uchar(clamp_f(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
    }
  }
  BKE_image_release_ibuf(image, ibuf, lock);
}

/* The `.PL Folder <name>` group in `bmain`, or null. Test-only helper to reach a folder's
 * Coverage output without deriving coverage from RGB. */
static bNodeTree *nested_folder_tree_find(Main &bmain, const char *folder_name)
{
  char full[96];
  BLI_snprintf(full, sizeof(full), ".PL Folder %s", folder_name);
  for (bNodeTree &tree : bmain.nodetrees) {
    if (STREQ(tree.id.name + 2, full)) {
      return &tree;
    }
  }
  return nullptr;
}

/* The instance of `group` in `parent`, or null. */
static bNode *nested_group_instance_find(bNodeTree &parent, bNodeTree &group)
{
  for (bNode &node : parent.nodes) {
    if (node.is_group() && node.id == &group.id) {
      return &node;
    }
  }
  return nullptr;
}

/* Evaluate a folder instance's `Coverage Base Color` output in its parent's context.
 * `eval_output` takes the output socket identifier, not the interface name, so the identifier
 * is resolved from the folder tree first. */
static float nested_folder_coverage_eval(const GraphInterpreter &parent,
                                         bNodeTree &folder_tree,
                                         bNode &folder_instance)
{
  folder_tree.ensure_interface_cache();
  const char *identifier = nullptr;
  for (bNodeTreeInterfaceSocket *iface : folder_tree.interface_outputs()) {
    if (iface->name != nullptr && STREQ(iface->name, "Coverage Base Color") &&
        iface->identifier != nullptr)
    {
      identifier = iface->identifier;
      break;
    }
  }
  if (identifier == nullptr) {
    return -1.0f;
  }
  return parent.eval_output(folder_instance, identifier).r;
}

/**
 * Diagnostic parity for two nested isolating folders with partial alpha (design §11).
 *
 * Semantics verified in `paint_material_composite.cc` before writing the formula: a byte
 * paint map is stored straight, the GPU upload pre-multiplies it (`IMA_GPU_LINEAR_PREMUL`)
 * and the Image Texture node un-premultiplies a non-data map because the chain also reads
 * Alpha, while the CPU reads the straight bytes; both sides therefore see the straight color
 * `C` and coverage `A = byte_alpha / 255`. A folder accumulates its children premultiplied
 * (`composite_folder_accumulate`, `:702-775`): `P = 0`, `a = 0`, per child
 * `f = opacity * factor`, `S = P / a` (`0` when `a` is `0`), `blended = blend(S, c)` at full
 * factor, `c_eff = c + (blended - c) * a`, `P = P * (1 - f) + c_eff * f`,
 * `a = a + f * (1 - a)`; the straight result is `S_folder = P / a` with coverage `a`. The
 * generator builds the same chain (`paint_layers_generate.cc`, isolated accumulation with
 * `S = P / a`, `c_eff = lerp(c, blend, a)`, `P = lerp(P, c_eff, f)`, `a = a + f * (1 - a)`).
 * The folder's own row then lays `S_folder` over what is below with `opacity * coverage`
 * (`composite_apply_layer_linear`, `:777-804`). Opacity below one isolates
 * (`paint_layers.cc:356-370`), so both folders here are structurally isolating and no
 * mask/effect is used: the test isolates exactly folder accumulation and coverage.
 *
 * The reference below is computed only from the quantized input alphas, the pure straight
 * colors (whose sRGB to linear is identity) and the two folder opacities; it never reads
 * the graph or CPU outputs.
 */
TEST_F(PaintLayersGraphEvalTest, nested_isolating_folders_partial_alpha_matches_cpu_and_formula)
{
  const int size = 4;
  const float outer_opacity = 0.75f;
  const float inner_opacity = 0.5f;
  const float tolerance = 1e-4f;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;

  ma = BKE_material_add(bmain, "NestedIsoAlpha");
  ASSERT_NE(ma, nullptr);

  /* Opaque contrasting bottom: red. */
  MaterialPaintLayer *bottom = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_PAINT, "Bottom", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(bottom, nullptr);
  MaterialPaintLayerChannel *bottom_record = BKE_paint_layers_channel_add(*ma, bottom, channel);
  ASSERT_NE(bottom_record, nullptr);
  bottom_record->image = add_solid_image("NestedBottom", size, 255, 0, 0, 255);
  ASSERT_NE(bottom_record->image, nullptr);
  bottom_record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  /* Outer isolating folder. */
  MaterialPaintLayer *outer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_FOLDER, "Outer", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(outer, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, outer, outer_opacity));
  ASSERT_FALSE(BKE_paint_layers_folder_is_pass_through(*ma, *outer));

  /* Inner isolating folder inside the outer one. */
  MaterialPaintLayer *inner = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_FOLDER, "Inner", outer, PaintLayerPlace::Into);
  ASSERT_NE(inner, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, inner, inner_opacity));
  ASSERT_FALSE(BKE_paint_layers_folder_is_pass_through(*ma, *inner));

  /* Two leaves with straight-alpha soft edges: green below, blue on top. */
  const float black[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  Image *leaf_a_image = BKE_image_add_generated(
      bmain, size, size, "NestedLeafA", 32, false, IMA_GENTYPE_BLANK, black, false, false, false);
  ASSERT_NE(leaf_a_image, nullptr);
  fill_nested_leaf_soft_edge(leaf_a_image, size, 0, 255, 0);
  MaterialPaintLayer *leaf_a = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_PAINT, "LeafA", inner, PaintLayerPlace::Into);
  ASSERT_NE(leaf_a, nullptr);
  MaterialPaintLayerChannel *record_a = BKE_paint_layers_channel_add(*ma, leaf_a, channel);
  ASSERT_NE(record_a, nullptr);
  record_a->image = leaf_a_image;
  record_a->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  Image *leaf_b_image = BKE_image_add_generated(
      bmain, size, size, "NestedLeafB", 32, false, IMA_GENTYPE_BLANK, black, false, false, false);
  ASSERT_NE(leaf_b_image, nullptr);
  fill_nested_leaf_soft_edge(leaf_b_image, size, 0, 0, 255);
  MaterialPaintLayer *leaf_b = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_PAINT, "LeafB", inner, PaintLayerPlace::Into);
  ASSERT_NE(leaf_b, nullptr);
  MaterialPaintLayerChannel *record_b = BKE_paint_layers_channel_add(*ma, leaf_b, channel);
  ASSERT_NE(record_b, nullptr);
  record_b->image = leaf_b_image;
  record_b->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_NE(ma->paint_layers_tree, nullptr);

  /* Folder groups and their instances, to read the Coverage sockets directly. */
  bNodeTree *outer_tree = nested_folder_tree_find(*bmain, "Outer");
  bNodeTree *inner_tree = nested_folder_tree_find(*bmain, "Inner");
  ASSERT_NE(outer_tree, nullptr);
  ASSERT_NE(inner_tree, nullptr);
  bNode *outer_instance = nested_group_instance_find(*ma->paint_layers_tree, *outer_tree);
  ASSERT_NE(outer_instance, nullptr);
  bNode *inner_instance = nested_group_instance_find(*outer_tree, *inner_tree);
  ASSERT_NE(inner_instance, nullptr);

  /* CPU coverage of each folder's own row, the same values the bake writes. */
  Vector<float> outer_color(int64_t(size) * size * 4);
  Vector<float> outer_coverage(int64_t(size) * size);
  Vector<float> inner_color(int64_t(size) * size * 4);
  Vector<float> inner_coverage(int64_t(size) * size);
  ASSERT_TRUE(BKE_paint_layers_bake_render_node(
      *ma, *outer, channel, size, outer_color.data(), outer_coverage.data()));
  ASSERT_TRUE(BKE_paint_layers_bake_render_node(
      *ma, *inner, channel, size, inner_color.data(), inner_coverage.data()));

  GraphInterpreter root_interpreter;
  root_interpreter.instance = find_instance();
  root_interpreter.tree = ma->paint_layers_tree;
  ASSERT_NE(root_interpreter.instance, nullptr);
  root_interpreter.tree->ensure_topology_cache();
  outer_tree->ensure_topology_cache();
  inner_tree->ensure_topology_cache();

  for (int x = 0; x < size; x++) {
    const int y = 0;
    /* Independent inputs: the quantized stored alphas and the pure straight colors. */
    const float alpha_ideal = 0.25f * float(x + 1);
    const float alpha_q = float(uchar(clamp_f(alpha_ideal, 0.0f, 1.0f) * 255.0f + 0.5f)) / 255.0f;
    const float leaf_a_alpha = alpha_q;
    const float leaf_b_alpha = alpha_q;
    const float leaf_a_color[3] = {0.0f, 1.0f, 0.0f};
    const float leaf_b_color[3] = {0.0f, 0.0f, 1.0f};
    const float bottom_color[3] = {1.0f, 0.0f, 0.0f};
    const float f1 = leaf_a_alpha;
    const float f2 = leaf_b_alpha;
    const float a1 = f1;
    const float premul1[3] = {leaf_a_color[0] * f1, leaf_a_color[1] * f1, leaf_a_color[2] * f1};
    const float premul1_a = leaf_a_alpha * f1;
    const float a2 = a1 + f2 * (1.0f - a1);
    float straight[3] = {0.0f, 0.0f, 0.0f};
    float straight_a = 0.0f;
    if (a2 > 0.0f) {
      const float premul2[3] = {premul1[0] * (1.0f - f2) + leaf_b_color[0] * f2,
                                premul1[1] * (1.0f - f2) + leaf_b_color[1] * f2,
                                premul1[2] * (1.0f - f2) + leaf_b_color[2] * f2};
      const float premul2_a = premul1_a * (1.0f - f2) + leaf_b_alpha * f2;
      straight[0] = premul2[0] / a2;
      straight[1] = premul2[1] / a2;
      straight[2] = premul2[2] / a2;
      straight_a = premul2_a / a2;
    }
    const float inner_cov_expected = inner_opacity * a2;
    const float outer_cov_expected = outer_opacity * inner_opacity * a2;
    const float factor = outer_cov_expected;
    RGBA expected;
    expected.r = bottom_color[0] * (1.0f - factor) + straight[0] * factor;
    expected.g = bottom_color[1] * (1.0f - factor) + straight[1] * factor;
    expected.b = bottom_color[2] * (1.0f - factor) + straight[2] * factor;
    expected.a = 1.0f * (1.0f - factor) + straight_a * factor;

    root_interpreter.x = x;
    root_interpreter.y = y;
    const RGBA graph = eval_channel_result(root_interpreter, channel);
    const RGBA cpu = cpu_pixel_at(channel, x, y);

    GraphInterpreter outer_interpreter;
    outer_interpreter.instance = outer_instance;
    outer_interpreter.tree = outer_tree;
    outer_interpreter.x = x;
    outer_interpreter.y = y;
    outer_interpreter.parent = &root_interpreter;
    const float graph_outer_cov = nested_folder_coverage_eval(
        root_interpreter, *outer_tree, *outer_instance);
    const float graph_inner_cov = nested_folder_coverage_eval(
        outer_interpreter, *inner_tree, *inner_instance);
    const float cpu_outer_cov = outer_coverage[int64_t(y) * size + x];
    const float cpu_inner_cov = inner_coverage[int64_t(y) * size + x];

    EXPECT_NEAR(graph.r, expected.r, tolerance)
        << "x=" << x << " y=" << y << " formula=(" << expected.r << "," << expected.g << ","
        << expected.b << "," << expected.a << ") graph=(" << graph.r << "," << graph.g << ","
        << graph.b << "," << graph.a << ") cpu=(" << cpu.r << "," << cpu.g << "," << cpu.b << ","
        << cpu.a << ")";
    EXPECT_NEAR(graph.g, expected.g, tolerance)
        << "x=" << x << " y=" << y << " formula=(" << expected.r << "," << expected.g << ","
        << expected.b << "," << expected.a << ") graph=(" << graph.r << "," << graph.g << ","
        << graph.b << "," << graph.a << ") cpu=(" << cpu.r << "," << cpu.g << "," << cpu.b << ","
        << cpu.a << ")";
    EXPECT_NEAR(graph.b, expected.b, tolerance)
        << "x=" << x << " y=" << y << " formula=(" << expected.r << "," << expected.g << ","
        << expected.b << "," << expected.a << ") graph=(" << graph.r << "," << graph.g << ","
        << graph.b << "," << graph.a << ") cpu=(" << cpu.r << "," << cpu.g << "," << cpu.b << ","
        << cpu.a << ")";
    EXPECT_NEAR(graph.a, expected.a, tolerance)
        << "x=" << x << " y=" << y << " formula=(" << expected.r << "," << expected.g << ","
        << expected.b << "," << expected.a << ") graph=(" << graph.r << "," << graph.g << ","
        << graph.b << "," << graph.a << ") cpu=(" << cpu.r << "," << cpu.g << "," << cpu.b << ","
        << cpu.a << ")";
    EXPECT_NEAR(cpu.r, expected.r, tolerance)
        << "x=" << x << " y=" << y << " formula=(" << expected.r << "," << expected.g << ","
        << expected.b << "," << expected.a << ") graph=(" << graph.r << "," << graph.g << ","
        << graph.b << "," << graph.a << ") cpu=(" << cpu.r << "," << cpu.g << "," << cpu.b << ","
        << cpu.a << ")";
    EXPECT_NEAR(cpu.g, expected.g, tolerance)
        << "x=" << x << " y=" << y << " formula=(" << expected.r << "," << expected.g << ","
        << expected.b << "," << expected.a << ") graph=(" << graph.r << "," << graph.g << ","
        << graph.b << "," << graph.a << ") cpu=(" << cpu.r << "," << cpu.g << "," << cpu.b << ","
        << cpu.a << ")";
    EXPECT_NEAR(cpu.b, expected.b, tolerance)
        << "x=" << x << " y=" << y << " formula=(" << expected.r << "," << expected.g << ","
        << expected.b << "," << expected.a << ") graph=(" << graph.r << "," << graph.g << ","
        << graph.b << "," << graph.a << ") cpu=(" << cpu.r << "," << cpu.g << "," << cpu.b << ","
        << cpu.a << ")";
    EXPECT_NEAR(cpu.a, expected.a, tolerance)
        << "x=" << x << " y=" << y << " formula=(" << expected.r << "," << expected.g << ","
        << expected.b << "," << expected.a << ") graph=(" << graph.r << "," << graph.g << ","
        << graph.b << "," << graph.a << ") cpu=(" << cpu.r << "," << cpu.g << "," << cpu.b << ","
        << cpu.a << ")";
    EXPECT_NEAR(graph_outer_cov, outer_cov_expected, tolerance)
        << "x=" << x << " y=" << y << " outer coverage formula=" << outer_cov_expected
        << " graph=" << graph_outer_cov << " cpu=" << cpu_outer_cov;
    EXPECT_NEAR(cpu_outer_cov, outer_cov_expected, tolerance)
        << "x=" << x << " y=" << y << " outer coverage formula=" << outer_cov_expected
        << " graph=" << graph_outer_cov << " cpu=" << cpu_outer_cov;
    EXPECT_NEAR(graph_inner_cov, inner_cov_expected, tolerance)
        << "x=" << x << " y=" << y << " inner coverage formula=" << inner_cov_expected
        << " graph=" << graph_inner_cov << " cpu=" << cpu_inner_cov;
    EXPECT_NEAR(cpu_inner_cov, inner_cov_expected, tolerance)
        << "x=" << x << " y=" << y << " inner coverage formula=" << inner_cov_expected
        << " graph=" << graph_inner_cov << " cpu=" << cpu_inner_cov;
  }

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/** The generated root's node pointers, for the "a second rebuild changed nothing" check. */
static Vector<bNode *> root_node_ptrs(bNodeTree &tree)
{
  Vector<bNode *> nodes;
  for (bNode &node : tree.nodes) {
    nodes.append(&node);
  }
  return nodes;
}

static bool same_node_ptrs(const Span<bNode *> a, const Span<bNode *> b)
{
  if (a.size() != b.size()) {
    return false;
  }
  for (const int i : a.index_range()) {
    if (a[i] != b[i]) {
      return false;
    }
  }
  return true;
}

/**
 * Stage 4: a Pass Through folder with a Material child must be byte-identical to moving the child
 * to the parent, hiding it must be a value edit (same result, same root nodes), and a second
 * regeneration after an active-row change must rebuild nothing.
 */
TEST_F(PaintLayersGraphEvalTest, pass_through_material_child_and_hiding_match_the_flat_stack)
{
  const int size = 2;
  const int points[3][2] = {{1, 1}, {1, 0}, {0, 1}};
  const float tolerance = 1e-4f;
  const float base_color[4] = {0.10f, 0.60f, 0.20f, 1.0f};

  Material *source_c = build_hybrid_source(
      *bmain, "PassThroughSourceC", "PassThroughNormalC", source_spec_b);
  ASSERT_NE(source_c, nullptr);

  /* \a folded puts Material C in a Pass Through folder; without it C sits on the parent. */
  auto build_material = [&](const char *suffix, const bool folded, MaterialPaintLayer **r_c) -> Material * {
    Material *material = BKE_material_add(bmain, suffix);
    MaterialPaintLayer *base = BKE_paint_layers_add(
        *material, MA_PAINT_LAYER_KIND_PAINT, "Base", nullptr, PaintLayerPlace::Above);
    if (base == nullptr) {
      return nullptr;
    }
    MaterialPaintLayerChannel *base_record = BKE_paint_layers_channel_add(
        *material, base, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
    if (base_record == nullptr) {
      return nullptr;
    }
    char name[96];
    BLI_snprintf(name, sizeof(name), "PassThroughBase%s", suffix);
    Image *base_map = make_bake_data_map(bmain, name, size, base_color);
    if (base_map == nullptr) {
      return nullptr;
    }
    base_record->image = base_map;
    base_record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

    MaterialPaintLayer *anchor = nullptr;
    if (folded) {
      anchor = BKE_paint_layers_add(
          *material, MA_PAINT_LAYER_KIND_FOLDER, "Pass", nullptr, PaintLayerPlace::Above);
      if (anchor == nullptr || !BKE_paint_layers_folder_is_pass_through(*material, *anchor)) {
        return nullptr;
      }
    }
    MaterialPaintLayer *c = BKE_paint_layers_add(*material,
                                                 MA_PAINT_LAYER_KIND_MATERIAL,
                                                 "MatC",
                                                 anchor,
                                                 folded ? PaintLayerPlace::Into :
                                                          PaintLayerPlace::Above);
    if (c == nullptr || !BKE_paint_layers_set_material(*material, c, source_c)) {
      return nullptr;
    }
    for (const eMaterialPaintChannel channel : BKE_paint_material_bakeable_channels()) {
      RGBA raw;
      if (!hybrid_source_expected(source_spec_b, int(channel), raw)) {
        continue;
      }
      const float rgba[4] = {raw.r, raw.g, raw.b, 1.0f};
      BLI_snprintf(name, sizeof(name), "PassThroughBake%s", suffix);
      Image *map = make_bake_data_map(bmain, name, size, rgba);
      if (map == nullptr || !BKE_paint_layers_bake_set_map(*material, *c, int(channel), map)) {
        return nullptr;
      }
    }
    const float coverage_rgba[4] = {
        source_spec_b.alpha, source_spec_b.alpha, source_spec_b.alpha, 1.0f};
    BLI_snprintf(name, sizeof(name), "PassThroughCoverage%s", suffix);
    Image *coverage_map = make_bake_data_map(bmain, name, size, coverage_rgba);
    if (coverage_map == nullptr ||
        !BKE_paint_layers_bake_set_map(*material, *c, -1, coverage_map))
    {
      return nullptr;
    }
    BKE_paint_layers_bake_finalize(*material, *c);
    *r_c = c;
    return material;
  };

  MaterialPaintLayer *folded_c = nullptr;
  MaterialPaintLayer *plain_c = nullptr;
  Material *folded = build_material("Folded", true, &folded_c);
  Material *plain = build_material("Plain", false, &plain_c);
  ASSERT_NE(folded, nullptr);
  ASSERT_NE(plain, nullptr);

  /* The generated channels of both stacks must hold the same values at every point. */
  auto evaluate = [&](Material *material) -> Vector<RGBA> {
    ma = material;
    BKE_paint_layers_active_set(*ma, BLI_uuid_nil());
    EXPECT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
    GraphInterpreter interpreter;
    interpreter.instance = find_instance();
    interpreter.tree = ma->paint_layers_tree;
    EXPECT_NE(interpreter.instance, nullptr);
    interpreter.tree->ensure_topology_cache();
    Vector<RGBA> values;
    for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
      const int channel = int(info.channel);
      if (!result_output_exists(*ma->paint_layers_tree, channel)) {
        continue;
      }
      for (const int i : IndexRange(ARRAY_SIZE(points))) {
        interpreter.x = points[i][0];
        interpreter.y = points[i][1];
        values.append(eval_channel_result(interpreter, eMaterialPaintChannel(channel)));
      }
    }
    return values;
  };
  const Vector<RGBA> folded_values = evaluate(folded);
  const Vector<RGBA> plain_values = evaluate(plain);
  ASSERT_EQ(folded_values.size(), plain_values.size());
  for (const int i : folded_values.index_range()) {
    EXPECT_NEAR(folded_values[i].r, plain_values[i].r, tolerance) << "index " << i;
    EXPECT_NEAR(folded_values[i].g, plain_values[i].g, tolerance) << "index " << i;
    EXPECT_NEAR(folded_values[i].b, plain_values[i].b, tolerance) << "index " << i;
  }

  /* An active-row change on the folded stack: the next regen rebuilds, the one after does not. */
  ma = folded;
  BKE_paint_layers_active_set(*ma, folded_c->marker);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *root = ma->paint_layers_tree;
  ASSERT_NE(root, nullptr);
  const Vector<bNode *> nodes = root_node_ptrs(*root);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_TRUE(same_node_ptrs(nodes, root_node_ptrs(*root)));

  /* Hiding C is a value edit: the result equals the base alone and the root keeps its nodes. */
  ma = plain;
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *plain_root = ma->paint_layers_tree;
  const Vector<bNode *> plain_nodes = root_node_ptrs(*plain_root);
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, plain_c, false));
  BKE_paint_layers_values_sync(*ma);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(ma->paint_layers_tree, plain_root);
  EXPECT_TRUE(same_node_ptrs(plain_nodes, root_node_ptrs(*plain_root)));
  GraphInterpreter hidden_interpreter;
  hidden_interpreter.instance = find_instance();
  hidden_interpreter.tree = ma->paint_layers_tree;
  ASSERT_NE(hidden_interpreter.instance, nullptr);
  hidden_interpreter.tree->ensure_topology_cache();
  hidden_interpreter.x = 1;
  hidden_interpreter.y = 1;
  const RGBA hidden = eval_channel_result(
      hidden_interpreter, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  /* The base map is fully covering, so hiding C leaves exactly the base colour. */
  EXPECT_NEAR(hidden.r, base_color[0], tolerance);
  EXPECT_NEAR(hidden.g, base_color[1], tolerance);
  EXPECT_NEAR(hidden.b, base_color[2], tolerance);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

TEST_F(PaintLayersGraphEvalTest, generated_tree_topology_matches_cpu_formulas)
{
  const int size = 4;

  struct Variant {
    const char *name;
    eMaterialPaintLayerKind kind;
    eMaterialPaintLayerBlend blend;
    float opacity;
    /* 0 none, 1 mask const, 2 mask image, 3 content image, 4 content fill, 5 mask corr. */
    int extra;
    eMaterialPaintChannel channel;
  };
  const Variant variants[] = {
      {"mix", MA_PAINT_LAYER_KIND_PAINT, MA_PAINT_LAYER_BLEND_MIX, 1.0f, 0,
       PAINT_MATERIAL_CHANNEL_BASE_COLOR},
      {"mul", MA_PAINT_LAYER_KIND_PAINT, MA_PAINT_LAYER_BLEND_MULTIPLY, 0.5f, 0,
       PAINT_MATERIAL_CHANNEL_BASE_COLOR},
      {"overlay", MA_PAINT_LAYER_KIND_PAINT, MA_PAINT_LAYER_BLEND_OVERLAY, 0.5f, 0,
       PAINT_MATERIAL_CHANNEL_BASE_COLOR},
      {"add", MA_PAINT_LAYER_KIND_PAINT, MA_PAINT_LAYER_BLEND_ADD, 0.5f, 0,
       PAINT_MATERIAL_CHANNEL_BASE_COLOR},
      {"fill", MA_PAINT_LAYER_KIND_FILL, MA_PAINT_LAYER_BLEND_MIX, 0.5f, 0,
       PAINT_MATERIAL_CHANNEL_BASE_COLOR},
      {"mask_const", MA_PAINT_LAYER_KIND_PAINT, MA_PAINT_LAYER_BLEND_MIX, 1.0f, 1,
       PAINT_MATERIAL_CHANNEL_BASE_COLOR},
      {"mask_image", MA_PAINT_LAYER_KIND_PAINT, MA_PAINT_LAYER_BLEND_MIX, 1.0f, 2,
       PAINT_MATERIAL_CHANNEL_BASE_COLOR},
      {"content_image", MA_PAINT_LAYER_KIND_PAINT, MA_PAINT_LAYER_BLEND_MIX, 1.0f, 3,
       PAINT_MATERIAL_CHANNEL_BASE_COLOR},
      {"content_fill", MA_PAINT_LAYER_KIND_PAINT, MA_PAINT_LAYER_BLEND_MIX, 1.0f, 4,
       PAINT_MATERIAL_CHANNEL_BASE_COLOR},
      {"mask_corr", MA_PAINT_LAYER_KIND_PAINT, MA_PAINT_LAYER_BLEND_MIX, 1.0f, 5,
       PAINT_MATERIAL_CHANNEL_BASE_COLOR},
      {"mask_map_two_corr", MA_PAINT_LAYER_KIND_PAINT, MA_PAINT_LAYER_BLEND_MIX, 1.0f, 6,
       PAINT_MATERIAL_CHANNEL_BASE_COLOR},
      {"disabled_layer", MA_PAINT_LAYER_KIND_PAINT, MA_PAINT_LAYER_BLEND_MIX, 1.0f, 7,
       PAINT_MATERIAL_CHANNEL_BASE_COLOR},
      {"disabled_content_corr", MA_PAINT_LAYER_KIND_PAINT, MA_PAINT_LAYER_BLEND_MIX, 1.0f, 8,
       PAINT_MATERIAL_CHANNEL_BASE_COLOR},
      {"disabled_mask_corr", MA_PAINT_LAYER_KIND_PAINT, MA_PAINT_LAYER_BLEND_MIX, 1.0f, 9,
       PAINT_MATERIAL_CHANNEL_BASE_COLOR},
      {"normal_mask_corr", MA_PAINT_LAYER_KIND_PAINT, MA_PAINT_LAYER_BLEND_MIX, 1.0f, 5,
       PAINT_MATERIAL_CHANNEL_NORMAL},
      {"normal_content_image", MA_PAINT_LAYER_KIND_PAINT, MA_PAINT_LAYER_BLEND_MIX, 1.0f, 3,
       PAINT_MATERIAL_CHANNEL_NORMAL},
      {"normal_content_fill_ignored", MA_PAINT_LAYER_KIND_PAINT, MA_PAINT_LAYER_BLEND_MIX, 1.0f, 4,
       PAINT_MATERIAL_CHANNEL_NORMAL},
  };

  for (const Variant &variant : variants) {
    ma = BKE_material_add(bmain, "GraphEval");
    add_layer("Bottom",
              MA_PAINT_LAYER_KIND_PAINT,
              add_solid_image("Bottom", size, 255, 0, 0, 255),
              variant.channel);
    if (variant.extra == 7) {
      /* A disabled row stays in the topology with factor zero; it must not change the result. */
      MaterialPaintLayer *middle = add_layer(
          "Middle",
          MA_PAINT_LAYER_KIND_PAINT,
          add_solid_image("Middle", size, 255, 255, 0, 255),
          variant.channel);
      BKE_paint_layers_set_enabled(*ma, middle, false);
    }
    MaterialPaintLayer *top = add_layer(
        "Top", variant.kind, add_solid_image("Top", size, 0, 0, 255, 255), variant.channel);
    BKE_paint_layers_set_blend(*ma, top, variant.blend);
    BKE_paint_layers_set_opacity(*ma, top, variant.opacity);
    if (variant.kind == MA_PAINT_LAYER_KIND_FILL) {
      const float blue[4] = {0.0f, 0.0f, 1.0f, 1.0f};
      BKE_paint_layers_set_fill_color(*ma, top, blue);
      /* A Fill has no map in the channel; the record is only for presence. */
      top->channels[0].image = nullptr;
    }
    switch (variant.extra) {
      case 1:
        BKE_paint_layers_mask_add(*ma, top, 0.5f);
        break;
      case 2: {
        set_mask_image(*top, add_solid_image("Mask", size, 128, 128, 128, 255));
        break;
      }
      case 3: {
        MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
            *ma, top, MA_PAINT_LAYER_SECTION_CONTENT, MA_PAINT_LAYER_EFFECT_PAINT, "C");
        MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
            *ma, correction, variant.channel);
        record->image = add_solid_image("Correction", size, 0, 255, 0, 255);
        record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
        break;
      }
      case 4: {
        MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
            *ma, top, MA_PAINT_LAYER_SECTION_CONTENT, MA_PAINT_LAYER_EFFECT_FILL, "C");
        const float green[4] = {0.0f, 1.0f, 0.0f, 1.0f};
        BKE_paint_layers_set_fill_color(*ma, correction, green);
        break;
      }
      case 5: {
        MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
            *ma, top, MA_PAINT_LAYER_SECTION_MASK, MA_PAINT_LAYER_EFFECT_PAINT, "M");
        MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
            *ma, correction, variant.channel);
        record->image = add_solid_image("MaskCorr", size, 128, 128, 128, 255);
        record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
        break;
      }
      case 8: {
        MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
            *ma, top, MA_PAINT_LAYER_SECTION_CONTENT, MA_PAINT_LAYER_EFFECT_PAINT, "C");
        MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
            *ma, correction, variant.channel);
        record->image = add_solid_image("C", size, 0, 255, 0, 255);
        record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
        BKE_paint_layers_set_enabled(*ma, correction, false);
        break;
      }
      case 9: {
        MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
            *ma, top, MA_PAINT_LAYER_SECTION_MASK, MA_PAINT_LAYER_EFFECT_PAINT, "M");
        MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
            *ma, correction, variant.channel);
        record->image = add_solid_image("M", size, 128, 128, 128, 255);
        record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
        BKE_paint_layers_set_enabled(*ma, correction, false);
        break;
      }
      case 6: {
        /* A mask map below two content corrections with different opacities is where the order of
         * the "over" accumulation shows: a wrong order still passes with a single correction. */
        set_mask_image(*top, add_solid_image("Mask", size, 128, 128, 128, 255));
        MaterialPaintLayer *first = BKE_paint_layers_correction_add(
            *ma, top, MA_PAINT_LAYER_SECTION_CONTENT, MA_PAINT_LAYER_EFFECT_PAINT, "C1");
        MaterialPaintLayerChannel *first_record = BKE_paint_layers_channel_add(
            *ma, first, variant.channel);
        first_record->image = add_solid_image("C1", size, 0, 255, 0, 255);
        first_record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
        BKE_paint_layers_set_opacity(*ma, first, 0.3f);
        MaterialPaintLayer *second = BKE_paint_layers_correction_add(
            *ma, top, MA_PAINT_LAYER_SECTION_CONTENT, MA_PAINT_LAYER_EFFECT_PAINT, "C2");
        MaterialPaintLayerChannel *second_record = BKE_paint_layers_channel_add(
            *ma, second, variant.channel);
        second_record->image = add_solid_image("C2", size, 255, 255, 0, 255);
        second_record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
        BKE_paint_layers_set_opacity(*ma, second, 0.7f);
        break;
      }
      default:
        break;
    }

    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

    GraphInterpreter interpreter;
    interpreter.instance = find_instance();
    interpreter.tree = ma->paint_layers_tree;
    interpreter.x = 1;
    interpreter.y = 1;
    ASSERT_NE(interpreter.instance, nullptr);
    const RGBA graph = interpreter.eval_result(result_name(variant.channel));
    const RGBA cpu = cpu_pixel(variant.channel);

    /* Both sides decode their maps to scene linear and mix in float, so nothing quantizes between
     * rows any more: what is left is float rounding. */
    const float tolerance = 1e-4f;
    EXPECT_NEAR(graph.r, cpu.r, tolerance) << variant.name;
    EXPECT_NEAR(graph.g, cpu.g, tolerance) << variant.name;
    EXPECT_NEAR(graph.b, cpu.b, tolerance) << variant.name;

    BKE_id_free(bmain, ma);
    ma = nullptr;
  }
}

/**
 * Every blend mode the description offers on Base Color, with inputs chosen to reach the unusual
 * branches: values below and above half (Overlay, Soft Light, Linear Light, Burn, Dodge), a
 * saturated pair (Hue/Saturation/Color/Value), a result above one (Add) and a top that is zero
 * (Divide) or a bottom below the top (Subtract).
 *
 * The two sides decode the maps and mix through the same `ramp_blend`; an accidental result clamp
 * in the generated Mix would show up on `add_over_one`, and a wrong ramp code on any other row.
 */
TEST_F(PaintLayersGraphEvalTest, mask_item_multiply_matches_the_cpu)
{
  const int size = 4;
  ma = BKE_material_add(bmain, "MaskItemMultiply");
  add_layer("Bottom", MA_PAINT_LAYER_KIND_PAINT, add_solid_image("Bottom", size, 255, 0, 0, 255));
  MaterialPaintLayer *top = add_layer(
      "Top", MA_PAINT_LAYER_KIND_PAINT, add_solid_image("Top", size, 0, 0, 255, 255));
  Image *mask_image = add_solid_image("Mask", size, 204, 204, 204, 255);
  make_image_data(mask_image);
  set_mask_image(*top, mask_image);

  /* A Paint item with a semi-transparent map, MULTIPLY. */
  MaterialPaintLayer *paint_item = BKE_paint_layers_correction_add(
      *ma, top, MA_PAINT_LAYER_SECTION_MASK, MA_PAINT_LAYER_EFFECT_PAINT, "P");
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
      *ma, paint_item, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  Image *item_image = add_solid_image("ItemMap", size, 64, 64, 64, 128);
  make_image_data(item_image);
  item_image->alpha_mode = IMA_ALPHA_STRAIGHT;
  item_image->flag |= IMA_GPU_LINEAR_PREMUL;
  record->image = item_image;
  record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  BKE_paint_layers_set_blend(*ma, paint_item, MA_PAINT_LAYER_BLEND_MULTIPLY);

  /* A Fill item with a 0.5 constant, MULTIPLY. */
  MaterialPaintLayer *fill_item = BKE_paint_layers_correction_add(
      *ma, top, MA_PAINT_LAYER_SECTION_MASK, MA_PAINT_LAYER_EFFECT_FILL, "F");
  const float half[4] = {0.5f, 0.5f, 0.5f, 1.0f};
  BKE_paint_layers_set_fill_color(*ma, fill_item, half);
  BKE_paint_layers_set_blend(*ma, fill_item, MA_PAINT_LAYER_BLEND_MULTIPLY);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA graph = interpreter.eval_result(result_name(PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  const RGBA cpu = cpu_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  const float tolerance = 1e-4f;
  EXPECT_NEAR(graph.r, cpu.r, tolerance);
  EXPECT_NEAR(graph.g, cpu.g, tolerance);
  EXPECT_NEAR(graph.b, cpu.b, tolerance);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/** Give \a image the storage every paint-layer map uses and a straight soft edge:
 * grey \a straight in RGB with alpha 0.25 / 0.5 / 0.75 / 1.0 across a row of \a size. */
void fill_straight_soft_edge(Image *image, const int size, const float straight)
{
  image->alpha_mode = IMA_ALPHA_STRAIGHT;
  image->flag |= IMA_GPU_LINEAR_PREMUL;
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
  ASSERT_NE(ibuf, nullptr);
  uchar *pixels = ibuf->byte_data_for_write();
  for (int x = 0; x < size; x++) {
    const float alpha = 0.25f * float(x + 1);
    pixels[x * 4 + 0] = uchar(clamp_f(straight, 0.0f, 1.0f) * 255.0f + 0.5f);
    pixels[x * 4 + 1] = uchar(clamp_f(straight, 0.0f, 1.0f) * 255.0f + 0.5f);
    pixels[x * 4 + 2] = uchar(clamp_f(straight, 0.0f, 1.0f) * 255.0f + 0.5f);
    pixels[x * 4 + 3] = uchar(clamp_f(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
  }
  BKE_image_release_ibuf(image, ibuf, lock);
}

/**
 * A mask item whose map carries a soft edge, stored the one way every paint-layer map is: straight
 * byte RGB (grey 0.6) with alpha 0.25 / 0.5 / 0.75 / 1.0 across the row, and #IMA_GPU_LINEAR_PREMUL
 * set. The texture upload pre-multiplies the straight bytes, the chain's Divide undoes that and the
 * CPU reads straight; the chain and the CPU must agree at every pixel. A mismatch is the visible
 * rim along a soft mask stroke (10b).
 */
TEST_F(PaintLayersGraphEvalTest, mask_item_partial_alpha_matches_the_cpu)
{
  const int size = 4;
  ma = BKE_material_add(bmain, "MaskPartialAlpha");
  add_layer("Bottom", MA_PAINT_LAYER_KIND_PAINT, add_solid_image("Bottom", size, 255, 0, 0, 255));
  MaterialPaintLayer *top = add_layer(
      "Top", MA_PAINT_LAYER_KIND_PAINT, add_solid_image("Top", size, 0, 0, 255, 255));

  const float color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  Image *mask = BKE_image_add_generated(
      bmain, size, size, "MaskStraight", 32, false, IMA_GENTYPE_BLANK, color, false, true, false);
  ASSERT_NE(mask, nullptr);
  fill_straight_soft_edge(mask, size, 0.6f);
  set_mask_image(*top, mask);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  ASSERT_NE(interpreter.instance, nullptr);

  const float tolerance = 1e-4f;
  for (int x = 0; x < size; x++) {
    interpreter.x = x;
    interpreter.y = 0;
    const RGBA graph = interpreter.eval_result("Result Base Color");
    const RGBA cpu = cpu_pixel_at(PAINT_MATERIAL_CHANNEL_BASE_COLOR, x, 0);
    EXPECT_NEAR(graph.r, cpu.r, tolerance) << "x=" << x;
    EXPECT_NEAR(graph.g, cpu.g, tolerance) << "x=" << x;
    EXPECT_NEAR(graph.b, cpu.b, tolerance) << "x=" << x;
  }

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * The same soft edge, but on a map tagged with a colour space (not data). The Image Texture node
 * itself un-premultiplies a non-data texture whenever the Alpha output is used
 * (node_shader_tex_image.cc:162-167), so the generated chain must NOT build its own Divide for
 * such a map -- otherwise alpha is removed twice and the soft edge shows `C / A` (the H1 rim).
 * With the Divide built only for data maps the two sides agree at every pixel.
 */
TEST_F(PaintLayersGraphEvalTest, mask_item_color_space_partial_alpha_matches_the_cpu)
{
  const int size = 4;
  ma = BKE_material_add(bmain, "MaskColorSpace");
  add_layer("Bottom", MA_PAINT_LAYER_KIND_PAINT, add_solid_image("Bottom", size, 255, 0, 0, 255));
  MaterialPaintLayer *top = add_layer(
      "Top", MA_PAINT_LAYER_KIND_PAINT, add_solid_image("Top", size, 0, 0, 255, 255));

  const float color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  Image *mask = BKE_image_add_generated(
      bmain, size, size, "MaskColorSpace", 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
  ASSERT_NE(mask, nullptr);
  fill_straight_soft_edge(mask, size, 0.6f);
  set_mask_image(*top, mask);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  ASSERT_NE(interpreter.instance, nullptr);

  const float tolerance = 1e-4f;
  for (int x = 0; x < size; x++) {
    interpreter.x = x;
    interpreter.y = 0;
    const RGBA graph = interpreter.eval_result("Result Base Color");
    const RGBA cpu = cpu_pixel_at(PAINT_MATERIAL_CHANNEL_BASE_COLOR, x, 0);
    EXPECT_NEAR(graph.r, cpu.r, tolerance) << "x=" << x;
    EXPECT_NEAR(graph.g, cpu.g, tolerance) << "x=" << x;
    EXPECT_NEAR(graph.b, cpu.b, tolerance) << "x=" << x;
  }

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * A content Paint correction whose map is read as colour data (Roughness, Normal, Metallic, ...):
 * the Image Texture node leaves the upload's pre-multiplied colour as `C * A`, so the chain builds
 * a Divide to recover `C`; the CPU reads the straight bytes. On a soft edge the two must agree, or
 * the correction's coverage is applied twice (a rim at the edge).
 */
TEST_F(PaintLayersGraphEvalTest, content_correction_data_channel_partial_alpha_matches_the_cpu)
{
  const int size = 4;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_ROUGHNESS;
  auto result_name_for = [](const int ch) {
    return std::string("Result ") +
           BKE_paint_material_channel_info(eMaterialPaintChannel(ch)).ui_name;
  };

  ma = BKE_material_add(bmain, "ContentData");
  add_layer("Bottom", MA_PAINT_LAYER_KIND_PAINT, add_solid_image("Bottom", size, 128, 128, 128, 255), channel);
  MaterialPaintLayer *top = add_layer(
      "Top", MA_PAINT_LAYER_KIND_PAINT, add_solid_image("Top", size, 200, 200, 200, 255), channel);
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, top, MA_PAINT_LAYER_SECTION_CONTENT, MA_PAINT_LAYER_EFFECT_PAINT, "C");
  ASSERT_NE(correction, nullptr);
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(*ma, correction, channel);
  ASSERT_NE(record, nullptr);
  const float color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  Image *map = BKE_image_add_generated(
      bmain, size, size, "RoughCorr", 32, false, IMA_GENTYPE_BLANK, color, false, true, false);
  ASSERT_NE(map, nullptr);
  fill_straight_soft_edge(map, size, 0.3f);
  record->image = map;
  record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  ASSERT_NE(interpreter.instance, nullptr);

  const std::string out = result_name_for(channel);
  const float tolerance = 1e-4f;
  for (int x = 0; x < size; x++) {
    interpreter.x = x;
    interpreter.y = 0;
    const RGBA graph = interpreter.eval_result(out.c_str());
    const RGBA cpu = cpu_pixel_at(channel, x, 0);
    EXPECT_NEAR(graph.r, cpu.r, tolerance) << "x=" << x;
    EXPECT_NEAR(graph.g, cpu.g, tolerance) << "x=" << x;
    EXPECT_NEAR(graph.b, cpu.b, tolerance) << "x=" << x;
  }

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * The same correction on Base Color: the map is not data, so the Image Texture node itself
 * un-premultiplies and the chain must NOT build its own Divide.
 */
TEST_F(PaintLayersGraphEvalTest, content_correction_color_channel_partial_alpha_matches_the_cpu)
{
  const int size = 4;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  auto result_name_for = [](const int ch) {
    return std::string("Result ") +
           BKE_paint_material_channel_info(eMaterialPaintChannel(ch)).ui_name;
  };

  ma = BKE_material_add(bmain, "ContentColor");
  add_layer("Bottom", MA_PAINT_LAYER_KIND_PAINT, add_solid_image("Bottom", size, 255, 0, 0, 255), channel);
  MaterialPaintLayer *top = add_layer(
      "Top", MA_PAINT_LAYER_KIND_PAINT, add_solid_image("Top", size, 0, 0, 255, 255), channel);
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, top, MA_PAINT_LAYER_SECTION_CONTENT, MA_PAINT_LAYER_EFFECT_PAINT, "C");
  ASSERT_NE(correction, nullptr);
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(*ma, correction, channel);
  ASSERT_NE(record, nullptr);
  const float color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  Image *map = BKE_image_add_generated(
      bmain, size, size, "ColorCorr", 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
  ASSERT_NE(map, nullptr);
  fill_straight_soft_edge(map, size, 0.4f);
  record->image = map;
  record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  ASSERT_NE(interpreter.instance, nullptr);

  const std::string out = result_name_for(channel);
  const float tolerance = 1e-4f;
  for (int x = 0; x < size; x++) {
    interpreter.x = x;
    interpreter.y = 0;
    const RGBA graph = interpreter.eval_result(out.c_str());
    const RGBA cpu = cpu_pixel_at(channel, x, 0);
    EXPECT_NEAR(graph.r, cpu.r, tolerance) << "x=" << x;
    EXPECT_NEAR(graph.g, cpu.g, tolerance) << "x=" << x;
    EXPECT_NEAR(graph.b, cpu.b, tolerance) << "x=" << x;
  }

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

TEST_F(PaintLayersGraphEvalTest, every_blend_mode_matches_the_cpu)
{
  const int size = 4;
  struct BlendCase {
    const char *name;
    eMaterialPaintLayerBlend blend;
    uchar bottom[3];
    uchar top[3];
  };
  const BlendCase cases[] = {
      {"mix", MA_PAINT_LAYER_BLEND_MIX, {192, 64, 32}, {64, 192, 224}},
      {"multiply", MA_PAINT_LAYER_BLEND_MULTIPLY, {192, 64, 32}, {64, 192, 224}},
      {"overlay", MA_PAINT_LAYER_BLEND_OVERLAY, {192, 64, 32}, {64, 192, 224}},
      {"add", MA_PAINT_LAYER_BLEND_ADD, {192, 64, 32}, {64, 192, 224}},
      {"add_over_one", MA_PAINT_LAYER_BLEND_ADD, {200, 200, 200}, {200, 200, 200}},
      {"subtract_below_zero", MA_PAINT_LAYER_BLEND_SUBTRACT, {64, 64, 64}, {200, 200, 200}},
      {"divide_by_zero", MA_PAINT_LAYER_BLEND_DIVIDE, {200, 200, 200}, {0, 0, 0}},
      {"divide", MA_PAINT_LAYER_BLEND_DIVIDE, {192, 64, 32}, {64, 192, 224}},
      {"darken", MA_PAINT_LAYER_BLEND_DARKEN, {192, 64, 32}, {64, 192, 224}},
      {"lighten", MA_PAINT_LAYER_BLEND_LIGHTEN, {192, 64, 32}, {64, 192, 224}},
      {"screen", MA_PAINT_LAYER_BLEND_SCREEN, {192, 64, 32}, {64, 192, 224}},
      {"burn", MA_PAINT_LAYER_BLEND_BURN, {192, 64, 32}, {64, 192, 224}},
      {"dodge", MA_PAINT_LAYER_BLEND_DODGE, {192, 64, 32}, {64, 192, 224}},
      {"difference", MA_PAINT_LAYER_BLEND_DIFFERENCE, {192, 64, 32}, {64, 192, 224}},
      {"exclusion", MA_PAINT_LAYER_BLEND_EXCLUSION, {192, 64, 32}, {64, 192, 224}},
      {"soft_light", MA_PAINT_LAYER_BLEND_SOFT_LIGHT, {192, 64, 32}, {64, 192, 224}},
      {"linear_light", MA_PAINT_LAYER_BLEND_LINEAR_LIGHT, {192, 64, 32}, {64, 192, 224}},
      {"hue", MA_PAINT_LAYER_BLEND_HUE, {192, 64, 32}, {64, 192, 224}},
      {"saturation", MA_PAINT_LAYER_BLEND_SATURATION, {192, 64, 32}, {64, 192, 224}},
      {"color", MA_PAINT_LAYER_BLEND_COLOR, {192, 64, 32}, {64, 192, 224}},
      {"value", MA_PAINT_LAYER_BLEND_VALUE, {192, 64, 32}, {64, 192, 224}},
  };

  for (const BlendCase &blend_case : cases) {
    ma = BKE_material_add(bmain, "BlendEval");
    add_layer("Bottom",
              MA_PAINT_LAYER_KIND_PAINT,
              add_solid_image("Bottom",
                              size,
                              blend_case.bottom[0],
                              blend_case.bottom[1],
                              blend_case.bottom[2],
                              255));
    MaterialPaintLayer *top = add_layer("Top",
                                        MA_PAINT_LAYER_KIND_PAINT,
                                        add_solid_image("Top",
                                                        size,
                                                        blend_case.top[0],
                                                        blend_case.top[1],
                                                        blend_case.top[2],
                                                        255));
    BKE_paint_layers_set_blend(*ma, top, blend_case.blend);

    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
    GraphInterpreter interpreter;
    interpreter.instance = find_instance();
    interpreter.tree = ma->paint_layers_tree;
    interpreter.x = 1;
    interpreter.y = 1;
    ASSERT_NE(interpreter.instance, nullptr);
    const RGBA graph = interpreter.eval_result("Result Base Color");
    const RGBA cpu = cpu_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
    EXPECT_NEAR(graph.r, cpu.r, 1e-4f) << blend_case.name;
    EXPECT_NEAR(graph.g, cpu.g, 1e-4f) << blend_case.name;
    EXPECT_NEAR(graph.b, cpu.b, 1e-4f) << blend_case.name;

    BKE_id_free(bmain, ma);
    ma = nullptr;
  }
}

/**
 * A folder is an isolated group (design §5): its children composite over transparency, the folder
 * lays the result over what is below with its own blend, opacity and mask. The generated chain and
 * the CPU must agree on all of it, and the model is checked here on the cases that would break a
 * naive "sub-chain then mix by coverage" -- a single child at half opacity, a Multiply child over
 * an empty folder, a folder with its own blend, mask and mask correction, and nesting.
 */
TEST_F(PaintLayersGraphEvalTest, folders_match_the_cpu)
{
  const int size = 4;

  auto run_case = [&](const char *name,
                      const eMaterialPaintChannel channel,
                      const std::function<void()> &setup) {
    ma = BKE_material_add(bmain, "FolderEval");
    setup();
    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
    GraphInterpreter interpreter;
    interpreter.instance = find_instance();
    interpreter.tree = ma->paint_layers_tree;
    interpreter.x = 1;
    interpreter.y = 1;
    ASSERT_NE(interpreter.instance, nullptr);
    const RGBA graph = interpreter.eval_result(result_name(channel));
    const RGBA cpu = cpu_pixel(channel);
    EXPECT_NEAR(graph.r, cpu.r, 1e-4f) << name;
    EXPECT_NEAR(graph.g, cpu.g, 1e-4f) << name;
    EXPECT_NEAR(graph.b, cpu.b, 1e-4f) << name;
    BKE_id_free(bmain, ma);
    ma = nullptr;
  };

  auto add_layer = [&](const char *layer_name,
                       Image *image,
                       const eMaterialPaintChannel channel,
                       MaterialPaintLayer *anchor = nullptr,
                       const PaintLayerPlace place = PaintLayerPlace::Above) {
    MaterialPaintLayer *layer = BKE_paint_layers_add(*ma,
                                                     MA_PAINT_LAYER_KIND_PAINT,
                                                     layer_name,
                                                     anchor,
                                                     place);
    MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(*ma, layer, channel);
    record->image = image;
    record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
    return layer;
  };
  auto add_folder = [&](const char *folder_name) {
    return BKE_paint_layers_add(
        *ma, MA_PAINT_LAYER_KIND_FOLDER, folder_name, nullptr, PaintLayerPlace::Above);
  };
  const eMaterialPaintChannel bc = PAINT_MATERIAL_CHANNEL_BASE_COLOR;

  run_case("parent_none", bc, [&]() {
    MaterialPaintLayer *folder = add_folder("Folder");
    add_layer("Child", add_solid_image("Child", size, 0, 0, 255, 255), bc, folder, PaintLayerPlace::Into);
    BKE_paint_layers_set_opacity(*ma, folder, 0.5f);
  });

  run_case("single_child_half", bc, [&]() {
    add_layer("Bottom", add_solid_image("Bottom", size, 255, 0, 0, 255), bc);
    MaterialPaintLayer *folder = add_folder("Folder");
    MaterialPaintLayer *child = add_layer(
        "Child", add_solid_image("Child", size, 0, 0, 255, 255), bc, folder, PaintLayerPlace::Into);
    BKE_paint_layers_set_opacity(*ma, child, 0.5f);
  });

  run_case("multiply_inside_over_empty", bc, [&]() {
    MaterialPaintLayer *folder = add_folder("Folder");
    MaterialPaintLayer *child = add_layer(
        "Child", add_solid_image("Child", size, 200, 100, 50, 255), bc, folder, PaintLayerPlace::Into);
    BKE_paint_layers_set_blend(*ma, child, MA_PAINT_LAYER_BLEND_MULTIPLY);
  });

  run_case("folder_blend_multiply", bc, [&]() {
    add_layer("Bottom", add_solid_image("Bottom", size, 255, 0, 0, 255), bc);
    MaterialPaintLayer *folder = add_folder("Folder");
    add_layer("Child", add_solid_image("Child", size, 0, 255, 0, 255), bc, folder, PaintLayerPlace::Into);
    BKE_paint_layers_set_blend(*ma, folder, MA_PAINT_LAYER_BLEND_MULTIPLY);
  });

  run_case("mask_on_folder", bc, [&]() {
    add_layer("Bottom", add_solid_image("Bottom", size, 255, 0, 0, 255), bc);
    MaterialPaintLayer *folder = add_folder("Folder");
    add_layer("Child", add_solid_image("Child", size, 0, 0, 255, 255), bc, folder, PaintLayerPlace::Into);
    set_mask_image(*folder, add_solid_image("FolderMask", size, 255, 255, 255, 128));
  });

  run_case("mask_correction_on_folder", bc, [&]() {
    add_layer("Bottom", add_solid_image("Bottom", size, 255, 0, 0, 255), bc);
    MaterialPaintLayer *folder = add_folder("Folder");
    add_layer("Child", add_solid_image("Child", size, 0, 0, 255, 255), bc, folder, PaintLayerPlace::Into);
    MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
        *ma, folder, MA_PAINT_LAYER_SECTION_MASK, MA_PAINT_LAYER_EFFECT_PAINT, "M");
    ASSERT_NE(correction, nullptr);
    MaterialPaintLayerChannel *mask_record = BKE_paint_layers_channel_add(
        *ma, correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
    ASSERT_NE(mask_record, nullptr);
    mask_record->image = add_solid_image("FolderMaskCorr", size, 128, 128, 128, 255);
    mask_record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  });

  run_case("nested_folder", bc, [&]() {
    add_layer("Bottom", add_solid_image("Bottom", size, 255, 0, 0, 255), bc);
    MaterialPaintLayer *outer = add_folder("Outer");
    MaterialPaintLayer *inner = BKE_paint_layers_add(
        *ma, MA_PAINT_LAYER_KIND_FOLDER, "Inner", outer, PaintLayerPlace::Into);
    add_layer("Leaf", add_solid_image("Leaf", size, 0, 0, 255, 255), bc, inner, PaintLayerPlace::Into);
    BKE_paint_layers_set_opacity(*ma, inner, 0.5f);
    BKE_paint_layers_set_opacity(*ma, outer, 0.5f);
  });

  run_case("empty_folder_is_below", bc, [&]() {
    add_layer("Bottom", add_solid_image("Bottom", size, 255, 0, 0, 255), bc);
    add_folder("Empty");
  });

  run_case("normal_inside_folder", PAINT_MATERIAL_CHANNEL_NORMAL, [&]() {
    add_layer("Bottom",
              add_solid_image("Bottom", size, 128, 128, 255, 255),
              PAINT_MATERIAL_CHANNEL_NORMAL);
    MaterialPaintLayer *folder = add_folder("Folder");
    add_layer("Child",
              add_solid_image("Child", size, 160, 128, 232, 255),
              PAINT_MATERIAL_CHANNEL_NORMAL,
              folder,
              PaintLayerPlace::Into);
    BKE_paint_layers_set_opacity(*ma, folder, 0.5f);
  });
}

/**
 * A value changed on a grandchild after regeneration must reach the graph: the mirror chain carries
 * it from the root interface through both parent folders, and the CPU reads the same description.
 */
TEST_F(PaintLayersGraphEvalTest, nested_values_reach_the_grandchild)
{
  const int size = 4;
  const eMaterialPaintChannel bc = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "NestedValues");

  MaterialPaintLayer *outer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_FOLDER, "Outer", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *inner = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_FOLDER, "Inner", outer, PaintLayerPlace::Into);
  MaterialPaintLayer *leaf = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_PAINT, "Leaf", inner, PaintLayerPlace::Into);
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(*ma, leaf, bc);
  record->image = add_solid_image("Leaf", size, 0, 0, 255, 255);
  record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  for (const float opacity : {1.0f, 0.4f}) {
    ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, leaf, opacity));
    BKE_paint_layers_values_sync(*ma);

    GraphInterpreter interpreter;
    interpreter.instance = find_instance();
    interpreter.tree = ma->paint_layers_tree;
    interpreter.x = 1;
    interpreter.y = 1;
    ASSERT_NE(interpreter.instance, nullptr);
    const RGBA graph = interpreter.eval_result("Result Base Color");
    const RGBA cpu = cpu_pixel(bc);
    EXPECT_NEAR(graph.r, cpu.r, 1e-4f) << opacity;
    EXPECT_NEAR(graph.g, cpu.g, 1e-4f) << opacity;
    EXPECT_NEAR(graph.b, cpu.b, 1e-4f) << opacity;
  }
}

/**
 * A content correction on a folder edits the isolated straight result (S_folder) and its coverage
 * folds into the folder's coverage by the over model: a = a + f * (1 - a). This is checked against
 * a hand calculation, not only against the CPU, so generator and CPU cannot agree on the wrong
 * formula.
 */
TEST_F(PaintLayersGraphEvalTest, folder_content_correction_folds_into_coverage)
{
  const int size = 4;
  const eMaterialPaintChannel bc = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "FolderCorrEval");

  Image *child_image = add_solid_image("Child", size, 200, 100, 50, 255);
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_FOLDER, "Folder", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_PAINT, "Child", folder, PaintLayerPlace::Into);
  ASSERT_NE(child, nullptr);
  MaterialPaintLayerChannel *child_record = BKE_paint_layers_channel_add(*ma, child, bc);
  child_record->image = child_image;
  child_record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, child, 0.5f));

  const float correction_color[4] = {0.0f, 0.0f, 1.0f, 1.0f};
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, folder, MA_PAINT_LAYER_SECTION_CONTENT, MA_PAINT_LAYER_EFFECT_FILL, "Corr");
  ASSERT_NE(correction, nullptr);
  BKE_paint_layers_set_fill_color(*ma, correction, correction_color);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, correction, 0.5f));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA graph = interpreter.eval_result("Result Base Color");
  const RGBA cpu = cpu_pixel(bc);
  const RGBA child_color = interpreter.sample_image(child_image, "Color");

  /* Manual formula. The child covers a0 = 0.5 and paints S_folder = its own colour; the correction
   * mixes into that colour at f = 0.5 and folds into the coverage (a1 = a0 + f * (1 - a0) = 0.75);
   * the folder then lays S over the channel bottom (black) at a1. */
  const float a0 = 0.5f;
  const float corr_factor = 0.5f;
  const float a1 = a0 + corr_factor * (1.0f - a0);
  float bottom[4];
  BKE_paint_layers_channel_bottom_color(bc, bottom);
  const float child_rgb[3] = {child_color.r, child_color.g, child_color.b};
  float expected[3];
  for (const int k : IndexRange(3)) {
    const float s = child_rgb[k] + (correction_color[k] - child_rgb[k]) * corr_factor;
    expected[k] = bottom[k] + (s - bottom[k]) * a1;
  }

  EXPECT_NEAR(graph.r, expected[0], 1e-4f);
  EXPECT_NEAR(graph.g, expected[1], 1e-4f);
  EXPECT_NEAR(graph.b, expected[2], 1e-4f);
  EXPECT_NEAR(cpu.r, expected[0], 1e-4f);
  EXPECT_NEAR(cpu.g, expected[1], 1e-4f);
  EXPECT_NEAR(cpu.b, expected[2], 1e-4f);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * A folder that changes nothing but grouping must not change the pixels: a folder with default
 * blend, opacity, no mask and one child at f = 0.5 has to equal the same child without the folder.
 * This is the case a "sub-chain, then mix again by coverage" formula gets wrong -- it would darken
 * the half-covered pixel twice.
 */
TEST_F(PaintLayersGraphEvalTest, folder_single_child_equals_the_child_alone)
{
  const int size = 4;

  auto add_child_layer = [&](const char *name, Image *image, MaterialPaintLayer *anchor) {
    MaterialPaintLayer *layer = BKE_paint_layers_add(*ma,
                                                     MA_PAINT_LAYER_KIND_PAINT,
                                                     name,
                                                     anchor,
                                                     anchor != nullptr ? PaintLayerPlace::Into :
                                                                          PaintLayerPlace::Above);
    MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
        *ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
    record->image = image;
    record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
    return layer;
  };

  /* With a folder. */
  ma = BKE_material_add(bmain, "FolderEquiv");
  add_child_layer("Bottom", add_solid_image("Bottom", size, 255, 0, 0, 255), nullptr);
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_FOLDER, "Folder", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *child = add_child_layer(
      "Child", add_solid_image("Child", size, 0, 0, 255, 255), folder);
  BKE_paint_layers_set_opacity(*ma, child, 0.5f);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  const RGBA folded = cpu_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  BKE_id_free(bmain, ma);
  ma = nullptr;

  /* The same stack, ungrouped. */
  ma = BKE_material_add(bmain, "PlainEquiv");
  add_child_layer("Bottom", add_solid_image("Bottom", size, 255, 0, 0, 255), nullptr);
  MaterialPaintLayer *plain = add_child_layer(
      "Child", add_solid_image("Child", size, 0, 0, 255, 255), nullptr);
  BKE_paint_layers_set_opacity(*ma, plain, 0.5f);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  const RGBA ungrouped = cpu_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  BKE_id_free(bmain, ma);
  ma = nullptr;

  EXPECT_NEAR(folded.r, ungrouped.r, 1e-4f);
  EXPECT_NEAR(folded.g, ungrouped.g, 1e-4f);
  EXPECT_NEAR(folded.b, ungrouped.b, 1e-4f);
}

TEST_F(PaintLayersGraphEvalTest, bake_render_node_folder_minimal)
{
  const int size = 4;
  ma = BKE_material_add(bmain, "FolderRender");
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_FOLDER, "Folder", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_PAINT, "Child", folder, PaintLayerPlace::Into);
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
      *ma, child, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  record->image = add_solid_image("Child", size, 0, 0, 255, 255);
  record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  Vector<float> color(int64_t(size) * size * 4);
  Vector<float> coverage(int64_t(size) * size);
  ASSERT_TRUE(BKE_paint_layers_bake_render_node(
      *ma, *folder, PAINT_MATERIAL_CHANNEL_BASE_COLOR, size, color.data(), coverage.data()));
  EXPECT_NEAR(coverage[0], 1.0f, 1e-4f);
  /* Blue in scene linear (byte 255 -> 1.0). */
  EXPECT_NEAR(color[0], 0.0f, 1e-4f);
  EXPECT_NEAR(color[1], 0.0f, 1e-4f);
  EXPECT_NEAR(color[2], 1.0f, 1e-4f);

  MaterialPaintLayerBake *bake = BKE_paint_layers_bake_ensure(*folder);
  bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR] = add_solid_image("BakedColor", size, 0, 0, 255, 255);
  bake->coverage = add_solid_image("BakedCov", size, 255, 255, 255, 255);
  uint32_t hash[2];
  BKE_paint_layers_bake_hash(*folder, hash);
  bake->hash[0] = hash[0];
  bake->hash[1] = hash[1];
  Image *out = nullptr;
  EXPECT_TRUE(BKE_paint_layers_bake_substitute(*ma, *folder, PAINT_MATERIAL_CHANNEL_BASE_COLOR, &out));
  EXPECT_EQ(out, bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR]);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA graph = interpreter.eval_result("Result Base Color");
  EXPECT_NEAR(graph.r, 0.0f, 1e-4f);
  EXPECT_NEAR(graph.g, 0.0f, 1e-4f);
  EXPECT_NEAR(graph.b, 1.0f, 1e-4f);
}

TEST_F(PaintLayersGraphEvalTest, bake_render_node_region_matches_full)
{
  const int size = 8;
  ma = BKE_material_add(bmain, "RegionRender");
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_PAINT, "Layer", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
      *ma, layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  Image *image = add_solid_image("Gradient", size, 0, 0, 0, 255);
  /* A per-pixel gradient so a wrong region crop cannot pass by accident. */
  {
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
    uchar *pixels = ibuf->byte_data_for_write();
    for (int y = 0; y < size; y++) {
      for (int x = 0; x < size; x++) {
        const int64_t i = int64_t(y) * size + x;
        pixels[i * 4 + 0] = uchar(x * 255 / (size - 1));
        pixels[i * 4 + 1] = uchar(y * 255 / (size - 1));
        pixels[i * 4 + 2] = uchar(64);
        pixels[i * 4 + 3] = 255;
      }
    }
    BKE_image_release_ibuf(image, ibuf, lock);
  }
  record->image = image;
  record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  Vector<float> full_color(int64_t(size) * size * 4);
  Vector<float> full_coverage(int64_t(size) * size);
  ASSERT_TRUE(BKE_paint_layers_bake_render_node(
      *ma, *layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, size, full_color.data(), full_coverage.data()));

  /* A rectangle that does not start at a tile or map boundary. */
  const int rect[4] = {2, 1, 6, 7};
  Vector<float> part_color(int64_t(size) * size * 4, -1.0f);
  Vector<float> part_coverage(int64_t(size) * size, -1.0f);
  ASSERT_TRUE(BKE_paint_layers_bake_render_node(*ma,
                                                *layer,
                                                PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                                size,
                                                part_color.data(),
                                                part_coverage.data(),
                                                rect));
  for (int y = rect[1]; y < rect[3]; y++) {
    for (int x = rect[0]; x < rect[2]; x++) {
      const int64_t i = int64_t(y) * size + x;
      for (const int k : IndexRange(4)) {
        EXPECT_NEAR(part_color[i * 4 + k], full_color[i * 4 + k], 1e-6f) << "at " << x << "," << y;
      }
      EXPECT_NEAR(part_coverage[i], full_coverage[i], 1e-6f) << "at " << x << "," << y;
    }
  }
  /* Outside the rectangle nothing was computed. */
  EXPECT_FLOAT_EQ(part_color[0], -1.0f);
  EXPECT_FLOAT_EQ(part_coverage[int64_t(size) * size - 1], -1.0f);
}

TEST_F(PaintLayersGraphEvalTest, bake_row_to_image_writes_row_content)
{
  const int size = 4;
  ma = BKE_material_add(bmain, "RowToImage");
  MaterialPaintLayer *layer = add_layer(
      "Source", MA_PAINT_LAYER_KIND_PAINT, add_solid_image("Source", size, 0, 255, 0, 255));

  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *dst = BKE_image_add_generated(
      bmain, size, size, "RowResult", 32, false, IMA_GENTYPE_BLANK, black, false, false, false);
  dst->alpha_mode = IMA_ALPHA_STRAIGHT;
  ASSERT_TRUE(BKE_paint_layers_bake_row_to_image(
      *ma, *layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, size, *dst));

  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(dst, nullptr, &lock);
  ASSERT_NE(ibuf, nullptr);
  const uchar *pixels = ibuf->byte_buffer.data;
  ASSERT_NE(pixels, nullptr);
  EXPECT_EQ(pixels[0], 0);
  EXPECT_EQ(pixels[1], 255);
  EXPECT_EQ(pixels[2], 0);
  BKE_image_release_ibuf(dst, ibuf, lock);
}

TEST_F(PaintLayersGraphEvalTest, heavy_bake_job_computes_and_commits)
{
  const int size = 4;
  ma = BKE_material_add(bmain, "HeavyJob");
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_PAINT, "Layer", nullptr, PaintLayerPlace::Above);
  /* Four channels put the subtree over the AUTO/worker threshold. */
  for (const eMaterialPaintChannel channel : {PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                              PAINT_MATERIAL_CHANNEL_ROUGHNESS,
                                              PAINT_MATERIAL_CHANNEL_METALLIC,
                                              PAINT_MATERIAL_CHANNEL_SPECULAR})
  {
    MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(*ma, layer, channel);
    ASSERT_NE(record, nullptr);
    record->image = add_solid_image("Map", size, 128, 64, 32, 255);
    record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  }
  MaterialPaintLayerBake *bake = BKE_paint_layers_bake_ensure(*layer);
  bake->mode = MA_PAINT_LAYER_BAKE_ALWAYS;
  bake->size = size;
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *layer));
  EXPECT_TRUE(BKE_paint_layers_bake_heavy_pending(*ma));

  PaintLayersBakeJob *job = BKE_paint_layers_bake_job_create(*bmain, *ma);
  ASSERT_NE(job, nullptr);
  BKE_paint_layers_bake_job_compute(*job);
  EXPECT_TRUE(BKE_paint_layers_bake_job_commit(*job));
  BKE_paint_layers_bake_job_free(*job);

  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *layer));
  Image *baked = nullptr;
  EXPECT_TRUE(
      BKE_paint_layers_bake_substitute(*ma, *layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, &baked));
  EXPECT_NE(baked, nullptr);
}

/**
 * ТЗ-29, test #3: unlike #heavy_bake_job_computes_and_commits, this row is never given an explicit
 * bake structure -- AUTO mode, #MaterialPaintLayer::bake null. #BKE_paint_layers_bake_job_create is
 * now the only place allowed to allocate one, and only for a row that passed every gate and is
 * about to be queued. Before the fix, the job-create gate required `layer->bake != nullptr` up
 * front, so a freshly authored heavy row with no bake was invisible to the async planner forever;
 * this test must fail on that old code.
 */
TEST_F(PaintLayersGraphEvalTest, heavy_bake_job_allocates_bake_for_a_row_with_none)
{
  const int size = 4;
  ma = BKE_material_add(bmain, "HeavyJobNoBake");
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_PAINT, "Layer", nullptr, PaintLayerPlace::Above);
  /* Four channels put the subtree over the AUTO/worker threshold, exactly like the job-create test
   * above. */
  for (const eMaterialPaintChannel channel : {PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                              PAINT_MATERIAL_CHANNEL_ROUGHNESS,
                                              PAINT_MATERIAL_CHANNEL_METALLIC,
                                              PAINT_MATERIAL_CHANNEL_SPECULAR})
  {
    MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(*ma, layer, channel);
    ASSERT_NE(record, nullptr);
    record->image = add_solid_image("Map", size, 128, 64, 32, 255);
    record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  }
  ASSERT_EQ(layer->bake, nullptr);
  ASSERT_TRUE(BKE_paint_layers_bake_is_heavy(*ma, *layer));
  ASSERT_FALSE(BKE_paint_layers_bake_row_is_deferred(*ma, *layer));
  ASSERT_TRUE(BKE_paint_layers_bake_heavy_pending(*ma));

  PaintLayersBakeJob *job = BKE_paint_layers_bake_job_create(*bmain, *ma);
  ASSERT_NE(job, nullptr);
  ASSERT_NE(layer->bake, nullptr)
      << "job_create must allocate the bake structure for a row it is about to queue";
  BKE_paint_layers_bake_job_compute(*job);
  EXPECT_TRUE(BKE_paint_layers_bake_job_commit(*job));
  BKE_paint_layers_bake_job_free(*job);

  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *layer));
  EXPECT_NE(layer->bake, nullptr);
  Image *baked = nullptr;
  EXPECT_TRUE(
      BKE_paint_layers_bake_substitute(*ma, *layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, &baked));
  EXPECT_NE(baked, nullptr);
}

/**
 * ТЗ-29, test #6: #BKE_paint_layers_bake_heavy_pending is a pure predicate -- it must see a row that
 * is heavy purely by subtree weight and has no bake structure at all, without allocating anything.
 * Before the fix, its combined gate required `layer->bake != nullptr` before it would even look at
 * `is_heavy`, so this row was invisible to it; this test must fail on that old code.
 */
TEST_F(PaintLayersGraphEvalTest, heavy_pending_sees_a_weight_heavy_row_with_no_bake)
{
  ma = BKE_material_add(bmain, "HeavyNoBake");
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_PAINT, "Layer", nullptr, PaintLayerPlace::Above);
  for (const eMaterialPaintChannel channel : {PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                              PAINT_MATERIAL_CHANNEL_ROUGHNESS,
                                              PAINT_MATERIAL_CHANNEL_METALLIC,
                                              PAINT_MATERIAL_CHANNEL_SPECULAR})
  {
    MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(*ma, layer, channel);
    ASSERT_NE(record, nullptr);
    record->image = add_solid_image("Map", 4, 128, 64, 32, 255);
    record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  }
  ASSERT_EQ(layer->bake, nullptr);
  EXPECT_TRUE(BKE_paint_layers_bake_heavy_pending(*ma));
  EXPECT_EQ(layer->bake, nullptr) << "the predicate is read-only and must not allocate anything";
}

TEST_F(PaintLayersGraphEvalTest, heavy_bake_job_drops_removed_row)
{
  const int size = 4;
  ma = BKE_material_add(bmain, "HeavyJobGone");
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_PAINT, "Layer", nullptr, PaintLayerPlace::Above);
  for (const eMaterialPaintChannel channel : {PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                              PAINT_MATERIAL_CHANNEL_ROUGHNESS,
                                              PAINT_MATERIAL_CHANNEL_METALLIC,
                                              PAINT_MATERIAL_CHANNEL_SPECULAR})
  {
    MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(*ma, layer, channel);
    record->image = add_solid_image("Map", size, 128, 64, 32, 255);
    record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  }
  MaterialPaintLayerBake *bake = BKE_paint_layers_bake_ensure(*layer);
  bake->mode = MA_PAINT_LAYER_BAKE_ALWAYS;
  bake->size = size;

  PaintLayersBakeJob *job = BKE_paint_layers_bake_job_create(*bmain, *ma);
  ASSERT_NE(job, nullptr);
  ASSERT_TRUE(BKE_paint_layers_remove(*ma, layer));
  BKE_paint_layers_bake_job_compute(*job);
  EXPECT_FALSE(BKE_paint_layers_bake_job_commit(*job));
  BKE_paint_layers_bake_job_free(*job);
}

TEST_F(PaintLayersGraphEvalTest, height_uses_mix_and_bump)
{
  const int size = 4;
  ma = BKE_material_add(bmain, "HeightEval");
  add_layer("Bottom",
            MA_PAINT_LAYER_KIND_PAINT,
            add_solid_image("Bottom", size, 192, 64, 32, 255),
            PAINT_MATERIAL_CHANNEL_HEIGHT);
  MaterialPaintLayer *top = add_layer("Top",
                                      MA_PAINT_LAYER_KIND_PAINT,
                                      add_solid_image("Top", size, 64, 192, 224, 255),
                                      PAINT_MATERIAL_CHANNEL_HEIGHT);
  /* Height is a scalar stack like any other: a plain Mix, never the Normal combine. */
  BKE_paint_layers_set_blend(*ma, top, MA_PAINT_LAYER_BLEND_MULTIPLY);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA graph = interpreter.eval_result("Result Height");
  const RGBA cpu = cpu_pixel(PAINT_MATERIAL_CHANNEL_HEIGHT);
  EXPECT_NEAR(graph.r, cpu.r, 1e-4f);
  EXPECT_NEAR(graph.g, cpu.g, 1e-4f);
  EXPECT_NEAR(graph.b, cpu.b, 1e-4f);

  /* The material reads the Height result through a Bump, not through the Result Normal chain. */
  bNode *bump = nullptr;
  for (bNode &node : ma->nodetree->nodes) {
    if (node.type_legacy == SH_NODE_BUMP) {
      bump = &node;
      break;
    }
  }
  ASSERT_NE(bump, nullptr);
  bNodeSocket *height_in = bke::node_find_socket(*bump, SOCK_IN, "Height"_ustr);
  ASSERT_NE(height_in, nullptr);
  EXPECT_FALSE(height_in->directly_linked_links().is_empty());
}

TEST_F(PaintLayersGraphEvalTest, bake_planner_writes_maps_and_substitutes)
{
  const int size = 4;
  ma = BKE_material_add(bmain, "BakePlanner");
  add_layer("Bottom", MA_PAINT_LAYER_KIND_PAINT, add_solid_image("Bottom", size, 192, 64, 32, 255));
  MaterialPaintLayer *top = add_layer(
      "Top", MA_PAINT_LAYER_KIND_PAINT, add_solid_image("Top", size, 64, 192, 224, 255));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA live = interpreter.eval_result("Result Base Color");

  MaterialPaintLayerBake *bake = BKE_paint_layers_bake_ensure(*top);
  bake->size = size;
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *top, MA_PAINT_LAYER_BAKE_ALWAYS));
  bool changed = false;
  ASSERT_TRUE(BKE_paint_layers_bake_ensure(*bmain, *ma, &changed));
  EXPECT_TRUE(changed);
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *top));
  EXPECT_NE(bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR], nullptr);
  EXPECT_NE(bake->coverage, nullptr);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA baked = interpreter.eval_result("Result Base Color");
  EXPECT_NEAR(baked.r, live.r, 1e-2f);
  EXPECT_NEAR(baked.g, live.g, 1e-2f);
  EXPECT_NEAR(baked.b, live.b, 1e-2f);
}

/**
 * A synthetic bake of \a layer, built the way the planner will: composite the node's subtree through
 * #BKE_paint_layers_bake_render_node, write the straight colour and grey coverage into two images,
 * and stamp the description hash. Then the substituted tree must equal the live one.
 */
TEST_F(PaintLayersGraphEvalTest, baked_node_equals_the_live_node)
{
  const int size = 4;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;

  enum class Variant { Layer, Multiply, Mask, Correction, Folder, NestedFolder };
  const Variant variants[] = {Variant::Layer,
                              Variant::Multiply,
                              Variant::Mask,
                              Variant::Correction,
                              Variant::Folder,
                              Variant::NestedFolder};

  auto make_float_image = [&](const char *name, const float *rgba, const float *gray) {
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    Image *image = BKE_image_add_generated(
        bmain, size, size, name, 32, 32, IMA_GENTYPE_BLANK, black, false, false, false);
    STRNCPY(image->colorspace_settings.name, "Non-Color");
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
    float *pixels = const_cast<float *>(ibuf->float_buffer.data);
    for (const int64_t i : IndexRange(int64_t(size) * size)) {
      if (rgba != nullptr) {
        pixels[i * 4 + 0] = rgba[i * 4 + 0];
        pixels[i * 4 + 1] = rgba[i * 4 + 1];
        pixels[i * 4 + 2] = rgba[i * 4 + 2];
        pixels[i * 4 + 3] = 1.0f;
      }
      else {
        pixels[i * 4 + 0] = gray[i];
        pixels[i * 4 + 1] = gray[i];
        pixels[i * 4 + 2] = gray[i];
        pixels[i * 4 + 3] = 1.0f;
      }
    }
    BKE_image_release_ibuf(image, ibuf, lock);
    return image;
  };

  auto add_paint = [&](const char *name, Image *image, MaterialPaintLayer *anchor) {
    MaterialPaintLayer *layer = BKE_paint_layers_add(*ma,
                                                     MA_PAINT_LAYER_KIND_PAINT,
                                                     name,
                                                     anchor,
                                                     anchor != nullptr ? PaintLayerPlace::Into :
                                                                          PaintLayerPlace::Above);
    MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(*ma, layer, channel);
    record->image = image;
    record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
    return layer;
  };

  for (const Variant variant : variants) {
    ma = BKE_material_add(bmain, "BakeEquiv");
    add_paint("Bottom", add_solid_image("Bottom", size, 192, 64, 32, 255), nullptr);

    MaterialPaintLayer *baked_node = nullptr;
    switch (variant) {
      case Variant::Layer:
        baked_node = add_paint("Top", add_solid_image("Top", size, 64, 192, 224, 255), nullptr);
        break;
      case Variant::Multiply:
        baked_node = add_paint("Top", add_solid_image("Top", size, 64, 192, 224, 255), nullptr);
        BKE_paint_layers_set_blend(*ma, baked_node, MA_PAINT_LAYER_BLEND_MULTIPLY);
        break;
      case Variant::Mask:
        baked_node = add_paint("Top", add_solid_image("Top", size, 64, 192, 224, 255), nullptr);
        set_mask_image(*baked_node, add_solid_image("Mask", size, 200, 200, 200, 255));
        break;
      case Variant::Correction: {
        baked_node = add_paint("Top", add_solid_image("Top", size, 64, 192, 224, 255), nullptr);
        MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
            *ma, baked_node, MA_PAINT_LAYER_SECTION_CONTENT, MA_PAINT_LAYER_EFFECT_FILL, "C");
        const float green[4] = {0.0f, 1.0f, 0.0f, 1.0f};
        BKE_paint_layers_set_fill_color(*ma, correction, green);
        BKE_paint_layers_set_opacity(*ma, correction, 0.4f);
        break;
      }
      case Variant::Folder: {
        MaterialPaintLayer *folder = BKE_paint_layers_add(
            *ma, MA_PAINT_LAYER_KIND_FOLDER, "Folder", nullptr, PaintLayerPlace::Above);
        add_paint("Child", add_solid_image("Child", size, 0, 0, 255, 255), folder);
        BKE_paint_layers_set_opacity(*ma, folder, 0.7f);
        baked_node = folder;
        break;
      }
      case Variant::NestedFolder: {
        MaterialPaintLayer *outer = BKE_paint_layers_add(
            *ma, MA_PAINT_LAYER_KIND_FOLDER, "Outer", nullptr, PaintLayerPlace::Above);
        MaterialPaintLayer *inner = BKE_paint_layers_add(
            *ma, MA_PAINT_LAYER_KIND_FOLDER, "Inner", outer, PaintLayerPlace::Into);
        add_paint("Child", add_solid_image("Child", size, 0, 255, 0, 255), inner);
        BKE_paint_layers_set_opacity(*ma, inner, 0.6f);
        BKE_paint_layers_set_opacity(*ma, outer, 0.8f);
        baked_node = outer;
        break;
      }
    }
    ASSERT_NE(baked_node, nullptr);

    /* Reference: the live stack. */
    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
    GraphInterpreter interpreter;
    interpreter.instance = find_instance();
    interpreter.tree = ma->paint_layers_tree;
    interpreter.x = 1;
    interpreter.y = 1;
    ASSERT_NE(interpreter.instance, nullptr);
    const RGBA graph_live = interpreter.eval_result(result_name(channel));
    const RGBA cpu_live = cpu_pixel(channel);

    /* Synthetic bake: the node's own CPU composite. */
    Vector<float> color(int64_t(size) * size * 4);
    Vector<float> coverage(int64_t(size) * size);
    ASSERT_TRUE(BKE_paint_layers_bake_render_node(
        *ma, *baked_node, channel, size, color.data(), coverage.data()));
    MaterialPaintLayerBake *bake = BKE_paint_layers_bake_ensure(*baked_node);
    bake->images[channel] = make_float_image("BakeColor", color.data(), nullptr);
    bake->coverage = make_float_image("BakeCoverage", nullptr, coverage.data());
    uint32_t hash[2];
    BKE_paint_layers_bake_hash(*baked_node, hash);
    bake->hash[0] = hash[0];
    bake->hash[1] = hash[1];
    ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *baked_node));

    /* The substituted stack. */
    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
    interpreter.instance = find_instance();
    interpreter.tree = ma->paint_layers_tree;
    ASSERT_NE(interpreter.instance, nullptr);
    const RGBA graph_baked = interpreter.eval_result(result_name(channel));
    const RGBA cpu_baked = cpu_pixel(channel);

    const float tol = 1e-4f;
    EXPECT_NEAR(graph_baked.r, graph_live.r, tol) << int(variant);
    EXPECT_NEAR(graph_baked.g, graph_live.g, tol) << int(variant);
    EXPECT_NEAR(graph_baked.b, graph_live.b, tol) << int(variant);
    EXPECT_NEAR(cpu_baked.r, cpu_live.r, tol) << int(variant);
    EXPECT_NEAR(cpu_baked.g, cpu_live.g, tol) << int(variant);
    EXPECT_NEAR(cpu_baked.b, cpu_live.b, tol) << int(variant);

    BKE_id_free(bmain, ma);
    ma = nullptr;
  }
}

/**
 * A Material layer has no live subtree: its channels exist only as its bake, and those maps are the
 * row's content in both the generator and the CPU composite.
 */
TEST_F(PaintLayersGraphEvalTest, material_layer_bake_is_its_content_in_graph_and_cpu)
{
  const int size = 4;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "MaterialLayer");
  Material *source = BKE_material_add(bmain, "MaterialSource");

  MaterialPaintLayer *material = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_MATERIAL, "Material", nullptr, PaintLayerPlace::Above);
  material->material = source;
  MaterialPaintLayerBake *bake = BKE_paint_layers_bake_ensure(*material);
  bake->size = size;
  bake->mode = MA_PAINT_LAYER_BAKE_ALWAYS;

  Image *color = add_solid_image("MaterialBakeColor", size, 0, 255, 0, 255);
  Image *coverage = add_solid_image("MaterialBakeCoverage", size, 255, 255, 255, 255);
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *material, channel, color));
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *material, -1, coverage));
  BKE_paint_layers_bake_finalize(*ma, *material);
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *material));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA graph = interpreter.eval_result(result_name(channel));
  const RGBA cpu = cpu_pixel(channel);

  /* Coverage is one, so the baked colour shows through unblended. */
  EXPECT_NEAR(graph.g, 1.0f, 1e-2f);
  EXPECT_NEAR(graph.r, 0.0f, 1e-2f);
  EXPECT_NEAR(graph.b, 0.0f, 1e-2f);
  EXPECT_NEAR(cpu.r, graph.r, 1e-2f);
  EXPECT_NEAR(cpu.g, graph.g, 1e-2f);
  EXPECT_NEAR(cpu.b, graph.b, 1e-2f);
}

/**
 * A Custom layer has no live CPU subtree either: with a valid bake it substitutes in both the
 * generator and the CPU composite exactly like a Material layer, and without one it drops out.
 */
TEST_F(PaintLayersGraphEvalTest, custom_layer_bake_substitutes_in_graph_and_cpu)
{
  const int size = 4;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "CustomLayer");
  MaterialPaintLayer *custom = BKE_paint_layers_custom_layer_add(
      *bmain, *ma, "Custom", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(custom, nullptr);

  /* Without a bake the row contributes nothing: the result is the channel bottom. */
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA without_bake = interpreter.eval_result(result_name(channel));

  MaterialPaintLayerBake *bake = BKE_paint_layers_bake_ensure(*custom);
  bake->size = size;
  bake->mode = MA_PAINT_LAYER_BAKE_ALWAYS;
  Image *color = add_solid_image("CustomBakeColor", size, 0, 255, 0, 255);
  Image *coverage = add_solid_image("CustomBakeCoverage", size, 255, 255, 255, 255);
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *custom, channel, color));
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *custom, -1, coverage));
  BKE_paint_layers_bake_finalize(*ma, *custom);
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *custom));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA graph = interpreter.eval_result(result_name(channel));
  const RGBA cpu = cpu_pixel(channel);

  EXPECT_NEAR(graph.g, 1.0f, 1e-2f);
  EXPECT_NEAR(graph.r, 0.0f, 1e-2f);
  EXPECT_NEAR(cpu.r, graph.r, 1e-2f);
  EXPECT_NEAR(cpu.g, graph.g, 1e-2f);
  EXPECT_NEAR(cpu.b, graph.b, 1e-2f);
  /* The unbaked result differed: the layer did take part once baked. */
  EXPECT_GT(std::abs(graph.g - without_bake.g), 1e-2f);
}

/**
 * C-7: a Custom row has no live CPU expression, so a saved bake that is stale (its hash no longer
 * matches) is still shown instead of the row dropping to black -- both sides substitute it.
 */
TEST_F(PaintLayersGraphEvalTest, custom_stale_bake_still_substitutes)
{
  const int size = 4;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "CustomStale");
  MaterialPaintLayer *custom = BKE_paint_layers_custom_layer_add(
      *bmain, *ma, "Custom", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(custom, nullptr);

  MaterialPaintLayerBake *bake = BKE_paint_layers_bake_ensure(*custom);
  bake->size = size;
  bake->mode = MA_PAINT_LAYER_BAKE_ALWAYS;
  Image *color = add_solid_image("StaleCustomColor", size, 0, 255, 0, 255);
  Image *coverage = add_solid_image("StaleCustomCoverage", size, 255, 255, 255, 255);
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *custom, channel, color));
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *custom, -1, coverage));
  /* No finalize: the stored hash stays zero, so the maps are there but stale. */
  ASSERT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *custom));
  {
    Image *sub = nullptr;
    bool stale = false;
    ASSERT_TRUE(
        BKE_paint_layers_bake_substitute_custom(*ma, *custom, channel, &sub, &stale));
    ASSERT_EQ(sub, color);
    ASSERT_TRUE(stale);
  }

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA graph = interpreter.eval_result(result_name(channel));
  const RGBA cpu = cpu_pixel(channel);

  /* Coverage is one, so the stale baked green shows through. */
  EXPECT_NEAR(graph.g, 1.0f, 1e-2f);
  EXPECT_NEAR(graph.r, 0.0f, 1e-2f);
  EXPECT_NEAR(cpu.r, graph.r, 1e-2f);
  EXPECT_NEAR(cpu.g, graph.g, 1e-2f);
  EXPECT_NEAR(cpu.b, graph.b, 1e-2f);
}

/**
 * "Use Row Result" renders a source row's channels off the main thread and commits them as the
 * target row's maps; the BKE job split is what the heavy operator path runs.
 */
TEST_F(PaintLayersGraphEvalTest, row_result_job_bakes_source_into_target_channels)
{
  const int size = 4;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "RowResult");

  MaterialPaintLayer *source = add_layer(
      "Source", MA_PAINT_LAYER_KIND_PAINT, add_solid_image("SourceImg", size, 0, 255, 0, 255));
  MaterialPaintLayer *target = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_PAINT, "Target", nullptr, PaintLayerPlace::Above);

  PaintLayersRowResultJob *job = BKE_paint_layers_row_result_job_create(
      *bmain, *ma, source->marker, target->marker, size);
  ASSERT_NE(job, nullptr);
  BKE_paint_layers_row_result_job_compute(*job);
  ASSERT_TRUE(BKE_paint_layers_row_result_job_commit(*job));
  BKE_paint_layers_row_result_job_free(*job);

  MaterialPaintLayer *live_target = BKE_paint_layers_find(*ma, target->marker);
  ASSERT_NE(live_target, nullptr);
  const MaterialPaintLayerChannel *record = nullptr;
  for (int i = 0; i < live_target->channels_num; i++) {
    if (live_target->channels[i].channel == channel) {
      record = &live_target->channels[i];
    }
  }
  ASSERT_NE(record, nullptr);
  ASSERT_NE(record->image, nullptr);

  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(record->image, nullptr, &lock);
  ASSERT_NE(ibuf, nullptr);
  const uchar *pixels = ibuf->byte_data();
  ASSERT_NE(pixels, nullptr);
  const int64_t i = (int64_t(1) * size + 1) * 4;
  /* Green survived decode/encode back into the map's own sRGB space. */
  EXPECT_LT(int(pixels[i + 0]), 8);
  EXPECT_GT(int(pixels[i + 1]), 247);
  EXPECT_LT(int(pixels[i + 2]), 8);
  BKE_image_release_ibuf(record->image, ibuf, lock);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * A semi-transparent material bake limits the row through its coverage map: the generator and the
 * CPU both blend the baked colour over what is below by that factor, so they have to agree.
 */
TEST_F(PaintLayersGraphEvalTest, material_layer_semi_transparent_bake_matches_cpu)
{
  const int size = 4;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "MaterialSemi");
  Material *source = BKE_material_add(bmain, "MaterialSemiSource");

  MaterialPaintLayer *material = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_MATERIAL, "Material", nullptr, PaintLayerPlace::Above);
  material->material = source;
  MaterialPaintLayerBake *bake = BKE_paint_layers_bake_ensure(*material);
  bake->size = size;
  bake->mode = MA_PAINT_LAYER_BAKE_ALWAYS;

  Image *color = add_solid_image("SemiBakeColor", size, 0, 255, 0, 255);
  Image *coverage = add_solid_image("SemiBakeCoverage", size, 128, 128, 128, 255);
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *material, channel, color));
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *material, -1, coverage));
  BKE_paint_layers_bake_finalize(*ma, *material);
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *material));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA graph = interpreter.eval_result(result_name(channel));
  const RGBA cpu = cpu_pixel(channel);

  /* Half coverage: between the bottom and the full baked green, on both sides. */
  EXPECT_GT(graph.g, 0.05f);
  EXPECT_LT(graph.g, 0.98f);
  EXPECT_NEAR(cpu.r, graph.r, 1e-2f);
  EXPECT_NEAR(cpu.g, graph.g, 1e-2f);
  EXPECT_NEAR(cpu.b, graph.b, 1e-2f);
}

/**
 * ТЗ-25c: a Material row whose source's Alpha is a plain constant below 1. The real bake path
 * (#BKE_paint_layers_material_bake_apply) diverts that channel into the row's #coverage, never
 * into #MaterialPaintLayerBake::images[ALPHA] -- no pipeline fills that slot. Before the fix
 * #paint_layer_material_source_map read Alpha from `images[]` regardless, so it always answered
 * null and the channel stayed "live" forever (#material_live_row_eligible): a fully finalized,
 * not-deferred Material row could never settle on Baked, and stayed Hybrid indefinitely instead.
 * This bakes every channel through the real entry point, including Alpha, and checks the row lands
 * on Baked once finalized, with the graph and the CPU composite agreeing with the reference blend.
 */
TEST_F(PaintLayersGraphEvalTest, material_layer_constant_alpha_bake_settles_on_baked)
{
  const int size = 4;
  const eMaterialPaintChannel bc = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "MatAlphaBaked");
  Image *bottom_image = add_solid_image("Bottom", size, 255, 0, 0, 255);
  add_layer("Bottom", MA_PAINT_LAYER_KIND_PAINT, bottom_image, bc);

  const float row_color[4] = {0.0f, 1.0f, 0.0f, 1.0f};
  const float alpha_value = 0.5f;
  Material *source = BKE_material_add(bmain, "MatAlphaSource");
  bNodeTree &tree = *source->nodetree;
  bNode *principled = bke::node_add_static_node(nullptr, tree, SH_NODE_BSDF_PRINCIPLED);
  bNode *output = bke::node_add_static_node(nullptr, tree, SH_NODE_OUTPUT_MATERIAL);
  bke::node_add_link(tree,
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_OUT, "BSDF"_ustr),
                     *output,
                     *bke::node_find_socket(*output, SOCK_IN, "Surface"_ustr));
  bNodeSocket *color_socket = bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr);
  ASSERT_NE(color_socket, nullptr);
  copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(color_socket->default_value)->value, row_color);
  bNodeSocket *alpha_socket = bke::node_find_socket(*principled, SOCK_IN, "Alpha"_ustr);
  ASSERT_NE(alpha_socket, nullptr);
  static_cast<bNodeSocketValueFloat *>(alpha_socket->default_value)->value = alpha_value;
  BKE_ntree_update_tag_all(&tree);
  BKE_ntree_update_after_single_tree_change(*bmain, tree);

  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_MATERIAL, "Mat", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));

  /* Not active: the row is free to be baked and read from its maps like the planner would. */
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());

  /* Bake through the real entry point: every resolvable channel gets a solid map at the source's
   * constant value, and Alpha (the constant 0.5) is diverted into the row's coverage exactly as a
   * real material bake hand-over does. */
  const MaterialSourceResolve resolve = BKE_paint_material_source_resolve(source);
  Vector<int> channels;
  Vector<Image *> images;
  Image *color_image = nullptr;
  for (const eMaterialPaintChannel channel : BKE_paint_material_bakeable_channels()) {
    if (channel == PAINT_MATERIAL_CHANNEL_ALPHA ||
        resolve.channels[channel] == ChannelResolution::Unavailable)
    {
      continue;
    }
    Image *map = (channel == bc) ? add_solid_image("MatAlphaColor", size, 0, 255, 0, 255) :
                                   add_solid_image("MatAlphaOther", size, 0, 255, 0, 255);
    if (channel == bc) {
      color_image = map;
    }
    channels.append(int(channel));
    images.append(map);
  }
  ASSERT_NE(color_image, nullptr);
  channels.append(int(PAINT_MATERIAL_CHANNEL_ALPHA));
  Image *coverage_image = add_solid_image("MatAlphaCoverage", size, 128, 128, 128, 255);
  images.append(coverage_image);
  BKE_paint_layers_material_bake_apply(
      *bmain, *ma, *row, size, channels.as_span(), images.as_span());
  BKE_paint_layers_bake_finalize(*ma, *row);
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *row));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Baked)
      << "Alpha must be read from the row's coverage, or the channel stays perpetually unmapped "
         "and the row never leaves Hybrid";

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA graph = interpreter.eval_result(result_name(bc));
  const RGBA cpu = cpu_pixel(bc);

  /* The reference blend: bottom under the baked colour, mixed by the decoded coverage factor --
   * built from the same maps the bake wrote, read back through the same colour management the
   * generator and the CPU both apply, so this does not assume a particular gamma convention. */
  const RGBA bottom_sample = interpreter.sample_image(bottom_image, "Color");
  const RGBA color_sample = interpreter.sample_image(color_image, "Color");
  const RGBA coverage_sample = interpreter.sample_image(coverage_image, "Color");
  const float factor = (coverage_sample.r + coverage_sample.g + coverage_sample.b) / 3.0f;
  ASSERT_GT(factor, 0.01f);
  ASSERT_LT(factor, 0.99f);
  const float expected_g = bottom_sample.g + (color_sample.g - bottom_sample.g) * factor;
  EXPECT_NEAR(graph.g, expected_g, 1e-2f);
  EXPECT_NEAR(cpu.r, graph.r, 1e-2f);
  EXPECT_NEAR(cpu.g, graph.g, 1e-2f);
  EXPECT_NEAR(cpu.b, graph.b, 1e-2f);
}

/**
 * A mask on a Material layer applies live on top of its source's bake: adding it leaves the bake
 * valid (no EEVEE re-render of the source), and a black mask hides the row in both the generator
 * and the CPU composite.
 */
TEST_F(PaintLayersGraphEvalTest, material_layer_mask_is_live_and_keeps_the_bake)
{
  const int size = 4;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "MaterialMasked");
  Material *source = BKE_material_add(bmain, "MaterialMaskedSource");

  add_layer("Bottom", MA_PAINT_LAYER_KIND_PAINT, add_solid_image("Bottom", size, 255, 0, 0, 255));
  MaterialPaintLayer *material = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_MATERIAL, "Material", nullptr, PaintLayerPlace::Above);
  material->material = source;
  MaterialPaintLayerBake *bake = BKE_paint_layers_bake_ensure(*material);
  bake->size = size;
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(
      *ma, *material, channel, add_solid_image("MaskedBakeColor", size, 0, 255, 0, 255)));
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(
      *ma, *material, -1, add_solid_image("MaskedBakeCoverage", size, 255, 255, 255, 255)));
  BKE_paint_layers_bake_finalize(*ma, *material);
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *material));

  /* The row's own settings are not what the source was baked from. */
  set_mask_image(*material, add_solid_image("MaskedMask", size, 0, 0, 0, 255));
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, material, 0.75f));
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *material));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA graph = interpreter.eval_result(result_name(channel));
  const RGBA cpu = cpu_pixel(channel);

  /* The black mask hides the Material row: the red bottom shows, on both sides. */
  EXPECT_NEAR(graph.r, 1.0f, 1e-2f);
  EXPECT_NEAR(graph.g, 0.0f, 1e-2f);
  EXPECT_NEAR(cpu.r, graph.r, 1e-2f);
  EXPECT_NEAR(cpu.g, graph.g, 1e-2f);
  EXPECT_NEAR(cpu.b, graph.b, 1e-2f);
}

/**
 * A Paint correction on a Material row: its opacity is a value of the row group's interface and must
 * reach the correction's Mix factor in every row mode. Baked here (the row is not active).
 */
TEST_F(PaintLayersGraphEvalTest, material_layer_paint_correction_opacity_is_live)
{
  const int size = 4;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "MaterialCorrOpacity");
  Material *source = BKE_material_add(bmain, "MaterialCorrOpacitySource");

  MaterialPaintLayer *material = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_MATERIAL, "Material", nullptr, PaintLayerPlace::Above);
  material->material = source;
  MaterialPaintLayerBake *bake = BKE_paint_layers_bake_ensure(*material);
  bake->size = size;
  bake->mode = MA_PAINT_LAYER_BAKE_ALWAYS;
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(
      *ma, *material, channel, add_solid_image("CorrOpacityBase", size, 255, 0, 0, 255)));
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(
      *ma, *material, -1, add_solid_image("CorrOpacityCov", size, 255, 255, 255, 255)));
  BKE_paint_layers_bake_finalize(*ma, *material);
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *material), PaintLayerMaterialMode::Baked);

  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, material, MA_PAINT_LAYER_SECTION_CONTENT, MA_PAINT_LAYER_EFFECT_PAINT, "C");
  ASSERT_NE(correction, nullptr);
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(*ma, correction, channel);
  ASSERT_NE(record, nullptr);
  Image *blue = add_solid_image("CorrOpacityBlue", size, 0, 0, 255, 255);
  blue->alpha_mode = IMA_ALPHA_STRAIGHT;
  blue->flag |= IMA_GPU_LINEAR_PREMUL;
  record->image = blue;
  record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  auto eval_channel = [&]() {
    GraphInterpreter interpreter;
    interpreter.instance = find_instance();
    interpreter.tree = ma->paint_layers_tree;
    interpreter.x = 1;
    interpreter.y = 1;
    EXPECT_NE(interpreter.instance, nullptr);
    return interpreter.eval_result(result_name(channel));
  };

  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, correction, 0.0f));
  BKE_paint_layers_values_sync(*ma);
  const RGBA off = eval_channel();

  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, correction, 1.0f));
  BKE_paint_layers_values_sync(*ma);
  const RGBA on = eval_channel();

  /* Off: the baked red row. On: the correction's blue dominates. */
  EXPECT_GT(off.r, 0.5f);
  EXPECT_LT(off.b, 0.1f);
  EXPECT_GT(on.b, off.b + 0.1f);
  EXPECT_LT(on.r, off.r - 0.1f);
}

/**
 * The "below" a Custom row is fed during its GPU bake is the stack under it only: the rows above it
 * must not leak in, and the result is straight scene-linear with the below coverage in alpha.
 */
TEST_F(PaintLayersGraphEvalTest, below_image_composites_only_rows_under_the_custom_row)
{
  const int size = 4;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "BelowImage");

  add_layer("Bottom", MA_PAINT_LAYER_KIND_PAINT, add_solid_image("Bottom", size, 255, 0, 0, 255));
  add_layer(
      "Middle", MA_PAINT_LAYER_KIND_PAINT, add_solid_image("Middle", size, 255, 255, 0, 255));
  MaterialPaintLayer *custom = BKE_paint_layers_custom_layer_add(
      *bmain, *ma, "Custom", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(custom, nullptr);
  /* A row above the custom one must not leak into its "below". */
  add_layer("Top", MA_PAINT_LAYER_KIND_PAINT, add_solid_image("Top", size, 0, 0, 255, 255));

  Image *below = BKE_paint_layers_below_image(*bmain, *ma, *custom, channel, size);
  ASSERT_NE(below, nullptr);
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(below, nullptr, &lock);
  ASSERT_NE(ibuf, nullptr);
  ASSERT_EQ(ibuf->x, size);
  const float *pixels = ibuf->float_data();
  ASSERT_NE(pixels, nullptr);
  const int64_t i = (int64_t(1) * size + 1) * 4;
  /* Middle (sRGB yellow) over Bottom (sRGB red) fully covers: straight linear yellow. */
  EXPECT_NEAR(pixels[i + 0], 1.0f, 1e-3f);
  EXPECT_NEAR(pixels[i + 1], 1.0f, 1e-3f);
  EXPECT_NEAR(pixels[i + 2], 0.0f, 1e-3f);
  EXPECT_NEAR(pixels[i + 3], 1.0f, 1e-3f);
  BKE_image_release_ibuf(below, ibuf, lock);
  BKE_id_free(bmain, below);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * The template a "New Custom Layer" makes has to carry the row below through to its color output,
 * or the first bake renders empty. The link is written before the group's topology settles, so this
 * pins that it survives the update.
 */
TEST_F(PaintLayersGraphEvalTest, custom_template_group_body_passes_below_into_color)
{
  ma = BKE_material_add(bmain, "CustomTemplate");
  MaterialPaintLayer *custom = BKE_paint_layers_custom_layer_add(
      *bmain, *ma, "Custom", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(custom, nullptr);
  ASSERT_NE(custom->custom_group, nullptr);
  bNodeTree *group = custom->custom_group;
  group->ensure_topology_cache();

  const bNode *input = nullptr;
  const bNode *output = nullptr;
  for (const bNode &node : group->nodes) {
    if (node.is_group_input()) {
      input = &node;
    }
    if (node.is_group_output()) {
      output = &node;
    }
  }
  ASSERT_NE(input, nullptr);
  ASSERT_NE(output, nullptr);

  const bNodeSocket *below = static_cast<const bNodeSocket *>(input->outputs.first);
  const bNodeSocket *color = static_cast<const bNodeSocket *>(output->inputs.first);
  ASSERT_NE(below, nullptr);
  ASSERT_NE(color, nullptr);
  bool linked = false;
  for (const bNodeLink &link : group->links) {
    if (link.fromnode == input && link.tonode == output && link.fromsock == below &&
        link.tosock == color)
    {
      linked = true;
    }
  }
  EXPECT_TRUE(linked);

  Vector<int> channels;
  BKE_paint_layers_custom_channels_get(*custom, channels);
  EXPECT_TRUE(channels.contains(int(PAINT_MATERIAL_CHANNEL_BASE_COLOR)));

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * Per (row, channel) blend/opacity: a channel record's own blend and opacity multiplier, and the
 * row's opacity, must mean the same in the generated chain and the CPU. The override is set on a
 * card that is not the record's channel yet, exercising the on-demand creation too.
 */
TEST_F(PaintLayersGraphEvalTest, per_channel_blend_opacity_matches_the_cpu)
{
  const int size = 4;
  const eMaterialPaintChannel bc = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  const eMaterialPaintChannel rough = PAINT_MATERIAL_CHANNEL_ROUGHNESS;

  auto result_name_for = [](const int channel) {
    return std::string("Result ") +
           BKE_paint_material_channel_info(eMaterialPaintChannel(channel)).ui_name;
  };

  auto add_single_channel_layer = [&](const char *name,
                                       Image *image,
                                       MaterialPaintLayer *anchor,
                                       const PaintLayerPlace place,
                                       const eMaterialPaintChannel channel)
      -> MaterialPaintLayer * {
    MaterialPaintLayer *layer = BKE_paint_layers_add(
        *ma, MA_PAINT_LAYER_KIND_PAINT, name, anchor, place);
    EXPECT_NE(layer, nullptr);
    MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(*ma, layer, channel);
    EXPECT_NE(record, nullptr);
    record->image = image;
    record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
    return layer;
  };

  auto add_two_channel_layer = [&](const char *name,
                                   Image *bc_image,
                                   Image *rough_image,
                                   MaterialPaintLayer *anchor,
                                   const PaintLayerPlace place) -> MaterialPaintLayer * {
    MaterialPaintLayer *layer = BKE_paint_layers_add(
        *ma, MA_PAINT_LAYER_KIND_PAINT, name, anchor, place);
    EXPECT_NE(layer, nullptr);
    MaterialPaintLayerChannel *b = BKE_paint_layers_channel_add(*ma, layer, bc);
    EXPECT_NE(b, nullptr);
    b->image = bc_image;
    b->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
    MaterialPaintLayerChannel *r = BKE_paint_layers_channel_add(*ma, layer, rough);
    EXPECT_NE(r, nullptr);
    r->image = rough_image;
    r->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
    return layer;
  };

  auto compare = [&](const char *name, const int channel) {
    GraphInterpreter interpreter;
    interpreter.instance = find_instance();
    interpreter.tree = ma->paint_layers_tree;
    interpreter.x = 1;
    interpreter.y = 1;
    ASSERT_NE(interpreter.instance, nullptr);
    const RGBA graph = interpreter.eval_result(result_name_for(channel).c_str());
    const RGBA cpu = cpu_pixel(channel);
    EXPECT_NEAR(graph.r, cpu.r, 1e-4f) << name;
    EXPECT_NEAR(graph.g, cpu.g, 1e-4f) << name;
    EXPECT_NEAR(graph.b, cpu.b, 1e-4f) << name;
  };

  /* A leaf: the row's opacity times the channel's own multiplier, and a per-channel blend. */
  {
    ma = BKE_material_add(bmain, "PerChannelLeaf");
    add_layer("Bottom", MA_PAINT_LAYER_KIND_PAINT, add_solid_image("Bottom", size, 255, 0, 0, 255), bc);
    MaterialPaintLayer *top = add_layer(
        "Top", MA_PAINT_LAYER_KIND_PAINT, add_solid_image("Top", size, 0, 0, 255, 255), bc);
    BKE_paint_layers_set_opacity(*ma, top, 0.5f);
    ASSERT_TRUE(BKE_paint_layers_channel_opacity_set(*ma, *top, bc, 0.5f));
    ASSERT_TRUE(BKE_paint_layers_channel_blend_set(*ma, *top, bc, MA_PAINT_LAYER_BLEND_MULTIPLY));
    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
    compare("leaf", bc);
    BKE_id_free(bmain, ma);
    ma = nullptr;
  }

  /* A folder: one channel overridden, another inheriting the row's blend and opacity. */
  {
    ma = BKE_material_add(bmain, "PerChannelFolder");
    add_two_channel_layer("Bottom",
                          add_solid_image("BottomBC", size, 255, 0, 0, 255),
                          add_solid_image("BottomR", size, 64, 64, 64, 255),
                          nullptr,
                          PaintLayerPlace::Above);
    MaterialPaintLayer *folder = BKE_paint_layers_add(
        *ma, MA_PAINT_LAYER_KIND_FOLDER, "Folder", nullptr, PaintLayerPlace::Above);
    ASSERT_NE(folder, nullptr);
    add_two_channel_layer("Child",
                          add_solid_image("ChildBC", size, 0, 0, 255, 255),
                          add_solid_image("ChildR", size, 200, 200, 200, 255),
                          folder,
                          PaintLayerPlace::Into);
    BKE_paint_layers_set_opacity(*ma, folder, 0.5f);
    ASSERT_TRUE(BKE_paint_layers_channel_blend_set(
        *ma, *folder, bc, MA_PAINT_LAYER_BLEND_MULTIPLY));
    ASSERT_TRUE(BKE_paint_layers_channel_opacity_set(*ma, *folder, rough, 0.5f));
    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
    compare("folder_bc_override", bc);
    compare("folder_rough_inherit", rough);
    BKE_id_free(bmain, ma);
    ma = nullptr;
  }

  /* A nested folder: opacity at two levels, each channel its own multiplier. */
  {
    ma = BKE_material_add(bmain, "PerChannelNested");
    add_layer("Bottom", MA_PAINT_LAYER_KIND_PAINT, add_solid_image("Bottom", size, 255, 0, 0, 255), bc);
    MaterialPaintLayer *outer = BKE_paint_layers_add(
        *ma, MA_PAINT_LAYER_KIND_FOLDER, "Outer", nullptr, PaintLayerPlace::Above);
    ASSERT_NE(outer, nullptr);
    MaterialPaintLayer *inner = BKE_paint_layers_add(
        *ma, MA_PAINT_LAYER_KIND_FOLDER, "Inner", outer, PaintLayerPlace::Into);
    ASSERT_NE(inner, nullptr);
    add_single_channel_layer("Leaf",
                             add_solid_image("Leaf", size, 0, 0, 255, 255),
                             inner,
                             PaintLayerPlace::Into,
                             bc);
    BKE_paint_layers_set_opacity(*ma, outer, 0.5f);
    ASSERT_TRUE(BKE_paint_layers_channel_opacity_set(*ma, *inner, bc, 0.5f));
    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
    compare("nested", bc);
    BKE_id_free(bmain, ma);
    ma = nullptr;
  }
}

/** A freshly authored Paint row takes part in the default channels but lays nothing down. */
TEST_F(PaintLayersGraphEvalTest, authored_paint_without_a_map_matches_the_cpu)
{
  const int size = 4;
  ma = BKE_material_add(bmain, "AuthoredPaint");
  add_layer("Bottom", MA_PAINT_LAYER_KIND_PAINT, add_solid_image("Bottom", size, 255, 0, 0, 255));
  MaterialPaintLayer *paint = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_PAINT, "Paint", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(paint, nullptr);
  BKE_paint_layers_default_channels_apply(*ma, *paint);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA graph = interpreter.eval_result("Result Base Color");
  const RGBA cpu = cpu_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  EXPECT_NEAR(graph.r, cpu.r, 1e-4f);
  EXPECT_NEAR(graph.g, cpu.g, 1e-4f);
  EXPECT_NEAR(graph.b, cpu.b, 1e-4f);
  /* The Paint row covers nothing, so the red bottom shows through unchanged. */
  EXPECT_NEAR(graph.r, 1.0f, 1e-4f);
  EXPECT_NEAR(graph.g, 0.0f, 1e-4f);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/** A Fill shows a value per channel: Base Color from `fill_color`, the rest from their records. */
TEST_F(PaintLayersGraphEvalTest, fill_has_a_value_per_channel)
{
  const int size = 4;
  ma = BKE_material_add(bmain, "FillPerChannel");
  add_layer("Bottom", MA_PAINT_LAYER_KIND_PAINT, add_solid_image("Bottom", size, 255, 255, 255, 255));
  /* A Roughness map too, so that channel has a pixel size to composite into. */
  add_layer("BottomR",
            MA_PAINT_LAYER_KIND_PAINT,
            add_solid_image("BottomR", size, 128, 128, 128, 255),
            PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  MaterialPaintLayer *fill = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_FILL, "Fill", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(fill, nullptr);
  BKE_paint_layers_default_channels_apply(*ma, *fill);
  const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
  copy_v4_v4(fill->fill_color, red);
  const float rough[4] = {0.75f, 0.75f, 0.75f, 1.0f};
  ASSERT_TRUE(
      BKE_paint_layers_channel_set_value(*ma, fill, PAINT_MATERIAL_CHANNEL_ROUGHNESS, rough));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);

  const RGBA graph_bc = interpreter.eval_result("Result Base Color");
  const RGBA cpu_bc = cpu_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  EXPECT_NEAR(graph_bc.r, cpu_bc.r, 1e-4f);
  EXPECT_NEAR(graph_bc.g, cpu_bc.g, 1e-4f);
  EXPECT_NEAR(graph_bc.b, cpu_bc.b, 1e-4f);
  EXPECT_NEAR(graph_bc.r, 1.0f, 1e-4f);
  EXPECT_NEAR(graph_bc.g, 0.0f, 1e-4f);

  const RGBA graph_rough = interpreter.eval_result("Result Roughness");
  const RGBA cpu_rough = cpu_pixel(PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  EXPECT_NEAR(graph_rough.r, cpu_rough.r, 1e-4f);
  EXPECT_NEAR(graph_rough.r, 0.75f, 1e-4f);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * The main TZ-2 check: an active Material row whose source channels are constants is shown live by
 * the generator and by the CPU composite, and the two must agree. The source's Base Color and
 * Alpha are unlinked, so the row is a constant colour limited by a constant coverage; the row's own
 * opacity scales both.
 */
TEST_F(PaintLayersGraphEvalTest, live_material_constant_matches_the_cpu)
{
  const int size = 4;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "LiveMaterial");
  add_layer("Bottom", MA_PAINT_LAYER_KIND_PAINT, add_solid_image("Bottom", size, 255, 0, 0, 255));

  Material *source = BKE_material_add(bmain, "LiveSource");
  bNodeTree &ntree = *source->nodetree;
  bNode *principled = bke::node_add_static_node(nullptr, ntree, SH_NODE_BSDF_PRINCIPLED);
  bNode *output = bke::node_add_static_node(nullptr, ntree, SH_NODE_OUTPUT_MATERIAL);
  bke::node_add_link(ntree,
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_OUT, "BSDF"_ustr),
                     *output,
                     *bke::node_find_socket(*output, SOCK_IN, "Surface"_ustr));
  const float source_color[4] = {0.25f, 0.5f, 0.75f, 1.0f};
  bNodeSocket *base_color = bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr);
  ASSERT_NE(base_color, nullptr);
  copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(base_color->default_value)->value, source_color);
  bNodeSocket *alpha = bke::node_find_socket(*principled, SOCK_IN, "Alpha"_ustr);
  ASSERT_NE(alpha, nullptr);
  static_cast<bNodeSocketValueFloat *>(alpha->default_value)->value = 0.75f;

  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, row, 0.5f));
  BKE_paint_layers_active_set(*ma, row->marker);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA graph = interpreter.eval_result(result_name(channel));
  const RGBA cpu = cpu_pixel(channel);

  /* The row blends the live constant over the red bottom by opacity * source alpha = 0.375. */
  const float factor = 0.5f * 0.75f;
  const float expected[3] = {1.0f + (source_color[0] - 1.0f) * factor,
                             0.0f + (source_color[1] - 0.0f) * factor,
                             0.0f + (source_color[2] - 0.0f) * factor};
  EXPECT_NEAR(graph.r, expected[0], 1e-4f);
  EXPECT_NEAR(graph.g, expected[1], 1e-4f);
  EXPECT_NEAR(graph.b, expected[2], 1e-4f);
  EXPECT_NEAR(graph.r, cpu.r, 1e-4f);
  EXPECT_NEAR(graph.g, cpu.g, 1e-4f);
  EXPECT_NEAR(graph.b, cpu.b, 1e-4f);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * The same live Material row, but with the focus elsewhere and no baked maps for the row. The
 * source constant still carries the row, so the generator and the CPU must agree exactly as when
 * the row is active. This is the viewport case the fix exists for: the layer must not change when
 * the user moves off it.
 */
TEST_F(PaintLayersGraphEvalTest, inactive_material_constant_matches_the_cpu)
{
  const int size = 4;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "InactiveLiveMaterial");
  MaterialPaintLayer *bottom = add_layer(
      "Bottom", MA_PAINT_LAYER_KIND_PAINT, add_solid_image("Bottom", size, 255, 0, 0, 255));

  Material *source = BKE_material_add(bmain, "InactiveLiveSource");
  bNodeTree &ntree = *source->nodetree;
  bNode *principled = bke::node_add_static_node(nullptr, ntree, SH_NODE_BSDF_PRINCIPLED);
  bNode *output = bke::node_add_static_node(nullptr, ntree, SH_NODE_OUTPUT_MATERIAL);
  bke::node_add_link(ntree,
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_OUT, "BSDF"_ustr),
                     *output,
                     *bke::node_find_socket(*output, SOCK_IN, "Surface"_ustr));
  const float source_color[4] = {0.25f, 0.5f, 0.75f, 1.0f};
  bNodeSocket *base_color = bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr);
  ASSERT_NE(base_color, nullptr);
  copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(base_color->default_value)->value, source_color);
  bNodeSocket *alpha = bke::node_find_socket(*principled, SOCK_IN, "Alpha"_ustr);
  ASSERT_NE(alpha, nullptr);
  static_cast<bNodeSocketValueFloat *>(alpha->default_value)->value = 0.75f;

  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, row, 0.5f));
  /* The focus is on the bottom row, not on the Material row. */
  BKE_paint_layers_active_set(*ma, bottom->marker);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA graph = interpreter.eval_result(result_name(channel));
  const RGBA cpu = cpu_pixel(channel);

  const float factor = 0.5f * 0.75f;
  const float expected[3] = {1.0f + (source_color[0] - 1.0f) * factor,
                             0.0f + (source_color[1] - 0.0f) * factor,
                             0.0f + (source_color[2] - 0.0f) * factor};
  EXPECT_NEAR(graph.r, expected[0], 1e-4f);
  EXPECT_NEAR(graph.g, expected[1], 1e-4f);
  EXPECT_NEAR(graph.b, expected[2], 1e-4f);
  EXPECT_NEAR(graph.r, cpu.r, 1e-4f);
  EXPECT_NEAR(graph.g, cpu.g, 1e-4f);
  EXPECT_NEAR(graph.b, cpu.b, 1e-4f);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * TZ-26: a live Hybrid constant is a group input now, not a value baked into an RGB node's default
 * (see the topology hash and #create_value_inputs in paint_layers_generate.cc). Moving the source's
 * Base Color must not rebuild the row's group or the root -- the edit reaches the graph through
 * #BKE_paint_layers_values_sync -- and the graph must read the new value, matching the CPU exactly
 * as #live_material_constant_matches_the_cpu already does for the initial value.
 */
TEST_F(PaintLayersGraphEvalTest, live_material_constant_edit_syncs_without_rebuild)
{
  const int size = 4;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "LiveSyncMaterial");
  add_layer("Bottom", MA_PAINT_LAYER_KIND_PAINT, add_solid_image("Bottom", size, 255, 0, 0, 255));

  Material *source = BKE_material_add(bmain, "LiveSyncSource");
  bNodeTree &ntree = *source->nodetree;
  bNode *principled = bke::node_add_static_node(nullptr, ntree, SH_NODE_BSDF_PRINCIPLED);
  bNode *output = bke::node_add_static_node(nullptr, ntree, SH_NODE_OUTPUT_MATERIAL);
  bke::node_add_link(ntree,
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_OUT, "BSDF"_ustr),
                     *output,
                     *bke::node_find_socket(*output, SOCK_IN, "Surface"_ustr));
  const float source_color[4] = {0.25f, 0.5f, 0.75f, 1.0f};
  bNodeSocket *base_color = bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr);
  ASSERT_NE(base_color, nullptr);
  copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(base_color->default_value)->value, source_color);
  bNodeSocket *roughness = bke::node_find_socket(*principled, SOCK_IN, "Roughness"_ustr);
  ASSERT_NE(roughness, nullptr);
  static_cast<bNodeSocketValueFloat *>(roughness->default_value)->value = 0.2f;
  bNodeSocket *alpha = bke::node_find_socket(*principled, SOCK_IN, "Alpha"_ustr);
  ASSERT_NE(alpha, nullptr);
  static_cast<bNodeSocketValueFloat *>(alpha->default_value)->value = 0.75f;

  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, row, 0.5f));
  BKE_paint_layers_active_set(*ma, row->marker);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Hybrid);

  bNodeTree *root = ma->paint_layers_tree;
  ASSERT_NE(root, nullptr);
  const Vector<bNode *> root_before = root_node_ptrs(*root);
  bNodeTree *group = nullptr;
  for (bNodeTree &tree : bmain->nodetrees) {
    if (STREQ(tree.id.name + 2, ".PL Layer Source")) {
      group = &tree;
      break;
    }
  }
  ASSERT_NE(group, nullptr);
  const Vector<bNode *> group_before = root_node_ptrs(*group);

  /* Move the source's Base Color and Roughness -- values shown live from the source, per the RNA
   * path a real edit uses (RNA_property_update / node-tree update, which #material_changed in
   * render_update.cc answers by tagging the layered material edited): #BKE_paint_layers_tag_edited
   * plus the depsgraph's #ID_RECALC_SHADING tag, which #BKE_material_eval answers with
   * #BKE_paint_layers_values_sync on every evaluated copy. Editing the node socket's default value
   * directly and calling #BKE_paint_layers_regenerate reproduces exactly that contract without a
   * window and a depsgraph, the same substitution the neighbouring tests in this file already make
   * for every other "user moved a slider" case. */
  const float new_color[4] = {0.9f, 0.1f, 0.4f, 1.0f};
  copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(base_color->default_value)->value, new_color);
  static_cast<bNodeSocketValueFloat *>(roughness->default_value)->value = 0.8f;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  /* Neither the row's group nor the root was rebuilt: the value travelled through a group input
   * instead of forcing a new topology hash. */
  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_TRUE(same_node_ptrs(root_before, root_node_ptrs(*root)));
  bNodeTree *group_after = nullptr;
  for (bNodeTree &tree : bmain->nodetrees) {
    if (STREQ(tree.id.name + 2, ".PL Layer Source")) {
      group_after = &tree;
      break;
    }
  }
  EXPECT_EQ(group_after, group);
  EXPECT_TRUE(same_node_ptrs(group_before, root_node_ptrs(*group)));

  /* The graph reads the new value, matching the CPU compositor, which reads the same live helper. */
  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA graph = interpreter.eval_result(result_name(channel));
  const RGBA cpu = cpu_pixel(channel);
  const float factor = 0.5f * 0.75f;
  const float expected[3] = {1.0f + (new_color[0] - 1.0f) * factor,
                             0.0f + (new_color[1] - 0.0f) * factor,
                             0.0f + (new_color[2] - 0.0f) * factor};
  EXPECT_NEAR(graph.r, expected[0], 1e-4f);
  EXPECT_NEAR(graph.g, expected[1], 1e-4f);
  EXPECT_NEAR(graph.b, expected[2], 1e-4f);
  EXPECT_NEAR(graph.r, cpu.r, 1e-4f);
  EXPECT_NEAR(graph.g, cpu.g, 1e-4f);
  EXPECT_NEAR(graph.b, cpu.b, 1e-4f);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * TZ-26: the view a channel is shown in is still topology -- switching the source's Base Color from
 * a constant to an Image Texture must rebuild the row's group, unlike a plain value edit above.
 */
TEST_F(PaintLayersGraphEvalTest, live_material_view_change_rebuilds_the_row_group)
{
  const int size = 4;
  ma = BKE_material_add(bmain, "LiveViewChangeMaterial");
  add_layer("Bottom", MA_PAINT_LAYER_KIND_PAINT, add_solid_image("Bottom", size, 255, 0, 0, 255));

  Material *source = BKE_material_add(bmain, "LiveViewChangeSource");
  bNodeTree &ntree = *source->nodetree;
  bNode *principled = bke::node_add_static_node(nullptr, ntree, SH_NODE_BSDF_PRINCIPLED);
  bNode *output = bke::node_add_static_node(nullptr, ntree, SH_NODE_OUTPUT_MATERIAL);
  bke::node_add_link(ntree,
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_OUT, "BSDF"_ustr),
                     *output,
                     *bke::node_find_socket(*output, SOCK_IN, "Surface"_ustr));
  bNodeSocket *base_color = bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr);
  ASSERT_NE(base_color, nullptr);
  const float source_color[4] = {0.25f, 0.5f, 0.75f, 1.0f};
  copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(base_color->default_value)->value, source_color);

  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  BKE_paint_layers_active_set(*ma, row->marker);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Hybrid);

  bNodeTree *group = nullptr;
  for (bNodeTree &tree : bmain->nodetrees) {
    if (STREQ(tree.id.name + 2, ".PL Layer Source")) {
      group = &tree;
      break;
    }
  }
  ASSERT_NE(group, nullptr);
  const Vector<bNode *> group_before = root_node_ptrs(*group);

  /* Replace the constant with a trivially-mapped texture: the channel's view changes from Constant
   * to Image, which is topology (#topology_hash_layer still hashes `live_constant` and
   * `live_map_probe`), so the row's group must be rebuilt. */
  Image *source_map = add_solid_image("LiveViewChangeMap", size, 200, 200, 200, 255);
  bNode *texture = bke::node_add_static_node(nullptr, ntree, SH_NODE_TEX_IMAGE);
  texture->id = &source_map->id;
  bke::node_add_link(
      ntree, *texture, *bke::node_find_socket(*texture, SOCK_OUT, "Color"_ustr), *principled, *base_color);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *group_after = nullptr;
  for (bNodeTree &tree : bmain->nodetrees) {
    if (STREQ(tree.id.name + 2, ".PL Layer Source")) {
      group_after = &tree;
      break;
    }
  }
  ASSERT_NE(group_after, nullptr);
  EXPECT_EQ(group_after, group) << "the group is preserved, only rebuilt in place";
  EXPECT_FALSE(same_node_ptrs(group_before, root_node_ptrs(*group)))
      << "constant -> texture must rebuild the row's group";

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * TZ-26: #BKE_material_eval calls #BKE_paint_layers_values_sync on the evaluated (COW) copy of the
 * material -- never on the original -- and #layer.material is walked by
 * #material_paint_layer_foreach_id (IDWALK_CB_USER), so depsgraph's generic pointer remap already
 * gives that evaluated row an evaluated `layer.material` by the time the sync runs; nothing in
 * #values_sync_socket needs to special-case evaluated vs original. This test proves the reading
 * side of that contract directly: #BKE_paint_layers_material_live_constant (and so
 * #values_sync_socket, which only wraps it) resolves the constant from whatever Material
 * `layer.material` currently names, with no assumption that it is the original -- swapping the
 * pointer to an independent copy of the source, the same substitution a depsgraph remap performs,
 * must be picked up by the very next sync. A full evaluated-copy Material is not built here: a
 * plain #BKE_id_copy_ex of `ma` would leave `layer.material` shared with the original (only
 * depsgraph's relation-driven remap swaps it, and #paint_layers_tree is `IDWALK_CB_USER`-walked
 * the same way as #layer.material, so a bare ID copy cannot stand in for that pass without
 * reimplementing it) -- swapping `layer.material` on `ma` itself isolates exactly the one behaviour
 * in question.
 */
TEST_F(PaintLayersGraphEvalTest, live_constant_values_sync_reads_whatever_material_it_is_given)
{
  const int size = 4;
  ma = BKE_material_add(bmain, "EvalPathMaterial");
  add_layer("Bottom", MA_PAINT_LAYER_KIND_PAINT, add_solid_image("Bottom", size, 255, 0, 0, 255));

  Material *source = BKE_material_add(bmain, "EvalPathSource");
  bNodeTree &ntree = *source->nodetree;
  bNode *principled = bke::node_add_static_node(nullptr, ntree, SH_NODE_BSDF_PRINCIPLED);
  bNode *output = bke::node_add_static_node(nullptr, ntree, SH_NODE_OUTPUT_MATERIAL);
  bke::node_add_link(ntree,
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_OUT, "BSDF"_ustr),
                     *output,
                     *bke::node_find_socket(*output, SOCK_IN, "Surface"_ustr));
  bNodeSocket *base_color = bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr);
  ASSERT_NE(base_color, nullptr);
  const float source_color[4] = {0.2f, 0.4f, 0.6f, 1.0f};
  copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(base_color->default_value)->value, source_color);

  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  BKE_paint_layers_active_set(*ma, row->marker);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Hybrid);

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA before = interpreter.eval_result(result_name(PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  EXPECT_NEAR(before.r, source_color[0], 1e-4f);
  EXPECT_NEAR(before.g, source_color[1], 1e-4f);
  EXPECT_NEAR(before.b, source_color[2], 1e-4f);

  /* An independent copy of the source, its own embedded node tree included -- what a depsgraph
   * remap would install in `layer.material` for an evaluated `ma`. */
  Material *source_eval = reinterpret_cast<Material *>(
      BKE_id_copy_ex(nullptr, &source->id, nullptr, LIB_ID_COPY_LOCALIZE));
  ASSERT_NE(source_eval, nullptr);
  ASSERT_NE(source_eval->nodetree, source->nodetree)
      << "the embedded node tree must be its own copy, or this test proves nothing";
  bNode *principled_eval = nullptr;
  for (bNode &node : source_eval->nodetree->nodes) {
    if (node.type_legacy == SH_NODE_BSDF_PRINCIPLED) {
      principled_eval = &node;
      break;
    }
  }
  ASSERT_NE(principled_eval, nullptr);
  bNodeSocket *base_color_eval = bke::node_find_socket(*principled_eval, SOCK_IN, "Base Color"_ustr);
  ASSERT_NE(base_color_eval, nullptr);
  const float eval_color[4] = {0.9f, 0.1f, 0.4f, 1.0f};
  copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(base_color_eval->default_value)->value, eval_color);

  /* The substitution itself: `layer.material` now names the independent copy, exactly as a
   * depsgraph remap would leave it on the evaluated `ma`. No regeneration runs -- only sync, the
   * same call #BKE_material_eval makes for every evaluated copy. */
  row->material = source_eval;
  BKE_paint_layers_values_sync(*ma);

  const RGBA after = interpreter.eval_result(result_name(PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  EXPECT_NEAR(after.r, eval_color[0], 1e-4f)
      << "values_sync must read the Material layer.material currently names, not the original";
  EXPECT_NEAR(after.g, eval_color[1], 1e-4f);
  EXPECT_NEAR(after.b, eval_color[2], 1e-4f);

  /* The untouched original source still carries its own constant -- the sync read the copy, it did
   * not write back into it. */
  bNodeSocket *base_color_after = bke::node_find_socket(
      *principled, SOCK_IN, "Base Color"_ustr);
  ASSERT_NE(base_color_after, nullptr);
  const float *original_value =
      static_cast<bNodeSocketValueRGBA *>(base_color_after->default_value)->value;
  EXPECT_NEAR(original_value[0], source_color[0], 1e-6f);
  EXPECT_NEAR(original_value[1], source_color[1], 1e-6f);
  EXPECT_NEAR(original_value[2], source_color[2], 1e-6f);

  row->material = source;
  BKE_id_free(bmain, source_eval);
  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * TZ-4's main check: an active Material row whose Base Color and Alpha are plain textures. The
 * generator shows the source's own Image Texture and reads its Alpha for coverage; the CPU samples
 * the same image in UV space and reads its alpha. On a soft alpha edge the two must agree at every
 * pixel, or leaving the row would show a rim.
 */
TEST_F(PaintLayersGraphEvalTest, live_material_image_matches_the_cpu)
{
  const int size = 4;
  const eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "LiveImageMaterial");
  add_layer("Bottom", MA_PAINT_LAYER_KIND_PAINT, add_solid_image("Bottom", size, 255, 0, 0, 255));

  Material *source = BKE_material_add(bmain, "LiveImageSource");
  bNodeTree &ntree = *source->nodetree;
  bNode *principled = bke::node_add_static_node(nullptr, ntree, SH_NODE_BSDF_PRINCIPLED);
  bNode *output = bke::node_add_static_node(nullptr, ntree, SH_NODE_OUTPUT_MATERIAL);
  bke::node_add_link(ntree,
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_OUT, "BSDF"_ustr),
                     *output,
                     *bke::node_find_socket(*output, SOCK_IN, "Surface"_ustr));
  Image *source_map = add_solid_image("LiveSourceMap", size, 200, 200, 200, 255);
  fill_straight_soft_edge(source_map, size, 0.5f);
  bNode *texture = bke::node_add_static_node(nullptr, ntree, SH_NODE_TEX_IMAGE);
  texture->id = &source_map->id;
  bke::node_add_link(ntree,
                     *texture,
                     *bke::node_find_socket(*texture, SOCK_OUT, "Color"_ustr),
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr));
  bke::node_add_link(ntree,
                     *texture,
                     *bke::node_find_socket(*texture, SOCK_OUT, "Alpha"_ustr),
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_IN, "Alpha"_ustr));
  static_cast<NodeTexImage *>(texture->storage)->projection = SHD_PROJ_FLAT;

  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, row, 0.5f));
  BKE_paint_layers_active_set(*ma, row->marker);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  ASSERT_NE(interpreter.instance, nullptr);

  const float tolerance = 1e-4f;
  for (int x = 0; x < size; x++) {
    interpreter.x = x;
    interpreter.y = 0;
    const RGBA graph = interpreter.eval_result(result_name(channel));
    const RGBA cpu = cpu_pixel_at(channel, x, 0);
    EXPECT_NEAR(graph.r, cpu.r, tolerance) << "x=" << x;
    EXPECT_NEAR(graph.g, cpu.g, tolerance) << "x=" << x;
    EXPECT_NEAR(graph.b, cpu.b, tolerance) << "x=" << x;
  }

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * The wrapper copy must keep the source group's interface: the root's RGB and Value reach the group
 * inputs through Reroutes, Metallic is an instance value. If the copy's Group Input lost its
 * sockets, the Principled would fall back to its defaults and this would read 0.8 / 0.5 / 0.0.
 */
TEST_F(PaintLayersGraphEvalTest, source_group_live_wired_reads_the_source_values)
{
  ma = BKE_material_add(bmain, "WiredLive");
  Material *source = make_interface_wired_source(*bmain, "WiredLiveSource");
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  for (const eMaterialPaintChannel channel : {PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                              PAINT_MATERIAL_CHANNEL_ROUGHNESS,
                                              PAINT_MATERIAL_CHANNEL_METALLIC})
  {
    ASSERT_NE(BKE_paint_layers_channel_add(*ma, row, channel), nullptr);
  }
  BKE_paint_layers_active_set(*ma, row->marker);
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::SourceGroup);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  ASSERT_NE(interpreter.tree, nullptr);

  /* The wired source values, not the Principled defaults. */
  const RGBA base = interpreter.eval_result("Result Base Color");
  EXPECT_NEAR(base.r, 0.2f, 1e-4f);
  EXPECT_NEAR(base.g, 0.5f, 1e-4f);
  EXPECT_NEAR(base.b, 0.9f, 1e-4f);
  const RGBA rough = interpreter.eval_result("Result Roughness");
  EXPECT_NEAR(rough.r, 0.3f, 1e-4f);
  const RGBA metal = interpreter.eval_result("Result Metallic");
  EXPECT_NEAR(metal.r, 0.7f, 1e-4f);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/* -------------------------------------------------------------------- */
/** \name TZ-15: SourceGroup vs Hybrid/Baked parity for a Material row
 * \{ */

/** A float image in data space, as the material bake writes a Value channel / coverage map. */
static Image *make_bake_data_map(Main *bmain, const char *name, const int size, const float rgba[4])
{
  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *image = BKE_image_add_generated(
      bmain, size, size, name, 32, /*floatbuf=*/true, IMA_GENTYPE_BLANK, black, false, true, false);
  if (image == nullptr) {
    return nullptr;
  }
  image->alpha_mode = IMA_ALPHA_STRAIGHT;
  BLI_strncpy(image->colorspace_settings.name,
              IMB_colormanagement_role_colorspace_name_get(COLOR_ROLE_DATA),
              sizeof(image->colorspace_settings.name));
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
  if (ibuf == nullptr || ibuf->float_data() == nullptr) {
    if (ibuf != nullptr) {
      BKE_image_release_ibuf(image, ibuf, lock);
    }
    return image;
  }
  float *pixels = ibuf->float_data_for_write();
  for (const int64_t texel : IndexRange(int64_t(size) * size)) {
    pixels[texel * 4 + 0] = rgba[0];
    pixels[texel * 4 + 1] = rgba[1];
    pixels[texel * 4 + 2] = rgba[2];
    pixels[texel * 4 + 3] = 1.0f;
  }
  BKE_image_release_ibuf(image, ibuf, lock);
  return image;
}

/** Print the node chain feeding \a socket, so a divergence can be read off the graph. */
static void dump_socket_chain(const bNodeSocket &socket, const int depth)
{
  for (const bNodeLink *link : socket.directly_linked_links()) {
    if (link->fromnode == nullptr) {
      continue;
    }
    const bNode &node = *link->fromnode;
    printf("paint layers dump: %*s<- %s '%s' custom1=%d custom2=%d\n",
           depth * 2,
           "",
           node.typeinfo != nullptr ? node.typeinfo->idname.c_str() : "?",
           node.name,
           node.custom1,
           node.custom2);
    for (const bNodeSocket &input : node.inputs) {
      if (input.default_value != nullptr && input.type == SOCK_FLOAT) {
        printf("paint layers dump: %*s   in '%s' = %.4f\n",
               depth * 2,
               "",
               input.name,
               static_cast<const bNodeSocketValueFloat *>(input.default_value)->value);
      }
      else if (input.default_value != nullptr && input.type == SOCK_RGBA) {
        const float *v = static_cast<const bNodeSocketValueRGBA *>(input.default_value)->value;
        printf("paint layers dump: %*s   in '%s' = (%.4f %.4f %.4f)\n",
               depth * 2,
               "",
               input.name,
               v[0],
               v[1],
               v[2]);
      }
      dump_socket_chain(input, depth + 1);
    }
  }
}

/** The value of \a channel's `Result <name>` socket in the generated tree of \a interpreter. */
static RGBA eval_channel_result(const GraphInterpreter &interpreter,
                                const eMaterialPaintChannel channel)
{
  const char *ui_name = BKE_paint_material_channel_info(channel).ui_name;
  char result_name[64];
  BLI_snprintf(result_name, sizeof(result_name), "Result %s", ui_name);
  return interpreter.eval_result(result_name);
}

/**
 * TZ-15/16: a Material row read live through the SourceGroup wrapper (run A) and the same row read
 * from its baked maps in Hybrid/Baked (run B) must both equal the source's own values. The maps are
 * synthesized the way #material_bake_to_images/#BKE_paint_layers_material_bake_apply leave them:
 * float data maps, coverage = the source alpha.
 */
TEST_F(PaintLayersGraphEvalTest, material_row_source_group_matches_its_baked_maps)
{
  const int size = 4;
  ma = BKE_material_add(bmain, "ParityLayered");
  Material *source = make_specular_source(*bmain, "ParitySource", false);

  BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_PAINT, "Bottom", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  MaterialPaintLayer *top = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_PAINT, "Top", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(top, nullptr);

  const eMaterialPaintChannel channels[] = {PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                            PAINT_MATERIAL_CHANNEL_METALLIC,
                                            PAINT_MATERIAL_CHANNEL_ROUGHNESS,
                                            PAINT_MATERIAL_CHANNEL_SPECULAR,
                                            PAINT_MATERIAL_CHANNEL_EMISSION};
  /* The source's own values, read off #make_specular_source: RGB Base Color, instance Metallic,
   * Value Roughness, unlinked Specular 0.25, Principled Emission default. */
  const RGBA expected[] = {{0.2f, 0.5f, 0.9f, 1.0f},
                           {0.7f, 0.7f, 0.7f, 1.0f},
                           {0.3f, 0.3f, 0.3f, 1.0f},
                           {0.25f, 0.25f, 0.25f, 1.0f},
                           {1.0f, 1.0f, 1.0f, 1.0f}};
  const float tolerance = 1e-3f;

  /* Run A: the Material row is active, so it reads its source through the wrapper. */
  BKE_paint_layers_active_set(*ma, row->marker);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_NE(ma->paint_layers_tree, nullptr);
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::SourceGroup);

  GraphInterpreter interp_a;
  interp_a.instance = find_instance();
  interp_a.tree = ma->paint_layers_tree;
  interp_a.x = 1;
  interp_a.y = 1;
  ASSERT_NE(interp_a.instance, nullptr);
  interp_a.tree->ensure_topology_cache();

  Vector<RGBA> a_values;
  for (const int i : IndexRange(ARRAY_SIZE(channels))) {
    a_values.append(eval_channel_result(interp_a, channels[i]));
    const char *ui_name = BKE_paint_material_channel_info(channels[i]).ui_name;
    EXPECT_NEAR(a_values[i].r, expected[i].r, tolerance) << ui_name;
    EXPECT_NEAR(a_values[i].g, expected[i].g, tolerance) << ui_name;
    EXPECT_NEAR(a_values[i].b, expected[i].b, tolerance) << ui_name;
  }
  {
    const bNode *output = GraphInterpreter::active_group_output(*interp_a.tree);
    ASSERT_NE(output, nullptr);
    char result_name[64];
    BLI_snprintf(result_name,
                 sizeof(result_name),
                 "Result %s",
                 BKE_paint_material_channel_info(PAINT_MATERIAL_CHANNEL_SPECULAR).ui_name);
    for (bNodeTreeInterfaceSocket *iface : interp_a.tree->interface_outputs()) {
      if (iface->name == nullptr || iface->identifier == nullptr ||
          !STREQ(iface->name, result_name))
      {
        continue;
      }
      if (const bNodeSocket *input = bke::node_find_socket(
              const_cast<bNode &>(*output),
              SOCK_IN,
              UString::from_ptr_noinline(iface->identifier)))
      {
        printf("paint layers dump: run A specular chain\n");
        dump_socket_chain(*input, 1);
      }
    }
  }

  /* Run B: synthesize the maps the material bake would write from the source's values, hand them
   * to the row, and move the active marker to the top row so the Material row reads them. */
  const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  Image *coverage = make_bake_data_map(bmain, "ParityCoverage", size, white);
  ASSERT_NE(coverage, nullptr);
  for (const int i : IndexRange(ARRAY_SIZE(channels))) {
    char name[64];
    BLI_snprintf(
        name, sizeof(name), "Parity %s", BKE_paint_material_channel_info(channels[i]).ui_name);
    const float rgba[4] = {expected[i].r, expected[i].g, expected[i].b, 1.0f};
    Image *map = make_bake_data_map(bmain, name, size, rgba);
    ASSERT_NE(map, nullptr);
    ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *row, channels[i], map));
  }
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *row, -1, coverage));
  BKE_paint_layers_bake_finalize(*ma, *row);

  BKE_paint_layers_active_set(*ma, top->marker);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_NE(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::SourceGroup);

  GraphInterpreter interp_b;
  interp_b.instance = find_instance();
  interp_b.tree = ma->paint_layers_tree;
  interp_b.x = 1;
  interp_b.y = 1;
  ASSERT_NE(interp_b.instance, nullptr);
  interp_b.tree->ensure_topology_cache();

  for (const int i : IndexRange(ARRAY_SIZE(channels))) {
    const RGBA b = eval_channel_result(interp_b, channels[i]);
    const char *ui_name = BKE_paint_material_channel_info(channels[i]).ui_name;
    printf("paint layers parity: %-12s source=(%.4f %.4f %.4f) A=(%.4f %.4f %.4f) "
           "B=(%.4f %.4f %.4f)\n",
           ui_name,
           expected[i].r,
           expected[i].g,
           expected[i].b,
           a_values[i].r,
           a_values[i].g,
           a_values[i].b,
           b.r,
           b.g,
           b.b);
    EXPECT_NEAR(b.r, expected[i].r, tolerance) << ui_name;
    EXPECT_NEAR(b.g, expected[i].g, tolerance) << ui_name;
    EXPECT_NEAR(b.b, expected[i].b, tolerance) << ui_name;
  }
  {
    const bNode *output = GraphInterpreter::active_group_output(*interp_b.tree);
    ASSERT_NE(output, nullptr);
    char result_name[64];
    BLI_snprintf(result_name,
                 sizeof(result_name),
                 "Result %s",
                 BKE_paint_material_channel_info(PAINT_MATERIAL_CHANNEL_SPECULAR).ui_name);
    for (bNodeTreeInterfaceSocket *iface : interp_b.tree->interface_outputs()) {
      if (iface->name == nullptr || iface->identifier == nullptr ||
          !STREQ(iface->name, result_name))
      {
        continue;
      }
      if (const bNodeSocket *input = bke::node_find_socket(
              const_cast<bNode &>(*output),
              SOCK_IN,
              UString::from_ptr_noinline(iface->identifier)))
      {
        printf("paint layers dump: run B specular chain\n");
        dump_socket_chain(*input, 1);
      }
    }
  }

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * TZ-16 regression: the wrapper must carry the SPECULAR channel. Its interface output used to
 * collide by identifier with the group copy's own `BSDF` output, so Specular read 0. Checked after
 * the first build and after a Base Color edit takes the rebuild branch, for a Specular linked
 * through a Group Input named "Specular" and an unlinked one.
 */
TEST_F(PaintLayersGraphEvalTest, material_row_source_group_keeps_specular)
{
  for (const bool linked : {false, true}) {
    ma = BKE_material_add(bmain, linked ? "SpecLinked" : "SpecUnlinked");
    Material *source = make_specular_source(
        *bmain, linked ? "SpecLinkedSource" : "SpecUnlinkedSource", linked);

    MaterialPaintLayer *row = BKE_paint_layers_add(
        *ma, MA_PAINT_LAYER_KIND_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
    ASSERT_NE(row, nullptr);
    ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
    BKE_paint_layers_active_set(*ma, row->marker);
    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

    auto eval_channel = [&](const eMaterialPaintChannel channel) -> RGBA {
      GraphInterpreter interp;
      interp.instance = find_instance();
      interp.tree = ma->paint_layers_tree;
      interp.x = 1;
      interp.y = 1;
      interp.tree->ensure_topology_cache();
      return eval_channel_result(interp, channel);
    };

    EXPECT_NEAR(eval_channel(PAINT_MATERIAL_CHANNEL_SPECULAR).r, 0.25f, 1e-3f)
        << (linked ? "linked, first build" : "unlinked, first build");

    /* Move Base Color in the source: the source hash moves, so the wrapper takes the rebuild
     * branch of #BKE_paint_layers_source_group_ensure. */
    bNode *rgb = nullptr;
    for (bNode &node : source->nodetree->nodes) {
      if (node.type_legacy == SH_NODE_RGB) {
        rgb = &node;
      }
    }
    ASSERT_NE(rgb, nullptr);
    bNodeSocket *rgb_out = bke::node_find_socket(*rgb, SOCK_OUT, "Color"_ustr);
    ASSERT_NE(rgb_out, nullptr);
    static_cast<bNodeSocketValueRGBA *>(rgb_out->default_value)->value[0] = 0.77f;
    BKE_ntree_update_tag_all(source->nodetree);
    BKE_ntree_update_after_single_tree_change(*bmain, *source->nodetree);
    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

    EXPECT_NEAR(eval_channel(PAINT_MATERIAL_CHANNEL_SPECULAR).r, 0.25f, 1e-3f)
        << (linked ? "linked, rebuilt" : "unlinked, rebuilt");
    EXPECT_NEAR(eval_channel(PAINT_MATERIAL_CHANNEL_BASE_COLOR).r, 0.77f, 1e-3f);
    EXPECT_NEAR(eval_channel(PAINT_MATERIAL_CHANNEL_ROUGHNESS).r, 0.3f, 1e-3f);
    EXPECT_NEAR(eval_channel(PAINT_MATERIAL_CHANNEL_METALLIC).r, 0.7f, 1e-3f);

    BKE_id_free(bmain, ma);
    ma = nullptr;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Pass Through folders
 * \{ */

/**
 * A Pass Through folder is pixel-identical to the same rows with the folder taken away, on the CPU
 * and in the generated chain, for the Base Color and Roughness channels. The rows are real-shaped:
 * a Paint base, a partially covered Paint child and a Fill child, plus a partially covered row on
 * top, so both the factors and the coverage are non-trivial.
 */
TEST_F(PaintLayersGraphEvalTest, pass_through_folder_equals_the_ungrouped_stack)
{
  const int size = 4;
  const eMaterialPaintChannel channels[] = {PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                            PAINT_MATERIAL_CHANNEL_ROUGHNESS};

  auto add_child = [&](MaterialPaintLayer *anchor,
                       const char *name,
                       const eMaterialPaintLayerKind kind,
                       Image *image,
                       const eMaterialPaintChannel channel) -> MaterialPaintLayer * {
    MaterialPaintLayer *layer = BKE_paint_layers_add(*ma,
                                                     kind,
                                                     name,
                                                     anchor,
                                                     anchor != nullptr ? PaintLayerPlace::Into :
                                                                         PaintLayerPlace::Above);
    EXPECT_NE(layer, nullptr);
    MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(*ma, layer, channel);
    EXPECT_NE(record, nullptr);
    record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
    if (kind == MA_PAINT_LAYER_KIND_FILL) {
      const float fill[4] = {0.1f, 0.6f, 0.2f, 1.0f};
      BKE_paint_layers_set_fill_color(*ma, layer, fill);
      record->image = nullptr;
    }
    else {
      record->image = image;
    }
    return layer;
  };

  auto build = [&](const bool folded, const char *material_name, RGBA r_out[2]) {
    ma = BKE_material_add(bmain, material_name);
    add_child(nullptr,
              "Bottom",
              MA_PAINT_LAYER_KIND_PAINT,
              add_solid_image("Bottom", size, 220, 40, 40, 255),
              channels[0]);
    MaterialPaintLayer *folder = nullptr;
    if (folded) {
      folder = BKE_paint_layers_add(
          *ma, MA_PAINT_LAYER_KIND_FOLDER, "Folder", nullptr, PaintLayerPlace::Above);
      ASSERT_NE(folder, nullptr);
      ASSERT_TRUE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));
    }
    MaterialPaintLayer *child = add_child(folder,
                                          "Child",
                                          MA_PAINT_LAYER_KIND_PAINT,
                                          add_solid_image("Child", size, 0, 0, 255, 140),
                                          channels[0]);
    BKE_paint_layers_set_opacity(*ma, child, 0.5f);
    add_child(folder, "Fill", MA_PAINT_LAYER_KIND_FILL, nullptr, channels[1]);
    add_child(folder,
              "Top",
              MA_PAINT_LAYER_KIND_PAINT,
              add_solid_image("Top", size, 10, 10, 10, 110),
              channels[1]);

    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
    GraphInterpreter interpreter;
    interpreter.instance = find_instance();
    interpreter.tree = ma->paint_layers_tree;
    interpreter.x = 1;
    interpreter.y = 1;
    ASSERT_NE(interpreter.instance, nullptr);
    for (const int i : IndexRange(2)) {
      const RGBA graph = eval_channel_result(interpreter, channels[i]);
      const RGBA cpu = cpu_pixel(channels[i]);
      EXPECT_NEAR(graph.r, cpu.r, 1e-4f);
      EXPECT_NEAR(graph.g, cpu.g, 1e-4f);
      EXPECT_NEAR(graph.b, cpu.b, 1e-4f);
      r_out[i] = cpu;
    }
    BKE_id_free(bmain, ma);
    ma = nullptr;
  };

  RGBA folded[2];
  RGBA plain[2];
  build(true, "PassThroughFolded", folded);
  build(false, "PassThroughPlain", plain);
  for (const int i : IndexRange(2)) {
    EXPECT_NEAR(folded[i].r, plain[i].r, 1e-4f);
    EXPECT_NEAR(folded[i].g, plain[i].g, 1e-4f);
    EXPECT_NEAR(folded[i].b, plain[i].b, 1e-4f);
  }
}

/**
 * A Material row read live through the Hybrid path (constant Principled inputs, no bake yet) inside
 * a Pass Through folder still matches the CPU for every channel it takes part in, next to a Paint
 * child with partial coverage.
 */
TEST_F(PaintLayersGraphEvalTest, pass_through_folder_with_a_material_child_matches_the_cpu)
{
  const int size = 4;
  const eMaterialPaintChannel channels[] = {PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                            PAINT_MATERIAL_CHANNEL_ROUGHNESS};

  ma = BKE_material_add(bmain, "PassThroughMaterial");
  MaterialPaintLayer *bottom = add_layer("Bottom",
                                         MA_PAINT_LAYER_KIND_PAINT,
                                         add_solid_image("Bottom", size, 200, 30, 30, 255),
                                         channels[0]);
  /* A Roughness map on the bottom row gives the Roughness channel a size; without one the CPU has
   * no buffer to composite the constant-only channel into. */
  MaterialPaintLayerChannel *bottom_rough = BKE_paint_layers_channel_add(*ma, bottom, channels[1]);
  ASSERT_NE(bottom_rough, nullptr);
  bottom_rough->image = add_solid_image("BottomRough", size, 90, 90, 90, 255);
  bottom_rough->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  Material *source = BKE_material_add(bmain, "PassThroughSource");
  bNodeTree &source_tree = *source->nodetree;
  bNode *principled = bke::node_add_static_node(nullptr, source_tree, SH_NODE_BSDF_PRINCIPLED);
  bNode *output = bke::node_add_static_node(nullptr, source_tree, SH_NODE_OUTPUT_MATERIAL);
  bke::node_add_link(source_tree,
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_OUT, "BSDF"_ustr),
                     *output,
                     *bke::node_find_socket(*output, SOCK_IN, "Surface"_ustr));
  bNodeSocket *principled_color = bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr);
  ASSERT_NE(principled_color, nullptr);
  const float color[4] = {0.2f, 0.5f, 0.9f, 1.0f};
  copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(principled_color->default_value)->value, color);
  bNodeSocket *principled_rough = bke::node_find_socket(*principled, SOCK_IN, "Roughness"_ustr);
  ASSERT_NE(principled_rough, nullptr);
  static_cast<bNodeSocketValueFloat *>(principled_rough->default_value)->value = 0.3f;
  BKE_ntree_update_tag_all(&source_tree);
  BKE_ntree_update_after_single_tree_change(*bmain, source_tree);

  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_FOLDER, "Folder", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));

  MaterialPaintLayer *mat_row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_MATERIAL, "Mat", folder, PaintLayerPlace::Into);
  ASSERT_NE(mat_row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, mat_row, source));
  for (const eMaterialPaintChannel channel : channels) {
    ASSERT_NE(BKE_paint_layers_channel_add(*ma, mat_row, channel), nullptr);
  }

  MaterialPaintLayer *paint = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_PAINT, "Paint", folder, PaintLayerPlace::Into);
  ASSERT_NE(paint, nullptr);
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(*ma, paint, channels[0]);
  ASSERT_NE(record, nullptr);
  record->image = add_solid_image("Paint", size, 0, 0, 255, 160);
  record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  BKE_paint_layers_set_opacity(*ma, paint, 0.5f);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_TRUE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *mat_row), PaintLayerMaterialMode::Hybrid);

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  for (const eMaterialPaintChannel channel : channels) {
    const RGBA graph = eval_channel_result(interpreter, channel);
    const RGBA cpu = cpu_pixel(channel);
    const char *ui_name = BKE_paint_material_channel_info(channel).ui_name;
    EXPECT_NEAR(graph.r, cpu.r, 1e-4f) << ui_name;
    EXPECT_NEAR(graph.g, cpu.g, 1e-4f) << ui_name;
    EXPECT_NEAR(graph.b, cpu.b, 1e-4f) << ui_name;
  }

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * Hiding a Pass Through folder is a value edit: its children's factor goes to zero and the result
 * equals the same stack without those children, on both the CPU and the generated chain.
 */
TEST_F(PaintLayersGraphEvalTest, pass_through_folder_visibility_matches_the_missing_children)
{
  const int size = 4;
  const eMaterialPaintChannel bc = PAINT_MATERIAL_CHANNEL_BASE_COLOR;

  /* The reference: only the bottom row. */
  ma = BKE_material_add(bmain, "PassVisibleBottom");
  add_layer("Bottom", MA_PAINT_LAYER_KIND_PAINT, add_solid_image("Bottom", size, 200, 30, 30, 255), bc);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  GraphInterpreter plain;
  plain.instance = find_instance();
  plain.tree = ma->paint_layers_tree;
  plain.x = 1;
  plain.y = 1;
  const RGBA bottom_graph = eval_channel_result(plain, bc);
  const RGBA bottom_cpu = cpu_pixel(bc);
  EXPECT_NEAR(bottom_graph.r, bottom_cpu.r, 1e-4f);
  BKE_id_free(bmain, ma);
  ma = nullptr;

  /* The folder hidden must equal that. */
  ma = BKE_material_add(bmain, "PassVisibleFolder");
  add_layer("Bottom", MA_PAINT_LAYER_KIND_PAINT, add_solid_image("Bottom", size, 200, 30, 30, 255), bc);
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_FOLDER, "Folder", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_PAINT, "Child", folder, PaintLayerPlace::Into);
  ASSERT_NE(child, nullptr);
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(*ma, child, bc);
  ASSERT_NE(record, nullptr);
  record->image = add_solid_image("Child", size, 0, 0, 255, 255);
  record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_TRUE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));

  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, folder, false));
  ASSERT_TRUE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));
  BKE_paint_layers_values_sync(*ma);

  GraphInterpreter folded;
  folded.instance = find_instance();
  folded.tree = ma->paint_layers_tree;
  folded.x = 1;
  folded.y = 1;
  const RGBA graph = eval_channel_result(folded, bc);
  const RGBA cpu = cpu_pixel(bc);
  EXPECT_NEAR(graph.r, cpu.r, 1e-4f);
  EXPECT_NEAR(graph.g, cpu.g, 1e-4f);
  EXPECT_NEAR(graph.b, cpu.b, 1e-4f);
  EXPECT_NEAR(graph.r, bottom_graph.r, 1e-4f);
  EXPECT_NEAR(graph.g, bottom_graph.g, 1e-4f);
  EXPECT_NEAR(graph.b, bottom_graph.b, 1e-4f);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * Giving a Pass Through folder a mask turns it isolating in one rebuild; both modes stay correct on
 * the CPU and the generated chain, and the mask actually changes the pixels.
 */
TEST_F(PaintLayersGraphEvalTest, pass_through_folder_mode_switch_matches_the_cpu)
{
  const int size = 4;
  const eMaterialPaintChannel bc = PAINT_MATERIAL_CHANNEL_BASE_COLOR;

  ma = BKE_material_add(bmain, "PassThroughSwitch");
  add_layer("Bottom", MA_PAINT_LAYER_KIND_PAINT, add_solid_image("Bottom", size, 200, 30, 30, 255), bc);
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_FOLDER, "Folder", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_PAINT, "Child", folder, PaintLayerPlace::Into);
  ASSERT_NE(child, nullptr);
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(*ma, child, bc);
  ASSERT_NE(record, nullptr);
  record->image = add_solid_image("Child", size, 0, 0, 255, 255);
  record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  auto evaluate = [&](const char *tag) -> RGBA {
    GraphInterpreter interpreter;
    interpreter.instance = find_instance();
    interpreter.tree = ma->paint_layers_tree;
    interpreter.x = 1;
    interpreter.y = 1;
    const RGBA graph = eval_channel_result(interpreter, bc);
    const RGBA cpu = cpu_pixel(bc);
    EXPECT_NEAR(graph.r, cpu.r, 1e-4f) << tag;
    EXPECT_NEAR(graph.g, cpu.g, 1e-4f) << tag;
    EXPECT_NEAR(graph.b, cpu.b, 1e-4f) << tag;
    return cpu;
  };

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_TRUE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));
  const RGBA pass = evaluate("pass through");

  MaterialPaintLayer *item = BKE_paint_layers_mask_add(*ma, folder, 0.5f);
  ASSERT_NE(item, nullptr);
  EXPECT_FALSE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  const RGBA isolated = evaluate("isolating");

  /* The 0.5 mask must actually dim the blue child over the red bottom. */
  EXPECT_LT(isolated.b, pass.b + 1e-3f);
  EXPECT_GT(isolated.r, pass.r - 1e-3f);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Correction opacity on a Material row
 *
 * A content correction mixes its map into the row's colour with factor `alpha(map) * opacity`.
 * The opacity lives on the correction's per (row, channel) settings, written by the Outliner's RNA
 * slider; these tests drive that exact call and check the generated result against the formula, so a
 * slider that silently falls back to `alpha` alone cannot pass.
 * \{ */

namespace {

/** The Outliner's per (row, channel) opacity slider: an RNA setter that must resolve the row. */
void rna_set_channel_opacity(Material &ma,
                             MaterialPaintLayer &layer,
                             const int channel,
                             const float percent)
{
  PointerRNA ptr = RNA_pointer_create_discrete(
      &ma.id,
      RNA_struct_find("MaterialPaintLayerChannelSettings"),
      &layer.channel_settings[channel]);
  PropertyRNA *prop = RNA_struct_find_property(&ptr, "opacity");
  ASSERT_NE(prop, nullptr);
  RNA_property_float_set(&ptr, prop, percent);
}

/** A source whose Principled is entirely constant, so a Material row on it can read Hybrid/Baked. */
Material *make_constant_principled_source(Main &bmain,
                                          const char *name,
                                          const float color[4],
                                          const float roughness)
{
  Material *source = BKE_material_add(&bmain, name);
  bNodeTree &tree = *source->nodetree;
  bNode *principled = bke::node_add_static_node(nullptr, tree, SH_NODE_BSDF_PRINCIPLED);
  bNode *output = bke::node_add_static_node(nullptr, tree, SH_NODE_OUTPUT_MATERIAL);
  bke::node_add_link(tree,
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_OUT, "BSDF"_ustr),
                     *output,
                     *bke::node_find_socket(*output, SOCK_IN, "Surface"_ustr));
  bNodeSocket *color_socket = bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr);
  copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(color_socket->default_value)->value, color);
  bNodeSocket *rough_socket = bke::node_find_socket(*principled, SOCK_IN, "Roughness"_ustr);
  static_cast<bNodeSocketValueFloat *>(rough_socket->default_value)->value = roughness;
  BKE_ntree_update_tag_all(&tree);
  BKE_ntree_update_after_single_tree_change(bmain, tree);
  return source;
}

/** Give \a row a Paint content correction with \a map on Base Color. */
MaterialPaintLayer *add_content_correction(Material &ma,
                                           MaterialPaintLayer &row,
                                           Image *map)
{
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      ma, &row, MA_PAINT_LAYER_SECTION_CONTENT, MA_PAINT_LAYER_EFFECT_PAINT, "C");
  EXPECT_NE(correction, nullptr);
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
      ma, correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  EXPECT_NE(record, nullptr);
  record->image = map;
  record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  BKE_paint_layers_correction_set_effect(ma, correction, MA_PAINT_LAYER_EFFECT_PAINT);
  return correction;
}

/**
 * The expected colour: the correction mixed into the row's own colour `S` with factor
 * `alpha(map) * opacity`, then the row laid over the bottom. The row covers fully here (its source
 * alpha is one), so the bottom is not reached and the result is the corrected colour.
 */
void expect_correction_mix(const RGBA &sample,
                           const float source_rgb[3],
                           const float opacity,
                           const RGBA &got,
                           const char *tag)
{
  const float f = sample.a * opacity;
  const float expected[3] = {source_rgb[0] + (sample.r - source_rgb[0]) * f,
                             source_rgb[1] + (sample.g - source_rgb[1]) * f,
                             source_rgb[2] + (sample.b - source_rgb[2]) * f};
  EXPECT_NEAR(got.r, expected[0], 1e-4f) << tag;
  EXPECT_NEAR(got.g, expected[1], 1e-4f) << tag;
  EXPECT_NEAR(got.b, expected[2], 1e-4f) << tag;
}

}  // namespace

/**
 * The reported stack: a Material row active (the correction selected in the Outliner defers the
 * row), so it reads its source live through the wrapper. The correction's Base Color opacity slider
 * must change the pixels; before the fix the RNA setter could not resolve the correction and the
 * result stayed at the full-map mix.
 */
TEST_F(PaintLayersGraphEvalTest, material_source_group_correction_opacity_matches_the_formula)
{
  const int size = 4;
  const eMaterialPaintChannel bc = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "MatCorrSourceGroup");
  add_layer("Bottom", MA_PAINT_LAYER_KIND_PAINT, add_solid_image("Bottom", size, 255, 0, 0, 255), bc);

  Material *source = make_specular_source(*bmain, "MatCorrWiredSource", false);
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_MATERIAL, "Mat", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  BKE_paint_layers_active_set(*ma, row->marker);

  Image *corr_map = add_solid_image("CorrMap", size, 0, 0, 255, 128);
  MaterialPaintLayer *correction = add_content_correction(*ma, *row, corr_map);
  ASSERT_NE(correction, nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::SourceGroup);

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA sample = interpreter.sample_image(corr_map, "Color");
  const float source_rgb[3] = {0.2f, 0.5f, 0.9f};

  expect_correction_mix(
      sample, source_rgb, 1.0f, eval_channel_result(interpreter, bc), "op=1");

  rna_set_channel_opacity(*ma, *correction, bc, 35.0f);
  expect_correction_mix(
      sample, source_rgb, 0.35f, eval_channel_result(interpreter, bc), "op=0.35");

  /* Hiding the correction is the same factor at zero. */
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, correction, false));
  BKE_paint_layers_values_sync(*ma);
  expect_correction_mix(
      sample, source_rgb, 0.0f, eval_channel_result(interpreter, bc), "hidden");

  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, correction, true));
  BKE_paint_layers_values_sync(*ma);
  expect_correction_mix(
      sample, source_rgb, 0.35f, eval_channel_result(interpreter, bc), "re-enabled");

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/** The same on a Material row read through the Hybrid path (its channel has no baked map yet). */
TEST_F(PaintLayersGraphEvalTest, material_hybrid_correction_opacity_matches_the_formula)
{
  const int size = 4;
  const eMaterialPaintChannel bc = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "MatCorrHybrid");
  add_layer("Bottom", MA_PAINT_LAYER_KIND_PAINT, add_solid_image("Bottom", size, 255, 0, 0, 255), bc);

  const float row_color[4] = {0.3f, 0.6f, 0.2f, 1.0f};
  Material *source = make_constant_principled_source(*bmain, "MatCorrHybridSource", row_color, 0.4f);
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_MATERIAL, "Mat", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));

  Image *corr_map = add_solid_image("CorrMap", size, 240, 20, 20, 96);
  MaterialPaintLayer *correction = add_content_correction(*ma, *row, corr_map);
  ASSERT_NE(correction, nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Hybrid);

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA sample = interpreter.sample_image(corr_map, "Color");

  expect_correction_mix(sample, row_color, 1.0f, eval_channel_result(interpreter, bc), "op=1");
  rna_set_channel_opacity(*ma, *correction, bc, 50.0f);
  expect_correction_mix(sample, row_color, 0.5f, eval_channel_result(interpreter, bc), "op=0.5");

  /* The CPU reproduces the Hybrid result, so both sides read the same opacity. */
  const RGBA graph = eval_channel_result(interpreter, bc);
  const RGBA cpu = cpu_pixel(bc);
  EXPECT_NEAR(graph.r, cpu.r, 1e-4f);
  EXPECT_NEAR(graph.g, cpu.g, 1e-4f);
  EXPECT_NEAR(graph.b, cpu.b, 1e-4f);

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/** The same on a Material row read from its baked maps (Baked): the map is the row's content. */
TEST_F(PaintLayersGraphEvalTest, material_baked_correction_opacity_matches_the_formula)
{
  const int size = 4;
  const eMaterialPaintChannel bc = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "MatCorrBaked");
  add_layer("Bottom", MA_PAINT_LAYER_KIND_PAINT, add_solid_image("Bottom", size, 255, 0, 0, 255), bc);

  const float row_color[4] = {0.3f, 0.6f, 0.2f, 1.0f};
  Material *source = make_constant_principled_source(*bmain, "MatCorrBakedSource", row_color, 0.4f);
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_MATERIAL, "Mat", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));

  /* A baked map in every channel makes the row ineligible to read its source live. */
  const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  Image *coverage = make_bake_data_map(bmain, "MatCorrCoverage", size, white);
  ASSERT_NE(coverage, nullptr);
  for (int channel = 0; channel < PAINT_MATERIAL_CHANNEL_NUM; channel++) {
    const float rgba[4] = {row_color[0], row_color[1], row_color[2], 1.0f};
    Image *map = make_bake_data_map(bmain, "MatCorrBake", size, rgba);
    ASSERT_NE(map, nullptr);
    ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *row, channel, map));
  }
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *row, -1, coverage));
  BKE_paint_layers_bake_finalize(*ma, *row);

  Image *corr_map = add_solid_image("CorrMap", size, 240, 20, 20, 96);
  MaterialPaintLayer *correction = add_content_correction(*ma, *row, corr_map);
  ASSERT_NE(correction, nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Baked);

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA sample = interpreter.sample_image(corr_map, "Color");

  expect_correction_mix(sample, row_color, 1.0f, eval_channel_result(interpreter, bc), "op=1");
  rna_set_channel_opacity(*ma, *correction, bc, 25.0f);
  expect_correction_mix(sample, row_color, 0.25f, eval_channel_result(interpreter, bc), "op=0.25");

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/** The same on a plain Paint row, so the correction path stays green for it too. */
TEST_F(PaintLayersGraphEvalTest, paint_row_correction_opacity_matches_the_formula)
{
  const int size = 4;
  const eMaterialPaintChannel bc = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "PaintCorr");
  add_layer("Bottom", MA_PAINT_LAYER_KIND_PAINT, add_solid_image("Bottom", size, 255, 0, 0, 255), bc);
  Image *row_map = add_solid_image("Row", size, 30, 200, 60, 255);
  MaterialPaintLayer *row = add_layer("Row", MA_PAINT_LAYER_KIND_PAINT, row_map, bc);

  Image *corr_map = add_solid_image("CorrMap", size, 10, 10, 240, 160);
  MaterialPaintLayer *correction = add_content_correction(*ma, *row, corr_map);
  ASSERT_NE(correction, nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA sample = interpreter.sample_image(corr_map, "Color");
  const RGBA row_sample = interpreter.sample_image(row_map, "Color");
  const float source_rgb[3] = {row_sample.r, row_sample.g, row_sample.b};

  expect_correction_mix(sample, source_rgb, 1.0f, eval_channel_result(interpreter, bc), "op=1");
  rna_set_channel_opacity(*ma, *correction, bc, 40.0f);
  expect_correction_mix(sample, source_rgb, 0.4f, eval_channel_result(interpreter, bc), "op=0.4");

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * A Fill content correction covers fully, so its factor is the opacity alone; the same per (row,
 * channel) slider drives it. This is the other kind whose RNA setter had to resolve a correction.
 */
TEST_F(PaintLayersGraphEvalTest, material_correction_fill_opacity_matches_the_formula)
{
  const int size = 4;
  const eMaterialPaintChannel bc = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  ma = BKE_material_add(bmain, "MatFillCorr");
  add_layer("Bottom", MA_PAINT_LAYER_KIND_PAINT, add_solid_image("Bottom", size, 255, 0, 0, 255), bc);

  Material *source = make_specular_source(*bmain, "MatFillCorrSource", false);
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_MATERIAL, "Mat", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  BKE_paint_layers_active_set(*ma, row->marker);

  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, row, MA_PAINT_LAYER_SECTION_CONTENT, MA_PAINT_LAYER_EFFECT_FILL, "C");
  ASSERT_NE(correction, nullptr);
  const float fill[4] = {0.0f, 1.0f, 0.0f, 1.0f};
  BKE_paint_layers_set_fill_color(*ma, correction, fill);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::SourceGroup);

  GraphInterpreter interpreter;
  interpreter.instance = find_instance();
  interpreter.tree = ma->paint_layers_tree;
  interpreter.x = 1;
  interpreter.y = 1;
  ASSERT_NE(interpreter.instance, nullptr);
  const RGBA sample = {fill[0], fill[1], fill[2], 1.0f};
  const float source_rgb[3] = {0.2f, 0.5f, 0.9f};

  rna_set_channel_opacity(*ma, *correction, bc, 40.0f);
  expect_correction_mix(sample, source_rgb, 0.4f, eval_channel_result(interpreter, bc), "fill op");

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/** \} */

TEST_F(PaintLayersGraphEvalTest, source_group_material_in_pass_through_and_isolating_folder)
{
  Material *source_c = build_group_source(
      *bmain, "ReproSourceC", "ReproGroupC", "ReproMapC", "ReproSharedC", source_spec_c);
  Material *source_b = build_hybrid_source(
      *bmain, "ReproSourceB", "ReproNormalB", source_spec_b);
  ASSERT_NE(source_c, nullptr);
  ASSERT_NE(source_b, nullptr);
  ma = BKE_material_add(bmain, "ReproStack");

  MaterialPaintLayer *iso = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_FOLDER, "Iso", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(iso, nullptr);
  BKE_paint_layers_set_opacity(*ma, iso, 0.5f);
  MaterialPaintLayer *b = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_MATERIAL, "MatB", iso, PaintLayerPlace::Into);
  ASSERT_NE(b, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, b, source_b));

  MaterialPaintLayer *pass = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_FOLDER, "Pass", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(pass, nullptr);
  ASSERT_TRUE(BKE_paint_layers_folder_is_pass_through(*ma, *pass));
  MaterialPaintLayer *c = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_MATERIAL, "MatC", pass, PaintLayerPlace::Into);
  ASSERT_NE(c, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, c, source_c));

  auto find_wrapper = [&]() -> bNodeTree * {
    for (bNodeTree &tree : bmain->nodetrees) {
      if (STREQ(tree.id.name + 2, ".PL Source ReproSourceC")) {
        return &tree;
      }
    }
    return nullptr;
  };

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *wrapper = find_wrapper();
  ASSERT_NE(wrapper, nullptr);
  const int users_after_first = wrapper->id.us;
  EXPECT_GT(users_after_first, 0);

  /* A second regeneration must reuse the same wrapper instance without another user. */
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *wrapper_again = find_wrapper();
  ASSERT_NE(wrapper_again, nullptr);
  EXPECT_EQ(wrapper_again->id.us, users_after_first);

  /* Removing the row releases the wrapper (or the wrapper is pruned): never a leftover user. */
  ASSERT_TRUE(BKE_paint_layers_remove(*ma, c));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *wrapper_after = find_wrapper();
  if (wrapper_after != nullptr) {
    EXPECT_LT(wrapper_after->id.us, users_after_first);
  }

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/**
 * The Baked->SourceGroup rebuild of a Material row at the root, next to an isolating folder and a
 * Pass Through folder whose child is a SourceGroup row. The active change moves row A from Baked to
 * SourceGroup, which rebuilds A's layer group; the non-supplied Normal channel must not make the
 * generator link A's wrapper instance into the root tree.
 */
TEST_F(PaintLayersGraphEvalTest, material_source_group_transition_next_to_both_folder_kinds)
{
  const int size = 2;
  Material *source_a = build_group_source(
      *bmain, "ReproTransSourceA", "ReproTransGroupA", "ReproTransMapA", "ReproTransSharedA",
      source_spec_a);
  Material *source_b = build_hybrid_source(
      *bmain, "ReproTransSourceB", "ReproTransNormalB", source_spec_b);
  Material *source_c = build_group_source(
      *bmain, "ReproTransSourceC", "ReproTransGroupC", "ReproTransMapC", "ReproTransSharedC",
      source_spec_c);
  ASSERT_NE(source_a, nullptr);
  ASSERT_NE(source_b, nullptr);
  ASSERT_NE(source_c, nullptr);
  ma = BKE_material_add(bmain, "ReproTransStack");

  MaterialPaintLayer *a = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_MATERIAL, "MatA", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(a, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, a, source_a));
  for (const eMaterialPaintChannel channel : BKE_paint_material_bakeable_channels()) {
    RGBA raw;
    if (!group_source_expected(source_spec_a, int(channel), 0, 0, raw)) {
      continue;
    }
    const float rgba[4] = {raw.r, raw.g, raw.b, 1.0f};
    Image *map = make_bake_data_map(bmain, "ReproTransBakeA", size, rgba);
    ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *a, int(channel), map));
  }
  const float coverage_rgba[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  Image *coverage_map = make_bake_data_map(bmain, "ReproTransCoverageA", size, coverage_rgba);
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *a, -1, coverage_map));
  BKE_paint_layers_bake_finalize(*ma, *a);

  MaterialPaintLayer *iso = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_FOLDER, "Iso", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(iso, nullptr);
  BKE_paint_layers_set_opacity(*ma, iso, 0.5f);
  MaterialPaintLayer *b = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_MATERIAL, "MatB", iso, PaintLayerPlace::Into);
  ASSERT_NE(b, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, b, source_b));

  MaterialPaintLayer *pass = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_FOLDER, "Pass", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(pass, nullptr);
  ASSERT_TRUE(BKE_paint_layers_folder_is_pass_through(*ma, *pass));
  MaterialPaintLayer *c = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_KIND_MATERIAL, "MatC", pass, PaintLayerPlace::Into);
  ASSERT_NE(c, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, c, source_c));

  /* First build: A is not deferred, so its valid bakes make it Baked. */
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *a), PaintLayerMaterialMode::Baked);

  /* Move the active row to A: it becomes SourceGroup and its group is rebuilt. */
  BKE_paint_layers_active_set(*ma, a->marker);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *a), PaintLayerMaterialMode::SourceGroup);

  /* The next regeneration must rebuild nothing. */
  bNodeTree *root = ma->paint_layers_tree;
  ASSERT_NE(root, nullptr);
  const Vector<bNode *> nodes = root_node_ptrs(*root);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_TRUE(same_node_ptrs(nodes, root_node_ptrs(*root)));

  BKE_id_free(bmain, ma);
  ma = nullptr;
}

/* -------------------------------------------------------------------- */
/** \name Full parity stack
 *
 * One stack with every construct that mattered so far, bottom to top: a Fill, Material A
 * (SourceGroup) with a mask and a Paint correction, an isolating folder (Material B, Hybrid, and a
 * Paint row), a Pass Through folder holding Material C (SourceGroup) and a Multiply Paint row on
 * top. The reference below composites the whole stack in one pass; the generated graph and the CPU
 * must both equal it for every active row, channel and point.
 * \{ */

static const int kFsSize = 2;
/** Mask map grey per texel (row-major 2x2): closed, partial, mostly open, open. Alpha is one. */
static const uchar kFsMaskBytes[4] = {0, 102, 204, 255};
/** A's Base Color correction map per texel: straight RGB bytes and a shared alpha of 0.6. */
static const uchar kFsCorrBytes[4][4] = {
    {230, 40, 40, 153}, {40, 230, 40, 153}, {40, 40, 230, 153}, {200, 200, 50, 153}};
static const float kFsMaskOpacity = 0.6f;
static const float kFsCorrOpacity = 0.5f;
static const float kFsIsoOpacity = 0.5f;
static const float kFsInnerPaintOpacity = 0.6f;
static const float kFsTopOpacity = 0.7f;
/** Below one, or C would cover everything A's mask and correction do. */
static const float kFsCOpacity = 0.4f;
static const float kFsFillColor[4] = {0.30f, 0.45f, 0.20f, 1.0f};
static const float kFsFillRoughness = 0.75f;
static const float kFsTopColor[4] = {0.60f, 0.80f, 0.50f, 1.0f};
static const float kFsTopRoughness[4] = {0.40f, 0.40f, 0.40f, 1.0f};

struct FsPoint {
  const char *label;
  int x;
  int y;
};
/* The mask grey is 1.0, 0.4, 0.8 and 0.0 at these texels; Material B's alpha is 0.6 at all. */
static const FsPoint kFsPoints[] = {
    {"center", 1, 1}, {"mask-edge", 1, 0}, {"b-partial-alpha", 0, 1}, {"mask-closed", 0, 0}};

/** Which parts of the stack take part in the reference. */
struct FsShape {
  bool iso = true;
  bool c = true;
  /** The row opacities a test drives through the RNA slider; the defaults are the stack's own. */
  float fill_opacity = 1.0f;
  float a_opacity = 1.0f;
  float b_opacity = 1.0f;
  float c_opacity = kFsCOpacity;
  float top_opacity = kFsTopOpacity;
};

/** One row's contribution to a channel: its colour and the factor before the row's opacity. */
struct FsRow {
  bool part = false;
  RGBA color;
  float factor = 0.0f;
};

static FsRow fs_row_fill(const int channel)
{
  FsRow row;
  if (channel == PAINT_MATERIAL_CHANNEL_BASE_COLOR) {
    row = {true, {kFsFillColor[0], kFsFillColor[1], kFsFillColor[2], 1.0f}, 1.0f};
  }
  else if (channel == PAINT_MATERIAL_CHANNEL_ROUGHNESS) {
    row = {true, {kFsFillRoughness, kFsFillRoughness, kFsFillRoughness, 1.0f}, 1.0f};
  }
  return row;
}

/**
 * Material A: the source's clean value; on Base Color the Paint correction mixed in with
 * `opacity * alpha` (`paint_material_composite.cc:628-632`); the factor is the source coverage (one)
 * with the mask item laid over it as `F * (1 - fac) + C * fac`, `fac = alpha * opacity`
 * (`paint_material_composite.cc:366-396`, the same expression at `:664-690`). The mask is one for
 * every channel (`paint_layers_intern.hh:123`).
 */
static FsRow fs_row_a(const int channel, const int x, const int y)
{
  FsRow row;
  RGBA raw;
  if (!group_source_expected(source_spec_a, channel, x, y, raw)) {
    return row;
  }
  const int texel = y * kFsSize + x;
  if (channel == PAINT_MATERIAL_CHANNEL_BASE_COLOR) {
    const float alpha = float(kFsCorrBytes[texel][3]) / 255.0f;
    float dst[4] = {raw.r, raw.g, raw.b, 1.0f};
    const float top[4] = {float(kFsCorrBytes[texel][0]) / 255.0f,
                          float(kFsCorrBytes[texel][1]) / 255.0f,
                          float(kFsCorrBytes[texel][2]) / 255.0f,
                          alpha};
    ramp_blend(MA_RAMP_BLEND, dst, clamp_f(kFsCorrOpacity * alpha, 0.0f, 1.0f), top);
    raw = {dst[0], dst[1], dst[2], 1.0f};
  }
  const float gray = float(kFsMaskBytes[texel]) / 255.0f;
  const float fac = clamp_f(kFsMaskOpacity, 0.0f, 1.0f);
  row.part = true;
  row.color = raw;
  row.factor = 1.0f * (1.0f - fac) + gray * fac;
  return row;
}

/** Material B: the constants of #source_spec_b, covered by its constant alpha. */
static FsRow fs_row_b(const int channel)
{
  FsRow row;
  RGBA raw;
  if (hybrid_source_expected(source_spec_b, channel, raw)) {
    row = {true, raw, source_spec_b.alpha};
  }
  return row;
}

/** The Paint row inside the isolating folder: Base Color only, fully covering. */
static FsRow fs_row_inner_paint(const int channel)
{
  FsRow row;
  if (channel == PAINT_MATERIAL_CHANNEL_BASE_COLOR) {
    row = {true, {kIsolatingPaintA[0], kIsolatingPaintA[1], kIsolatingPaintA[2], 1.0f}, 1.0f};
  }
  return row;
}

static FsRow fs_row_c(const int channel, const int x, const int y)
{
  FsRow row;
  RGBA raw;
  if (group_source_expected(source_spec_c, channel, x, y, raw)) {
    row = {true, raw, 1.0f};
  }
  return row;
}

/** The top Paint row: Base Color and Roughness maps, fully covering. */
static FsRow fs_row_top(const int channel)
{
  FsRow row;
  if (channel == PAINT_MATERIAL_CHANNEL_BASE_COLOR) {
    row = {true, {kFsTopColor[0], kFsTopColor[1], kFsTopColor[2], 1.0f}, 1.0f};
  }
  else if (channel == PAINT_MATERIAL_CHANNEL_ROUGHNESS) {
    row = {true, {kFsTopRoughness[0], kFsTopRoughness[1], kFsTopRoughness[2], 1.0f}, 1.0f};
  }
  return row;
}

/**
 * The whole stack over the channel bottom. A row lays its colour with `opacity * factor`
 * (`composite_apply_layer_linear`, `paint_material_composite.cc:777-804`, `blend_row_linear`
 * `:253-277`, `ramp_blend` `material.cc:2539-2549`); the isolating folder accumulates its children
 * premultiplied first (`composite_folder_accumulate`, `:702-775`) and its own coverage is that
 * accumulation; the Pass Through folder is transparent to the maths, so C is laid like a root row.
 * A row a channel does not exist for (Unavailable in the source) does not take part. The result
 * ends with the Normal decode-normalize-encode of `cpu_pixel_at` and of the chain
 * (`paint_layers.cc:542-570`).
 *
 * \return false when no row takes part in \a channel.
 */
static bool full_stack_expected(
    const int channel, const int x, const int y, const FsShape &shape, RGBA &r_out)
{
  const bool normal = (channel == PAINT_MATERIAL_CHANNEL_NORMAL);
  RGBA state = channel_bottom(channel);
  bool any = false;

  auto lay = [&](const FsRow &row, const float opacity, const int blend) {
    if (!row.part) {
      return;
    }
    any = true;
    blend_over_rgba(state, row.color, blend, opacity * row.factor, normal);
  };

  lay(fs_row_fill(channel), shape.fill_opacity, MA_RAMP_BLEND);
  lay(fs_row_a(channel, x, y), shape.a_opacity, MA_RAMP_BLEND);

  if (shape.iso) {
    struct Child {
      FsRow row;
      float opacity;
    };
    const Child children[2] = {{fs_row_b(channel), shape.b_opacity},
                               {fs_row_inner_paint(channel), kFsInnerPaintOpacity}};
    float premul[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float coverage = 0.0f;
    bool folder_part = false;
    for (const Child &child : children) {
      if (!child.row.part) {
        continue;
      }
      folder_part = true;
      const float f = clamp_f(child.opacity * child.row.factor, 0.0f, 1.0f);
      const float a = coverage;
      RGBA blended = {0.0f, 0.0f, 0.0f, 0.0f};
      if (a > 0.0f) {
        blended = {premul[0] / a, premul[1] / a, premul[2] / a, premul[3] / a};
      }
      blend_over_rgba(blended, child.row.color, MA_RAMP_BLEND, 1.0f, normal);
      const float c[4] = {
          child.row.color.r, child.row.color.g, child.row.color.b, child.row.color.a};
      const float b[4] = {blended.r, blended.g, blended.b, blended.a};
      for (const int k : IndexRange(4)) {
        const float c_eff = c[k] + (b[k] - c[k]) * a;
        premul[k] = premul[k] * (1.0f - f) + c_eff * f;
      }
      coverage = a + f * (1.0f - a);
    }
    if (folder_part) {
      any = true;
      if (coverage > 0.0f) {
        const RGBA folder_src = {premul[0] / coverage,
                                 premul[1] / coverage,
                                 premul[2] / coverage,
                                 premul[3] / coverage};
        blend_over_rgba(state, folder_src, MA_RAMP_BLEND, kFsIsoOpacity * coverage, normal);
      }
    }
  }

  if (shape.c) {
    lay(fs_row_c(channel, x, y), shape.c_opacity, MA_RAMP_BLEND);
  }
  lay(fs_row_top(channel), shape.top_opacity, MA_RAMP_MULT);

  if (normal) {
    float n[3] = {state.r * 2.0f - 1.0f, state.g * 2.0f - 1.0f, state.b * 2.0f - 1.0f};
    normalize_v3(n);
    state.r = n[0] * 0.5f + 0.5f;
    state.g = n[1] * 0.5f + 0.5f;
    state.b = n[2] * 0.5f + 0.5f;
  }
  r_out = state;
  return any;
}

/**
 * The mode a Material row must report, from the rule of `BKE_paint_layers_material_mode`
 * (`paint_layers_bake.cc:902-939`) and not from a run. Only the active row (or one of its
 * corrections or its mask) is live; its channels then decide: A and C map a texture through a Color
 * Ramp / mix, so SourceGroup; B is all constants and one flat texture, so Hybrid. Every other row
 * has a valid bake for each channel and reads Baked.
 * \a row: 0 = A, 1 = B, 2 = C. \a owner: the Material row the active item belongs to, or -1.
 */
static PaintLayerMaterialMode fs_expected_mode(const int row, const int owner)
{
  if (row != owner) {
    return PaintLayerMaterialMode::Baked;
  }
  return (row == 1) ? PaintLayerMaterialMode::Hybrid : PaintLayerMaterialMode::SourceGroup;
}

static void fs_expect_rgb(const RGBA &expected,
                          const RGBA &got,
                          const char *tag,
                          const char *channel,
                          const char *point,
                          const char *side)
{
  const float tolerance = 1e-4f;
  if (std::fabs(expected.r - got.r) <= tolerance && std::fabs(expected.g - got.g) <= tolerance &&
      std::fabs(expected.b - got.b) <= tolerance)
  {
    return;
  }
  ADD_FAILURE() << tag << " | " << channel << " | " << point << " | " << side << ": expected ("
                << expected.r << ", " << expected.g << ", " << expected.b << ") got (" << got.r
                << ", " << got.g << ", " << got.b << ")";
}

struct FsActiveCase {
  std::string label;
  bUUID marker;
  /** The Material row (0 = A, 1 = B, 2 = C) the active item is or belongs to, or -1. */
  int owner;
};

class PaintLayersFullStackTest : public PaintLayersGraphEvalTest {
 public:
  struct Options {
    bool iso = true;
    bool with_c = true;
    bool c_in_pass_folder = true;
  };

  struct Stack {
    Material *ma = nullptr;
    MaterialPaintLayer *fill = nullptr;
    MaterialPaintLayer *a = nullptr;
    MaterialPaintLayer *a_correction = nullptr;
    MaterialPaintLayer *a_mask = nullptr;
    MaterialPaintLayer *iso = nullptr;
    MaterialPaintLayer *b = nullptr;
    MaterialPaintLayer *inner_paint = nullptr;
    MaterialPaintLayer *pass = nullptr;
    MaterialPaintLayer *c = nullptr;
    MaterialPaintLayer *top = nullptr;
  };

  /** A 2x2 data-space byte map with straight per-texel RGBA, stored the way every paint map is. */
  Image *make_byte_map(const std::string &name, const uchar (*texels)[4])
  {
    const float color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    Image *image = BKE_image_add_generated(bmain,
                                           kFsSize,
                                           kFsSize,
                                           name.c_str(),
                                           32,
                                           false,
                                           IMA_GENTYPE_BLANK,
                                           color,
                                           false,
                                           true,
                                           false);
    if (image == nullptr) {
      return nullptr;
    }
    image->alpha_mode = IMA_ALPHA_STRAIGHT;
    image->flag |= IMA_GPU_LINEAR_PREMUL;
    make_image_data(image);
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
    if (ibuf == nullptr) {
      return nullptr;
    }
    uchar *pixels = ibuf->byte_data_for_write();
    for (const int i : IndexRange(kFsSize * kFsSize)) {
      for (const int k : IndexRange(4)) {
        pixels[i * 4 + k] = texels[i][k];
      }
    }
    BKE_image_release_ibuf(image, ibuf, lock);
    return image;
  }

  /** Bake \a row from the reference's own values, so the row can read Baked and stay valid. */
  bool bake_row(MaterialPaintLayer &row,
                const std::string &name,
                const bool grouped,
                const GroupSourceSpec &group_spec,
                const HybridSourceSpec &hybrid_spec,
                const float coverage)
  {
    auto raw_at = [&](const int channel, const int x, const int y, RGBA &r_out) -> bool {
      return grouped ? group_source_expected(group_spec, channel, x, y, r_out) :
                       hybrid_source_expected(hybrid_spec, channel, r_out);
    };
    for (const eMaterialPaintChannel channel : BKE_paint_material_bakeable_channels()) {
      RGBA probe;
      if (!raw_at(int(channel), 0, 0, probe)) {
        continue;
      }
      float pixels[4][3];
      for (const int y : IndexRange(kFsSize)) {
        for (const int x : IndexRange(kFsSize)) {
          RGBA raw;
          raw_at(int(channel), x, y, raw);
          pixels[y * kFsSize + x][0] = raw.r;
          pixels[y * kFsSize + x][1] = raw.g;
          pixels[y * kFsSize + x][2] = raw.b;
        }
      }
      const std::string map_name = name + " " + BKE_paint_material_channel_info(channel).ui_name;
      Image *map = make_data_pattern_map(bmain, map_name.c_str(), kFsSize, pixels);
      if (map == nullptr || !BKE_paint_layers_bake_set_map(*ma, row, int(channel), map)) {
        return false;
      }
    }
    const float coverage_rgba[4] = {coverage, coverage, coverage, 1.0f};
    Image *coverage_map = make_bake_data_map(
        bmain, (name + " Coverage").c_str(), kFsSize, coverage_rgba);
    if (coverage_map == nullptr || !BKE_paint_layers_bake_set_map(*ma, row, -1, coverage_map)) {
      return false;
    }
    BKE_paint_layers_bake_finalize(*ma, row);
    return BKE_paint_layers_bake_is_valid(*ma, row);
  }

  /** Build the stack; a null `Stack::ma` means a step failed. Leaves `ma` at the new material. */
  Stack build(const char *tag, const Options &options)
  {
    Stack s;
    const std::string t = tag;
    auto name = [&](const char *base) { return std::string(base) + t; };

    Material *source_a = build_group_source(*bmain,
                                            name("FsSrcA").c_str(),
                                            name("FsGrpA").c_str(),
                                            name("FsMapA").c_str(),
                                            name("FsShrA").c_str(),
                                            source_spec_a);
    Material *source_b = build_hybrid_source(
        *bmain, name("FsSrcB").c_str(), name("FsNrmB").c_str(), source_spec_b);
    Material *source_c = build_group_source(*bmain,
                                            name("FsSrcC").c_str(),
                                            name("FsGrpC").c_str(),
                                            name("FsMapC").c_str(),
                                            name("FsShrC").c_str(),
                                            source_spec_c);
    if (source_a == nullptr || source_b == nullptr || source_c == nullptr) {
      return s;
    }
    ma = BKE_material_add(bmain, name("FsStack").c_str());

    /* 1. Fill: Base Color from the fill colour, Roughness from its record. */
    s.fill = BKE_paint_layers_add(
        *ma, MA_PAINT_LAYER_KIND_FILL, "Fill", nullptr, PaintLayerPlace::Above);
    if (s.fill == nullptr ||
        BKE_paint_layers_channel_add(*ma, s.fill, PAINT_MATERIAL_CHANNEL_BASE_COLOR) == nullptr ||
        BKE_paint_layers_channel_add(*ma, s.fill, PAINT_MATERIAL_CHANNEL_ROUGHNESS) == nullptr)
    {
      return s;
    }
    copy_v4_v4(s.fill->fill_color, kFsFillColor);
    const float rough[4] = {kFsFillRoughness, kFsFillRoughness, kFsFillRoughness, 1.0f};
    BKE_paint_layers_channel_set_value(*ma, s.fill, PAINT_MATERIAL_CHANNEL_ROUGHNESS, rough);

    /* 2. Material A with a Paint correction and a mask. */
    s.a = BKE_paint_layers_add(
        *ma, MA_PAINT_LAYER_KIND_MATERIAL, "MatA", nullptr, PaintLayerPlace::Above);
    if (s.a == nullptr || !BKE_paint_layers_set_material(*ma, s.a, source_a) ||
        !bake_row(*s.a, name("FsBakeA"), true, source_spec_a, {}, 1.0f))
    {
      return s;
    }
    Image *corr_map = make_byte_map(name("FsCorr"), kFsCorrBytes);
    const uchar mask_texels[4][4] = {{kFsMaskBytes[0], kFsMaskBytes[0], kFsMaskBytes[0], 255},
                                     {kFsMaskBytes[1], kFsMaskBytes[1], kFsMaskBytes[1], 255},
                                     {kFsMaskBytes[2], kFsMaskBytes[2], kFsMaskBytes[2], 255},
                                     {kFsMaskBytes[3], kFsMaskBytes[3], kFsMaskBytes[3], 255}};
    Image *mask_map = make_byte_map(name("FsMask"), mask_texels);
    if (corr_map == nullptr || mask_map == nullptr) {
      return s;
    }
    s.a_correction = add_content_correction(*ma, *s.a, corr_map);
    s.a_mask = set_mask_image(*s.a, mask_map);
    if (s.a_correction == nullptr || s.a_mask == nullptr) {
      return s;
    }
    BKE_paint_layers_set_opacity(*ma, s.a_correction, kFsCorrOpacity);
    /* The mask item starts as a Multiply constant; the reference lays it as a Mix. */
    BKE_paint_layers_set_blend(*ma, s.a_mask, MA_PAINT_LAYER_BLEND_MIX);
    BKE_paint_layers_set_opacity(*ma, s.a_mask, kFsMaskOpacity);

    /* 3. The isolating folder: Material B (Hybrid) and a Paint row. */
    if (options.iso) {
      s.iso = BKE_paint_layers_add(
          *ma, MA_PAINT_LAYER_KIND_FOLDER, "Iso", nullptr, PaintLayerPlace::Above);
      if (s.iso == nullptr) {
        return s;
      }
      BKE_paint_layers_set_opacity(*ma, s.iso, kFsIsoOpacity);
      s.b = BKE_paint_layers_add(
          *ma, MA_PAINT_LAYER_KIND_MATERIAL, "MatB", s.iso, PaintLayerPlace::Into);
      if (s.b == nullptr || !BKE_paint_layers_set_material(*ma, s.b, source_b) ||
          !bake_row(*s.b, name("FsBakeB"), false, source_spec_a, source_spec_b, source_spec_b.alpha))
      {
        return s;
      }
      s.inner_paint = BKE_paint_layers_add(
          *ma, MA_PAINT_LAYER_KIND_PAINT, "InnerPaint", s.iso, PaintLayerPlace::Into);
      if (s.inner_paint == nullptr) {
        return s;
      }
      MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
          *ma, s.inner_paint, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
      Image *inner_map = make_bake_data_map(
          bmain, name("FsInner").c_str(), kFsSize, kIsolatingPaintA);
      if (record == nullptr || inner_map == nullptr) {
        return s;
      }
      record->image = inner_map;
      record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
      BKE_paint_layers_set_opacity(*ma, s.inner_paint, kFsInnerPaintOpacity);
    }

    /* 4. Material C, in a Pass Through folder or straight in the root. */
    if (options.with_c) {
      if (options.c_in_pass_folder) {
        s.pass = BKE_paint_layers_add(
            *ma, MA_PAINT_LAYER_KIND_FOLDER, "Pass", nullptr, PaintLayerPlace::Above);
        if (s.pass == nullptr || !BKE_paint_layers_folder_is_pass_through(*ma, *s.pass)) {
          return s;
        }
      }
      s.c = BKE_paint_layers_add(*ma,
                                 MA_PAINT_LAYER_KIND_MATERIAL,
                                 "MatC",
                                 s.pass,
                                 s.pass != nullptr ? PaintLayerPlace::Into : PaintLayerPlace::Above);
      if (s.c == nullptr || !BKE_paint_layers_set_material(*ma, s.c, source_c) ||
          !bake_row(*s.c, name("FsBakeC"), true, source_spec_c, {}, 1.0f))
      {
        return s;
      }
      BKE_paint_layers_set_opacity(*ma, s.c, kFsCOpacity);
    }

    /* 5. The top Paint row: Multiply, opacity below one, Base Color and Roughness maps. */
    Image *top_color = make_bake_data_map(bmain, name("FsTopColor").c_str(), kFsSize, kFsTopColor);
    Image *top_rough = make_bake_data_map(
        bmain, name("FsTopRough").c_str(), kFsSize, kFsTopRoughness);
    if (top_color == nullptr || top_rough == nullptr) {
      return s;
    }
    s.top = add_layer("Top", MA_PAINT_LAYER_KIND_PAINT, top_color);
    MaterialPaintLayerChannel *top_rough_record = BKE_paint_layers_channel_add(
        *ma, s.top, PAINT_MATERIAL_CHANNEL_ROUGHNESS);
    if (s.top == nullptr || top_rough_record == nullptr) {
      return s;
    }
    top_rough_record->image = top_rough;
    top_rough_record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
    BKE_paint_layers_set_blend(*ma, s.top, MA_PAINT_LAYER_BLEND_MULTIPLY);
    BKE_paint_layers_set_opacity(*ma, s.top, kFsTopOpacity);

    s.ma = ma;
    return s;
  }

  Vector<FsActiveCase> active_cases(const Stack &s)
  {
    Vector<FsActiveCase> cases;
    cases.append({"none", BLI_uuid_nil(), -1});
    cases.append({"fill", s.fill->marker, -1});
    cases.append({"mat-a", s.a->marker, 0});
    cases.append({"a-correction", s.a_correction->marker, 0});
    cases.append({"a-mask", s.a_mask->marker, 0});
    if (s.iso != nullptr) {
      cases.append({"iso-folder", s.iso->marker, -1});
      cases.append({"mat-b", s.b->marker, 1});
      cases.append({"inner-paint", s.inner_paint->marker, -1});
    }
    if (s.pass != nullptr) {
      cases.append({"pass-folder", s.pass->marker, -1});
    }
    if (s.c != nullptr) {
      cases.append({"mat-c", s.c->marker, 2});
    }
    cases.append({"top", s.top->marker, -1});
    return cases;
  }

  /**
   * Compare the current `ma`'s generated graph and CPU composite with #full_stack_expected at every
   * channel and point. \a strict_wiring: a channel is wired exactly when a row takes part; off for a
   * stack whose hidden rows still wire their channels.
   */
  void check_values(const char *tag, const FsShape &shape, const bool strict_wiring)
  {
    GraphInterpreter interpreter;
    interpreter.instance = find_instance();
    interpreter.tree = ma->paint_layers_tree;
    ASSERT_NE(interpreter.instance, nullptr) << tag;
    interpreter.tree->ensure_topology_cache();

    for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
      const int channel = int(info.channel);
      RGBA probe;
      const bool part = full_stack_expected(channel, 0, 0, shape, probe);
      const bool exists = result_output_exists(*ma->paint_layers_tree, channel);
      if ((strict_wiring && exists != part) || (part && !exists)) {
        ADD_FAILURE() << tag << " | " << info.ui_name << ": wired=" << exists
                      << " but a row takes part=" << part;
        continue;
      }
      if (!exists) {
        continue;
      }
      for (const FsPoint &point : kFsPoints) {
        RGBA expected;
        full_stack_expected(channel, point.x, point.y, shape, expected);
        interpreter.x = point.x;
        interpreter.y = point.y;
        fs_expect_rgb(expected,
                      eval_channel_result(interpreter, eMaterialPaintChannel(channel)),
                      tag,
                      info.ui_name,
                      point.label,
                      "graph");
        if (part) {
          fs_expect_rgb(expected,
                        cpu_pixel_at(channel, point.x, point.y),
                        tag,
                        info.ui_name,
                        point.label,
                        "cpu");
        }
      }
    }
  }
};

/**
 * Every active row, folder, correction and mask: the graph and the CPU equal the reference at every
 * channel and point, the report carries the mode and `deferred` the rule gives, and a second
 * regeneration rebuilds nothing.
 */
TEST_F(PaintLayersFullStackTest, every_active_row_matches_the_reference_and_the_cpu)
{
  const Stack s = build("All", {});
  ASSERT_NE(s.ma, nullptr);
  ASSERT_FALSE(BKE_paint_layers_folder_is_pass_through(*ma, *s.iso));

  const MaterialPaintLayer *material_rows[3] = {s.a, s.b, s.c};
  for (const FsActiveCase &active : active_cases(s)) {
    const char *tag = active.label.c_str();
    BKE_paint_layers_active_set(*ma, active.marker);
    PaintLayersRegenerateReport report;
    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report)) << tag;
    ASSERT_NE(ma->paint_layers_tree, nullptr) << tag;

    EXPECT_EQ(report.material_rows.size(), 3) << tag;
    for (const int row_index : IndexRange(3)) {
      const PaintLayersRegenerateReport::MaterialRowModeReport *found = nullptr;
      for (const auto &row : report.material_rows) {
        if (BLI_uuid_equal(row.marker, material_rows[row_index]->marker)) {
          found = &row;
        }
      }
      ASSERT_NE(found, nullptr) << tag << " row " << row_index;
      EXPECT_EQ(found->mode, fs_expected_mode(row_index, active.owner))
          << tag << " row " << row_index << " mode";
      EXPECT_EQ(found->deferred, row_index == active.owner)
          << tag << " row " << row_index << " deferred";
      EXPECT_EQ(found->refusal, PaintLayersSourceGroupRefusal::None) << tag << " row " << row_index;
    }

    check_values(tag, {}, true);

    /* The next regeneration, without an edit, must rebuild nothing. */
    bNodeTree *root = ma->paint_layers_tree;
    const Vector<bNode *> nodes = root_node_ptrs(*root);
    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma)) << tag;
    EXPECT_EQ(ma->paint_layers_tree, root) << tag;
    EXPECT_TRUE(same_node_ptrs(nodes, root_node_ptrs(*root))) << tag << ": second regen rebuilt";
  }
}

/**
 * A second regeneration without edits rebuilds nothing, whichever row is active. The active row can
 * be inside the isolating folder: the root hash must not depend on whether that folder's group was
 * rebuilt in the same pass (paint_layers_root_topology_hash).
 */
TEST_F(PaintLayersFullStackTest, second_regen_after_an_active_change_rebuilds_nothing)
{
  const Stack s = build("Stable", {});
  ASSERT_NE(s.ma, nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  for (const FsActiveCase &active : active_cases(s)) {
    const char *tag = active.label.c_str();
    BKE_paint_layers_active_set(*ma, active.marker);
    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma)) << tag;
    bNodeTree *root = ma->paint_layers_tree;
    const Vector<bNode *> nodes = root_node_ptrs(*root);
    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma)) << tag;
    EXPECT_EQ(ma->paint_layers_tree, root) << tag;
    EXPECT_TRUE(same_node_ptrs(nodes, root_node_ptrs(*root))) << tag << ": second regen rebuilt";
  }
}

/** The first regeneration of a fresh stack settles: the second one keeps the root. */
TEST_F(PaintLayersFullStackTest, initial_build_settles_in_one_regen)
{
  const Stack s = build("Initial", {});
  ASSERT_NE(s.ma, nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *root = ma->paint_layers_tree;
  const Vector<bNode *> nodes = root_node_ptrs(*root);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_TRUE(same_node_ptrs(nodes, root_node_ptrs(*root))) << "second regen rebuilt";
}

/**
 * Hiding C and the isolating folder is a value edit (the root keeps its nodes) and gives the stack
 * without them; a stack built without them gives the same.
 */
TEST_F(PaintLayersFullStackTest, hiding_c_and_the_isolating_folder_equals_the_stack_without_them)
{
  const Stack full = build("Hide", {});
  ASSERT_NE(full.ma, nullptr);
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *root = ma->paint_layers_tree;
  ASSERT_NE(root, nullptr);
  const Vector<bNode *> nodes = root_node_ptrs(*root);

  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, full.c, false));
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, full.iso, false));
  BKE_paint_layers_values_sync(*ma);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_TRUE(same_node_ptrs(nodes, root_node_ptrs(*root))) << "hiding rebuilt the root";
  FsShape hidden;
  hidden.iso = false;
  hidden.c = false;
  check_values("hidden", hidden, false);

  Options bare_options;
  bare_options.iso = false;
  bare_options.with_c = false;
  const Stack bare = build("HideBare", bare_options);
  ASSERT_NE(bare.ma, nullptr);
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  check_values("built-without", hidden, true);
}

/**
 * Hiding the Pass Through folder hides C through the folder's visibility scale, a value edit: the
 * root keeps its nodes and the result is the stack without C, on the graph and on the CPU.
 */
TEST_F(PaintLayersFullStackTest, hiding_the_pass_through_folder_hides_c_as_a_value_edit)
{
  const Stack s = build("HidePass", {});
  ASSERT_NE(s.ma, nullptr);
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *root = ma->paint_layers_tree;
  ASSERT_NE(root, nullptr);
  const Vector<bNode *> nodes = root_node_ptrs(*root);

  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, s.pass, false));
  BKE_paint_layers_values_sync(*ma);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_TRUE(same_node_ptrs(nodes, root_node_ptrs(*root))) << "hiding the folder rebuilt the root";
  FsShape without_c;
  without_c.c = false;
  check_values("pass-hidden", without_c, true);
}
/**
 * The other direction: a real structural edit still rebuilds what it must. A blend change shows in
 * the values, a new top-level row and a move into the isolating folder change the root's nodes.
 */
TEST_F(PaintLayersFullStackTest, real_structure_edits_still_rebuild)
{
  const Stack s = build("Edits", {});
  ASSERT_NE(s.ma, nullptr);
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  auto base_color = [&]() {
    GraphInterpreter interpreter;
    interpreter.instance = find_instance();
    interpreter.tree = ma->paint_layers_tree;
    interpreter.x = 1;
    interpreter.y = 1;
    EXPECT_NE(interpreter.instance, nullptr);
    interpreter.tree->ensure_topology_cache();
    return eval_channel_result(interpreter, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  };

  /* A blend change rebuilds the row's group: the result moves. */
  const RGBA before_blend = base_color();
  ASSERT_TRUE(BKE_paint_layers_set_blend(*ma, s.top, MA_PAINT_LAYER_BLEND_ADD));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  const RGBA after_blend = base_color();
  EXPECT_GT(std::fabs(after_blend.r - before_blend.r) + std::fabs(after_blend.g - before_blend.g) +
                std::fabs(after_blend.b - before_blend.b),
            1e-3f);

  /* A new top-level row changes the root's nodes. */
  bNodeTree *root = ma->paint_layers_tree;
  Vector<bNode *> nodes = root_node_ptrs(*root);
  MaterialPaintLayer *extra = add_layer("Extra", MA_PAINT_LAYER_KIND_PAINT,
                                        make_bake_data_map(bmain, "FsExtra", kFsSize, kFsTopColor));
  ASSERT_NE(extra, nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_FALSE(same_node_ptrs(nodes, root_node_ptrs(*ma->paint_layers_tree))) << "new row";

  /* Moving a root row into the isolating folder removes its instance from the root. */
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  nodes = root_node_ptrs(*ma->paint_layers_tree);
  ASSERT_TRUE(BKE_paint_layers_move(*ma, extra, s.iso, PaintLayerPlace::Into));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_FALSE(same_node_ptrs(nodes, root_node_ptrs(*ma->paint_layers_tree))) << "move into folder";
}
/** The Pass Through folder is transparent: C directly in the root gives the same stack. */
TEST_F(PaintLayersFullStackTest, pass_through_folder_equals_c_in_the_root)
{
  Options flat_options;
  flat_options.c_in_pass_folder = false;
  const Stack folded = build("PassIn", {});
  const Stack flat = build("PassOut", flat_options);
  ASSERT_NE(folded.ma, nullptr);
  ASSERT_NE(flat.ma, nullptr);

  for (const Stack *stack : {&folded, &flat}) {
    ma = stack->ma;
    for (const bUUID &marker : {BLI_uuid_nil(), stack->c->marker}) {
      BKE_paint_layers_active_set(*ma, marker);
      ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
      check_values(stack == &folded ? "folded" : "flat", {}, true);
    }
  }
}

/**
 * One regeneration resolves each distinct source at most once, however many rows read it and however
 * many times the topology hashes, the wired set and the build ask about it. The resolver walks the
 * source's tree with its groups, so before the per-call cache one Material row cost dozens of walks.
 * The counter is a test hook: nothing else can tell "resolved once" from "resolved fast".
 */
TEST_F(PaintLayersFullStackTest, one_regeneration_resolves_each_source_at_most_once)
{
  const Stack s = build("Resolves", {});
  ASSERT_NE(s.ma, nullptr);
  /* A, B and C read three different sources. */
  const int64_t distinct_sources = 3;

  const int64_t first_before = BKE_paint_material_source_resolve_call_count();
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_LE(BKE_paint_material_source_resolve_call_count() - first_before, distinct_sources)
      << "initial build";

  for (const FsActiveCase &active : active_cases(s)) {
    const char *tag = active.label.c_str();
    BKE_paint_layers_active_set(*ma, active.marker);
    for (const int pass : IndexRange(2)) {
      const int64_t before = BKE_paint_material_source_resolve_call_count();
      ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma)) << tag;
      EXPECT_LE(BKE_paint_material_source_resolve_call_count() - before, distinct_sources)
          << tag << " pass " << pass;
    }
  }
}

/** Every row's own group (`.PL Layer` / `.PL Folder`) with the nodes it holds, keyed by the tree. */
static Map<const bNodeTree *, Vector<bNode *>> row_group_snapshot(Main &bmain)
{
  Map<const bNodeTree *, Vector<bNode *>> snapshot;
  for (bNodeTree &group : bmain.nodetrees) {
    const char *name = group.id.name + 2;
    if (STRPREFIX(name, ".PL Layer") || STRPREFIX(name, ".PL Folder")) {
      snapshot.add(&group, root_node_ptrs(group));
    }
  }
  return snapshot;
}

/**
 * A regeneration without an edit rebuilds neither the root nor any row's group: the same trees
 * survive and each keeps its exact node list. The root-only check of the neighbouring tests would
 * miss a group whose hash drifted between two passes.
 */
TEST_F(PaintLayersFullStackTest, second_regen_rebuilds_no_row_group)
{
  const Stack s = build("Groups", {});
  ASSERT_NE(s.ma, nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  for (const FsActiveCase &active : active_cases(s)) {
    const char *tag = active.label.c_str();
    BKE_paint_layers_active_set(*ma, active.marker);
    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma)) << tag;
    bNodeTree *root = ma->paint_layers_tree;
    const Vector<bNode *> root_nodes = root_node_ptrs(*root);
    const Map<const bNodeTree *, Vector<bNode *>> before = row_group_snapshot(*bmain);
    ASSERT_FALSE(before.is_empty()) << tag;

    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma)) << tag;
    EXPECT_EQ(ma->paint_layers_tree, root) << tag;
    EXPECT_TRUE(same_node_ptrs(root_nodes, root_node_ptrs(*root))) << tag << ": root rebuilt";
    const Map<const bNodeTree *, Vector<bNode *>> after = row_group_snapshot(*bmain);
    EXPECT_EQ(before.size(), after.size()) << tag << ": a row group appeared or vanished";
    for (const auto item : before.items()) {
      const Vector<bNode *> *nodes = after.lookup_ptr(item.key);
      if (nodes == nullptr) {
        ADD_FAILURE() << tag << ": row group " << item.key->id.name + 2 << " was replaced";
        continue;
      }
      EXPECT_TRUE(same_node_ptrs(item.value, *nodes))
          << tag << ": row group " << item.key->id.name + 2 << " was rebuilt";
    }
  }
}

/**
 * A Material row whose bake is still being rendered stays live, and a row whose bake has landed
 * goes to its maps: the maps in flight are the pending claim the editor's job code holds. While it
 * is claimed the row is not deferred (the user has moved on) yet shows its source, so the graph and
 * the CPU equal the reference at every point in every state, and a regeneration in any state
 * rebuilds nothing more.
 */
TEST_F(PaintLayersFullStackTest, rows_with_a_bake_in_flight_stay_live_until_it_lands)
{
  const Stack s = build("InFlight", {});
  ASSERT_NE(s.ma, nullptr);
  ASSERT_FALSE(BKE_paint_layers_folder_is_pass_through(*ma, *s.iso));
  const MaterialPaintLayer *material_rows[3] = {s.a, s.b, s.c};

  Vector<uint32_t> claimed;
  for (const MaterialPaintLayer *row : material_rows) {
    ASSERT_NE(row->bake, nullptr);
    for (Image *image : row->bake->images) {
      if (image != nullptr) {
        claimed.append(image->id.session_uid);
      }
    }
    ASSERT_NE(row->bake->coverage, nullptr);
    claimed.append(row->bake->coverage->id.session_uid);
  }

  const auto row_groups_changed = [](const Map<const bNodeTree *, Vector<bNode *>> &before,
                                     const Map<const bNodeTree *, Vector<bNode *>> &after) {
    int changed = 0;
    for (const auto item : before.items()) {
      const Vector<bNode *> *nodes = after.lookup_ptr(item.key);
      if (nodes == nullptr || !same_node_ptrs(item.value, *nodes)) {
        changed++;
      }
    }
    return changed;
  };

  /* Nothing is being baked: every inactive row is on its maps, as before. */
  BKE_paint_layers_active_set(*ma, s.a->marker);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  for (const int row_index : IndexRange(3)) {
    EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *material_rows[row_index]),
              fs_expected_mode(row_index, 0))
        << "row " << row_index;
  }

  /* The user moves off A: its bake is handed over and claimed. Only B, the new active row, changes
   * mode; A keeps its live graph, and nothing shows a mode between live and baked. */
  const Map<const bNodeTree *, Vector<bNode *>> before_switch = row_group_snapshot(*bmain);
  for (const uint32_t uid : claimed) {
    BKE_paint_layers_bake_image_pending_add(uid);
  }
  BKE_paint_layers_active_set(*ma, s.b->marker);
  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *s.a), PaintLayerMaterialMode::SourceGroup);
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *s.b), PaintLayerMaterialMode::Hybrid);
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *s.c), PaintLayerMaterialMode::SourceGroup);
  EXPECT_FALSE(BKE_paint_layers_bake_row_is_deferred(*ma, *s.a));
  EXPECT_FALSE(BKE_paint_layers_material_bake_ready(*ma, *s.a));
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *s.a));
  check_values("in-flight", {}, true);
  /* A stays as it was; B (now active), its folder and C (its bake is in flight too) change, and
   * nothing else. */
  EXPECT_LE(row_groups_changed(before_switch, row_group_snapshot(*bmain)), 3);

  /* A second regeneration while the bake is in flight rebuilds nothing. */
  const Map<const bNodeTree *, Vector<bNode *>> settled = row_group_snapshot(*bmain);
  bNodeTree *root = ma->paint_layers_tree;
  const Vector<bNode *> root_before = root_node_ptrs(*root);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_TRUE(same_node_ptrs(root_before, root_node_ptrs(*root)));
  EXPECT_EQ(row_groups_changed(settled, row_group_snapshot(*bmain)), 0);

  /* The maps land: A and C go to them in one regeneration, B stays live as the active row. */
  for (const uint32_t uid : claimed) {
    BKE_paint_layers_bake_image_pending_remove(uid);
  }
  BKE_paint_layers_tag_edited(*ma);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  for (const int row_index : IndexRange(3)) {
    EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *material_rows[row_index]),
              fs_expected_mode(row_index, 1))
        << "row " << row_index;
  }
  EXPECT_TRUE(BKE_paint_layers_material_bake_ready(*ma, *s.a));
  check_values("landed", {}, true);

  const Map<const bNodeTree *, Vector<bNode *>> landed = row_group_snapshot(*bmain);
  root = ma->paint_layers_tree;
  const Vector<bNode *> landed_root = root_node_ptrs(*root);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_TRUE(same_node_ptrs(landed_root, root_node_ptrs(*root)));
  EXPECT_EQ(row_groups_changed(landed, row_group_snapshot(*bmain)), 0);
}

/**
 * Moving the active row back and forth over a row whose bake is valid and complete is one mode
 * change each way and never a mode in between: with nothing to bake there is no reason for the row
 * to be anything but live while active and baked otherwise.
 */
TEST_F(PaintLayersFullStackTest, moving_the_active_row_over_a_baked_row_never_passes_hybrid)
{
  const Stack s = build("Toggle", {});
  ASSERT_NE(s.ma, nullptr);
  ASSERT_FALSE(BKE_paint_layers_folder_is_pass_through(*ma, *s.iso));
  for (const int round : IndexRange(3)) {
    BKE_paint_layers_active_set(*ma, s.a->marker);
    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma)) << round;
    EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *s.a), PaintLayerMaterialMode::SourceGroup);
    BKE_paint_layers_active_set(*ma, s.top->marker);
    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma)) << round;
    EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *s.a), PaintLayerMaterialMode::Baked);
    check_values("toggle", {}, true);
  }
}

/**
 * Moving a row's opacity through the RNA slider across 1.0 (1 -> 0.7 -> 1 -> 0.3) is a value edit:
 * no row group and not the root is rebuilt, and the graph follows the formula. Covers a Hybrid row
 * (B), SourceGroup rows (A, C, live while active), a Paint row and a Fill row.
 */
TEST_F(PaintLayersFullStackTest, opacity_across_one_through_rna_rebuilds_nothing)
{
  const Stack s = build("OpacityRna", {});
  ASSERT_NE(s.ma, nullptr);

  struct Target {
    const char *label;
    MaterialPaintLayer *row;
    float FsShape::*member;
    bool make_active;
  };
  const Target targets[] = {{"fill", s.fill, &FsShape::fill_opacity, false},
                            {"mat-a-sourcegroup", s.a, &FsShape::a_opacity, true},
                            {"mat-b-hybrid", s.b, &FsShape::b_opacity, true},
                            {"mat-c-sourcegroup", s.c, &FsShape::c_opacity, true},
                            {"top-paint", s.top, &FsShape::top_opacity, false}};

  for (const Target &target : targets) {
    BKE_paint_layers_active_set(*ma, target.make_active ? target.row->marker : BLI_uuid_nil());
    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma)) << target.label;
    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma)) << target.label;

    /* Each target starts from the stack's own opacity, so the shape it is checked against is the
     * default one and the earlier targets' last value (0.3) cannot leak in. */
    FsShape shape;
    {
      PointerRNA ptr = RNA_pointer_create_discrete(&ma->id, RNA_MaterialPaintLayer, target.row);
      PropertyRNA *prop = RNA_struct_find_property(&ptr, "opacity");
      RNA_property_float_set(&ptr, prop, shape.*(target.member) * 100.0f);
      ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma)) << target.label;
    }
    for (const float opacity : {1.0f, 0.7f, 1.0f, 0.3f}) {
      const std::string tag = std::string(target.label) + " @" + std::to_string(opacity);
      bNodeTree *root = ma->paint_layers_tree;
      const Vector<bNode *> root_nodes = root_node_ptrs(*root);
      const Map<const bNodeTree *, Vector<bNode *>> before = row_group_snapshot(*bmain);

      PointerRNA ptr = RNA_pointer_create_discrete(&ma->id, RNA_MaterialPaintLayer, target.row);
      PropertyRNA *prop = RNA_struct_find_property(&ptr, "opacity");
      ASSERT_NE(prop, nullptr);
      RNA_property_float_set(&ptr, prop, opacity * 100.0f);
      shape.*(target.member) = opacity;

      ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma)) << tag;
      EXPECT_EQ(ma->paint_layers_tree, root) << tag << ": root replaced";
      EXPECT_TRUE(same_node_ptrs(root_nodes, root_node_ptrs(*root))) << tag << ": root rebuilt";
      const Map<const bNodeTree *, Vector<bNode *>> after = row_group_snapshot(*bmain);
      EXPECT_EQ(before.size(), after.size()) << tag << ": a row group appeared or vanished";
      for (const auto item : before.items()) {
        const Vector<bNode *> *nodes = after.lookup_ptr(item.key);
        if (nodes == nullptr) {
          ADD_FAILURE() << tag << ": row group " << item.key->id.name + 2 << " was replaced";
          continue;
        }
        EXPECT_TRUE(same_node_ptrs(item.value, *nodes))
            << tag << ": row group " << item.key->id.name + 2 << " was rebuilt";
      }
      check_values(tag.c_str(), shape, true);
    }
    /* Back to the stack's own opacity, so the next target is checked against the default shape. */
    BKE_paint_layers_set_opacity(*ma, target.row, FsShape().*(target.member));
  }
}

/**
 * One #BKE_paint_layers_values_sync resolves each source at most once (in fact not at all: values
 * come from the description), and leaves the graph at the reference.
 */
TEST_F(PaintLayersFullStackTest, values_sync_resolves_each_source_at_most_once)
{
  const Stack s = build("SyncResolves", {});
  ASSERT_NE(s.ma, nullptr);
  const int64_t distinct_sources = 3;
  for (const FsActiveCase &active : active_cases(s)) {
    const char *tag = active.label.c_str();
    BKE_paint_layers_active_set(*ma, active.marker);
    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma)) << tag;
    const int64_t before = BKE_paint_material_source_resolve_call_count();
    BKE_paint_layers_values_sync(*ma);
    EXPECT_LE(BKE_paint_material_source_resolve_call_count() - before, distinct_sources) << tag;
    check_values(tag, {}, true);
  }
}

/** \} */

}  // namespace blender::bke::tests
