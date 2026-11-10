/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shdnodes
 */

#include "DNA_texture_types.h"

#include "BKE_colorband.hh"

#include "BLI_color_types.hh"

#include "NOD_multi_function.hh"

#include "node_shader_util.hh"
#include "node_util.hh"

namespace blender {

namespace nodes::node_shader_color_ramp_cc {

static void sh_node_valtorgb_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_input<decl::Float>("Factor"_ustr, "Fac"_ustr)
      .default_value(0.5f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description(
          "The value used to map onto the color gradient. 0.0 results in the leftmost color, "
          "while 1.0 results in the rightmost");
  b.add_output<decl::Color>("Color"_ustr);
  b.add_output<decl::Float>("Alpha"_ustr);
}

static void node_shader_init_valtorgb(bNodeTree * /*ntree*/, bNode *node)
{
  node->storage = BKE_colorband_add(true);
}

static int gpu_shader_valtorgb(GPUMaterial *mat,
                               bNode *node,
                               bNodeExecData * /*execdata*/,
                               GPUNodeStack *in,
                               GPUNodeStack *out)
{
  ColorBand *coba = static_cast<ColorBand *>(node->storage);
  float *array, layer;
  int size;

  /* Common / easy case optimization. */
  if (coba->tot == 1 || (coba->tot == 2 && coba->color_mode == COLBAND_BLEND_RGB &&
                         ELEM(coba->ipotype,
                              COLBAND_INTERP_LINEAR,
                              COLBAND_INTERP_CONSTANT,
                              COLBAND_INTERP_EASE)))
  {
    /* The stops become uniforms (some of them computed here); a ramp edit that keeps the shape
     * rewrites them in place (see #GPU_material_values_update). */
    GPU_material_ramp_track(mat, node, coba, -1);
  }
  if (coba->tot == 1) {
    return GPU_link(
        mat,
        "set_rgba",
        GPU_uniform_ramp(mat, node, &coba->data[0].r, GPU_RAMP_UNIFORM_COLOR_SINGLE),
        &out[0].link);
  }
  if ((coba->tot == 2) && (coba->color_mode == COLBAND_BLEND_RGB)) {
    float mul_bias[2];
    switch (coba->ipotype) {
      case COLBAND_INTERP_LINEAR:
        BKE_colorband_valtorgb_mul_bias(*coba, mul_bias);
        return GPU_stack_link(mat,
                              node,
                              "valtorgb_opti_linear",
                              in,
                              out,
                              GPU_uniform_ramp(mat, node, mul_bias, GPU_RAMP_UNIFORM_MUL_BIAS_0),
                              GPU_uniform_ramp(mat, node, &coba->data[0].r, GPU_RAMP_UNIFORM_COLOR_0),
                              GPU_uniform_ramp(mat, node, &coba->data[1].r, GPU_RAMP_UNIFORM_COLOR_1));
      case COLBAND_INTERP_CONSTANT:
        mul_bias[1] = BKE_colorband_valtorgb_const_pos(*coba);
        return GPU_stack_link(
            mat,
            node,
            "valtorgb_opti_constant",
            in,
            out,
            GPU_uniform_ramp(mat, node, &mul_bias[1], GPU_RAMP_UNIFORM_CONST_POS),
            GPU_uniform_ramp(mat, node, &coba->data[0].r, GPU_RAMP_UNIFORM_COLOR_0),
            GPU_uniform_ramp(mat, node, &coba->data[1].r, GPU_RAMP_UNIFORM_COLOR_1));
      case COLBAND_INTERP_EASE:
        BKE_colorband_valtorgb_mul_bias(*coba, mul_bias);
        return GPU_stack_link(mat,
                              node,
                              "valtorgb_opti_ease",
                              in,
                              out,
                              GPU_uniform_ramp(mat, node, mul_bias, GPU_RAMP_UNIFORM_MUL_BIAS_0),
                              GPU_uniform_ramp(mat, node, &coba->data[0].r, GPU_RAMP_UNIFORM_COLOR_0),
                              GPU_uniform_ramp(mat, node, &coba->data[1].r, GPU_RAMP_UNIFORM_COLOR_1));
      default:
        break;
    }
  }

  BKE_colorband_evaluate_table_rgba(coba, &array, &size);
  GPUNodeLink *tex = GPU_color_band(mat, size, array, &layer);
  GPU_material_ramp_track(mat, node, coba, int(layer));

  if (coba->ipotype == COLBAND_INTERP_CONSTANT) {
    return GPU_stack_link(mat, node, "valtorgb_nearest", in, out, tex, GPU_constant(&layer));
  }

  return GPU_stack_link(mat, node, "valtorgb", in, out, tex, GPU_constant(&layer));
}

class ColorBandFunction : public mf::MultiFunction {
 private:
  /** Take ownership of the tree because it contains the color ramp. */
  std::shared_ptr<const bNodeTree> tree_;
  const ColorBand &color_band_;

