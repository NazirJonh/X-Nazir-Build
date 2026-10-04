#include "paint_layers_generate_intern.hh"
/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The paint-layer generator: `Material::paint_layers` (the DNA description) becomes a node tree.
 *
 * `paint_layers_tree_build` is the topology half: interface, per-layer nodes and the links between
 * them, with no #Main involved. `BKE_paint_layers_regenerate` is the #Main-side half: it owns the
 * generated group, the instance node in the material's embedded tree, and the routing into the
 * Principled BSDF.
 *
 * Topology and values are separated: the channel chains and the interface are a function of the
 * description's *structure* alone, while the animatable values (`opacity`, `enabled`, a Fill
 * constant) are inputs of each layer group's own interface. Their current values sit on that
 * group's instance node in its parent tree and are copied there by #BKE_paint_layers_values_sync
 * (called from the material evaluation too).
 */

#include "BKE_paint_layers_debug.hh"
#include "BKE_paint_layers_generate.hh"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_map.hh"
#include "BLI_math_vector.h"
#include "BLI_set.hh"
#include "BLI_string.h"
#include "BLI_string_ref.hh"
#include "BLI_string_utf8.h"
#include "BLI_threads.h"
#include "BLI_time.h"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "BKE_global.hh"
#include "BKE_idprop.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "IMB_colormanagement.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_interface.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_material_composite.hh"
#include "BKE_paint_material_resolve.hh"

#include "paint_layers_intern.hh"

#include "paint_layers_generate_build_intern.hh"

#include "NOD_socket.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_tree_interface_types.h"
#include "DNA_node_types.h"
#include "DNA_scene_types.h"
#include "DNA_uuid_types.h"

#include "paint_material_composite_internal.hh"

/* The correction and mask builders of #PaintLayersChainBuilder, with the grey/
 * coverage helpers they share. */

