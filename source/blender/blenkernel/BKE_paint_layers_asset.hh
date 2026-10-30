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
#include <string>

#include "BLI_function_ref.hh"
#include "BLI_span.hh"
#include "BLI_string_ref.hh"

#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_import.hh"

#include "DNA_ID.h"

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
 * \param image_policy: what happens to the painted maps: copied with their pixels by default, or
 * external ones shared by reference with painted ones copied (#PaintLayerImageImportPolicy::Share)
 * or left as flat values (#PaintLayerImageImportPolicy::Strip). The maps assigned to a Fill row or
 * correction are textures, not painted data: they are always copied with their pixels, whatever the
 * policy says, so the asset carries them.
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

/* -------------------------------------------------------------------- */
/** \name Exposed parameters
 *
 * A smart asset exposes parameters to its instances: the asset carries a list of what to show,
 * and an import marks the copied rows' parameters so the instance's panel can find them. The list
 * is the selection: #BKE_paint_layers_asset_params_expose builds it with every kind parameter of
 * every saved item (a filter's radius, a mask operator's tolerance), and
 * #BKE_paint_layers_asset_param_expose_set trims it per parameter from the asset's own panel.
 *
 * The asset's list lives in the material's custom properties under #ASSET_PARAMS_PROP_NAME and is
 * mirrored into the asset metadata, the way the asset type tag is. It is keyed by item name --
 * the one identity that survives a copy into another material, unlike the marker, which is
 * re-issued on import. Each entry holds one integer per exposed parameter, named after the
 * parameter and storing its position in the kind's schema, so an import can tell a schema that
 * moved from one that kept its layout.
 *
 * The instance side is a marker subgroup of the same name inside a copied item's
 * #MaterialPaintLayer::system_properties, one integer per exposed parameter. The values stay
 * where they always were -- the row's own properties, copied as locals -- so the marker is a
 * binding-lite label: it survives undo, save and load with the row, and removing it (make unique)
 * changes nothing about the pixels.
 * \{ */

/** The group name, on the asset material and inside a marked item's system properties. */
constexpr const char *ASSET_PARAMS_PROP_NAME = "paint_asset_params";

/** The exposed-parameters list of \a ma, or null when it is not a smart asset with one. */
const IDProperty *BKE_paint_layers_asset_params_get(const Material &ma);

/** Whether \a ma carries an exposed-parameters list. */
bool BKE_paint_layers_asset_params_has(const Material &ma);

/**
 * (Re)build \a ma's exposed-parameters list from its own stack: every kind parameter of every row,
 * correction, mask item and folder child, keyed by item name. Mirror it into the asset metadata
 * when the material is already marked as an asset.
 *
 * This is the expose-everything build an asset starts with; trimming it afterwards is
 * #BKE_paint_layers_asset_param_expose_set's job. A re-save must not wipe that trimming: when \a
 * ma already carries a list, an item it has an entry for keeps the author's subset -- only the
 * parameters the entry names stay exposed, their stored positions refreshed to the current schema
 * -- and an item the list has no entry for is new to it and arrives with everything exposed.
 * Entries of items gone from the stack go with them.
 */
void BKE_paint_layers_asset_params_expose(Material &ma);

/**
 * Whether \a ma's exposed-parameters list has an entry for \a item's parameter \a param_name: the
 * same list an import reads its marks from, so false here means the parameter imports as a plain
 * local. False too when \a ma carries no list (it is not a smart asset).
 */
bool BKE_paint_layers_asset_param_is_exposed(const Material &ma,
                                             const MaterialPaintLayer &item,
                                             const char *param_name);

/**
 * Add or remove \a item's \a param_name from \a ma's exposed-parameters list. The list is the
 * selection: what an import marks and binds, and what the instances' panel shows. An added entry
 * carries the position the item's own schema has the parameter at, so an import can still tell a
 * schema that moved from one that kept its layout; removing the last entry of an item removes the
 * item's group with it. The asset-metadata mirror is refreshed either way.
 *
 * Does nothing when \a ma has no list of its own (it is not a smart asset) or the item's kind has
 * no parameter of that name. The list is keyed by item name, so two same-named items share one
 * entry and toggling one toggles both, the way #BKE_paint_layers_asset_params_expose builds it.
 */
void BKE_paint_layers_asset_param_expose_set(Material &ma,
                                             const MaterialPaintLayer &item,
                                             const char *param_name,
                                             bool expose);

/**
 * Mark the exposed parameters on the items just copied from \a src, reading \a src's
 * exposed-parameters list and checking every entry against the copied item's own kind schema. An
 * entry whose name has no item, or whose position no longer names that parameter, is skipped: the
 * asset and the target build may disagree, and a stale label is worse than a missing one. \a items
 * are the top-level copies (imported rows, or the mask items a mask import staged).
 */
void BKE_paint_layers_asset_params_mark_imported(const Material &src,
                                                 Span<const MaterialPaintLayer *> items);

/**
 * Remove the exposed-parameter markers from \a ma's stack, and the instance records with them.
 * With \a instance_id of 0 (the asset-create path) every marker of every item goes and all
 * instances are removed. With an instance's id only that instance's rows lose their markers and
 * only its record goes; the other instances keep following their assets. The values are the rows'
 * own by then; this only ends the binding.
 */
void BKE_paint_layers_asset_params_unbind(Material &ma, int instance_id = 0);

/**
 * Whether \a item's parameter \a param_name is an active instance parameter: the exposed marker is
 * there and not switched off. A marker of any shape but an Int 0 reads as active -- an import
 * writes Int 1 and older files carry markers without a usable value, so only the explicit 0 means
 * the user turned the parameter off on this instance.
 */
bool BKE_paint_layers_item_param_is_exposed(const MaterialPaintLayer &item,
                                            const char *param_name);

/**
 * Whether the asset offered \a item's parameter \a param_name to this instance at all: the exposed
 * marker is there, active or switched off. This is what the instance panel lists; a parameter
 * without the marker was never offered and the panel shows nothing for it.
 */
bool BKE_paint_layers_item_param_is_offered(const MaterialPaintLayer &item,
                                            const char *param_name);

/**
 * Switch \a item's offered parameter \a param_name on or off for this instance alone: the marker
 * stays (Int 1 / Int 0), so the parameter stays offered -- off only greys the panel row out and
 * makes a reload take the asset's own value instead of the carried one. The asset and the other
 * instances are not touched. A parameter without the marker is not offered, and there is nothing
 * to switch: nothing is written and no marker is made.
 */
void BKE_paint_layers_item_param_enabled_set(MaterialPaintLayer &item,
                                             const char *param_name,
                                             bool enabled);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Smart-asset instances
 *
 * A material may hold several drops of Smart assets -- the same asset twice, or different assets
 * side by side -- and each drop is an instance: its own source label and its own set of rows.
 * The instances live in the material's custom properties under #PAINT_ASSET_INSTANCES_PROP_NAME,
 * one subgroup per instance keyed by its id as a string; the single #PAINT_ASSET_SOURCE_PROP_NAME
 * of the one-instance format is read as instance 1 until the next record or reload writes the
 * new group (a read never mutates the properties).
 *
 * A row belongs to an instance through that instance's `rows` marker set only -- nothing is
 * written into the row -- so a duplicated row (a new marker) belongs to no instance, and the
 * nested items of a row follow the nearest root above them. \{ */

/** The custom-property group holding one subgroup per instance. */
constexpr const char *PAINT_ASSET_INSTANCES_PROP_NAME = "paint_asset_instances";
/**
 * The one-instance label of the format before instances existed; read as instance 1 and replaced
 * by the instances group on the next record or reload.
 */
constexpr const char *PAINT_ASSET_SOURCE_PROP_NAME = "paint_asset_source";

/** What one drop of a Smart asset into a material carries, read out of its record. */
struct PaintLayerAssetInstanceInfo {
  /** Unique in the material, monotonically growing; ids of removed instances are not reused. */
  int id = 0;
  /** Ordinal among the instances of the same asset (name and library) in this material, from 1. */
  int number = 0;
  /** The asset material's name, as it was at the drop. */
  std::string name;
  /** The library file the asset came from, empty for one in the current file. */
  std::string library;
  PaintLayerAssetType type = PaintLayerAssetType::Material;

  /** The user-facing label: "<asset name> #<number>". */
  std::string label() const
  {
    return name + " #" + std::to_string(number);
  }
};

/** Whether \a ma holds at least one smart-asset instance (of the current or the old format). */
bool BKE_paint_layers_asset_instances_any(const Material &ma);

/**
 * The record of the instance \a instance_id, as its custom-property subgroup, or null when the
 * material holds no such instance (including the one-instance format for id 1).
 */
const IDProperty *BKE_paint_layers_asset_instance_find(const Material &ma, int instance_id);

/**
 * Visit every instance of \a ma, oldest first, with its read-out info. Return false from \a fn to
 * stop.
 */
void BKE_paint_layers_asset_instances_foreach(
    const Material &ma, FunctionRef<bool(const PaintLayerAssetInstanceInfo &)> fn);

/**
 * The id of the instance whose `rows` set names \a marker, or -1 for none. The marker has to be
 * the row's own: a marker of a nested item is the caller's to lift to its root first
 * (#BKE_paint_layers_asset_instance_of_row does that walk); a duplicated row's fresh marker
 * belongs to no instance.
 */
int BKE_paint_layers_asset_instance_of_marker(const Material &ma, const bUUID &marker);

/** The instance \a row belongs to: the nearest ancestor of the row -- the row itself included --
 * whose marker an instance's `rows` set names, at any depth, since a drop Into a folder lays its
 * roots nested. -1 when the row is not of an instance (or not of the stack at all); the rows
 * nested under a root answer for it, and a row lifted out of its root belongs to none. */
int BKE_paint_layers_asset_instance_of_row(const Material &ma, const MaterialPaintLayer &row);

/** The label ("<asset name> #<number>") of the instance \a instance_id, or empty for none. */
std::string BKE_paint_layers_asset_instance_label(const Material &ma, int instance_id);

/**
 * Record a fresh instance: one drop of \a source into \a ma. The id is `max(existing)+1` and the
 * number is `max(number of the same-asset instances)+1`, so a repeated drop reads as
 * "<name> #2" instead of replacing the first one. A one-instance label still on the material is
 * migrated into the instances group first, so it keeps its id 1.
 *
 * \return the new instance's id.
 */
int BKE_paint_layers_asset_instance_record(Material &ma,
                                           const Material &source,
                                           PaintLayerAssetType type,
                                           Span<const MaterialPaintLayer *> imported_rows);

/** What #BKE_paint_layers_asset_reload came back with. */
enum class PaintLayerAssetReloadResult : int8_t {
  /** The rows were replaced from the source; parameter values were carried over. */
  Reloaded = 0,
  /** The material holds no smart-asset instance with the id the caller named. */
  NoSource = 1,
  /** The label names no material in this file: the source was never appended, or is gone. */
  SourceMissing = 2,
  /** The source has no rows to import any more, or the import was refused. */
  ImportFailed = 3,
};

/**
 * Replace the rows the instance \a instance_id received from its smart asset with fresh copies of
 * the asset's rows, carrying the exposed-parameter values the user changed on the instance over
 * to the new rows. A parameter the asset no longer exposes stopped being an instance parameter:
 * the fresh copy takes the asset's own value instead of the carried one. A parameter the instance
 * switched off stays off on the fresh markers, and its value follows the asset the same way.
 * Every other edit the user
 * made to the imported rows (painted pixels, added layers, moved items) is lost: the asset is the
 * thing being followed. The other instances of the material are not touched, and the reloaded
 * instance keeps its id, its number and its place in the stack: the import lands in the seam
 * between the rows' neighbours, or at the top when the neighbours are gone.
 */
PaintLayerAssetReloadResult BKE_paint_layers_asset_reload(Main &bmain, Material &ma, int instance_id);

/** \} */

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
