/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 *
 * Implementation of #pbr_material_from_images / #pbr_material_mark_asset, see the header for the
 * contracts. Node graph conventions follow the FBX importer (`fbx_import_material.cc`); the
 * Normal Map convention option uses the core `NodeShaderNormalMap::convention` field
 * (OpenGL/DirectX).
 */

#include "ED_asset_pbr_material.hh"

#include "BKE_asset.hh"
#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_material.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_update.hh"

#include "BLI_map.hh"
#include "BLI_set.hh"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "DNA_asset_types.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"

#include "ED_asset_mark_clear.hh"

#include "IMB_colormanagement.hh"

#include "RNA_types.hh"

namespace blender::ed::asset {

/* -------------------------------------------------------------------- */
/** \name Enum items
 * \{ */

const EnumPropertyItem *pbr_normal_format_items()
{
  static const EnumPropertyItem items[] = {
      {int(PbrNormalFormat::OpenGL),
       "OPENGL",
       0,
       "OpenGL",
       "Source normal maps follow the OpenGL convention (Blender default)"},
      {int(PbrNormalFormat::DirectX),
       "DIRECTX",
       0,
       "DirectX",
       "Source normal maps follow the DirectX convention; the Normal Map node converts them to "
       "OpenGL"},
      {0, nullptr, 0, nullptr, nullptr},
  };
  return items;
}

const EnumPropertyItem *pbr_height_mode_items()
{
  static const EnumPropertyItem items[] = {
      {int(PbrHeightMode::None),
       "NONE",
       0,
       "None",
       "Leave the height texture unconnected in the node tree"},
      {int(PbrHeightMode::Bump),
       "BUMP",
       0,
       "Bump",
       "Connect the height texture through a Bump node into the normal input"},
      {int(PbrHeightMode::Displacement),
       "DISPLACEMENT",
       0,
       "Displacement",
       "Connect the height texture through a Displacement node to the material output"},
      {0, nullptr, 0, nullptr, nullptr},
  };
  return items;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Channel registry
 * \{ */

/* Built-in map type identifiers (see the built-in rows of the Name Matching preferences). */
static constexpr const char *map_id_base_color = "BASE_COLOR";
static constexpr const char *map_id_ao = "AO";
static constexpr const char *map_id_mask = "MASK";

/** How a built-in channel is wired; channels needing special nodes get their own kind. */
enum class PbrChannelKind : int8_t {
  BaseColor,
  Ao,
  /** Image color output straight into a Principled BSDF input. */
  Scalar,
  /** Like #Scalar, plus the (otherwise zero) emission strength is raised. */
  Emission,
  /** Image alpha output into Principled Alpha, switches the material to blended rendering. */
  Alpha,
  /** Image -> Normal Map node, wired into the normal chain. */
  Normal,
  /** Image placed for the Bump/Displacement chain. */
  Height,
};

struct PbrChannel {
  const char *identifier;
  PbrChannelKind kind;
  /** Principled BSDF input for #PbrChannelKind::Scalar / #PbrChannelKind::Emission. */
  const char *bsdf_socket;
  /** Data maps use the non-color color space, color maps keep the image's own. */
  bool is_data;
};

/* Processing order of the channels (after Base Color and AO, which are wired together). */
static constexpr PbrChannel pbr_channels[] = {
    {map_id_base_color, PbrChannelKind::BaseColor, "Base Color", false},
    {map_id_ao, PbrChannelKind::Ao, "", true},
    {"METALLIC", PbrChannelKind::Scalar, "Metallic", true},
    {"ROUGHNESS", PbrChannelKind::Scalar, "Roughness", true},
    {"SPECULAR", PbrChannelKind::Scalar, "Specular IOR Level", true},
    {"EMISSION", PbrChannelKind::Emission, "Emission Color", false},
    {"ALPHA", PbrChannelKind::Alpha, "Alpha", true},
    {"NORMAL", PbrChannelKind::Normal, "", true},
    {"HEIGHT", PbrChannelKind::Height, "", true},
};

static constexpr const char *socket_emission_strength = "Emission Strength";

/**
 * Utility maps (Patterns, Grunge, custom types) are placed unconnected. Mask is a color map
 * because it is authored as a visible image; everything else is treated as data.
 */
static bool utility_map_is_data(const std::string &identifier)
{
  return !identifier.empty() && identifier != map_id_mask;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Node helpers
 * \{ */

/* Nodes are arranged in columns by type, with manually placed x coordinates
 * based on node widths. */
static constexpr float node_locx_image = -1200.0f;
static constexpr float node_locx_normalmap = -900.0f;
static constexpr float node_locx_mid = -600.0f;
static constexpr float node_locx_bsdf = -300.0f;
static constexpr float node_locx_output = 0.0f;

/* Nodes are arranged in rows; one row for each image being used. */
static constexpr float node_locy_top = 300.0f;
static constexpr float node_locy_step = 300.0f;

static bNode *add_node(bNodeTree *ntree, const int type, const float x, const float y)
{
  bNode *node = bke::node_add_static_node(nullptr, *ntree, type);
  if (node == nullptr) {
    BLI_assert_unreachable();
    return nullptr;
  }
  node->location[0] = x;
  node->location[1] = y;
  return node;
}

/**
 * Link `from.output -> to.input`. With \a only_if_free the link is skipped when the destination
 * already has one (first link wins, so an earlier wiring of the same input is never
 * overwritten); multi-input nodes like Vector Math pass false.
 */
static bool link_sockets(bNodeTree *ntree,
                         bNode *from_node,
                         const char *from_socket_id,
                         bNode *to_node,
                         const char *to_socket_id,
                         const bool only_if_free = true)
{
  if (from_node == nullptr || to_node == nullptr) {
    return false;
  }
  bNodeSocket *from_sock = bke::node_find_socket(*from_node, SOCK_OUT, UString(from_socket_id));
  bNodeSocket *to_sock = bke::node_find_socket(*to_node, SOCK_IN, UString(to_socket_id));
  if (from_sock == nullptr || to_sock == nullptr) {
    BLI_assert_unreachable();
    return false;
  }
  if (only_if_free && bke::node_count_socket_links(*ntree, *to_sock) != 0) {
    return false;
  }
  bke::node_add_link(*ntree, *from_node, *from_sock, *to_node, *to_sock);
  return true;
}

static void set_socket_float(const char *socket_id, const float value, bNode *node)
{
  bNodeSocket *socket = bke::node_find_socket(*node, SOCK_IN, UString(socket_id));
  BLI_assert(socket != nullptr && socket->type == SOCK_FLOAT);
  if (socket != nullptr) {
    socket->default_value_typed<bNodeSocketValueFloat>()->value = value;
  }
}

/**
 * Add an image texture node for \a image. Data maps get the non-color color space assigned, like
 * the FBX importer does for its non-color sockets. The image is only modified (and its buffers
 * invalidated) when its color space actually differs, so user images that are already correct
 * are left untouched.
 */
static bNode &image_node_add(
    Main *bmain, bNodeTree *ntree, Image &image, const bool is_data, const float locy)
{
  bNode *node = add_node(ntree, SH_NODE_TEX_IMAGE, node_locx_image, locy);
  BLI_assert(node != nullptr);
  node->id = &image.id;
  id_us_plus(&image.id);

  if (is_data) {
    const char *data_name = IMB_colormanagement_role_colorspace_name_get(COLOR_ROLE_DATA);
    if (!STREQ(image.colorspace_settings.name, data_name)) {
      STRNCPY_UTF8(image.colorspace_settings.name, data_name);
      /* Same invalidation the RNA setter does, otherwise the buffer / GPU texture stay in the
       * old color space. */
      BKE_image_signal(bmain, &image, nullptr, IMA_SIGNAL_COLORMANAGE);
    }
  }
  return *node;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Material building
 * \{ */

struct BuildContext {
  Main *bmain;
  const PbrMaterialBuildParams &params;
  Material *material;
  bNodeTree *ntree;
  bNode *bsdf;
  bNode *output;
  PbrMaterialResult &result;
  /** Y location of the next image node row. */
  float locy = node_locy_top;

  bNode *normal_map_node = nullptr;
  bNode *height_image_node = nullptr;

  bNode &image_node(Image &image, const bool is_data)
  {
    bNode &node = image_node_add(this->bmain, this->ntree, image, is_data, this->locy);
    this->locy -= node_locy_step;
    return node;
  }
};

/* Base color and AO are wired together so the AO mix can replace the plain base color link
 * without re-linking. */
static void wire_base_color_and_ao(BuildContext &ctx)
{
  Image *base_color_image = ctx.params.maps.lookup_default(map_id_base_color, nullptr);
  Image *ao_image = ctx.params.maps.lookup_default(map_id_ao, nullptr);
  const bool ao_mix = ao_image != nullptr && ctx.params.mix_ao;
  bNodeTree *ntree = ctx.ntree;

  if (ao_mix && base_color_image != nullptr) {
    bNode &base_node = ctx.image_node(*base_color_image, false);
    bNode &ao_node = ctx.image_node(*ao_image, true);

    /* Multiply AO into the base color (classic glTF-style AO). The second input of the
     * Vector Math node is named "Vector" too; its unique socket identifier is "Vector_001". */
    bNode *mix = add_node(ntree, SH_NODE_VECTOR_MATH, node_locx_mid, ctx.locy + node_locy_step);
    mix->custom1 = NODE_VECTOR_MATH_MULTIPLY;
    link_sockets(ntree, &ao_node, "Color", mix, "Vector", false);
    link_sockets(ntree, &base_node, "Color", mix, "Vector_001", false);
    link_sockets(ntree, mix, "Vector", ctx.bsdf, "Base Color");

    ctx.result.wired_map_types.append(map_id_base_color);
    ctx.result.wired_map_types.append(map_id_ao);
    return;
  }

  if (base_color_image != nullptr) {
    bNode &base_node = ctx.image_node(*base_color_image, false);
    link_sockets(ntree, &base_node, "Color", ctx.bsdf, "Base Color");
    ctx.result.wired_map_types.append(map_id_base_color);
  }
  if (ao_image != nullptr) {
    bNode &ao_node = ctx.image_node(*ao_image, true);
    if (ao_mix) {
      /* No base color map to multiply with: wire AO as the base color itself. */
      link_sockets(ntree, &ao_node, "Color", ctx.bsdf, "Base Color");
    }
    ctx.result.wired_map_types.append(map_id_ao);
  }
}

/* All remaining built-in channels, driven by the #pbr_channels table. */
static void wire_registered_maps(BuildContext &ctx)
{
  bNodeTree *ntree = ctx.ntree;

  for (const PbrChannel &channel : pbr_channels) {
    if (ELEM(channel.kind, PbrChannelKind::BaseColor, PbrChannelKind::Ao)) {
      continue;
    }
    Image *image = ctx.params.maps.lookup_default(channel.identifier, nullptr);
    if (image == nullptr) {
      continue;
    }

    switch (channel.kind) {
      case PbrChannelKind::Scalar: {
        bNode &image_node = ctx.image_node(*image, channel.is_data);
        link_sockets(ntree, &image_node, "Color", ctx.bsdf, channel.bsdf_socket);
        break;
      }
      case PbrChannelKind::Emission: {
        bNode &image_node = ctx.image_node(*image, channel.is_data);
        link_sockets(ntree, &image_node, "Color", ctx.bsdf, channel.bsdf_socket);
        /* Emission is invisible with the default zero strength. */
        set_socket_float(socket_emission_strength, 1.0f, ctx.bsdf);
        break;
      }
      case PbrChannelKind::Alpha: {
        bNode &image_node = ctx.image_node(*image, channel.is_data);
        if (link_sockets(ntree, &image_node, "Alpha", ctx.bsdf, channel.bsdf_socket)) {
          ctx.material->blend_method = MA_BM_BLEND;
          ctx.material->surface_render_method = MA_SURFACE_METHOD_FORWARD;
        }
        break;
      }
      case PbrChannelKind::Normal: {
        const float locy = ctx.locy;
        bNode &image_node = ctx.image_node(*image, channel.is_data);
        ctx.normal_map_node = add_node(ntree, SH_NODE_NORMAL_MAP, node_locx_normalmap, locy);
        NodeShaderNormalMap *storage = static_cast<NodeShaderNormalMap *>(
            ctx.normal_map_node->storage);
        BLI_assert(storage != nullptr);
        if (storage != nullptr) {
          storage->convention = (ctx.params.normal_format == PbrNormalFormat::DirectX) ?
                                    SHD_NORMAL_MAP_CONVENTION_DIRECTX :
                                    SHD_NORMAL_MAP_CONVENTION_OPENGL;
        }
        link_sockets(ntree, &image_node, "Color", ctx.normal_map_node, "Color");
        /* The Normal Map output is wired into the (possibly bumped) normal chain afterwards,
         * once the height map has been placed. */
        break;
      }
      case PbrChannelKind::Height: {
        ctx.height_image_node = &ctx.image_node(*image, channel.is_data);
        break;
      }
      case PbrChannelKind::BaseColor:
      case PbrChannelKind::Ao:
        BLI_assert_unreachable();
        break;
    }
    ctx.result.wired_map_types.append(channel.identifier);
  }
}

/* Utility maps (Mask, Patterns, Grunge, custom types): image nodes without any link, ready for
 * the user / the paint system to pick up. */
static void add_utility_maps(BuildContext &ctx)
{
  Set<std::string> handled;
  for (const std::string &identifier : ctx.result.wired_map_types) {
    handled.add(identifier);
  }

  for (const MapItem<std::string, Image *> item : ctx.params.maps.items()) {
    if (item.value == nullptr || handled.contains(item.key)) {
      continue;
    }
    ctx.image_node(*item.value, utility_map_is_data(item.key));
    ctx.result.wired_map_types.append(item.key);
  }
}

/* Normal chain: Normal Map -> Bump -> Principled. The Bump node takes the height map and the
 * (optional) Normal Map output feeds its normal input, so both effects combine. The
 * Displacement variant goes through a Displacement node, as the output socket is a vector. */
static void wire_normal_and_height(BuildContext &ctx)
{
  bNodeTree *ntree = ctx.ntree;
  const bool has_height = ctx.height_image_node != nullptr;

  /* TODO: Bump/Displacement strength, scale and midlevel are not configurable yet; the nodes
   * use their defaults. */
  if (has_height && ctx.params.height_mode == PbrHeightMode::Bump) {
    bNode *bump = add_node(ntree, SH_NODE_BUMP, node_locx_mid, node_locy_top);
    link_sockets(ntree, ctx.height_image_node, "Color", bump, "Height");
    link_sockets(ntree, ctx.normal_map_node, "Normal", bump, "Normal");
    link_sockets(ntree, bump, "Normal", ctx.bsdf, "Normal");
  }
  else if (ctx.normal_map_node != nullptr) {
    link_sockets(ntree, ctx.normal_map_node, "Normal", ctx.bsdf, "Normal");
  }

  if (has_height && ctx.params.height_mode == PbrHeightMode::Displacement) {
    bNode *displacement = add_node(ntree, SH_NODE_DISPLACEMENT, node_locx_mid, node_locy_top);
    link_sockets(ntree, ctx.height_image_node, "Color", displacement, "Height");
    link_sockets(ntree, displacement, "Displacement", ctx.output, "Displacement");
    ctx.material->displacement_method = MA_DISPLACEMENT_BOTH;
  }
}

PbrMaterialResult pbr_material_from_images(Main *bmain, const PbrMaterialBuildParams &params)
{
  BLI_assert(!params.name.empty());
  BLI_assert(bmain != nullptr);

  PbrMaterialResult result;
  result.material = BKE_material_add(bmain, params.name.c_str());
  bNodeTree *ntree = result.material->nodetree;

  bNode *bsdf = add_node(ntree, SH_NODE_BSDF_PRINCIPLED, node_locx_bsdf, node_locy_top);
  bNode *output = add_node(ntree, SH_NODE_OUTPUT_MATERIAL, node_locx_output, node_locy_top);
  link_sockets(ntree, bsdf, "BSDF", output, "Surface");
  bke::node_set_active(*ntree, *output);

  BuildContext ctx{bmain, params, result.material, ntree, bsdf, output, result};
  wire_base_color_and_ao(ctx);
  wire_registered_maps(ctx);
  add_utility_maps(ctx);
  wire_normal_and_height(ctx);

  BKE_ntree_update_after_single_tree_change(*bmain, *ntree);
  return result;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Asset marking
 * \{ */

bool pbr_material_mark_asset(const bContext *C,
                             Material &material,
                             const PbrMaterialAssetParams &asset_params,
                             const Span<std::string> map_type_identifiers)
{
  ID *id = &material.id;
  if (id->asset_data == nullptr) {
    if (!mark_id(id)) {
      return false;
    }
  }

  AssetMetaData *asset_data = id->asset_data;
  if (!BLI_uuid_is_nil(asset_params.catalog_id)) {
    BKE_asset_metadata_catalog_id_set(
        asset_data, asset_params.catalog_id, asset_params.catalog_simple_name.c_str());
  }
  if (!asset_params.tag.empty()) {
    BKE_asset_metadata_tag_ensure(asset_data, asset_params.tag.c_str());
  }
  for (const std::string &identifier : map_type_identifiers) {
    BKE_asset_metadata_map_tag_ensure(asset_data, identifier.c_str());
  }

  generate_preview(C, id);
  return true;
}

/** \} */

}  // namespace blender::ed::asset