namespace blender {
namespace bke::paint_layers {

std::pair<bNode *, bNodeSocket *> PaintLayersChainBuilder::build_grey_of_map(
    bNodeTree &tree, const float location_x, const float location_y, Image &image, const float offset_y)
{
  bNode *map = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
  bNode *separate = bke::node_add_node(nullptr, tree, "ShaderNodeSeparateXYZ"_ustr);
  bNode *add_xy = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
  bNode *add_z = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
  bNode *divide = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
  if (map == nullptr || separate == nullptr || add_xy == nullptr || add_z == nullptr ||
  divide == nullptr)
  {
return {nullptr, nullptr};
  }
  map->id = &image.id;
  id_us_plus(&image.id);
  map->location[0] = location_x - 90.0f;
  map->location[1] = location_y + offset_y;
  add_xy->custom1 = NODE_MATH_ADD;
  add_z->custom1 = NODE_MATH_ADD;
  divide->custom1 = NODE_MATH_DIVIDE;
  bNodeSocket *map_color = socket_out(*map, "Color");
  bNodeSocket *sep_vector = socket_in(*separate, "Vector");
  bNodeSocket *sep_x = socket_out(*separate, "X");
  bNodeSocket *sep_y = socket_out(*separate, "Y");
  bNodeSocket *sep_z = socket_out(*separate, "Z");
  bNodeSocket *xy_a = socket_in(*add_xy, "Value");
  bNodeSocket *xy_b = socket_in(*add_xy, "Value_001");
  bNodeSocket *z_a = socket_in(*add_z, "Value");
  bNodeSocket *z_b = socket_in(*add_z, "Value_001");
  bNodeSocket *d_a = socket_in(*divide, "Value");
  bNodeSocket *d_b = socket_in(*divide, "Value_001");
  if (map_color == nullptr || sep_vector == nullptr || sep_x == nullptr ||
  sep_y == nullptr || sep_z == nullptr || xy_a == nullptr || xy_b == nullptr ||
  z_a == nullptr || z_b == nullptr || d_a == nullptr || d_b == nullptr)
  {
return {nullptr, nullptr};
  }
  bke::node_add_link(tree, *map, *map_color, *separate, *sep_vector);
  bke::node_add_link(tree, *separate, *sep_x, *add_xy, *xy_a);
  bke::node_add_link(tree, *separate, *sep_y, *add_xy, *xy_b);
  bke::node_add_link(tree, *add_xy, *socket_out(*add_xy, "Value"), *add_z, *z_a);
  bke::node_add_link(tree, *separate, *sep_z, *add_z, *z_b);
  bke::node_add_link(tree, *add_z, *socket_out(*add_z, "Value"), *divide, *d_a);
  if (d_b->default_value != nullptr) {
static_cast<bNodeSocketValueFloat *>(d_b->default_value)->value = 3.0f;
  }
  return {divide, socket_out(*divide, "Value")};
}

void PaintLayersChainBuilder::build_content_correction(
    const MaterialPaintLayer *layer,
    const RowTarget &target,
    const bool substituted,
    const int channel,
    bNodeTree &tree,
    bNode *group_input,
    const float location_x,
    const float location_y,
    const bool track_content_alpha,
    ChainLayer &current,
    bNode *&content_cov_node,
    bNodeSocket *&content_cov,
    bNode *&folder_coverage_node,
    bNodeSocket *&folder_coverage)
{
  const Material &ma = outer_.ma_;
  const PaintLayersBuildContext &ctx = outer_.ctx_;
  auto &correction_opacity_inputs = outer_.correction_opacity_inputs_;
  auto &correction_fill_inputs = outer_.correction_fill_inputs_;
  auto &correction_fill_channel_inputs = outer_.correction_fill_channel_inputs_;
  auto &correction_live_constant_inputs = outer_.correction_live_constant_inputs_;
if (!substituted) {
  const bool normal_channel = channel == PAINT_MATERIAL_CHANNEL_NORMAL;
  for (const MaterialPaintLayer *effect : paint_layers_build_effects(ma, *layer)) {
    const MaterialPaintLayer &correction = *effect;
    if (!ELEM(correction.source,
              MA_PAINT_LAYER_SOURCE_IMAGE,
              MA_PAINT_LAYER_SOURCE_CONSTANT,
              MA_PAINT_LAYER_SOURCE_MESH_MAP,
              MA_PAINT_LAYER_SOURCE_MATERIAL,
              MA_PAINT_LAYER_SOURCE_NODE_GROUP,
              MA_PAINT_LAYER_SOURCE_STACK))
    {
      continue;
    }
    const bool fill = BKE_paint_layers_source_type(correction) ==
                      PaintLayerSourceType::Constant;
    /* A constant normal makes no sense; the CPU skips this correction as well. */
    if (normal_channel && fill) {
      continue;
    }
    bNodeSocket *correction_opacity = nullptr;
    if (Map<int, bNodeTreeInterfaceSocket *> *opacity_by_channel =
            correction_opacity_inputs.lookup_ptr(&correction))
    {
      if (bNodeTreeInterfaceSocket **opacity_iface =
              opacity_by_channel->lookup_ptr(channel))
      {
        correction_opacity = group_input_socket(group_input, **opacity_iface);
      }
    }
    /* [PL-DIAG] The chain order of a row's content corrections, in build order (the first is the
     * lowest). Prints only. */
    printf("[PL-DIAG] correction chain row='%s' channel=%d marker=%08x warm=%d fill=%d enabled=%d "
           "effective_opacity=%.3f opacity_input=%d\n",
           layer->name,
           channel,
           unsigned(correction.marker.time_low),
           int(paint_layers_warm_item_present(ma, correction.marker)),
           int(fill),
           int((correction.flag & MA_PAINT_LAYER_ENABLED) != 0),
           BKE_paint_layers_channel_opacity_effective(correction, channel),
           int(correction_opacity != nullptr));
    bNode *correction_source = nullptr;
    bNodeSocket *correction_color = nullptr;
    bNodeSocket *correction_alpha = nullptr;
    /* The node owning the colour socket: the group input for a Fill, the map for a Paint. */
    bNode *correction_color_node = nullptr;
    /* A Material correction's own coverage, exactly like a Layer row of that source
     * (#BKE_paint_layers_material_lives_from_source's caller reads `layer_factor` the same
     * way): the source's Alpha input decides its transparency, never the content channel's own
     * map alpha (a Material row has no coverage of its own -- see the comment on `content_cov`
     * below). Populated only for a Material correction; every other kind keeps using
     * `correction_alpha`, its own content map's alpha. */
    bNode *correction_material_coverage_node = nullptr;
    bNodeSocket *correction_material_coverage_socket = nullptr;
    /* Whether the effect's map is read as colour data. The Image Texture node only
     * un-premultiplies a non-data texture, so a data map needs the chain's own Divide (the
     * same rule the mask chain follows). */
    bool content_map_is_data = false;
    if (fill) {
      /* A live record's per-channel socket wins; every other channel reads the single socket,
       * which carries fill_color. A no-record Fill and a mask item have only the single socket. */
      bNodeTreeInterfaceSocket *fill_iface = nullptr;
      if (Map<int, bNodeTreeInterfaceSocket *> *fill_by_channel =
              correction_fill_channel_inputs.lookup_ptr(&correction))
      {
        if (bNodeTreeInterfaceSocket **channel_iface = fill_by_channel->lookup_ptr(channel)) {
          fill_iface = *channel_iface;
        }
      }
      if (fill_iface == nullptr) {
        if (bNodeTreeInterfaceSocket **legacy_iface =
                correction_fill_inputs.lookup_ptr(&correction))
        {
          fill_iface = *legacy_iface;
        }
      }
      if (fill_iface != nullptr) {
        correction_color = group_input_socket(group_input, *fill_iface);
        correction_color_node = group_input;
      }
      if (correction_color == nullptr) {
        continue;
      }
    }
    else if (correction.source == MA_PAINT_LAYER_SOURCE_MATERIAL) {
      /* An Effect correction with source Material behaves exactly as a Layer row of that
       * source: the same Baked/Hybrid/SourceGroup resolution, through the one helper both
       * share. */
      const RowMaterialSource row_source = outer_.resolve_row_material_source(
          correction, channel, tree, false);
      if (row_source.live_constant) {
        if (Map<int, bNodeTreeInterfaceSocket *> *live_by_channel =
                correction_live_constant_inputs.lookup_ptr(&correction))
        {
          if (bNodeTreeInterfaceSocket **live_iface = live_by_channel->lookup_ptr(channel)) {
            correction_color = group_input_socket(group_input, **live_iface);
            correction_color_node = group_input;
          }
        }
        if (correction_color == nullptr) {
          continue;
        }
      }
      else if (row_source.live_map) {
        bNode *map = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
        if (map == nullptr) {
          continue;
        }
        map->id = &row_source.live_map_image->id;
        id_us_plus(&row_source.live_map_image->id);
        map->location[0] = location_x;
        map->location[1] = location_y - 160.0f;
        if (NodeTexImage *dst = static_cast<NodeTexImage *>(map->storage)) {
          if (row_source.live_map_iuser != nullptr) {
            dst->iuser = *row_source.live_map_iuser;
          }
        }
        correction_color = socket_out(*map, "Color");
        correction_alpha = socket_out(*map, "Alpha");
        correction_color_node = map;
        correction_source = map;
        if (correction_color == nullptr) {
          continue;
        }
      }
      else if (row_source.source_group_instance != nullptr &&
               row_source.source_group_socket != nullptr)
      {
        /* The whole source graph goes through the wrapper's COLOR:<CHANNEL> output, exactly
         * like a Layer row in SourceGroup mode. */
        correction_color = row_source.source_group_socket;
        correction_color_node = row_source.source_group_instance;
      }
      else {
        /* Baked: the correction's own external bake, read through the same resolver a Layer
         * row's Baked mode uses. */
        Image *correction_image = paint_layer_channel_image(ma, correction, channel);
        if (correction_image == nullptr) {
          continue;
        }
        content_map_is_data = IMB_colormanagement_space_name_is_data(
            correction_image->colorspace_settings.name);
        correction_source = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
        if (correction_source == nullptr) {
          continue;
        }
        correction_source->id = &correction_image->id;
        id_us_plus(&correction_image->id);
        correction_source->location[0] = location_x;
        correction_source->location[1] = location_y - 160.0f;
        correction_color = socket_out(*correction_source, "Color");
        correction_alpha = socket_out(*correction_source, "Alpha");
        correction_color_node = correction_source;
        if (correction_color == nullptr) {
          continue;
        }
      }
      /* The correction's own coverage: the same Baked/Hybrid/SourceGroup precedence
       * #resolve_row_material_source uses for content, but read on the Alpha channel and
       * ending at the wrapper's dedicated COVERAGE output (SourceGroup) or the correction's
       * own bake coverage (Baked) -- mirrors `layer_factor` above (~2276-2356) exactly, with
       * `correction` standing in for `*layer`. */
      const RowMaterialSource alpha_source = outer_.resolve_row_material_source(
          correction, PAINT_MATERIAL_CHANNEL_ALPHA, tree, false);
      if (alpha_source.live_constant) {
        bNode *value = bke::node_add_static_node(nullptr, tree, SH_NODE_VALUE);
        bNodeSocket *value_out = (value != nullptr) ? socket_out(*value, "Value") : nullptr;
        if (value != nullptr && value_out != nullptr && value_out->default_value != nullptr) {
          value->location[0] = location_x - 90.0f;
          value->location[1] = location_y - 240.0f;
          static_cast<bNodeSocketValueFloat *>(value_out->default_value)->value =
              alpha_source.live_value[0];
          correction_material_coverage_node = value;
          correction_material_coverage_socket = value_out;
        }
      }
      else if (alpha_source.live_map) {
        bNode *map = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
        bNodeSocket *map_alpha = (map != nullptr) ? socket_out(*map, "Alpha") : nullptr;
        if (map != nullptr && map_alpha != nullptr) {
          map->id = &alpha_source.live_map_image->id;
          id_us_plus(&alpha_source.live_map_image->id);
          map->location[0] = location_x - 90.0f;
          map->location[1] = location_y - 240.0f;
          if (NodeTexImage *dst = static_cast<NodeTexImage *>(map->storage)) {
            if (alpha_source.live_map_iuser != nullptr) {
              dst->iuser = *alpha_source.live_map_iuser;
            }
          }
          correction_material_coverage_node = map;
          correction_material_coverage_socket = map_alpha;
        }
      }
      else if (alpha_source.source_group_instance != nullptr &&
               alpha_source.source_group_tree != nullptr)
      {
        bNodeSocket *coverage_out = source_group_output(*alpha_source.source_group_tree,
                                                         *alpha_source.source_group_instance,
                                                         PAINT_MATERIAL_CHANNEL_ALPHA,
                                                         true);
        if (coverage_out != nullptr) {
          correction_material_coverage_node = alpha_source.source_group_instance;
          correction_material_coverage_socket = coverage_out;
        }
        else if (correction.bake != nullptr && correction.bake->coverage != nullptr) {
          std::tie(correction_material_coverage_node, correction_material_coverage_socket) =
              build_grey_of_map(tree, location_x, location_y, *correction.bake->coverage, -240.0f);
        }
      }
      else if (correction.bake != nullptr && correction.bake->coverage != nullptr) {
        std::tie(correction_material_coverage_node, correction_material_coverage_socket) =
            build_grey_of_map(tree, location_x, location_y, *correction.bake->coverage, -240.0f);
      }
    }
    else if (correction.source == MA_PAINT_LAYER_SOURCE_NODE_GROUP) {
      /* A Node Group correction is Baked-only, exactly like a Layer row of that source: no
       * live path exists, so a correction with no valid bake yet takes no part (mirrors the
       * Custom row's own #custom_bake_missing_warn_once skip). */
      Image *correction_baked = nullptr;
      if (!BKE_paint_layers_bake_substitute(ma, correction, channel, &correction_baked) &&
          !BKE_paint_layers_bake_substitute_custom(
              ma, correction, channel, &correction_baked, nullptr))
      {
        continue;
      }
      if (correction_baked == nullptr) {
        continue;
      }
      correction_source = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
      if (correction_source == nullptr) {
        continue;
      }
      correction_source->id = &correction_baked->id;
      id_us_plus(&correction_baked->id);
      correction_source->location[0] = location_x;
      correction_source->location[1] = location_y - 160.0f;
      correction_color = socket_out(*correction_source, "Color");
      correction_alpha = socket_out(*correction_source, "Alpha");
      correction_color_node = correction_source;
      if (correction_color == nullptr) {
        continue;
      }
    }
    else if (correction.source == MA_PAINT_LAYER_SOURCE_STACK) {
      /* A Stack correction composites its own children in isolation, exactly like a Layer
       * folder composites its own (#build_list over `layer->children` above, ~1957-1993):
       * accumulate premultiplied colour and coverage starting from transparent, then the
       * same Vector Math Divide straightens the result to `S = P / a`. The straight colour
       * is this correction's content; the coverage stands in for a Material correction's own
       * Alpha input below (`correction_material_coverage_*`), never a content map's alpha --
       * a Stack correction has no map of its own, only its children's accumulated result. */
      bNode *p_zero = bke::node_add_static_node(nullptr, tree, SH_NODE_RGB);
      bNodeSocket *p_zero_out = (p_zero != nullptr) ? socket_out(*p_zero, "Color") : nullptr;
      if (p_zero_out == nullptr) {
        continue;
      }
      if (p_zero_out->default_value != nullptr) {
        copy_v4_fl(static_cast<bNodeSocketValueRGBA *>(p_zero_out->default_value)->value,
                   0.0f);
      }
      ChainLayer sub_previous;
      sub_previous.source_node = p_zero;
      sub_previous.source = p_zero_out;
      ChainResult sub = build_list(correction.children, sub_previous, true, target, channel);
      if (sub.chain.source == nullptr || sub.coverage == nullptr) {
        continue;
      }
      bNode *divide = bke::node_add_node(nullptr, tree, "ShaderNodeVectorMath"_ustr);
      bNodeSocket *div_a = (divide != nullptr) ? socket_in(*divide, "Vector") : nullptr;
      bNodeSocket *div_b = (divide != nullptr) ? socket_in(*divide, "Vector_001") : nullptr;
      bNodeSocket *div_out = (divide != nullptr) ? socket_out(*divide, "Vector") : nullptr;
      if (div_out == nullptr) {
        continue;
      }
      divide->custom1 = NODE_VECTOR_MATH_DIVIDE;
      divide->location[0] = location_x;
      divide->location[1] = location_y - 160.0f;
      bke::node_add_link(tree, *sub.chain.source_node, *sub.chain.source, *divide, *div_a);
      bke::node_add_link(tree, *sub.coverage_node, *sub.coverage, *divide, *div_b);
      correction_color = div_out;
      correction_color_node = divide;
      correction_material_coverage_node = sub.coverage_node;
      correction_material_coverage_socket = sub.coverage;
    }
    else {
      const bool correction_mesh_map = correction.source == MA_PAINT_LAYER_SOURCE_MESH_MAP;
      Image *correction_image = paint_layer_channel_image(ma, correction, channel);
      if (correction_image == nullptr) {
        continue;
      }
      /* Maps from files saved before #IMA_GPU_LINEAR_PREMUL existed, or assigned by hand,
       * get it here; its texture is rebuilt because the storage format changes. A MESH_MAP
       * atlas is a material-owned image, not a paint map, so it is left alone. */
      if (!correction_mesh_map && (correction_image->flag & IMA_GPU_LINEAR_PREMUL) == 0) {
        correction_image->flag |= IMA_GPU_LINEAR_PREMUL;
        BKE_image_free_gputextures(correction_image);
      }
      content_map_is_data = !correction_mesh_map &&
                            IMB_colormanagement_space_name_is_data(
                                correction_image->colorspace_settings.name);
      correction_source = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
      if (correction_source != nullptr) {
        correction_source->id = &correction_image->id;
        id_us_plus(&correction_image->id);
        correction_source->location[0] = location_x;
        correction_source->location[1] = location_y - 160.0f;
        if (correction_mesh_map) {
          if (NodeTexImage *storage = static_cast<NodeTexImage *>(
                  correction_source->storage))
          {
            storage->extension = SHD_IMAGE_EXTENSION_EXTEND;
          }
        }
        correction_color = socket_out(*correction_source, "Color");
        correction_alpha = socket_out(*correction_source, "Alpha");
        correction_color_node = correction_source;
        /* A scalar atlas spreads its R across RGB, matching the row and the CPU. */
        if (correction_mesh_map &&
            paint_layer_mesh_map_is_scalar(correction.mesh_map_type))
        {
          bNode *separate = bke::node_add_node(nullptr, tree, "ShaderNodeSeparateXYZ"_ustr);
          bNode *combine = bke::node_add_node(nullptr, tree, "ShaderNodeCombineXYZ"_ustr);
          if (separate != nullptr && combine != nullptr) {
            bNodeSocket *sep_vector = socket_in(*separate, "Vector");
            bNodeSocket *sep_x = socket_out(*separate, "X");
            bNodeSocket *combine_x = socket_in(*combine, "X");
            bNodeSocket *combine_y = socket_in(*combine, "Y");
            bNodeSocket *combine_z = socket_in(*combine, "Z");
            if (sep_vector != nullptr && sep_x != nullptr && combine_x != nullptr &&
                combine_y != nullptr && combine_z != nullptr)
            {
              separate->location[0] = location_x + 80.0f;
              separate->location[1] = location_y - 160.0f;
              combine->location[0] = location_x + 160.0f;
              combine->location[1] = location_y - 160.0f;
              bke::node_add_link(
                  tree, *correction_source, *correction_color, *separate, *sep_vector);
              bke::node_add_link(tree, *separate, *sep_x, *combine, *combine_x);
              bke::node_add_link(tree, *separate, *sep_x, *combine, *combine_y);
              bke::node_add_link(tree, *separate, *sep_x, *combine, *combine_z);
              correction_color_node = combine;
              correction_color = socket_out(*combine, "Vector");
            }
          }
        }
      }
      if (correction_color == nullptr) {
        continue;
      }
    }
    /* On the Normal channel the correction is the same Normal Combine the row itself uses;
     * elsewhere the row's own blend mode is a MixRGB. */
    bNode *correction_mix = nullptr;
    bNodeSocket *mix_color1 = nullptr;
    bNodeSocket *mix_color2 = nullptr;
    bNodeSocket *mix_fac = nullptr;
    bNodeSocket *mix_out = nullptr;
    if (normal_channel && !BKE_paint_layers_normal_replace(correction)) {
      if (ctx.normal_combine_group == nullptr || tree.typeinfo == nullptr ||
          tree.typeinfo->group_idname == nullptr)
      {
        continue;
      }
      correction_mix = bke::node_add_node(nullptr, tree, tree.typeinfo->group_idname);
      if (correction_mix == nullptr) {
        continue;
      }
      correction_mix->id = &ctx.normal_combine_group->id;
      id_us_plus(&ctx.normal_combine_group->id);
      correction_mix->location[0] = location_x + 120.0f;
      correction_mix->location[1] = location_y;
      /* A hand-assigned group only grows its instance sockets once its declaration is built;
       * this instantiates them from the group's interface without needing #Main or a whole
       * tree update. */
      nodes::update_node_declaration_and_sockets(tree, *correction_mix);
      mix_color1 = bke::node_find_socket(
          *correction_mix, SOCK_IN, UString::from_ptr_noinline(NORMAL_COMBINE_ID_A));
      mix_color2 = bke::node_find_socket(
          *correction_mix, SOCK_IN, UString::from_ptr_noinline(NORMAL_COMBINE_ID_B));
      mix_fac = bke::node_find_socket(
          *correction_mix, SOCK_IN, UString::from_ptr_noinline(NORMAL_COMBINE_ID_FACTOR));
      mix_out = bke::node_find_socket(
          *correction_mix, SOCK_OUT, UString::from_ptr_noinline(NORMAL_COMBINE_ID_RESULT));
    }
    else {
      correction_mix = mix_node_add(tree,
                                    BKE_paint_layers_blend_to_ramp(
                                        eMaterialPaintLayerBlend(BKE_paint_layers_channel_blend_effective(correction, channel))),
                                    location_x + 120.0f,
                                    location_y);
      if (correction_mix == nullptr) {
        continue;
      }
      mix_color1 = socket_in(*correction_mix, "A_Color");
      mix_color2 = socket_in(*correction_mix, "B_Color");
      mix_fac = socket_in(*correction_mix, "Factor_Float");
      mix_out = socket_out(*correction_mix, "Result_Color");
    }
    STRNCPY_UTF8(correction_mix->label, correction.name);
    if (mix_color1 == nullptr || mix_color2 == nullptr || mix_fac == nullptr ||
        mix_out == nullptr)
    {
      /* The sockets are the node's own declaration; a missing one is a build error. */
      BLI_assert_msg(false, "blend node sockets were not declared");
      continue;
    }
    /* The Normal Combine decodes its A input as a tangent normal, but wherever the row has no
     * content yet its colour is the map's empty black, which decodes to `(-1, -1, -1)` and drags
     * every correction laid over such a texel into a black result. There the layer holds no normal
     * at all, so the input falls back to the flat one by the row's coverage before this correction
     * (the same `alpha` the CPU composite starts from). A row that covers fully needs nothing. */
    bNode *below_node = current.source_node;
    bNodeSocket *below_socket = current.source;
    if (normal_channel && !BKE_paint_layers_normal_replace(correction)) {
      const bool folder_row = BKE_paint_layers_is_folder(*layer);
      bNode *base_coverage_node = folder_row ? folder_coverage_node : content_cov_node;
      bNodeSocket *base_coverage = folder_row ? folder_coverage : content_cov;
      if (base_coverage_node != nullptr && base_coverage != nullptr) {
        bNode *flat_fallback = mix_node_add(tree, MA_RAMP_BLEND, location_x + 80.0f, location_y);
        bNodeSocket *flat_color = (flat_fallback != nullptr) ? socket_in(*flat_fallback, "A_Color") :
                                                               nullptr;
        bNodeSocket *content_color = (flat_fallback != nullptr) ?
                                         socket_in(*flat_fallback, "B_Color") :
                                         nullptr;
        bNodeSocket *coverage_fac = (flat_fallback != nullptr) ?
                                        socket_in(*flat_fallback, "Factor_Float") :
                                        nullptr;
        bNodeSocket *fallback_out = (flat_fallback != nullptr) ?
                                        socket_out(*flat_fallback, "Result_Color") :
                                        nullptr;
        if (flat_color != nullptr && content_color != nullptr && coverage_fac != nullptr &&
            fallback_out != nullptr && flat_color->default_value != nullptr)
        {
          const float flat_normal[4] = {0.5f, 0.5f, 1.0f, 1.0f};
          copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(flat_color->default_value)->value,
                     flat_normal);
          bke::node_add_link(tree, *current.source_node, *current.source, *flat_fallback, *content_color);
          bke::node_add_link(tree, *base_coverage_node, *base_coverage, *flat_fallback, *coverage_fac);
          below_node = flat_fallback;
          below_socket = fallback_out;
        }
      }
    }
    bke::node_add_link(tree, *below_node, *below_socket, *correction_mix, *mix_color1);
    /* A data map reaches the chain as `C * A` (the upload pre-multiplied it and the Image
     * Texture node leaves a data texture alone); divide by the map's alpha to get `C`, so the
     * coverage is applied once, exactly as the CPU reads the straight bytes. A non-data map
     * is already straight: the node un-premultiplied it, so nothing is built. */
    if (!fill && content_map_is_data && correction_source != nullptr &&
        correction_alpha != nullptr)
    {
      bNode *combine = bke::node_add_node(nullptr, tree, "ShaderNodeCombineXYZ"_ustr);
      bNode *straighten = bke::node_add_static_node(nullptr, tree, SH_NODE_VECTOR_MATH);
      if (combine != nullptr && straighten != nullptr) {
        bNodeSocket *combine_x = socket_in(*combine, "X");
        bNodeSocket *combine_y = socket_in(*combine, "Y");
        bNodeSocket *combine_z = socket_in(*combine, "Z");
        bNodeSocket *combine_out = socket_out(*combine, "Vector");
        bNodeSocket *vector_in = socket_in(*straighten, "Vector");
        bNodeSocket *divisor_in = socket_in(*straighten, "Vector_001");
        bNodeSocket *vector_out = socket_out(*straighten, "Vector");
        if (combine_x != nullptr && combine_y != nullptr && combine_z != nullptr &&
            combine_out != nullptr && vector_in != nullptr && divisor_in != nullptr &&
            vector_out != nullptr)
        {
          straighten->custom1 = NODE_VECTOR_MATH_DIVIDE;
          combine->location[0] = location_x;
          combine->location[1] = location_y - 240.0f;
          straighten->location[0] = location_x + 40.0f;
          straighten->location[1] = location_y - 160.0f;
          bke::node_add_link(
              tree, *correction_source, *correction_alpha, *combine, *combine_x);
          bke::node_add_link(
              tree, *correction_source, *correction_alpha, *combine, *combine_y);
          bke::node_add_link(
              tree, *correction_source, *correction_alpha, *combine, *combine_z);
          bke::node_add_link(
              tree, *correction_source, *correction_color, *straighten, *vector_in);
          bke::node_add_link(
              tree, *combine, *combine_out, *straighten, *divisor_in);
          correction_color_node = straighten;
          correction_color = vector_out;
        }
      }
    }
    /* The colour arrives from a group input (Fill) or a map (Paint). */
    if (correction_color_node != nullptr) {
      bke::node_add_link(
          tree, *correction_color_node, *correction_color, *correction_mix, *mix_color2);
    }
    bNodeSocket *correction_coverage_socket = nullptr;
    bNode *correction_coverage_node = nullptr;
    if (correction_opacity != nullptr && group_input != nullptr) {
      /* A Material or Stack correction's per-pixel coverage is its own Alpha-channel
       * resolution or its subtree's accumulated coverage (`correction_material_coverage_*`,
       * built above like `layer_factor`), never the content channel's own map alpha -- neither
       * has a content map of its own (see the comment on `content_cov`). Every other kind uses
       * its content map's alpha (`correction_alpha`) the way it always did. A Fill correction's
       * factor is its opacity alone, with no per-pixel term to scale it; a Material or Stack
       * correction with none of its own (Baked with no bake coverage yet, or an empty subtree)
       * behaves the same way for this purpose. */
      const bool material_correction = ELEM(
          correction.source, MA_PAINT_LAYER_SOURCE_MATERIAL, MA_PAINT_LAYER_SOURCE_STACK);
      bNodeSocket *coverage_map_socket = material_correction ? correction_material_coverage_socket :
                                                               correction_alpha;
      bNode *coverage_map_node = material_correction ? correction_material_coverage_node :
                                                        correction_source;
      if (fill || coverage_map_socket == nullptr || coverage_map_node == nullptr) {
        /* A flat correction's factor is its opacity; it covers fully. */
        correction_coverage_socket = correction_opacity;
        correction_coverage_node = group_input;
        bke::node_add_link(
            tree, *group_input, *correction_opacity, *correction_mix, *mix_fac);
      }
      else {
        bNode *correction_factor = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
        if (correction_factor == nullptr) {
          continue;
        }
        correction_factor->custom1 = NODE_MATH_MULTIPLY;
        correction_factor->location[0] = location_x + 40.0f;
        correction_factor->location[1] = location_y + 160.0f;
        bNodeSocket *factor_value = socket_in(*correction_factor, "Value");
        bNodeSocket *factor_coverage = socket_in(*correction_factor, "Value_001");
        bNodeSocket *factor_out = socket_out(*correction_factor, "Value");
        if (factor_value == nullptr || factor_coverage == nullptr || factor_out == nullptr) {
          continue;
        }
        bke::node_add_link(
            tree, *group_input, *correction_opacity, *correction_factor, *factor_value);
        bke::node_add_link(
            tree, *coverage_map_node, *coverage_map_socket, *correction_factor, *factor_coverage);
        bke::node_add_link(
            tree, *correction_factor, *factor_out, *correction_mix, *mix_fac);
        correction_coverage_socket = factor_out;
        correction_coverage_node = correction_factor;
      }
    }
    /* A content correction changes the colour and, by the over model, the coverage the row
     * lays with: coverage = coverage + f * (1 - coverage). The base is the row's own
     * coverage: a folder's isolated result (design §5), or a leaf's mask map. A leaf with no
     * mask map covers fully, so there the update is the identity and nothing is built. */
    if (correction_coverage_socket != nullptr && correction_coverage_node != nullptr) {
      bNode *coverage_base_node = nullptr;
      bNodeSocket *coverage_base_socket = nullptr;
      const bool folder = BKE_paint_layers_is_folder(*layer);
      if (folder) {
        coverage_base_node = folder_coverage_node;
        coverage_base_socket = folder_coverage;
      }
      else {
        /* A leaf with no map alpha covers fully, so the update is the identity and nothing
         * is built; the mask is not part of this base, it multiplies the result below. */
        coverage_base_node = content_cov_node;
        coverage_base_socket = content_cov;
      }
      if (coverage_base_node != nullptr && coverage_base_socket != nullptr) {
        bNode *one_minus = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
        bNode *scaled = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
        bNode *joined = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
        if (one_minus != nullptr && scaled != nullptr && joined != nullptr) {
          one_minus->custom1 = NODE_MATH_SUBTRACT;
          scaled->custom1 = NODE_MATH_MULTIPLY;
          joined->custom1 = NODE_MATH_ADD;
          one_minus->location[0] = location_x + 150.0f;
          one_minus->location[1] = location_y - 420.0f;
          scaled->location[0] = location_x + 230.0f;
          scaled->location[1] = location_y - 420.0f;
          joined->location[0] = location_x + 310.0f;
          joined->location[1] = location_y - 420.0f;
          bNodeSocket *om_a = socket_in(*one_minus, "Value");
          bNodeSocket *om_b = socket_in(*one_minus, "Value_001");
          bNodeSocket *sc_a = socket_in(*scaled, "Value");
          bNodeSocket *sc_b = socket_in(*scaled, "Value_001");
          bNodeSocket *jo_a = socket_in(*joined, "Value");
          bNodeSocket *jo_b = socket_in(*joined, "Value_001");
          if (om_a != nullptr && om_b != nullptr && sc_a != nullptr && sc_b != nullptr &&
              jo_a != nullptr && jo_b != nullptr)
          {
            if (om_a->default_value != nullptr) {
              static_cast<bNodeSocketValueFloat *>(om_a->default_value)->value = 1.0f;
            }
            bke::node_add_link(
                tree, *coverage_base_node, *coverage_base_socket, *one_minus, *om_b);
            bke::node_add_link(
                tree, *correction_coverage_node, *correction_coverage_socket, *scaled, *sc_a);
            bke::node_add_link(tree,
                               *one_minus,
                               *socket_out(*one_minus, "Value"),
                               *scaled,
                               *sc_b);
            bke::node_add_link(
                tree, *coverage_base_node, *coverage_base_socket, *joined, *jo_a);
            bke::node_add_link(
                tree, *scaled, *socket_out(*scaled, "Value"), *joined, *jo_b);
            if (folder) {
              folder_coverage_node = joined;
              folder_coverage = socket_out(*joined, "Value");
            }
            else {
              content_cov_node = joined;
              content_cov = socket_out(*joined, "Value");
            }
          }
        }
      }
    }
    /* The content alpha follows the colour that was just corrected: a content correction
     * raises the row's alpha by the over model, `a = a + fac * (1 - a)`, exactly as the CPU
     * folds it while it blends the correction in (#composite_layer_render). The fac is the
     * same `opacity * A` the coverage update above uses. A row that tracks no content alpha
     * (Material, Normal, a channel outside the image-paint set) builds nothing. */
    if (track_content_alpha && current.content_alpha != nullptr &&
        current.content_alpha_node != nullptr &&
        correction_coverage_socket != nullptr && correction_coverage_node != nullptr)
    {
      bNode *alpha_one_minus = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
      bNode *alpha_scaled = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
      bNode *alpha_joined = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
      if (alpha_one_minus != nullptr && alpha_scaled != nullptr && alpha_joined != nullptr) {
        bNodeSocket *aom_a = socket_in(*alpha_one_minus, "Value");
        bNodeSocket *aom_b = socket_in(*alpha_one_minus, "Value_001");
        bNodeSocket *asc_a = socket_in(*alpha_scaled, "Value");
        bNodeSocket *asc_b = socket_in(*alpha_scaled, "Value_001");
        bNodeSocket *ajo_a = socket_in(*alpha_joined, "Value");
        bNodeSocket *ajo_b = socket_in(*alpha_joined, "Value_001");
        if (aom_a != nullptr && aom_b != nullptr && asc_a != nullptr && asc_b != nullptr &&
            ajo_a != nullptr && ajo_b != nullptr)
        {
          alpha_one_minus->custom1 = NODE_MATH_SUBTRACT;
          alpha_scaled->custom1 = NODE_MATH_MULTIPLY;
          alpha_joined->custom1 = NODE_MATH_ADD;
          alpha_one_minus->location[0] = location_x + 150.0f;
          alpha_one_minus->location[1] = location_y - 480.0f;
          alpha_scaled->location[0] = location_x + 230.0f;
          alpha_scaled->location[1] = location_y - 480.0f;
          alpha_joined->location[0] = location_x + 310.0f;
          alpha_joined->location[1] = location_y - 480.0f;
          if (aom_a->default_value != nullptr) {
            static_cast<bNodeSocketValueFloat *>(aom_a->default_value)->value = 1.0f;
          }
          bke::node_add_link(tree,
                             *current.content_alpha_node,
                             *current.content_alpha,
                             *alpha_one_minus,
                             *aom_b);
          bke::node_add_link(tree,
                             *correction_coverage_node,
                             *correction_coverage_socket,
                             *alpha_scaled,
                             *asc_a);
          bke::node_add_link(tree,
                             *alpha_one_minus,
                             *socket_out(*alpha_one_minus, "Value"),
                             *alpha_scaled,
                             *asc_b);
          bke::node_add_link(tree,
                             *current.content_alpha_node,
                             *current.content_alpha,
                             *alpha_joined,
                             *ajo_a);
          bke::node_add_link(tree,
                             *alpha_scaled,
                             *socket_out(*alpha_scaled, "Value"),
                             *alpha_joined,
                             *ajo_b);
          current.content_alpha_node = alpha_joined;
          current.content_alpha = socket_out(*alpha_joined, "Value");
        }
      }
    }
    current.source_node = correction_mix;
    current.source = mix_out;
  }
}
}

bNodeSocket *PaintLayersChainBuilder::group_input_socket(bNode *group_input,
                                                               const bNodeTreeInterfaceSocket &iface)
{
  if (group_input == nullptr || iface.identifier == nullptr) {
    return nullptr;
  }
  return bke::node_find_socket(
      *group_input, SOCK_OUT, UString::from_ptr_noinline(iface.identifier));
}


std::pair<bNode *, bNodeSocket *> PaintLayersChainBuilder::resolve_correction_coverage(
    bNodeTree &tree, const float location_x, const float location_y, const MaterialPaintLayer &row)
{
  if (row.source == MA_PAINT_LAYER_SOURCE_NODE_GROUP) {
    /* A Node Group has no live path at all (it is Baked-only); its "coverage" is its own
     * bake coverage, the same map a substituted row's factor would read. */
    if (row.bake != nullptr && row.bake->coverage != nullptr) {
      return build_grey_of_map(tree, location_x, location_y, *row.bake->coverage, -240.0f);
    }
    return {nullptr, nullptr};
  }
  const RowMaterialSource alpha_source = outer_.resolve_row_material_source(
      row, PAINT_MATERIAL_CHANNEL_ALPHA, tree, false);
  if (alpha_source.live_constant) {
    bNode *value = bke::node_add_static_node(nullptr, tree, SH_NODE_VALUE);
    bNodeSocket *value_out = (value != nullptr) ? socket_out(*value, "Value") : nullptr;
    if (value != nullptr && value_out != nullptr && value_out->default_value != nullptr) {
      value->location[0] = location_x - 90.0f;
      value->location[1] = location_y - 240.0f;
      static_cast<bNodeSocketValueFloat *>(value_out->default_value)->value =
          alpha_source.live_value[0];
      return {value, value_out};
    }
    return {nullptr, nullptr};
  }
  if (alpha_source.live_map) {
    bNode *map = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
    bNodeSocket *map_alpha = (map != nullptr) ? socket_out(*map, "Alpha") : nullptr;
    if (map != nullptr && map_alpha != nullptr) {
      map->id = &alpha_source.live_map_image->id;
      id_us_plus(&alpha_source.live_map_image->id);
      map->location[0] = location_x - 90.0f;
      map->location[1] = location_y - 240.0f;
      if (NodeTexImage *dst = static_cast<NodeTexImage *>(map->storage)) {
        if (alpha_source.live_map_iuser != nullptr) {
          dst->iuser = *alpha_source.live_map_iuser;
        }
      }
      return {map, map_alpha};
    }
    return {nullptr, nullptr};
  }
  if (alpha_source.source_group_instance != nullptr &&
      alpha_source.source_group_tree != nullptr)
  {
    bNodeSocket *coverage_out = source_group_output(*alpha_source.source_group_tree,
                                                     *alpha_source.source_group_instance,
                                                     PAINT_MATERIAL_CHANNEL_ALPHA,
                                                     true);
    if (coverage_out != nullptr) {
      return {alpha_source.source_group_instance, coverage_out};
    }
  }
  if (row.bake != nullptr && row.bake->coverage != nullptr) {
    return build_grey_of_map(tree, location_x, location_y, *row.bake->coverage, -240.0f);
  }
  return {nullptr, nullptr};
}

/* The per-pixel coverage the mask stack builds on: the Material source coverage when there is
 * one, one otherwise. Null until an item needs it. */
void PaintLayersChainBuilder::ensure_factor_base(bNodeTree &tree,
                                                 const float location_x,
                                                 const float location_y,
                                                 bNode *&layer_factor_node,
                                                 bNodeSocket *&layer_factor_socket)
{
  if (layer_factor_socket != nullptr) {
    return;
  }
  bNode *white = bke::node_add_static_node(nullptr, tree, SH_NODE_RGB);
  if (white != nullptr) {
    white->location[0] = location_x - 60.0f;
    white->location[1] = location_y - 320.0f;
    bNodeSocket *white_out = socket_out(*white, "Color");
    if (white_out != nullptr && white_out->default_value != nullptr) {
      static_cast<bNodeSocketValueRGBA *>(white_out->default_value)->value[0] = 1.0f;
      static_cast<bNodeSocketValueRGBA *>(white_out->default_value)->value[1] = 1.0f;
      static_cast<bNodeSocketValueRGBA *>(white_out->default_value)->value[2] = 1.0f;
      static_cast<bNodeSocketValueRGBA *>(white_out->default_value)->value[3] = 1.0f;
    }
    layer_factor_node = white;
    layer_factor_socket = white_out;
  }
}

void PaintLayersChainBuilder::build_mask_item(
    const MaterialPaintLayer *layer,
    const int channel,
    bNodeTree &tree,
    bNode *group_input,
    const float location_x,
    const float location_y,
    const RowTarget &target,
    ChainLayer &current,
    bNode *&layer_factor_node,
    bNodeSocket *&layer_factor_socket)
{
  const Material &ma = outer_.ma_;
  auto &correction_opacity_inputs = outer_.correction_opacity_inputs_;
  auto &correction_fill_inputs = outer_.correction_fill_inputs_;
  auto &correction_live_constant_inputs = outer_.correction_live_constant_inputs_;
if (current.opacity != nullptr)
{
  const Vector<const MaterialPaintLayer *> mask_items = paint_layers_build_mask_items(ma, *layer);
  const bool has_mask_corrections = !mask_items.is_empty();
  if (has_mask_corrections) {
    ensure_factor_base(tree, location_x, location_y, layer_factor_node, layer_factor_socket);
    bNode *factor_node = layer_factor_node;
    bNodeSocket *factor_socket = layer_factor_socket;
    for (const MaterialPaintLayer *mask_item : mask_items) {
      const MaterialPaintLayer &correction = *mask_item;
      const bool fill = BKE_paint_layers_source_type(correction) ==
                        PaintLayerSourceType::Constant;
      const bool mask_material_or_group = ELEM(correction.source,
                                                MA_PAINT_LAYER_SOURCE_MATERIAL,
                                                MA_PAINT_LAYER_SOURCE_NODE_GROUP,
                                                MA_PAINT_LAYER_SOURCE_STACK);
      /* A Normal mask channel has no visual meaning to reduce to one number (it is a
       * tangent-space direction, not a scalar or a colour); the item is quietly skipped,
       * exactly like a Node Group correction with no bake yet. */
      if (mask_material_or_group &&
          correction.mask_channel == PAINT_MATERIAL_CHANNEL_NORMAL)
      {
        continue;
      }
      bNodeSocket *correction_opacity = nullptr;
      if (Map<int, bNodeTreeInterfaceSocket *> *opacity_by_channel =
              correction_opacity_inputs.lookup_ptr(&correction))
      {
        if (bNodeTreeInterfaceSocket **opacity_iface =
                opacity_by_channel->lookup_ptr(channel))
        {
          correction_opacity = group_input_socket(group_input, **opacity_iface);
        }
      }
      /* The raw Color whose mean is the correction's color `C`: the Fill constant or the
       * map's Color output read as-is. */
      bNode *gray_source_node = nullptr;
      bNodeSocket *gray_source_color = nullptr;
      bNode *correction_map = nullptr;
      bNodeSocket *correction_alpha = nullptr;
      /* Whether the map is read as colour data. The Image Texture node only un-premultiplies
       * a premultiplied texture when its colorspace is not data (node_shader_tex_image.cc),
       * so only a data map still needs the chain's own Divide. */
      bool mask_map_is_data = false;
      bool mask_map_is_mesh = false;
      /* A Material/Node Group mask's own coverage (its source's Alpha), independent of the
       * grey it lays over the factor -- reused as-is when `mask_channel` is Alpha (the grey
       * itself is this number) or as the item's Fac multiplier otherwise. */
      bNode *correction_coverage_node = nullptr;
      bNodeSocket *correction_coverage_socket = nullptr;
      if (fill) {
        if (bNodeTreeInterfaceSocket **fill_iface =
                correction_fill_inputs.lookup_ptr(&correction))
        {
          gray_source_color = group_input_socket(group_input, **fill_iface);
          gray_source_node = group_input;
        }
        if (gray_source_color == nullptr) {
          continue;
        }
      }
      else if (mask_material_or_group) {
        const bool mask_alpha_channel = correction.mask_channel ==
                                        PAINT_MATERIAL_CHANNEL_ALPHA;
        if (correction.source == MA_PAINT_LAYER_SOURCE_STACK) {
          /* A Stack mask item reads its own children's accumulated result, exactly like a
           * Stack content correction and a Layer folder's own children (#build_list,
           * mirrors ~1957-1993 and the content correction's own Stack branch above): straight
           * colour + coverage, starting from transparent. The subtree's coverage is this
           * item's Fac multiplier on every channel (below) and, on the Alpha channel, the
           * grey itself -- there is no separate content map to read.
           *
           * The subtree builds in a fixed channel, never in the owner's current `channel`: on
           * the Alpha convention it always reads Base Color (exactly like every other Paint
           * mask -- #paint_layer_mask_correction_image always reads Base Color too), and
           * otherwise it reads `mask_channel` itself. Both are constant across every channel
           * the owner row generates, or the same mask would answer differently depending on
           * which of the owner's own channels happens to be built at the time -- the bug
           * #stack_mask_fixed_channel_applies_to_every_owner_channel_matches_the_cpu and
           * #stack_mask_alpha_reads_base_color_on_every_owner_channel_matches_the_cpu regress. */
          const int mask_build_channel = mask_alpha_channel ?
                                             int(PAINT_MATERIAL_CHANNEL_BASE_COLOR) :
                                             int(correction.mask_channel);
          bNode *p_zero = bke::node_add_static_node(nullptr, tree, SH_NODE_RGB);
          bNodeSocket *p_zero_out = (p_zero != nullptr) ? socket_out(*p_zero, "Color") :
                                                          nullptr;
          if (p_zero_out == nullptr) {
            continue;
          }
          if (p_zero_out->default_value != nullptr) {
            copy_v4_fl(
                static_cast<bNodeSocketValueRGBA *>(p_zero_out->default_value)->value, 0.0f);
          }
          ChainLayer sub_previous;
          sub_previous.source_node = p_zero;
          sub_previous.source = p_zero_out;
          ChainResult sub = build_list(
              correction.children, sub_previous, true, target, mask_build_channel);
          if (sub.chain.source == nullptr || sub.coverage == nullptr) {
            continue;
          }
          bNode *divide = bke::node_add_node(nullptr, tree, "ShaderNodeVectorMath"_ustr);
          bNodeSocket *div_a = (divide != nullptr) ? socket_in(*divide, "Vector") : nullptr;
          bNodeSocket *div_b = (divide != nullptr) ? socket_in(*divide, "Vector_001") :
                                                     nullptr;
          bNodeSocket *div_out = (divide != nullptr) ? socket_out(*divide, "Vector") :
                                                       nullptr;
          if (div_out == nullptr) {
            continue;
          }
          divide->custom1 = NODE_VECTOR_MATH_DIVIDE;
          divide->location[0] = location_x - 220.0f;
          divide->location[1] = location_y - 320.0f;
          bke::node_add_link(
              tree, *sub.chain.source_node, *sub.chain.source, *divide, *div_a);
          bke::node_add_link(tree, *sub.coverage_node, *sub.coverage, *divide, *div_b);
          correction_coverage_node = sub.coverage_node;
          correction_coverage_socket = sub.coverage;
          if (!mask_alpha_channel) {
            const bool mask_is_color = BKE_paint_material_channel_info(
                                            eMaterialPaintChannel(correction.mask_channel))
                                            .is_color;
            if (mask_is_color) {
              bNode *rgbtobw = bke::node_add_static_node(nullptr, tree, SH_NODE_RGBTOBW);
              bNodeSocket *bw_in = (rgbtobw != nullptr) ? socket_in(*rgbtobw, "Color") :
                                                          nullptr;
              bNodeSocket *bw_out = (rgbtobw != nullptr) ? socket_out(*rgbtobw, "Val") :
                                                           nullptr;
              if (bw_in == nullptr || bw_out == nullptr) {
                continue;
              }
              rgbtobw->location[0] = location_x - 140.0f;
              rgbtobw->location[1] = location_y - 320.0f;
              bke::node_add_link(tree, *divide, *div_out, *rgbtobw, *bw_in);
              gray_source_node = rgbtobw;
              gray_source_color = bw_out;
            }
            else {
              bNode *separate = bke::node_add_node(nullptr, tree, "ShaderNodeSeparateXYZ"_ustr);
              bNodeSocket *sep_vector = (separate != nullptr) ?
                                            socket_in(*separate, "Vector") :
                                            nullptr;
              bNodeSocket *sep_x = (separate != nullptr) ? socket_out(*separate, "X") :
                                                           nullptr;
              if (sep_vector == nullptr || sep_x == nullptr) {
                continue;
              }
              bke::node_add_link(tree, *divide, *div_out, *separate, *sep_vector);
              gray_source_node = separate;
              gray_source_color = sep_x;
            }
          }
        }
        else if (mask_alpha_channel) {
          /* The channel itself is the source's coverage: no separate grey/coverage split. */
          std::tie(correction_coverage_node, correction_coverage_socket) =
              resolve_correction_coverage(tree, location_x, location_y, correction);
          if (correction_coverage_node == nullptr || correction_coverage_socket == nullptr) {
            continue;
          }
        }
        else {
          const bool mask_is_color = BKE_paint_material_channel_info(
                                          eMaterialPaintChannel(correction.mask_channel))
                                          .is_color;
          bNode *value_node = nullptr;
          bNodeSocket *value_socket = nullptr;
          if (correction.source == MA_PAINT_LAYER_SOURCE_MATERIAL) {
            const RowMaterialSource row_source = outer_.resolve_row_material_source(
                correction, correction.mask_channel, tree, false);
            if (row_source.live_constant) {
              if (Map<int, bNodeTreeInterfaceSocket *> *live_by_channel =
                      correction_live_constant_inputs.lookup_ptr(&correction))
              {
                if (bNodeTreeInterfaceSocket **live_iface =
                        live_by_channel->lookup_ptr(correction.mask_channel))
                {
                  value_socket = group_input_socket(group_input, **live_iface);
                  value_node = group_input;
                }
              }
            }
            else if (row_source.live_map) {
              bNode *map = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
              if (map != nullptr) {
                map->id = &row_source.live_map_image->id;
                id_us_plus(&row_source.live_map_image->id);
                map->location[0] = location_x - 220.0f;
                map->location[1] = location_y - 320.0f;
                if (NodeTexImage *dst = static_cast<NodeTexImage *>(map->storage)) {
                  if (row_source.live_map_iuser != nullptr) {
                    dst->iuser = *row_source.live_map_iuser;
                  }
                }
                value_socket = socket_out(*map, "Color");
                value_node = map;
              }
            }
            else if (row_source.source_group_instance != nullptr &&
                     row_source.source_group_socket != nullptr)
            {
              value_socket = row_source.source_group_socket;
              value_node = row_source.source_group_instance;
            }
            else {
              Image *mask_image = paint_layer_channel_image(
                  ma, correction, correction.mask_channel);
              if (mask_image != nullptr) {
                bNode *map = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
                if (map != nullptr) {
                  map->id = &mask_image->id;
                  id_us_plus(&mask_image->id);
                  map->location[0] = location_x - 220.0f;
                  map->location[1] = location_y - 320.0f;
                  value_socket = socket_out(*map, "Color");
                  value_node = map;
                }
              }
            }
          }
          else {
            /* Node Group: Baked-only, like the content correction of the same source. */
            Image *mask_baked = nullptr;
            if (BKE_paint_layers_bake_substitute(
                    ma, correction, correction.mask_channel, &mask_baked) ||
                BKE_paint_layers_bake_substitute_custom(
                    ma, correction, correction.mask_channel, &mask_baked, nullptr))
            {
              if (mask_baked != nullptr) {
                bNode *map = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
                if (map != nullptr) {
                  map->id = &mask_baked->id;
                  id_us_plus(&mask_baked->id);
                  map->location[0] = location_x - 220.0f;
                  map->location[1] = location_y - 320.0f;
                  value_socket = socket_out(*map, "Color");
                  value_node = map;
                }
              }
            }
          }
          if (value_socket == nullptr || value_node == nullptr) {
            continue;
          }
          if (mask_is_color) {
            bNode *rgbtobw = bke::node_add_static_node(nullptr, tree, SH_NODE_RGBTOBW);
            bNodeSocket *bw_in = (rgbtobw != nullptr) ? socket_in(*rgbtobw, "Color") :
                                                        nullptr;
            bNodeSocket *bw_out = (rgbtobw != nullptr) ? socket_out(*rgbtobw, "Val") :
                                                         nullptr;
            if (bw_in == nullptr || bw_out == nullptr) {
              continue;
            }
            rgbtobw->location[0] = location_x - 140.0f;
            rgbtobw->location[1] = location_y - 320.0f;
            bke::node_add_link(tree, *value_node, *value_socket, *rgbtobw, *bw_in);
            gray_source_node = rgbtobw;
            gray_source_color = bw_out;
          }
          else {
            /* A scalar channel's map or constant stores its value spread across R=G=B
             * (#socket_default_to_constant's SOCK_FLOAT case, and every scalar bake/AOV);
             * Separate X reads it directly, exactly like a MESH_MAP scalar atlas. */
            bNode *separate = bke::node_add_node(nullptr, tree, "ShaderNodeSeparateXYZ"_ustr);
            bNodeSocket *sep_vector = (separate != nullptr) ? socket_in(*separate, "Vector") :
                                                              nullptr;
            bNodeSocket *sep_x = (separate != nullptr) ? socket_out(*separate, "X") : nullptr;
            if (sep_vector == nullptr || sep_x == nullptr) {
              continue;
            }
            bke::node_add_link(tree, *value_node, *value_socket, *separate, *sep_vector);
            gray_source_node = separate;
            gray_source_color = sep_x;
          }
          /* This item's own Fac multiplier: the source's coverage, resolved once more (its
           * own Alpha channel, independent of `mask_channel`). */
          std::tie(correction_coverage_node, correction_coverage_socket) =
              resolve_correction_coverage(tree, location_x, location_y, correction);
        }
      }
      else {
        Image *correction_image = paint_layer_mask_correction_image(
            ma, correction, channel);
        if (correction_image == nullptr) {
          continue;
        }
        mask_map_is_data = IMB_colormanagement_space_name_is_data(
            correction_image->colorspace_settings.name);
        correction_map = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
        if (correction_map == nullptr) {
          continue;
        }
        correction_map->id = &correction_image->id;
        id_us_plus(&correction_image->id);
        correction_map->location[0] = location_x - 220.0f;
        correction_map->location[1] = location_y - 320.0f;
        /* A MESH_MAP mask item reads the atlas R, the same value the CPU reads, and is
         * sampled Extend like every other atlas read. */
        mask_map_is_mesh = correction.source == MA_PAINT_LAYER_SOURCE_MESH_MAP;
        if (mask_map_is_mesh) {
          if (NodeTexImage *storage = static_cast<NodeTexImage *>(correction_map->storage)) {
            storage->extension = SHD_IMAGE_EXTENSION_EXTEND;
          }
        }
        correction_alpha = socket_out(*correction_map, "Alpha");
        gray_source_color = socket_out(*correction_map, "Color");
        gray_source_node = correction_map;
      }
      bNode *correction_gray_node = nullptr;
      bNodeSocket *correction_gray = nullptr;
      if (mask_material_or_group) {
        /* Already reduced to one number above: the coverage itself (Alpha channel) or the
         * RGBTOBW/Separate-X result (colour/scalar channel) -- no further mean/Divide here. */
        if (correction.mask_channel == PAINT_MATERIAL_CHANNEL_ALPHA) {
          correction_gray_node = correction_coverage_node;
          correction_gray = correction_coverage_socket;
        }
        else {
          correction_gray_node = gray_source_node;
          correction_gray = gray_source_color;
        }
      }
      else if (mask_map_is_mesh) {
        /* A scalar atlas: its R is the coverage directly; no mean, no Divide (the atlas is
         * Non-Color and its alpha is ignored). */
        bNode *separate = bke::node_add_node(nullptr, tree, "ShaderNodeSeparateXYZ"_ustr);
        if (separate == nullptr) {
          continue;
        }
        bNodeSocket *sep_x = socket_out(*separate, "X");
        bNodeSocket *sep_vector = socket_in(*separate, "Vector");
        if (sep_x == nullptr || sep_vector == nullptr) {
          continue;
        }
        bke::node_add_link(
            tree, *gray_source_node, *gray_source_color, *separate, *sep_vector);
        correction_gray_node = separate;
        correction_gray = sep_x;
      }
      else {
        bNode *separate = bke::node_add_node(nullptr, tree, "ShaderNodeSeparateXYZ"_ustr);
        bNode *add_xy = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
        bNode *add_z = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
        bNode *divide = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
        if (separate == nullptr || add_xy == nullptr || add_z == nullptr ||
            divide == nullptr)
        {
          continue;
        }
        bNodeSocket *sep_x = socket_out(*separate, "X");
        bNodeSocket *sep_y = socket_out(*separate, "Y");
        bNodeSocket *sep_z = socket_out(*separate, "Z");
        bNodeSocket *sep_vector = socket_in(*separate, "Vector");
        add_xy->custom1 = NODE_MATH_ADD;
        add_z->custom1 = NODE_MATH_ADD;
        divide->custom1 = NODE_MATH_DIVIDE;
        bNodeSocket *xy_a = socket_in(*add_xy, "Value");
        bNodeSocket *xy_b = socket_in(*add_xy, "Value_001");
        bNodeSocket *z_a = socket_in(*add_z, "Value");
        bNodeSocket *z_b = socket_in(*add_z, "Value_001");
        bNodeSocket *d_a = socket_in(*divide, "Value");
        bNodeSocket *d_b = socket_in(*divide, "Value_001");
        if (sep_x == nullptr || sep_y == nullptr || sep_z == nullptr ||
            sep_vector == nullptr || xy_a == nullptr || xy_b == nullptr ||
            z_a == nullptr || z_b == nullptr || d_a == nullptr || d_b == nullptr)
        {
          continue;
        }
        bke::node_add_link(
            tree, *gray_source_node, *gray_source_color, *separate, *sep_vector);
        bke::node_add_link(tree, *separate, *sep_x, *add_xy, *xy_a);
        bke::node_add_link(tree, *separate, *sep_y, *add_xy, *xy_b);
        bke::node_add_link(tree, *add_xy, *socket_out(*add_xy, "Value"), *add_z, *z_a);
        bke::node_add_link(tree, *separate, *sep_z, *add_z, *z_b);
        bke::node_add_link(tree, *add_z, *socket_out(*add_z, "Value"), *divide, *d_a);
        if (d_b->default_value != nullptr) {
          static_cast<bNodeSocketValueFloat *>(d_b->default_value)->value = 3.0f;
        }
        correction_gray_node = divide;
        correction_gray = socket_out(*divide, "Value");
      }
      bNode *correction_mix = mix_node_add(
          tree,
          BKE_paint_layers_blend_to_ramp(eMaterialPaintLayerBlend(correction.blend)),
          location_x + 60.0f,
          location_y - 320.0f);
      if (correction_mix == nullptr || correction_gray == nullptr ||
          correction_gray_node == nullptr || factor_socket == nullptr)
      {
        continue;
      }
      bNodeSocket *mix_color1 = socket_in(*correction_mix, "A_Color");
      bNodeSocket *mix_color2 = socket_in(*correction_mix, "B_Color");
      bNodeSocket *mix_fac = socket_in(*correction_mix, "Factor_Float");
      bNodeSocket *mix_out = socket_out(*correction_mix, "Result_Color");
      if (mix_color1 == nullptr || mix_color2 == nullptr || mix_fac == nullptr ||
          mix_out == nullptr)
      {
        continue;
      }
      bke::node_add_link(tree, *factor_node, *factor_socket, *correction_mix, *mix_color1);
      /* B is what the factor blends towards: the Fill constant or the map's straightened grey.
       * The Mix node above carries the item's own blend mode, so `F = mix(F_below, blend(F_below,
       * C), A * op)`; Mix reads as the plain over the chain always used. */
      bNode *b_node = correction_gray_node;
      bNodeSocket *b_socket = correction_gray;
      /* A Material/Node Group mask never premultiplies: its grey is either the source's own
       * coverage (Alpha channel) or a value/RGBTOBW read straight from a live constant,
       * live texture or bake, none of which the texture-upload premultiply touches -- only a
       * user-painted Image/MeshMap map needs the straighten-by-A step below. */
      if (!fill && !mask_material_or_group) {
        if (correction_alpha == nullptr || correction_map == nullptr) {
          continue;
        }
        if (mask_map_is_data && !mask_map_is_mesh) {
          /* The map is stored straight, but the texture upload pre-multiplied its bytes by A,
           * so the sampled grey already carries A once; mixing it by `A * op` would apply A
           * twice. The Image Texture node leaves a data texture pre-multiplied, so the chain
           * straightens it: `mix(F, C / A, A * op)` is `F * (1 - A * op) + C * A * op`. Where
           * A is 0 the factor is 0 too, and Math Divide yields 0 there. */
          bNode *straighten = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
          if (straighten == nullptr) {
            continue;
          }
          straighten->custom1 = NODE_MATH_DIVIDE;
          straighten->location[0] = location_x - 20.0f;
          straighten->location[1] = location_y - 380.0f;
          bNodeSocket *straighten_value = socket_in(*straighten, "Value");
          bNodeSocket *straighten_alpha = socket_in(*straighten, "Value_001");
          bNodeSocket *straighten_out = socket_out(*straighten, "Value");
          if (straighten_value == nullptr || straighten_alpha == nullptr ||
              straighten_out == nullptr)
          {
            continue;
          }
          bke::node_add_link(
              tree, *correction_gray_node, *correction_gray, *straighten, *straighten_value);
          bke::node_add_link(
              tree, *correction_map, *correction_alpha, *straighten, *straighten_alpha);
          b_node = straighten;
          b_socket = straighten_out;
        }
        /* A non-data map is already straight: the Image Texture node un-premultiplied it,
         * because the chain also reads the Alpha output, so no Divide is built. */
      }
      bke::node_add_link(tree, *b_node, *b_socket, *correction_mix, *mix_color2);
      /* Fac is `A * op`: the map alpha times the correction row's own opacity input. The
       * per-channel sockets all carry the same row value, so the current channel's is
       * enough. A Fill covers fully, so its factor is the opacity alone. */
      if (correction_opacity == nullptr || group_input == nullptr) {
        continue;
      }
      /* A Material/Node Group mask on its own Alpha channel is the coverage itself, so its
       * Fac is the opacity alone (the coordinator's decision: squaring the same number as
       * both the grey and its own multiplier would be wrong) -- flat, exactly like Fill and
       * a MESH_MAP atlas. On every other channel its Fac multiplies by the source's own
       * coverage (`correction_coverage_*`, resolved above), never a texture's own alpha. */
      const bool mask_alpha_flat = mask_material_or_group &&
                                   correction.mask_channel == PAINT_MATERIAL_CHANNEL_ALPHA;
      if (fill || mask_map_is_mesh || mask_alpha_flat) {
        /* A Fill covers fully; a MESH_MAP atlas' alpha is ignored (spec M2 §1), so its factor
         * is the item's opacity alone and no Alpha multiply is built. */
        bke::node_add_link(
            tree, *group_input, *correction_opacity, *correction_mix, *mix_fac);
      }
      else if (mask_material_or_group) {
        if (correction_coverage_node == nullptr || correction_coverage_socket == nullptr) {
          /* No coverage of its own yet (Baked with no bake coverage, say): flat, exactly
           * like the row-level factor falls back to full coverage in the same state. */
          bke::node_add_link(
              tree, *group_input, *correction_opacity, *correction_mix, *mix_fac);
        }
        else {
          bNode *fac_multiply = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
          if (fac_multiply == nullptr) {
            continue;
          }
          fac_multiply->custom1 = NODE_MATH_MULTIPLY;
          bNodeSocket *fac_value = socket_in(*fac_multiply, "Value");
          bNodeSocket *fac_coverage = socket_in(*fac_multiply, "Value_001");
          bNodeSocket *fac_out = socket_out(*fac_multiply, "Value");
          if (fac_value == nullptr || fac_coverage == nullptr || fac_out == nullptr) {
            continue;
          }
          bke::node_add_link(
              tree, *group_input, *correction_opacity, *fac_multiply, *fac_value);
          bke::node_add_link(tree,
                             *correction_coverage_node,
                             *correction_coverage_socket,
                             *fac_multiply,
                             *fac_coverage);
          bke::node_add_link(tree, *fac_multiply, *fac_out, *correction_mix, *mix_fac);
        }
      }
      else {
        if (correction_alpha == nullptr || correction_map == nullptr) {
          continue;
        }
        bNode *fac_multiply = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
        if (fac_multiply == nullptr) {
          continue;
        }
        fac_multiply->custom1 = NODE_MATH_MULTIPLY;
        bNodeSocket *fac_value = socket_in(*fac_multiply, "Value");
        bNodeSocket *fac_coverage = socket_in(*fac_multiply, "Value_001");
        bNodeSocket *fac_out = socket_out(*fac_multiply, "Value");
        if (fac_value == nullptr || fac_coverage == nullptr || fac_out == nullptr) {
          continue;
        }
        bke::node_add_link(
            tree, *group_input, *correction_opacity, *fac_multiply, *fac_value);
        bke::node_add_link(
            tree, *correction_map, *correction_alpha, *fac_multiply, *fac_coverage);
        bke::node_add_link(tree, *fac_multiply, *fac_out, *correction_mix, *mix_fac);
      }
      layer_factor_node = correction_mix;
      layer_factor_socket = mix_out;
      /* The next item lays over this one: its base is this item's result. */
      factor_node = correction_mix;
      factor_socket = mix_out;
    }
  }
}
}

}  // namespace bke::paint_layers
}  // namespace blender