 public:
  ColorBandFunction(const ColorBand &color_band, std::shared_ptr<const bNodeTree> tree)
      : tree_(tree), color_band_(color_band)
  {
    static const mf::Signature signature = []() {
      mf::Signature signature;
      mf::SignatureBuilder builder{"Color Band", signature};
      builder.single_input<float>("Value");
      builder.single_output<ColorGeometry4f>("Color");
      builder.single_output<float>("Alpha");
      return signature;
    }();
    this->set_signature(&signature);
  }

  void call(const IndexMask &mask, mf::Params params, mf::Context /*context*/) const override
  {
    const VArray<float> &values = params.readonly_single_input<float>(0, "Value");
    MutableSpan<ColorGeometry4f> colors = params.uninitialized_single_output<ColorGeometry4f>(
        1, "Color");
    MutableSpan<float> alphas = params.uninitialized_single_output<float>(2, "Alpha");

    mask.foreach_index([&](const int64_t i) {
      ColorGeometry4f color;
      BKE_colorband_evaluate(&color_band_, values[i], color);
      colors[i] = color;
      alphas[i] = color.a;
    });
  }

  void hash_unique(UniqueHashBytes &hash) const override
  {
    static constexpr int8_t id = 0;
    hash.add(&id);
    hash.add(&color_band_);
  }
};

static void sh_node_valtorgb_build_multi_function(nodes::NodeMultiFunctionBuilder &builder)
{
  const bNode &bnode = builder.node();
  const ColorBand *color_band = static_cast<const ColorBand *>(bnode.storage);
  builder.construct_and_set_matching_fn<ColorBandFunction>(*color_band, builder.shared_tree());
}

NODE_SHADER_MATERIALX_BEGIN
#ifdef WITH_MATERIALX
{
  /* TODO: Implement */
  NodeItem res = empty();
  return res;
}
#endif
NODE_SHADER_MATERIALX_END

}  // namespace nodes::node_shader_color_ramp_cc

void register_node_type_sh_valtorgb()
{
  namespace file_ns = nodes::node_shader_color_ramp_cc;

  static bke::bNodeType ntype;

  common_node_type_base(&ntype, "ShaderNodeValToRGB"_ustr, SH_NODE_VALTORGB);
  ntype.ui_name = "Color Ramp";
  ntype.ui_description = "Map values to colors with the use of a gradient";
  ntype.enum_name_legacy = "VALTORGB";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = file_ns::sh_node_valtorgb_declare;
  ntype.initfunc = file_ns::node_shader_init_valtorgb;
  ntype.default_width = bke::NodeWidth::_240;
  bke::node_type_storage(
      ntype, "ColorBand", node_free_standard_storage, node_copy_standard_storage);
  ntype.gpu_fn = file_ns::gpu_shader_valtorgb;
  ntype.build_multi_function = file_ns::sh_node_valtorgb_build_multi_function;
  ntype.materialx_fn = file_ns::node_shader_materialx;

  bke::node_register_type(ntype);
}

}  // namespace blender
