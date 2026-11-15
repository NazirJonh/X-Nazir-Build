/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 *
 * Building Principled BSDF materials from a set of PBR images (map-type identifier -> image),
 * and marking the result as an asset with catalog/tags/preview.
 *
 * Shared by the "Import Images as Assets" dialog (#ASSET_OT_image_import_mark) and the
 * Python-facing #ASSET_OT_pbr_material_from_images operator, so addons can implement their own
 * PBR-textures-as-material workflows with the exact same result. The node graph conventions
 * (socket names, color spaces, Normal Map convention) follow the FBX/USD/OBJ importers.
 */

#pragma once

#include "BLI_map.hh"
#include "BLI_span.hh"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include <cstdint>
#include <string>

namespace blender {

struct bContext;
struct EnumPropertyItem;
struct Image;
struct Main;
struct Material;

namespace ed::asset {

/** Normal map encoding convention (see #NodeShaderNormalMap::convention). */
enum class PbrNormalFormat : int8_t {
  /** Source normal maps are already in the OpenGL convention (Blender default). */
  OpenGL = 0,
  /** Source normal maps use the DirectX convention; the Normal Map node flips the green
   * channel internally (all render engines support this). */
  DirectX = 1,
};

/** How a Height map is wired into the material. */
enum class PbrHeightMode : int8_t {
  /** Place the image node without connecting it. */
  None = 0,
  /** Height -> Bump.Height -> (Normal Map ->) Principled Normal. */
  Bump = 1,
  /** Height -> Displacement node -> Material Output Displacement,
   * #Material::displacement_method = BOTH. */
  Displacement = 2,
};

/**
 * RNA enum items for the options above, so adding a mode only touches the enum and the item
 * list next to it. The arrays are terminated with a null identifier.
 */
const EnumPropertyItem *pbr_normal_format_items();
const EnumPropertyItem *pbr_height_mode_items();

struct PbrMaterialBuildParams {
  /** Final material name; uniquified by #BKE_material_add. Must not be empty. */
  std::string name;
  PbrNormalFormat normal_format = PbrNormalFormat::OpenGL;
  PbrHeightMode height_mode = PbrHeightMode::Bump;
  /** Multiply an AO map into Base Color instead of leaving a loose image node. */
  bool mix_ao = true;
  /** Map type identifier (e.g. "BASE_COLOR") -> image. Null images are skipped. */
  Map<std::string, Image *> maps;
};

struct PbrMaterialResult {
  Material *material = nullptr;
  /** Map type identifiers actually placed in the material, in stable order. */
  Vector<std::string> wired_map_types;
};

/** Asset metadata applied by #pbr_material_mark_asset. */
struct PbrMaterialAssetParams {
  /** Nil when the material should not be assigned to a catalog. */
  bUUID catalog_id = {};
  std::string catalog_simple_name;
  /** Optional free-form asset tag. */
  std::string tag;
};

/**
 * Build a Principled BSDF material from a set of PBR images.
 *
 * Creates the material in \a bmain, builds the node tree (image texture nodes, Normal Map node
 * with the requested convention, Bump/Displacement for the height map, AO mixing) and tags the
 * tree update. Does NOT mark the material as an asset and does NOT generate a preview; use
 * #pbr_material_mark_asset for that.
 *
 * The node users of the images in \a params.maps are incremented (the image nodes hold them).
 * Data maps get the non-color color space assigned (the image is only touched when its color
 * space differs).
 */
PbrMaterialResult pbr_material_from_images(Main *bmain, const PbrMaterialBuildParams &params);

/**
 * Mark \a material as asset and set up its metadata:
 * - catalog (when the catalog id of \a asset_params is not nil);
 * - the free-form tag (when non-empty);
 * - a map-tag for every identifier in \a map_type_identifiers (so the material participates in
 *   name-match filters like its textures do);
 * - a fresh rendered preview.
 *
 * \return false when the material could not be marked as asset; it then stays a regular
 * material.
 */
bool pbr_material_mark_asset(const bContext *C,
                             Material &material,
                             const PbrMaterialAssetParams &asset_params,
                             Span<std::string> map_type_identifiers);

}  // namespace ed::asset
}  // namespace blender
