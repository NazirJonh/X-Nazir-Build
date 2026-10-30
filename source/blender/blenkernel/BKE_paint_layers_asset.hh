/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * Smart paint assets: a Material that holds a saved part of a paint-layer stack, tagged so the
 * Asset Browser and the stack's drop actions can tell it apart from an ordinary material.
 *
 * The asset is a Material because the stack is DNA on a Material; the tag lives in the material's
 * custom properties, which survive an append from an asset library even where the asset metadata
 * itself is cleared. They are the primary source of truth; the asset metadata group mirrors them
 * for the Asset Browser and Python.
 */

#include <cstdint>

#include "BLI_span.hh"
#include "BLI_string_ref.hh"

#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_import.hh"

namespace blender {

/**
 * What a smart asset holds.
 *
 * Adding a type touches: this enum, #BKE_paint_layers_asset_type_label,
 * #asset_type_is_valid (paint_layers_asset.cc), the create/insert/can_insert branches there, the
 * operator's RNA enum in outliner_stack_smart_asset.cc, and the Save menu in space_outliner.py.
 */
enum class PaintLayerAssetType : int8_t {
  None = 0,
  /** A stack, usually a folder and everything under it. */
  Material = 1,
  /** One row, or a folder saved on its own. */
  Layer = 2,
  /** One row's mask stack. */
  Mask = 3,
};

/** The user-facing name of a Smart asset of \a type ("Smart Material", "Smart Layer", "Smart Mask"). */
const char *BKE_paint_layers_asset_type_label(PaintLayerAssetType type);

/**
 * What a save of \a rows would lose or drag along. Read-only: it inspects the source and mutates
 * nothing.
 */
struct PaintLayerAssetPortability {
  /** Blocking. */
  int curve_patch_num = 0;
  /** Reader names an Anchor Point outside the saved set. */
  int foreign_reader_num = 0;
  /** File image that is neither packed nor generated. */
  int external_image_num = 0;
  /** Sequence/movie/viewer image: copy has no pixels. */
  int movie_image_num = 0;
  /** Row or correction that reads another material. */
  int material_source_num = 0;
  /** Active object masks that will not be saved. */
  int object_mask_num = 0;

