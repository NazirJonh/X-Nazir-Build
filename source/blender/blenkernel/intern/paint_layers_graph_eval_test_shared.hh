/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */


#pragma once

/** Shared file-static helpers for the split graph-eval TUs (plan 9.3).
 *
 * Moved verbatim from `paint_layers_graph_eval_test.cc`, with `static` added where the
 * original relied on its anonymous namespace: one copy per TU, so no ODR risk.
 */

#include "intern/paint_layers_graph_eval_test_fixture.hh"

namespace blender::bke::tests {

static const float kIsolatingPaintA[4] = {0.70f, 0.30f, 0.10f, 1.0f};

/* Defined further down, next to the other source builders. */
static Material *make_interface_wired_source(Main &bmain, const char *name);

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


/* Whether the group's interface carries an output socket named \a socket_name. */
static bool folder_interface_has_socket(bNodeTree &tree, const char *socket_name)
{
  tree.ensure_interface_cache();
  for (bNodeTreeInterfaceSocket *iface : tree.interface_outputs()) {
    if (iface->name != nullptr && STREQ(iface->name, socket_name)) {
      return true;
    }
  }
  return false;
}


/* Evaluate a folder instance's `Content Alpha Base Color` output in its parent's context, or
 * -1 when the group carries no such socket (the untracked rows leave none). */
static float nested_folder_content_alpha_eval(const GraphInterpreter &parent,
                                              bNodeTree &folder_tree,
                                              bNode &folder_instance)
{
  folder_tree.ensure_interface_cache();
  const char *identifier = nullptr;
  for (bNodeTreeInterfaceSocket *iface : folder_tree.interface_outputs()) {
    if (iface->name != nullptr && STREQ(iface->name, "Content Alpha Base Color") &&
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


/* Evaluate a folder instance's output socket named \a socket_name in its parent's context, or -1
 * when the group carries no such socket. Generalizes the Base Color helpers above to any channel. */
static float nested_folder_named_output_eval(const GraphInterpreter &parent,
                                             bNodeTree &folder_tree,
                                             bNode &folder_instance,
                                             const char *socket_name)
{
  folder_tree.ensure_interface_cache();
  const char *identifier = nullptr;
  for (bNodeTreeInterfaceSocket *iface : folder_tree.interface_outputs()) {
    if (iface->name != nullptr && STREQ(iface->name, socket_name) && iface->identifier != nullptr) {
      identifier = iface->identifier;
      break;
    }
  }
  if (identifier == nullptr) {
    return -1.0f;
  }
  return parent.eval_output(folder_instance, identifier).r;
}


/* Quantized alpha of #fill_nested_leaf_soft_edge's stored byte at column \a x. */
static float soft_edge_alpha_q(const int x)
{
  const float alpha_ideal = 0.25f * float(x + 1);
  return float(uchar(clamp_f(alpha_ideal, 0.0f, 1.0f) * 255.0f + 0.5f)) / 255.0f;
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
 * The row's encoded Normal Result from the combination of \a bottom_enc and a source whose decoded
 * normal is \a detail: the chain's decode-combine-normalize-encode plus the final normalize applied
 * by `cpu_pixel_at`/the chain. \a fac is the row factor.
 */
static void normal_result_reference(const RGBA &bottom_enc,
                                    const float detail[3],
                                    const float fac,
                                    float r_out[3])
{
  float base[3] = {bottom_enc.r * 2.0f - 1.0f,
                   bottom_enc.g * 2.0f - 1.0f,
                   bottom_enc.b * 2.0f - 1.0f};
  /* The bottom row's own Result is the chain's final decode-normalize-encode, so the combine's A is
   * the bottom vector already normalized; a near-flat map barely shows it, a tilted one does. */
  normalize_v3(base);
  float combined[3] = {base[0] + detail[0], base[1] + detail[1], base[2] * detail[2]};
  normalize_v3(combined);
  const float encoded[3] = {combined[0] * 0.5f + 0.5f,
                            combined[1] * 0.5f + 0.5f,
                            combined[2] * 0.5f + 0.5f};
  const float raw[3] = {bottom_enc.r * (1.0f - fac) + encoded[0] * fac,
                        bottom_enc.g * (1.0f - fac) + encoded[1] * fac,
                        bottom_enc.b * (1.0f - fac) + encoded[2] * fac};
  float final_n[3] = {raw[0] * 2.0f - 1.0f, raw[1] * 2.0f - 1.0f, raw[2] * 2.0f - 1.0f};
  normalize_v3(final_n);
  r_out[0] = final_n[0] * 0.5f + 0.5f;
  r_out[1] = final_n[1] * 0.5f + 0.5f;
  r_out[2] = final_n[2] * 0.5f + 0.5f;
}

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


/** The value of \a channel's `Result <name>` socket in the generated tree of \a interpreter. */
static RGBA eval_channel_result(const GraphInterpreter &interpreter,
                                const eMaterialPaintChannel channel)
{
  const char *ui_name = BKE_paint_material_channel_info(channel).ui_name;
  char result_name[64];
  BLI_snprintf(result_name, sizeof(result_name), "Result %s", ui_name);
  return interpreter.eval_result(result_name);
}


/** The per (row, channel) opacity write through the BKE setter (9.2): the BKE write plus the
 * values sync the RNA slider's update runs, so the graph below reads the new factor. */
void bke_set_channel_opacity(Material &ma,
                             MaterialPaintLayer &layer,
                             const int channel,
                             const float factor)
{
  EXPECT_TRUE(BKE_paint_layers_channel_opacity_set(ma, layer, channel, factor));
  BKE_paint_layers_values_sync(ma);
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
      ma, &row, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_IMAGE, "C");
  EXPECT_NE(correction, nullptr);
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
      ma, correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  EXPECT_NE(record, nullptr);
  record->image = map;
  record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  BKE_paint_layers_correction_source_set(ma, correction, MA_PAINT_LAYER_SOURCE_IMAGE);
  return correction;
}


static void expect_correction_mix(const RGBA &sample,
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

static Vector<bNode *> root_node_ptrs(bNodeTree &tree)
{
  Vector<bNode *> nodes;
  for (bNode &node : tree.nodes) {
    nodes.append(&node);
  }
  return nodes;
}

static Material *make_wrapper_forced_source(Main &bmain, const char *name, bNode **r_principled)
{
  Material *source = BKE_material_add(&bmain, name);
  bNodeTree &ntree = *source->nodetree;
  bNode *principled = bke::node_add_static_node(nullptr, ntree, SH_NODE_BSDF_PRINCIPLED);
  bNode *output = bke::node_add_static_node(nullptr, ntree, SH_NODE_OUTPUT_MATERIAL);
  bke::node_add_link(ntree,
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_OUT, "BSDF"_ustr),
                     *output,
                     *bke::node_find_socket(*output, SOCK_IN, "Surface"_ustr));
  bNode *noise = bke::node_add_static_node(nullptr, ntree, SH_NODE_TEX_NOISE);
  bke::node_add_link(ntree,
                     *noise,
                     *bke::node_find_socket(*noise, SOCK_OUT, "Color"_ustr),
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr));
  *r_principled = principled;
  return source;
}

static bNodeTree *wrapper_tree_find(Main &bmain, const char *source_name)
{
  char full[MAX_ID_NAME - 2];
  SNPRINTF(full, ".PL Source %s", source_name);
  for (bNodeTree &tree : bmain.nodetrees) {
    if (STREQ(tree.id.name + 2, full)) {
      return &tree;
    }
  }
  return nullptr;
}

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


}  // namespace blender::bke::tests
