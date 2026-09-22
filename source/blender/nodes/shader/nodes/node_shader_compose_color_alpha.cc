/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shdnodes
 */

#include "node_shader_util.hh"

namespace blender {

namespace nodes::node_shader_compose_color_alpha_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Color>("Color"_ustr)
      .default_value({0.0f, 0.0f, 0.0f, 1.0f})
      .description("Color whose RGB components are used");
  b.add_input<decl::Float>("Alpha"_ustr)
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description("Alpha component of the output color");
  b.add_output<decl::Color>("Color"_ustr);
}

static int gpu_shader_compose_color_alpha(GPUMaterial *mat,
                                           bNode *node,
                                           bNodeExecData * /*execdata*/,
                                           GPUNodeStack *in,
                                           GPUNodeStack *out)
{
  return GPU_stack_link(mat, node, "compose_color_alpha", in, out);
}

}  // namespace nodes::node_shader_compose_color_alpha_cc

void register_node_type_sh_compose_color_alpha()
{
  namespace file_ns = nodes::node_shader_compose_color_alpha_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(
      &ntype, "ShaderNodeComposeColorAlpha"_ustr, SH_NODE_COMPOSE_COLOR_ALPHA);
  ntype.ui_name = "Compose Color Alpha";
  ntype.ui_description = "Replace a color's alpha component";
  ntype.enum_name_legacy = "COMPOSE_COLOR_ALPHA";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = file_ns::node_declare;
  ntype.gpu_fn = file_ns::gpu_shader_compose_color_alpha;

  bke::node_register_type(ntype);
}

}  // namespace blender