  bool blocking() const
  {
    return curve_patch_num > 0;
  }
};

/**
 * Inspect what saving \a rows of \a source as a Smart asset of \a type would carry.
 *
 * The walk follows the branch saved by the type: #PaintLayerAssetType::Mask visits only the single
 * row's mask stack, every other type visits rows, effects, mask items and children. An Anchor
 * Point Reader counts as foreign when its marker belongs to no node of the saved branch.
 */
PaintLayerAssetPortability BKE_paint_layers_asset_portability_check(
    const Material &source, Span<const MaterialPaintLayer *> rows, PaintLayerAssetType type);

/** The asset type stored on \a ma, or None when it is not a smart asset. */
PaintLayerAssetType BKE_paint_layers_asset_type_get(const Material &ma);

/**
 * The Asset Browser catalog a new smart asset of \a type is placed in by default, e.g. for a mask
 * "Paint/Smart Masks". Returns an empty string for #PaintLayerAssetType::None.
 */
const char *BKE_paint_layers_asset_default_catalog(PaintLayerAssetType type);

/**
 * Store \a type and format version on \a ma. The material custom properties are primary; the
 * values are mirrored into the asset metadata when the material is already marked as an asset.
 */
void BKE_paint_layers_asset_type_set(Material &ma, PaintLayerAssetType type);

/**
 * Apply the metadata a freshly marked Smart asset carries: the type mirror, a description, the two
 * tags the Asset Browser groups it by and its catalog. An empty \a description falls back to the
 * default text.
 *
 * The catalog is passed as its parts, so this does not depend on the asset system. Calling it
 * again is safe: the tags are ensured rather than added.
 */
void BKE_paint_layers_asset_metadata_apply(Material &asset,
                                           PaintLayerAssetType type,
                                           StringRef description,
                                           const bUUID &catalog_id,
                                           StringRef catalog_simple_name);

/**
 * Remove the asset tag from both the material custom properties and the asset metadata. The
 * material becomes ordinary again; this is the future "Convert to regular material" operation.
 *
 * Clearing only the asset metadata (the Asset Browser's "Clear Asset") deliberately leaves the
 * custom-property tag in place: it is what makes an appended asset keep its kind.
 */
void BKE_paint_layers_asset_type_clear(Material &ma);

/**
 * The stack row whose mask a Smart Mask asset holds: the first row with a mask stack, or null when
 * the asset is malformed.
 */
const MaterialPaintLayer *BKE_paint_layers_asset_mask_holder_find(const Material &asset);

/**
 * Whether a Smart asset can be inserted into \a dst at \a anchor by \a place. This is the one
 * predicate behind both the drop's `can_accept` and #BKE_paint_layers_asset_insert, so a gesture
 * the UI accepts cannot fail silently later.
 *
 * \param r_reason: when the answer is false, a translated hint for the UI; unchanged otherwise.
 */
bool BKE_paint_layers_asset_can_insert(const Material &dst,
                                       const Material &asset,
                                       const MaterialPaintLayer *anchor,
                                       PaintLayerPlace place,
                                       const char **r_reason = nullptr);

/**
 * Create a new Material holding copies of \a rows (or of the single row's mask, for
 * #PaintLayerAssetType::Mask) from \a source.
 *
 * A Layer asset holds exactly one stack row (a folder counts as one); a Mask asset holds exactly
 * one row's mask. The material is not marked as an asset here: that is the editor's job
 * (#ed::asset::mark_id in ED_asset_mark_clear.hh), together with the preview and the library
 * placement. The type is already stored in the material's custom properties; the editor calls
 * #BKE_paint_layers_asset_type_set again after marking, to mirror it into the asset metadata
 * (idempotent). The stack rows are imported with #PAINT_LAYER_IMPORT_ENABLE_SOURCE_CHANNELS, so
 * the asset keeps every channel it uses. Returns null when the import is refused; the new material
 * is freed in that case.
 *
 * \param image_policy: what happens to the imported maps: copied with their pixels by default, or
 * external ones shared by reference with painted ones copied (#PaintLayerImageImportPolicy::Share)
 * or left as flat values (#PaintLayerImageImportPolicy::Strip).
 * \param r_result: the import's report, its #PaintLayerImportResult::stripped_maps included.
 */
Material *BKE_paint_layers_asset_create(Main &bmain,
                                        Material &source,
                                        Span<const MaterialPaintLayer *> rows,
                                        PaintLayerAssetType type,
                                        const char *name,
                                        PaintLayerImageImportPolicy image_policy =
                                            PaintLayerImageImportPolicy::Copy,
                                        PaintLayerImportResult *r_result = nullptr);

/**
 * Whether the mask branch of \a row holds an Anchor Point marker or reader: a Smart Mask is built
 * from the base mask and its corrections only, and the points belong to the row they slice, so
 * they cannot travel in a mask asset.
 */
bool BKE_paint_layers_mask_branch_has_anchor(const MaterialPaintLayer &row);

/**
 * Insert the rows (or mask) of \a asset into \a dst at \a anchor by \a place. The asset type
 * decides the import mode and flags; the caller only names the destination. File textures are read
 * by reference (#PaintLayerImageImportPolicy::Share): a drop must not mint a copy of the same image
 * every time, and a stroke on a shared map copies it first.
 *
 * \param append_mask: Smart Mask only; lay the asset's corrections over the target's own mask
 * instead of replacing it.
 * \return false when #BKE_paint_layers_asset_can_insert refuses the insert; \a dst is then
 * untouched.
 */
bool BKE_paint_layers_asset_insert(Main &bmain,
                                   Material &dst,
                                   Material &asset,
                                   MaterialPaintLayer *anchor,
                                   PaintLayerPlace place,
                                   PaintLayerImportResult *r_result = nullptr,
                                   bool append_mask = false);

}  // namespace blender
