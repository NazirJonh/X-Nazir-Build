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

}  // namespace blender::bke::tests
