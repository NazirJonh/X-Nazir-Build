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
#include "paint_layers_generate_layout.hh"
#include "paint_layers_generate_subgroup.hh"

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

/* One word for a mask item's source, for the `Mask <N>: <name> (<word>)` frame label. */
static const char *mask_source_word(const MaterialPaintLayer &correction, const bool fill)
{
  /* Why words: the frame label is UI only, so it spells the DNA source enum plainly. */
  if (fill) {
    return "Fill";
  }
  switch (correction.source) {
    case MA_PAINT_LAYER_SOURCE_CONSTANT: {
      return "Fill";
    }
    case MA_PAINT_LAYER_SOURCE_MESH_MAP: {
      return "Mesh Map";
    }
    case MA_PAINT_LAYER_SOURCE_MATERIAL: {
      return "Material";
    }
    case MA_PAINT_LAYER_SOURCE_NODE_GROUP: {
      return "Node Group";
    }
    case MA_PAINT_LAYER_SOURCE_STACK: {
      return "Stack";
    }
    case MA_PAINT_LAYER_SOURCE_IMAGE:
    default: {
      return "Image";
    }
  }
}

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
  const Vector<const MaterialPaintLayer *> effects = paint_layers_build_effects(ma, *layer);
  /* Why indexed: each effect owns eight Content-column rows, so neighbours never share one. */
  for (int effect_index = 0; effect_index < effects.size(); effect_index++) {
    const MaterialPaintLayer *effect = effects[effect_index];
    const MaterialPaintLayer &correction = *effect;
    /* Why grid rows: the mix, factor, coverage and alpha nodes spread over the
     * effect block in the Content column. */
    const int effect_base_row = effect_index * (layout::kMaskItemRows + 3);
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
    /* A Fill whose channel holds an assigned map reads it like a Paint correction's map. */
    const bool fill = BKE_paint_layers_source_type(correction) == PaintLayerSourceType::Constant &&
                      !paint_layer_fill_reads_map(ma, correction, channel);
    /* A constant normal makes no sense; the CPU skips this correction as well. */
    if (normal_channel && fill) {
      continue;
    }
    /* A switched-off channel lays nothing; the CPU skips it the same way. */
    if (paint_layer_fill_effect_channel_off(ma, correction, channel)) {
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
    /* The nodes this effect built, framed as `Content: <name>` once placed. Shared nodes
     * (the group input, a wrapper instance, the row Mapping) never join: they outlive one
     * effect and must not hang under its frame. */
    Vector<bNode *> frame_nodes;
    /* A mapped Normal correction map is remapped after the data-map straighten below: that
     * Divide is not linear-compatible with the remap, so the order is fixed. */
    bool normal_remap_pending = false;
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
        live_map_configure(
            *map, tree, group_input, correction, channel, location_x - 180.0f, location_y - 160.0f);
        correction_color = socket_out(*map, "Color");
        correction_alpha = socket_out(*map, "Alpha");
        correction_color_node = map;
        correction_source = map;
        frame_nodes.append(map);
        if (correction_color == nullptr) {
          continue;
        }
        if (normal_channel) {
          /* The Mapping moved only the read point; the tangent-space vectors follow it. */
          correction_color = outer_.normal_remap_ensure(tree,
                                                        correction,
                                                        *map,
                                                        *correction_color,
                                                        correction_color_node,
                                                        location_x + 60.0f,
                                                        location_y - 400.0f);
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
        frame_nodes.append(correction_source);
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
          frame_nodes.append(value);
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
          live_map_configure(*map,
                             tree,
                             group_input,
                             correction,
                             PAINT_MATERIAL_CHANNEL_ALPHA,
                             location_x - 270.0f,
                             location_y - 240.0f);
          correction_material_coverage_node = map;
          correction_material_coverage_socket = map_alpha;
          frame_nodes.append(map);
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
      frame_nodes.append(correction_source);
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
      frame_nodes.append(p_zero);
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
      frame_nodes.append(divide);
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
        /* Why grid: the effect map opens its effect block in the Source column. */
        layout::place(*correction_source, layout::Column::Source, effect_base_row + 1);
        frame_nodes.append(correction_source);
        if (correction_mesh_map) {
          if (NodeTexImage *storage = static_cast<NodeTexImage *>(
                  correction_source->storage))
          {
            storage->extension = SHD_IMAGE_EXTENSION_EXTEND;
          }
        }
        else if (std::pair<bNode *, bNodeSocket *> mapping = outer_.mapping_vector_ensure(
                     tree, group_input, correction, location_x - 180.0f, location_y - 160.0f);
                 mapping.first != nullptr && mapping.second != nullptr)
        {
          /* A mapped Fill reads its map through its own Mapping node; tiling needs Repeat. */
          if (NodeTexImage *storage = static_cast<NodeTexImage *>(
                  correction_source->storage))
          {
            storage->extension = SHD_IMAGE_EXTENSION_REPEAT;
          }
          texture_vector_link_mapped(
              tree, *correction_source, *mapping.first, *mapping.second);
        }
        correction_color = socket_out(*correction_source, "Color");
        correction_alpha = socket_out(*correction_source, "Alpha");
        correction_color_node = correction_source;
        normal_remap_pending = normal_channel && !correction_mesh_map;
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
              frame_nodes.append(separate);
              frame_nodes.append(combine);
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
      /* Why grid: the effect blend sits in the Content column on its block row. */
      layout::place(*correction_mix, layout::Column::Content, effect_base_row + 0);
      frame_nodes.append(correction_mix);
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
      /* Why grid: the effect blend sits in the Content column on its block row. */
      const layout::Cursor effect_mix_cursor = layout::at(layout::Column::Content,
                                                          effect_base_row + 0);
      correction_mix = mix_node_add(tree,
                                    BKE_paint_layers_blend_to_ramp(
                                        eMaterialPaintLayerBlend(BKE_paint_layers_channel_blend_effective(correction, channel))),
                                     effect_mix_cursor.x,
                                     effect_mix_cursor.y);
      if (correction_mix == nullptr) {
        continue;
      }
      frame_nodes.append(correction_mix);
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
        bNode *flat_fallback = mix_node_add(tree, MA_RAMP_BLEND, 0.0f, 0.0f);
        if (flat_fallback != nullptr) {
          /* Why extra: the fallback Mix has no named slot in the correction block. */
          layout::place_extra(*flat_fallback);
          frame_nodes.append(flat_fallback);
        }
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
        frame_nodes.append(combine);
        frame_nodes.append(straighten);
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
          /* Why extra: the straighten pair owns no named grid slot, so it takes two free cells in
           * this channel's lane instead of colliding with the laid-out chain. */
          layout::place_extra(*combine);
          layout::place_extra(*straighten);
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
    if (normal_remap_pending && correction_color_node != nullptr && correction_color != nullptr) {
      /* The Mapping moved only the read point; the tangent-space vectors follow it. */
      correction_color = outer_.normal_remap_ensure(tree,
                                                    correction,
                                                    *correction_color_node,
                                                    *correction_color,
                                                    correction_color_node,
                                                    location_x + 60.0f,
                                                    location_y - 400.0f);
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
        /* Why grid: the effect factor follows its Mix one row below. */
        layout::place(*correction_factor, layout::Column::Content, effect_base_row + 1);
        frame_nodes.append(correction_factor);
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
          /* Why grid: the coverage over spreads over three rows of the block. */
          layout::place(*one_minus, layout::Column::Content, effect_base_row + 2);
          layout::place(*scaled, layout::Column::Content, effect_base_row + 3);
          layout::place(*joined, layout::Column::Content, effect_base_row + 4);
          frame_nodes.append(one_minus);
          frame_nodes.append(scaled);
          frame_nodes.append(joined);
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
        frame_nodes.append(alpha_one_minus);
        frame_nodes.append(alpha_scaled);
        frame_nodes.append(alpha_joined);
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
          /* Why grid: the content-alpha over spreads over three rows past coverage. */
          layout::place(*alpha_one_minus, layout::Column::Content, effect_base_row + 5);
          layout::place(*alpha_scaled, layout::Column::Content, effect_base_row + 6);
          layout::place(*alpha_joined, layout::Column::Content, effect_base_row + 7);
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
    /* Why after place: every node above already sits on its grid row, so the frame only
     * parents them. A skipped effect builds nothing, so it frames nothing. */
    if (!frame_nodes.is_empty()) {
      char frame_label[64];
      SNPRINTF(frame_label, "Content: %s", correction.name);
      layout::frame_add(tree, frame_label, frame_nodes);
    }
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


void PaintLayersChainBuilder::live_map_configure(bNode &map,
                                                 bNodeTree &tree,
                                                 bNode *group_input,
                                                 const MaterialPaintLayer &row,
                                                 const int channel,
                                                 const float location_x,
                                                 const float location_y)
{
  NodeTexImage *dst = static_cast<NodeTexImage *>(map.storage);
  if (dst == nullptr) {
    return;
  }
  /* The source node's sampling settings travel with the live map, like a Layer row's. */
  MaterialSourceResolve resolve_local;
  const MaterialSourceResolve &resolve = PaintLayersRegenCache::resolve_get(
      row.material, outer_.cache_, resolve_local);
  const bNode *src_node = (channel >= 0 && channel < PAINT_MATERIAL_CHANNEL_NUM) ?
                              resolve.images[channel].node :
                              nullptr;
  if (const NodeTexImage *src_storage = (src_node != nullptr) ?
                                            static_cast<const NodeTexImage *>(src_node->storage) :
                                            nullptr)
  {
    dst->interpolation = src_storage->interpolation;
    dst->extension = src_storage->extension;
    dst->projection = src_storage->projection;
  }
  /* Tiling needs Repeat: the CPU remap repeats unconditionally. One shared Mapping per row. */
  if (std::pair<bNode *, bNodeSocket *> mapping = outer_.mapping_vector_ensure(
          tree, group_input, row, location_x, location_y);
      mapping.first != nullptr && mapping.second != nullptr)
  {
    dst->extension = SHD_IMAGE_EXTENSION_REPEAT;
    texture_vector_link_mapped(tree, map, *mapping.first, *mapping.second);
  }
}

std::pair<bNode *, bNodeSocket *> PaintLayersChainBuilder::resolve_correction_coverage(
    bNodeTree &tree,
    bNode *group_input,
    const float location_x,
    const float location_y,
    const MaterialPaintLayer &row)
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
      live_map_configure(*map,
                         tree,
                         group_input,
                         row,
                         PAINT_MATERIAL_CHANNEL_ALPHA,
                         location_x - 270.0f,
                         location_y - 240.0f);
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
  /* Why unused: positions come from the layout grid now, the root cursor only
   * steps the group instances in the parent tree. */
  (void)location_x;
  (void)location_y;
  if (layer_factor_socket != nullptr) {
    return;
  }
  bNode *white = bke::node_add_static_node(nullptr, tree, SH_NODE_RGB);
  if (white != nullptr) {
    /* Why layout grid: the neutral base sits one row above the first mask item,
     * so it never shares the item rows below. */
    layout::place(*white, layout::Column::Mask, -1);
    layout::label(*white, "Mask base");
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

bool PaintLayersChainBuilder::build_mask_element_chain(
    bNodeTree &tree,
    const MaterialPaintLayer &correction,
    const int mask_base_row,
    const MaskElementGrey grey_mode,
    bNode *gray_source_node,
    bNodeSocket *gray_source_color,
    bNode *opacity_node,
    bNodeSocket *opacity_socket,
    bNode *multiply_node,
    bNodeSocket *multiply_socket,
    bNode *map_node,
    bNodeSocket *map_alpha,
    const bool fill,
    const bool straighten_grey,
    Vector<bNode *> &frame_nodes,
    bNode *&factor_node,
    bNodeSocket *&factor_socket)
{
  /* The grey the item blends with, from the already resolved source socket. */
  bNode *gray_out_node = nullptr;
  bNodeSocket *gray_out = nullptr;
  if (grey_mode == MaskElementGrey::AlreadyScalar) {
    gray_out_node = gray_source_node;
    gray_out = gray_source_color;
  }
  else if (grey_mode == MaskElementGrey::SeparateX) {
    bNode *separate = bke::node_add_node(nullptr, tree, "ShaderNodeSeparateXYZ"_ustr);
    if (separate == nullptr) {
      return false;
    }
    layout::place(*separate, layout::Column::Mask, mask_base_row + 1);
    frame_nodes.append(separate);
    bNodeSocket *sep_vector = socket_in(*separate, "Vector");
    bNodeSocket *sep_x = socket_out(*separate, "X");
    if (sep_vector == nullptr || sep_x == nullptr || gray_source_node == nullptr ||
        gray_source_color == nullptr)
    {
      return false;
    }
    bke::node_add_link(tree, *gray_source_node, *gray_source_color, *separate, *sep_vector);
    gray_out_node = separate;
    gray_out = sep_x;
  }
  else {
    /* The mask reads the map as the same byte-mean the CPU computes: (R + G + B) / 3. */
    bNode *separate = bke::node_add_node(nullptr, tree, "ShaderNodeSeparateXYZ"_ustr);
    bNode *add_xy = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
    bNode *add_z = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
    bNode *divide = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
    if (separate == nullptr || add_xy == nullptr || add_z == nullptr || divide == nullptr) {
      return false;
    }
    add_xy->custom1 = NODE_MATH_ADD;
    add_z->custom1 = NODE_MATH_ADD;
    divide->custom1 = NODE_MATH_DIVIDE;
    layout::place(*separate, layout::Column::Mask, mask_base_row + 1);
    layout::place(*add_xy, layout::Column::Mask, mask_base_row + 2);
    layout::place(*add_z, layout::Column::Mask, mask_base_row + 3);
    layout::place(*divide, layout::Column::Mask, mask_base_row + 4);
    layout::label(*divide, "Mask grey");
    frame_nodes.append(separate);
    frame_nodes.append(add_xy);
    frame_nodes.append(add_z);
    frame_nodes.append(divide);
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
    if (sep_vector == nullptr || sep_x == nullptr || sep_y == nullptr || sep_z == nullptr ||
        xy_a == nullptr || xy_b == nullptr || z_a == nullptr || z_b == nullptr ||
        d_a == nullptr || d_b == nullptr || gray_source_node == nullptr ||
        gray_source_color == nullptr)
    {
      return false;
    }
    bke::node_add_link(tree, *gray_source_node, *gray_source_color, *separate, *sep_vector);
    bke::node_add_link(tree, *separate, *sep_x, *add_xy, *xy_a);
    bke::node_add_link(tree, *separate, *sep_y, *add_xy, *xy_b);
    bke::node_add_link(tree, *add_xy, *socket_out(*add_xy, "Value"), *add_z, *z_a);
    bke::node_add_link(tree, *separate, *sep_z, *add_z, *z_b);
    bke::node_add_link(tree, *add_z, *socket_out(*add_z, "Value"), *divide, *d_a);
    if (d_b->default_value != nullptr) {
      static_cast<bNodeSocketValueFloat *>(d_b->default_value)->value = 3.0f;
    }
    gray_out_node = divide;
    gray_out = socket_out(*divide, "Value");
  }

  /* Why grid: each item blends in the Factor column on its own base row. */
  const layout::Cursor mix_cursor = layout::at(layout::Column::Factor, mask_base_row + 0);
  bNode *item_mix = mix_node_add(
      tree,
      BKE_paint_layers_blend_to_ramp(eMaterialPaintLayerBlend(correction.blend)),
      mix_cursor.x,
      mix_cursor.y);
  if (item_mix == nullptr || gray_out == nullptr || gray_out_node == nullptr ||
      factor_socket == nullptr)
  {
    return false;
  }
  frame_nodes.append(item_mix);
  if (fill) {
    layout::label(*item_mix, "Mask: Fill");
  }
  else {
    layout::label(*item_mix, "Mask: Image");
  }
  bNodeSocket *mix_a = socket_in(*item_mix, "A_Color");
  bNodeSocket *mix_b = socket_in(*item_mix, "B_Color");
  bNodeSocket *mix_fac = socket_in(*item_mix, "Factor_Float");
  bNodeSocket *mix_out = socket_out(*item_mix, "Result_Color");
  if (mix_a == nullptr || mix_b == nullptr || mix_fac == nullptr || mix_out == nullptr) {
    return false;
  }
  bke::node_add_link(tree, *factor_node, *factor_socket, *item_mix, *mix_a);
  /* B is what the factor blends towards: the Fill constant or the map's straightened grey. The
   * Mix node carries the item's own blend mode, so `F = mix(F_below, blend(F_below, C), A * op)`;
   * Mix reads as the plain over the chain always used. */
  bNode *b_node = gray_out_node;
  bNodeSocket *b_socket = gray_out;
  if (straighten_grey && map_node != nullptr && map_alpha != nullptr) {
    /* The map is stored straight, but the texture upload pre-multiplied its bytes by A, so the
     * sampled grey already carries A once; mixing it by `A * op` would apply A twice. The Image
     * Texture node leaves a data texture pre-multiplied, so the chain straightens it:
     * `mix(F, C / A, A * op)` is `F * (1 - A * op) + C * A * op`. Where A is 0 the factor is 0
     * too, and Math Divide yields 0 there. */
    bNode *straighten = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
    if (straighten == nullptr) {
      return false;
    }
    straighten->custom1 = NODE_MATH_DIVIDE;
    /* Why grid: data-map straighten sits in Factor column past the Mix row. */
    layout::place(*straighten, layout::Column::Factor, mask_base_row + 2);
    frame_nodes.append(straighten);
    bNodeSocket *st_value = socket_in(*straighten, "Value");
    bNodeSocket *st_alpha = socket_in(*straighten, "Value_001");
    bNodeSocket *st_out = socket_out(*straighten, "Value");
    if (st_value == nullptr || st_alpha == nullptr || st_out == nullptr) {
      return false;
    }
    bke::node_add_link(tree, *gray_out_node, *gray_out, *straighten, *st_value);
    bke::node_add_link(tree, *map_node, *map_alpha, *straighten, *st_alpha);
    b_node = straighten;
    b_socket = st_out;
  }
  bke::node_add_link(tree, *b_node, *b_socket, *item_mix, *mix_b);
  if (opacity_node == nullptr || opacity_socket == nullptr) {
    return false;
  }
  if (multiply_socket == nullptr) {
    /* A Fill covers fully, a MeshMap atlas carries no alpha of its own, and on the Alpha channel
     * the grey already is that number: the item factor is its opacity alone. */
    bke::node_add_link(tree, *opacity_node, *opacity_socket, *item_mix, *mix_fac);
  }
  else {
    /* Fac is `A * op`: the map alpha (or the source's coverage) times the item's opacity. */
    bNode *fac_mul = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
    if (fac_mul == nullptr) {
      return false;
    }
    fac_mul->custom1 = NODE_MATH_MULTIPLY;
    /* Why grid: the mask factor sits in Factor column past the Mix row. */
    layout::place(*fac_mul, layout::Column::Factor, mask_base_row + 1);
    layout::label(*fac_mul, "Mask opacity");
    frame_nodes.append(fac_mul);
    bNodeSocket *fac_value = socket_in(*fac_mul, "Value");
    bNodeSocket *fac_coverage = socket_in(*fac_mul, "Value_001");
    bNodeSocket *fac_out = socket_out(*fac_mul, "Value");
    if (fac_value == nullptr || fac_coverage == nullptr || fac_out == nullptr) {
      return false;
    }
    bke::node_add_link(tree, *opacity_node, *opacity_socket, *fac_mul, *fac_value);
    bke::node_add_link(tree, *multiply_node, *multiply_socket, *fac_mul, *fac_coverage);
    bke::node_add_link(tree, *fac_mul, *fac_out, *item_mix, *mix_fac);
  }
  factor_node = item_mix;
  factor_socket = mix_out;
  return true;
}

/* The interface input of \a sub's shared tree by name, or null. A later channel of the row looks
 * the socket up here instead of a stale local pointer, so every instance binds the very same
 * interface entry. */
static bNodeTreeInterfaceSocket *interface_input(SubGroup &sub, const char *name)
{
  if (sub.tree == nullptr) {
    return nullptr;
  }
  sub.tree->ensure_interface_cache();
  for (bNodeTreeInterfaceSocket *socket : sub.tree->interface_inputs()) {
    if (socket->name != nullptr && STREQ(socket->name, name)) {
      return socket;
    }
  }
  return nullptr;
}

/* The one interface output of \a sub's shared tree, or null. Not looked up by name: the interface
 * name is made unique across inputs and outputs, so the output "Factor" is stored as "Factor 2"
 * once the input "Factor" exists, and a name lookup would silently find nothing. */
static bNodeTreeInterfaceSocket *interface_output(SubGroup &sub)
{
  if (sub.tree == nullptr) {
    return nullptr;
  }
  sub.tree->ensure_interface_cache();
  const Span<bNodeTreeInterfaceSocket *> outputs = sub.tree->interface_outputs();
  return outputs.is_empty() ? nullptr : outputs.first();
}

void PaintLayersChainBuilder::build_mask_item(
    const MaterialPaintLayer *layer,
    const int channel,
    const bool substituted,
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
  /* Why packed first: a Fill/Image/MeshMap-only stack builds once inside its own `.PL Mask` group,
   * so the row group keeps one instance instead of the flat nodes. Material/Node Group/Stack items
   * and multi-channel rows stay flat: their subtrees would need groups in groups and per-channel
   * factors a single Factor pair cannot carry. */
  {
    /* Why no packing for a substituted row: its bake already folded in mask x opacity (see
     * #build_row_source), so the mask chain must be built flat exactly as HEAD did, never wrapped
     * in `.PL Mask`; the flat warm spare is what the group tests expect. */
    bool packable = current.opacity != nullptr && group_input != nullptr && !substituted;
    Vector<const MaterialPaintLayer *> probe;
    if (packable) {
      probe = paint_layers_build_mask_items(ma, *layer);
      /* Why drop deferred: an Image item without its map builds no chain, exactly as the flat
       * path already skips it through #paint_layers_warm_defers_item. Keeping it would add an
       * interface input on the `.PL Mask` instance that no node reads, so the code shape would
       * move the moment a mask is added, before its map exists. */
      for (int i = int(probe.size()) - 1; i >= 0; i--) {
        if (paint_layers_warm_defers_item(ma, *layer, *probe[i], true)) {
          probe.remove(i);
        }
      }
      if (probe.is_empty()) {
        packable = false;
      }
    }
    if (packable) {
      for (const MaterialPaintLayer *item : probe) {
        if (ELEM(item->source,
                 MA_PAINT_LAYER_SOURCE_MATERIAL,
                 MA_PAINT_LAYER_SOURCE_NODE_GROUP,
                 MA_PAINT_LAYER_SOURCE_STACK))
        {
          packable = false;
          break;
        }
      }
    }
    if (packable) {
      if (!outer_.ctx_.subgroup_tree_get) {
        packable = false;
      }
    }
    if (packable) {
      if (outer_.layer_groups_.lookup_ptr(layer) == nullptr) {
        packable = false;
      }
    }
    if (packable) {
      SubGroup sub = subgroup_ensure(outer_.ctx_, *layer, TREE_SUBKIND_MASK, tree, channel);
      if (sub.tree != nullptr && sub.instance != nullptr && sub.group_input != nullptr &&
          sub.group_output != nullptr)
      {
        /* Why Base Color: a mask reads the same Base Color record on every channel, so the current
         * channel decides nothing and the probe below uses it only to tell Fill from mapped. */
        const int probe_channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
        bool need_uv = false;
        Vector<char> is_fill;
        Vector<char> is_mapped;
        is_fill.reserve(probe.size());
        is_mapped.reserve(probe.size());
        for (int i = 0; i < probe.size(); i++) {
          is_fill.append(0);
          is_mapped.append(0);
        }
        for (int i = 0; i < probe.size(); i++) {
          const MaterialPaintLayer &item = *probe[i];
          Image *probe_image = paint_layer_mask_correction_image(ma, item, probe_channel);
          const bool fill = BKE_paint_layers_source_type(item) == PaintLayerSourceType::Constant &&
                            probe_image == nullptr;
          if (fill) {
            is_fill[i] = 1;
          }
          if (!fill && item.source != MA_PAINT_LAYER_SOURCE_MESH_MAP &&
              BKE_paint_layers_mapping_applies(ma, item, outer_.cache_))
          {
            is_mapped[i] = 1;
            need_uv = true;
          }
        }
        if (!sub.built) {
          /* Why bind only: this is channel two or later of the row, so the `.PL Mask` tree and its
           * interface already stand. Only this instance's own sockets are linked; the interface is
           * looked up in the shared tree, never rebuilt, and the nodes are never duplicated. */
          auto instance_input = [&](bNodeTreeInterfaceSocket *iface) -> bNodeSocket * {
            if (iface == nullptr || iface->identifier == nullptr) {
              return nullptr;
            }
            return bke::node_find_socket(
                *sub.instance, SOCK_IN, UString::from_ptr_noinline(iface->identifier));
          };
          if (layer_factor_node != nullptr && layer_factor_socket != nullptr) {
            bNodeSocket *dst = instance_input(interface_input(sub, "Factor"));
            if (dst != nullptr) {
              bke::node_add_link(tree,
                                 *layer_factor_node,
                                 *layer_factor_socket,
                                 *sub.instance,
                                 *dst);
            }
          }
          sub.tree->ensure_interface_cache();
          auto iface_inputs = sub.tree->interface_inputs();
          /* The interface was created in a fixed order -- Factor first, then every item's [Fill],
           * Opacity, [Mapping Offset, Scale, Rotation], then [UV] -- so walk it in that order. A
           * name lookup alone would bind the wrong socket when two items share a name. */
          int cursor = 1;
          for (int i = 0; i < probe.size(); i++) {
            const MaterialPaintLayer &item = *probe[i];
            bNodeTreeInterfaceSocket *fill_iface = nullptr;
            if (is_fill[i] != 0 && cursor < iface_inputs.size()) {
              fill_iface = iface_inputs[cursor++];
            }
            bNodeTreeInterfaceSocket *opacity_iface = (cursor < iface_inputs.size()) ?
                                                         iface_inputs[cursor++] :
                                                         nullptr;
            bNodeTreeInterfaceSocket *offset_iface = nullptr;
            bNodeTreeInterfaceSocket *scale_iface = nullptr;
            bNodeTreeInterfaceSocket *rotation_iface = nullptr;
            if (is_mapped[i] != 0) {
              if (cursor < iface_inputs.size()) {
                offset_iface = iface_inputs[cursor++];
              }
              if (cursor < iface_inputs.size()) {
                scale_iface = iface_inputs[cursor++];
              }
              if (cursor < iface_inputs.size()) {
                rotation_iface = iface_inputs[cursor++];
              }
            }
            if (opacity_iface != nullptr) {
              bNodeSocket *dst = instance_input(opacity_iface);
              bNodeSocket *src = nullptr;
              if (Map<int, bNodeTreeInterfaceSocket *> *by_channel =
                      correction_opacity_inputs.lookup_ptr(&item))
              {
                if (bNodeTreeInterfaceSocket **found = by_channel->lookup_ptr(channel)) {
                  src = group_input_socket(group_input, **found);
                }
              }
              if (dst != nullptr && src != nullptr && group_input != nullptr) {
                bke::node_add_link(tree, *group_input, *src, *sub.instance, *dst);
              }
            }
            if (fill_iface != nullptr) {
              bNodeSocket *dst = instance_input(fill_iface);
              bNodeSocket *src = nullptr;
              if (bNodeTreeInterfaceSocket **found = correction_fill_inputs.lookup_ptr(&item)) {
                src = group_input_socket(group_input, **found);
              }
              if (dst != nullptr && src != nullptr && group_input != nullptr) {
                bke::node_add_link(tree, *group_input, *src, *sub.instance, *dst);
              }
            }
            auto link_mapping =
                [&](Map<const MaterialPaintLayer *, bNodeTreeInterfaceSocket *> &inputs,
                    bNodeTreeInterfaceSocket *dst_iface) {
                  if (dst_iface == nullptr) {
                    return;
                  }
                  bNodeSocket *dst = instance_input(dst_iface);
                  bNodeSocket *src = nullptr;
                  if (bNodeTreeInterfaceSocket **found = inputs.lookup_ptr(&item)) {
                    src = group_input_socket(group_input, **found);
                  }
                  if (dst != nullptr && src != nullptr && group_input != nullptr) {
                    bke::node_add_link(tree, *group_input, *src, *sub.instance, *dst);
                  }
                };
            link_mapping(outer_.mapping_offset_inputs_, offset_iface);
            link_mapping(outer_.mapping_scale_inputs_, scale_iface);
            link_mapping(outer_.mapping_rotation_inputs_, rotation_iface);
          }
          if (need_uv) {
            bNodeTreeInterfaceSocket *uv_iface = (cursor < iface_inputs.size()) ?
                                                     iface_inputs[cursor++] :
                                                     nullptr;
            if (uv_iface != nullptr) {
              bNodeSocket *dst = instance_input(uv_iface);
              auto uv_pair = generated_uv_map_ensure(tree, BKE_paint_layers_uv_map_name(ma));
              if (dst != nullptr && uv_pair.first != nullptr && uv_pair.second != nullptr) {
                bke::node_add_link(tree, *uv_pair.first, *uv_pair.second, *sub.instance, *dst);
              }
            }
          }
          /* The instance now carries the factor of this channel's own row chain. */
          bNodeTreeInterfaceSocket *factor_out = interface_output(sub);
          if (factor_out != nullptr && factor_out->identifier != nullptr) {
            bNodeSocket *instance_out = bke::node_find_socket(
                *sub.instance, SOCK_OUT, UString::from_ptr_noinline(factor_out->identifier));
            if (instance_out != nullptr) {
              layer_factor_node = sub.instance;
              layer_factor_socket = instance_out;
            }
          }
          return;
        }
        /* Why lane 0: the panel's contents are built once for the whole row, whatever channel
         * entered first, so they never belong to a channel lane; the scope restores the caller's
         * lane when the panel is done. */
        layout::ChannelLaneScope panel_lane(0);
        bNodeTreeInterfaceSocket *factor_in = subgroup_add_input(sub, "Factor", "NodeSocketFloat");
        if (factor_in != nullptr && factor_in->socket_data != nullptr) {
          static_cast<bNodeSocketValueFloat *>(factor_in->socket_data)->value = 1.0f;
        }
        Vector<bNodeTreeInterfaceSocket *> fill_ifaces;
        Vector<bNodeTreeInterfaceSocket *> opacity_ifaces;
        Vector<bNodeTreeInterfaceSocket *> map_offset_ifaces;
        Vector<bNodeTreeInterfaceSocket *> map_scale_ifaces;
        Vector<bNodeTreeInterfaceSocket *> map_rotation_ifaces;
        fill_ifaces.reserve(probe.size());
        opacity_ifaces.reserve(probe.size());
        map_offset_ifaces.reserve(probe.size());
        map_scale_ifaces.reserve(probe.size());
        map_rotation_ifaces.reserve(probe.size());
        for (int i = 0; i < probe.size(); i++) {
          fill_ifaces.append(nullptr);
          opacity_ifaces.append(nullptr);
          map_offset_ifaces.append(nullptr);
          map_scale_ifaces.append(nullptr);
          map_rotation_ifaces.append(nullptr);
        }
        for (int i = 0; i < probe.size(); i++) {
          const MaterialPaintLayer &item = *probe[i];
          if (is_fill[i] != 0) {
            char base[128];
            SNPRINTF(base, "%s Fill", item.name[0] != '\0' ? item.name : "Mask");
            bNodeTreeInterfaceSocket *fill_iface = subgroup_add_input(
                sub, base, "NodeSocketColor");
            if (fill_iface != nullptr && fill_iface->socket_data != nullptr) {
              copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(fill_iface->socket_data)->value,
                         item.fill_color);
            }
            fill_ifaces[i] = fill_iface;
          }
          {
            char base[128];
            SNPRINTF(base, "%s Opacity", item.name[0] != '\0' ? item.name : "Mask");
            bNodeTreeInterfaceSocket *opacity_iface = subgroup_add_input(
                sub, base, "NodeSocketFloat");
            if (opacity_iface != nullptr && opacity_iface->socket_data != nullptr) {
              static_cast<bNodeSocketValueFloat *>(opacity_iface->socket_data)->value =
                  BKE_paint_layers_effective_opacity(item);
            }
            opacity_ifaces[i] = opacity_iface;
          }
          if (is_mapped[i] != 0) {
            char base[128];
            SNPRINTF(base, "%s Mapping Offset", item.name[0] != '\0' ? item.name : "Mask");
            map_offset_ifaces[i] = subgroup_add_input(sub, base, "NodeSocketVector");
            SNPRINTF(base, "%s Mapping Scale", item.name[0] != '\0' ? item.name : "Mask");
            map_scale_ifaces[i] = subgroup_add_input(sub, base, "NodeSocketVector");
            SNPRINTF(base, "%s Mapping Rotation", item.name[0] != '\0' ? item.name : "Mask");
            map_rotation_ifaces[i] = subgroup_add_input(sub, base, "NodeSocketVector");
          }
        }
        bNodeTreeInterfaceSocket *uv_iface = nullptr;
        if (need_uv) {
          uv_iface = subgroup_add_input(sub, "UV", "NodeSocketVector");
        }
        bNodeTreeInterfaceSocket *factor_out = subgroup_add_output(
            sub, "Factor", "NodeSocketFloat");
        nodes::update_node_declaration_and_sockets(tree, *sub.instance);
        /* Why links: the role sockets stay in the row group, so every subgroup input reads its
         * value from the row Group Input output of the same meaning. */
        auto instance_input = [&](bNodeTreeInterfaceSocket *iface) -> bNodeSocket * {
          if (iface == nullptr || iface->identifier == nullptr) {
            return nullptr;
          }
          return bke::node_find_socket(
              *sub.instance, SOCK_IN, UString::from_ptr_noinline(iface->identifier));
        };
        if (factor_in != nullptr) {
          bNodeSocket *dst = instance_input(factor_in);
          if (dst != nullptr && layer_factor_node != nullptr && layer_factor_socket != nullptr) {
            bke::node_add_link(tree, *layer_factor_node, *layer_factor_socket, *sub.instance, *dst);
          }
        }
        for (int i = 0; i < probe.size(); i++) {
          const MaterialPaintLayer &item = *probe[i];
          if (opacity_ifaces[i] != nullptr) {
            bNodeSocket *dst = instance_input(opacity_ifaces[i]);
            bNodeSocket *src = nullptr;
            if (Map<int, bNodeTreeInterfaceSocket *> *by_channel =
                    correction_opacity_inputs.lookup_ptr(&item))
            {
              if (bNodeTreeInterfaceSocket **found = by_channel->lookup_ptr(channel)) {
                src = group_input_socket(group_input, **found);
              }
            }
            if (dst != nullptr && src != nullptr && group_input != nullptr) {
              bke::node_add_link(tree, *group_input, *src, *sub.instance, *dst);
            }
          }
          if (fill_ifaces[i] != nullptr) {
            bNodeSocket *dst = instance_input(fill_ifaces[i]);
            bNodeSocket *src = nullptr;
            if (bNodeTreeInterfaceSocket **found = correction_fill_inputs.lookup_ptr(&item)) {
              src = group_input_socket(group_input, **found);
            }
            if (dst != nullptr && src != nullptr && group_input != nullptr) {
              bke::node_add_link(tree, *group_input, *src, *sub.instance, *dst);
            }
          }
          if (map_offset_ifaces[i] != nullptr || map_scale_ifaces[i] != nullptr ||
              map_rotation_ifaces[i] != nullptr)
          {
            auto link_mapping = [&](Map<const MaterialPaintLayer *, bNodeTreeInterfaceSocket *> &inputs,
                                    bNodeTreeInterfaceSocket *dst_iface) {
              if (dst_iface == nullptr) {
                return;
              }
              bNodeSocket *dst = instance_input(dst_iface);
              bNodeSocket *src = nullptr;
              if (bNodeTreeInterfaceSocket **found = inputs.lookup_ptr(&item)) {
                src = group_input_socket(group_input, **found);
              }
              if (dst != nullptr && src != nullptr && group_input != nullptr) {
                bke::node_add_link(tree, *group_input, *src, *sub.instance, *dst);
              }
            };
            link_mapping(outer_.mapping_offset_inputs_, map_offset_ifaces[i]);
            link_mapping(outer_.mapping_scale_inputs_, map_scale_ifaces[i]);
            link_mapping(outer_.mapping_rotation_inputs_, map_rotation_ifaces[i]);
          }
        }
        if (uv_iface != nullptr) {
          bNodeSocket *dst = instance_input(uv_iface);
          auto uv_pair = generated_uv_map_ensure(tree, BKE_paint_layers_uv_map_name(ma));
          if (dst != nullptr && uv_pair.first != nullptr && uv_pair.second != nullptr) {
            bke::node_add_link(tree, *uv_pair.first, *uv_pair.second, *sub.instance, *dst);
          }
        }
        bNodeTree &sub_tree = *sub.tree;
        bNode *sub_input = sub.group_input;
        bNode *sub_output = sub.group_output;
        auto sub_output_socket = [&](bNodeTreeInterfaceSocket *iface) -> bNodeSocket * {
          if (iface == nullptr || iface->identifier == nullptr || sub_input == nullptr) {
            return nullptr;
          }
          return bke::node_find_socket(
              *sub_input, SOCK_OUT, UString::from_ptr_noinline(iface->identifier));
        };
        bNode *factor_node = nullptr;
        bNodeSocket *factor_socket = nullptr;
        if (factor_in != nullptr) {
          factor_node = sub_input;
          factor_socket = sub_output_socket(factor_in);
        }
        for (int mask_index = 0; mask_index < probe.size(); mask_index++) {
          const MaterialPaintLayer &correction = *probe[mask_index];
          const int mask_base_row = mask_index * layout::kMaskItemRows;
          Vector<bNode *> frame_nodes;
          Image *correction_image = paint_layer_mask_correction_image(ma, correction, channel);
          const bool fill = is_fill[mask_index] != 0;
          const bool is_mesh = correction.source == MA_PAINT_LAYER_SOURCE_MESH_MAP;
          const bool mapped = is_mapped[mask_index] != 0;
          if (!fill && !is_mesh && correction_image == nullptr) {
            /* Why skip: an Image item without its map builds nothing yet, like the flat path, but
             * its rows stay reserved through the shared index. */
            continue;
          }
          bNodeSocket *item_opacity = (opacity_ifaces[mask_index] != nullptr) ?
                                          sub_output_socket(opacity_ifaces[mask_index]) :
                                          nullptr;
          bNode *gray_node = nullptr;
          bNodeSocket *gray_color = nullptr;
          bNode *item_map = nullptr;
          bNodeSocket *item_alpha = nullptr;
          bool item_is_data = false;
          if (fill) {
            gray_node = sub_input;
            gray_color = (fill_ifaces[mask_index] != nullptr) ?
                             sub_output_socket(fill_ifaces[mask_index]) :
                             nullptr;
            if (gray_color == nullptr) {
              continue;
            }
          }
          else {
            if (correction_image == nullptr) {
              continue;
            }
            if (!is_mesh) {
              item_is_data = IMB_colormanagement_space_name_is_data(
                  correction_image->colorspace_settings.name);
            }
            item_map = bke::node_add_static_node(nullptr, sub_tree, SH_NODE_TEX_IMAGE);
            if (item_map == nullptr) {
              continue;
            }
            item_map->id = &correction_image->id;
            id_us_plus(&correction_image->id);
            layout::place(*item_map, layout::Column::Mask, mask_base_row + 0);
            frame_nodes.append(item_map);
            if (is_mesh) {
              if (NodeTexImage *storage = static_cast<NodeTexImage *>(item_map->storage)) {
                storage->extension = SHD_IMAGE_EXTENSION_EXTEND;
              }
            }
            if (mapped) {
              bNode *mapping = bke::node_add_static_node(nullptr, sub_tree, SH_NODE_MAPPING);
              if (mapping == nullptr) {
                continue;
              }
              mapping->custom1 = NODE_MAPPING_TYPE_POINT;
              /* Why Source: the item's Image Texture owns Mask base+0, so its Mapping moves two
               * columns left, into Source, and the two share the item row without ever meeting. */
              layout::place(*mapping, layout::Column::Source, mask_base_row + 0);
              frame_nodes.append(mapping);
              bNodeSocket *map_vector = socket_in(*mapping, "Vector");
              bNodeSocket *map_location = socket_in(*mapping, "Location");
              bNodeSocket *map_rotation = socket_in(*mapping, "Rotation");
              bNodeSocket *map_scale = socket_in(*mapping, "Scale");
              bNodeSocket *map_out = socket_out(*mapping, "Vector");
              bNodeSocket *uv_socket = (uv_iface != nullptr) ? sub_output_socket(uv_iface) :
                                                               nullptr;
              bNodeSocket *offset_socket = (map_offset_ifaces[mask_index] != nullptr) ?
                                               sub_output_socket(map_offset_ifaces[mask_index]) :
                                               nullptr;
              bNodeSocket *scale_socket = (map_scale_ifaces[mask_index] != nullptr) ?
                                              sub_output_socket(map_scale_ifaces[mask_index]) :
                                              nullptr;
              bNodeSocket *rotation_socket = (map_rotation_ifaces[mask_index] != nullptr) ?
                                                 sub_output_socket(
                                                     map_rotation_ifaces[mask_index]) :
                                                 nullptr;
              if (map_vector == nullptr || map_out == nullptr) {
                continue;
              }
              if (uv_socket != nullptr && sub_input != nullptr) {
                bke::node_add_link(sub_tree, *sub_input, *uv_socket, *mapping, *map_vector);
              }
              if (map_location != nullptr && offset_socket != nullptr && sub_input != nullptr) {
                bke::node_add_link(sub_tree, *sub_input, *offset_socket, *mapping, *map_location);
              }
              if (map_rotation != nullptr && rotation_socket != nullptr && sub_input != nullptr) {
                bke::node_add_link(
                    sub_tree, *sub_input, *rotation_socket, *mapping, *map_rotation);
              }
              if (map_scale != nullptr && scale_socket != nullptr && sub_input != nullptr) {
                bke::node_add_link(sub_tree, *sub_input, *scale_socket, *mapping, *map_scale);
              }
              if (NodeTexImage *storage = static_cast<NodeTexImage *>(item_map->storage)) {
                storage->extension = SHD_IMAGE_EXTENSION_REPEAT;
              }
              bNodeSocket *tex_vector = socket_in(*item_map, "Vector");
              if (tex_vector != nullptr) {
                bke::node_add_link(sub_tree, *mapping, *map_out, *item_map, *tex_vector);
              }
            }
            item_alpha = socket_out(*item_map, "Alpha");
            gray_color = socket_out(*item_map, "Color");
            gray_node = item_map;
          }
          /* The packed path only differs in where its sockets come from: the subgroup's own
           * inputs. The chain itself is the shared one. */
          const MaskElementGrey grey_mode = is_mesh ? MaskElementGrey::SeparateX :
                                                       MaskElementGrey::Mean;
          bNode *multiply_node = nullptr;
          bNodeSocket *multiply_socket = nullptr;
          if (!fill && !is_mesh) {
            multiply_node = item_map;
            multiply_socket = item_alpha;
          }
          if (!build_mask_element_chain(sub_tree,
                                        correction,
                                        mask_base_row,
                                        grey_mode,
                                        gray_node,
                                        gray_color,
                                        sub_input,
                                        item_opacity,
                                        multiply_node,
                                        multiply_socket,
                                        item_map,
                                        item_alpha,
                                        fill,
                                        (!fill && !is_mesh && item_is_data),
                                        frame_nodes,
                                        factor_node,
                                        factor_socket))
          {
            continue;
          }
          if (!frame_nodes.is_empty()) {
            char frame_label[64];
            SNPRINTF(frame_label,
                     "Mask %d: %s (%s)",
                     mask_index + 1,
                     correction.name,
                     mask_source_word(correction, fill));
            layout::frame_add(sub_tree, frame_label, frame_nodes);
          }
        }
        if (factor_out != nullptr && factor_out->identifier != nullptr && sub_output != nullptr &&
            factor_node != nullptr && factor_socket != nullptr)
        {
          bNodeSocket *dst = bke::node_find_socket(
              *sub_output, SOCK_IN, UString::from_ptr_noinline(factor_out->identifier));
          if (dst != nullptr) {
            bke::node_add_link(sub_tree, *factor_node, *factor_socket, *sub_output, *dst);
          }
        }
        nodes::update_node_declaration_and_sockets(sub_tree, *sub_output);
        nodes::update_node_declaration_and_sockets(tree, *sub.instance);
        generated_uv_maps_wire(sub_tree, BKE_paint_layers_uv_map_name(ma));
        if (factor_out != nullptr && factor_out->identifier != nullptr) {
          bNodeSocket *instance_out = bke::node_find_socket(
              *sub.instance, SOCK_OUT, UString::from_ptr_noinline(factor_out->identifier));
          if (instance_out != nullptr) {
            layer_factor_node = sub.instance;
            layer_factor_socket = instance_out;
          }
        }
        /* Why always return: the subgroup tree, its interface and its instance are already built,
         * so falling through to the flat path would stack flat mask nodes on top of a half-wired
         * instance. The built path is atomic with its return. */
        return;
      }
    }
  }
  if (current.opacity != nullptr) {
    const Vector<const MaterialPaintLayer *> mask_items = paint_layers_build_mask_items(ma, *layer);
    const bool has_mask_corrections = !mask_items.is_empty();
    if (has_mask_corrections) {
      ensure_factor_base(tree, location_x, location_y, layer_factor_node, layer_factor_socket);
      bNode *factor_node = layer_factor_node;
      bNodeSocket *factor_socket = layer_factor_socket;
      /* Why indexed: the deterministic row of a mask item is its position in the
       * stack, so a skipped item still reserves its rows and neighbours never move. */
      for (int mask_index = 0; mask_index < mask_items.size(); mask_index++) {
        const MaterialPaintLayer *mask_item = mask_items[mask_index];
        const MaterialPaintLayer &correction = *mask_item;
        /* Why grid rows: one item owns kMaskItemRows Mask-column slots, the Mix
         * and factor nodes share the same rows in the Factor column. */
        const int mask_base_row = mask_index * layout::kMaskItemRows;
        /* The nodes this item built, framed as `Mask <N>: <name> (<source>)` once placed.
         * Shared nodes (the group input, a wrapper instance, the row Mapping, a nested
         * subtree) never join: they outlive one item and must not hang under its frame. */
        Vector<bNode *> frame_nodes;
        /* A Fill mask that carries its own map reads it like a Paint mask's map below; the grey
         * chain reduces it the same way. A Fill without a map stays the constant. */
        const bool fill = BKE_paint_layers_source_type(correction) ==
                              PaintLayerSourceType::Constant &&
                          paint_layer_mask_correction_image(ma, correction, channel) == nullptr;
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
            frame_nodes.append(p_zero);
            if (p_zero != nullptr) {
              /* Why grid: the Stack seed opens its own item block in Mask column. */
              layout::place(*p_zero, layout::Column::Mask, mask_base_row + 0);
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
            /* Why grid: the Stack straighten keeps its own item rows in Mask column. */
            layout::place(*divide, layout::Column::Mask, mask_base_row + 1);
            layout::label(*divide, "Mask stack");
            frame_nodes.append(divide);
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
                /* Why grid: grey reduction follows its divide one row below. */
                layout::place(*rgbtobw, layout::Column::Mask, mask_base_row + 2);
                bke::node_add_link(tree, *divide, *div_out, *rgbtobw, *bw_in);
                gray_source_node = rgbtobw;
                gray_source_color = bw_out;
                frame_nodes.append(rgbtobw);
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
                /* Why grid: scalar Stack grey follows its divide one row below. */
                layout::place(*separate, layout::Column::Mask, mask_base_row + 2);
                bke::node_add_link(tree, *divide, *div_out, *separate, *sep_vector);
                gray_source_node = separate;
                gray_source_color = sep_x;
                frame_nodes.append(separate);
              }
            }
          }
          else if (mask_alpha_channel) {
            /* The channel itself is the source's coverage: no separate grey/coverage split. */
            std::tie(correction_coverage_node, correction_coverage_socket) =
                resolve_correction_coverage(tree, group_input, location_x, location_y, correction);
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
                  /* Why grid: live mask maps open their item block in Mask column. */
                  layout::place(*map, layout::Column::Mask, mask_base_row + 0);
                  frame_nodes.append(map);
                  if (NodeTexImage *dst = static_cast<NodeTexImage *>(map->storage)) {
                    if (row_source.live_map_iuser != nullptr) {
                      dst->iuser = *row_source.live_map_iuser;
                    }
                  }
                  live_map_configure(*map,
                                     tree,
                                     group_input,
                                     correction,
                                     correction.mask_channel,
                                     location_x - 400.0f,
                                     location_y - 320.0f);
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
                      /* Why grid: baked mask maps open their item block in Mask column. */
                      layout::place(*map, layout::Column::Mask, mask_base_row + 0);
                      frame_nodes.append(map);
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
                  /* Why grid: Node Group bake maps open their item block in Mask column. */
                  layout::place(*map, layout::Column::Mask, mask_base_row + 0);
                  frame_nodes.append(map);
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
              /* Why grid: material mask grey follows its map one row below. */
              layout::place(*rgbtobw, layout::Column::Mask, mask_base_row + 1);
              bke::node_add_link(tree, *value_node, *value_socket, *rgbtobw, *bw_in);
              gray_source_node = rgbtobw;
              gray_source_color = bw_out;
              frame_nodes.append(rgbtobw);
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
              /* Why grid: scalar material mask grey follows its map one row below. */
              layout::place(*separate, layout::Column::Mask, mask_base_row + 1);
              bke::node_add_link(tree, *value_node, *value_socket, *separate, *sep_vector);
              gray_source_node = separate;
              gray_source_color = sep_x;
              frame_nodes.append(separate);
            }
            /* This item's own Fac multiplier: the source's coverage, resolved once more (its
             * own Alpha channel, independent of `mask_channel`). */
            std::tie(correction_coverage_node, correction_coverage_socket) =
                resolve_correction_coverage(tree, group_input, location_x, location_y, correction);
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
          /* Why grid: painted mask maps open their item block in Mask column. */
          layout::place(*correction_map, layout::Column::Mask, mask_base_row + 0);
          frame_nodes.append(correction_map);
          /* A MESH_MAP mask item reads the atlas R, the same value the CPU reads, and is
           * sampled Extend like every other atlas read. */
          mask_map_is_mesh = correction.source == MA_PAINT_LAYER_SOURCE_MESH_MAP;
          if (mask_map_is_mesh) {
            if (NodeTexImage *storage = static_cast<NodeTexImage *>(correction_map->storage)) {
              storage->extension = SHD_IMAGE_EXTENSION_EXTEND;
            }
          }
          else if (std::pair<bNode *, bNodeSocket *> mapping = outer_.mapping_vector_ensure(
                       tree, group_input, correction, location_x - 400.0f, location_y - 320.0f);
                   mapping.first != nullptr && mapping.second != nullptr)
          {
            /* A mapped Fill mask reads its map through its own Mapping node; tiling needs Repeat. */
            if (NodeTexImage *storage = static_cast<NodeTexImage *>(correction_map->storage)) {
              storage->extension = SHD_IMAGE_EXTENSION_REPEAT;
            }
            texture_vector_link_mapped(tree, *correction_map, *mapping.first, *mapping.second);
          }
          correction_alpha = socket_out(*correction_map, "Alpha");
          gray_source_color = socket_out(*correction_map, "Color");
          gray_source_node = correction_map;
        }
        /* The grey the item blends: a Material/Node Group value is already scalar (the
         * source's own coverage on the Alpha channel, its reduced value otherwise), a MeshMap
         * atlas is its R alone, and a painted colour takes the weighted mean. */
        MaskElementGrey grey_mode = MaskElementGrey::Mean;
        bNode *gray_in_node = gray_source_node;
        bNodeSocket *gray_in = gray_source_color;
        if (mask_material_or_group) {
          grey_mode = MaskElementGrey::AlreadyScalar;
          if (correction.mask_channel == PAINT_MATERIAL_CHANNEL_ALPHA) {
            gray_in_node = correction_coverage_node;
            gray_in = correction_coverage_socket;
          }
        }
        else if (mask_map_is_mesh) {
          grey_mode = MaskElementGrey::SeparateX;
        }
        /* Fac is `A * op`: the map alpha or the source's own coverage times the row's opacity.
         * A Fill, a MeshMap atlas and a Material/Node Group mask on its Alpha channel carry no
         * separate multiplier (the grey already is that number), so their factor is the opacity
         * alone. */
        const bool mask_alpha_flat = mask_material_or_group &&
                                     correction.mask_channel == PAINT_MATERIAL_CHANNEL_ALPHA;
        bNode *multiply_node = nullptr;
        bNodeSocket *multiply_socket = nullptr;
        if (!fill && !mask_map_is_mesh && !mask_alpha_flat) {
          if (mask_material_or_group) {
            multiply_node = correction_coverage_node;
            multiply_socket = correction_coverage_socket;
          }
          else {
            multiply_node = correction_map;
            multiply_socket = correction_alpha;
          }
        }
        if (!build_mask_element_chain(tree,
                                      correction,
                                      mask_base_row,
                                      grey_mode,
                                      gray_in_node,
                                      gray_in,
                                      group_input,
                                      correction_opacity,
                                      multiply_node,
                                      multiply_socket,
                                      correction_map,
                                      correction_alpha,
                                      fill,
                                      (!fill && !mask_material_or_group && mask_map_is_data &&
                                       !mask_map_is_mesh),
                                      frame_nodes,
                                      factor_node,
                                      factor_socket))
        {
          continue;
        }
        layer_factor_node = factor_node;
        layer_factor_socket = factor_socket;
        /* Why after place: every node above already sits on its grid rows, so the frame only
         * parents them. A skipped item builds nothing, so it frames nothing; N counts from 1
         * in stack order, matching the reserved item rows. */
        if (!frame_nodes.is_empty()) {
          char frame_label[64];
          SNPRINTF(frame_label,
                   "Mask %d: %s (%s)",
                   mask_index + 1,
                   correction.name,
                   mask_source_word(correction, fill));
          layout::frame_add(tree, frame_label, frame_nodes);
        }
      }
    }
  }
}

}  // namespace bke::paint_layers
}  // namespace blender
