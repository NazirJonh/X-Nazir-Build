/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 */

#pragma once

#include <optional>

namespace blender {

struct Brush;
struct ID;
struct Main;
struct ReportList;
struct Scene;
struct View3D;
struct ViewLayer;
namespace asset_system {
class AssetRepresentation;
}

namespace ed::asset {

struct ImportInstantiateContext {
  Scene *scene;
  ViewLayer *view_layer;
  View3D *view3d;
};

/**
 * If the asset already has a corresponding local #ID, return it. Otherwise, link or append the
 * asset's data-block, using "Append & Reuse" if the method is unspecified.
 *
 * \note This can return null! Importing can fail if the asset was deleted or moved since the asset
 * library was loaded.
 *
 * \param import_method: Overrides library's default importing method.
 * If not set and the library has no default, #ASSET_IMPORT_APPEND_REUSE will be used.
 */
ID *asset_local_id_ensure_imported(
    Main &bmain,
    const asset_system::AssetRepresentation &asset,
    int flags = 0, /* #eFileSel_Params_Flag + #eBLOLibLinkFlags */
    const std::optional<eAssetImportMethod> import_method = std::nullopt,
    const std::optional<ImportInstantiateContext> instantiate_context = std::nullopt,
    ReportList *reports = nullptr);

/**
 * #asset_local_id_ensure_imported for a brush asset, always "Append & Reuse", plus the asset's
 * thumbnail copied onto a freshly appended brush (its asset data, preview included, is cleared on
 * append). Only for the brush pickers that list the file's own brushes; every other caller keeps
 * the plain import. Null when the import failed or the asset is not a brush.
 */
Brush *asset_local_brush_ensure_imported(Main &bmain,
                                         const asset_system::AssetRepresentation &asset,
                                         ReportList *reports = nullptr);

}  // namespace ed::asset
}  // namespace blender
