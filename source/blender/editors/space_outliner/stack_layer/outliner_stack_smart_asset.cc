/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 *
 * Saving a part of a paint-layer stack as a Smart asset: a Material asset carrying copies of the
 * selected rows (or of one row's mask), tagged so the stack's drop actions can insert it again.
 */

#include <algorithm>
#include <optional>
#include <string>

#include "BKE_asset.hh"
#include "BKE_asset_edit.hh"
#include "BKE_context.hh"
#include "BKE_lib_id.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_asset.hh"
#include "BKE_paint_layers_generate.hh"
#include "BKE_preferences.h"
#include "BKE_preview_image.hh"
#include "BKE_report.hh"

#include "BLI_function_ref.hh"
#include "BLI_span.hh"
#include "BLI_string_ref.hh"
#include "BLI_string.h"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "BLT_translation.hh"

#include "DNA_ID.h"
#include "DNA_material_types.h"
#include "DNA_space_types.h"
#include "DNA_userdef_types.h"
#include "DNA_uuid_types.h"

#include "AS_asset_catalog.hh"
#include "AS_asset_catalog_path.hh"
#include "AS_asset_library.hh"

#include "ED_asset.hh"
#include "ED_asset_library.hh"
#include "ED_render.hh"

#include "MEM_guardedalloc.h"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_prototypes.hh"

#include "UI_interface_icons.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "../outliner_intern.hh"
#include "outliner_stack_layers_intern.hh"
#include "outliner_stack_source.hh"
#include "outliner_stack_source_paint_layers_intern.hh"

namespace blender::ed::outliner {

namespace {

const EnumPropertyItem smart_asset_save_type_items[] = {
    {int(PaintLayerAssetType::Material),
     "MATERIAL",
     0,
     "Smart Material",
     "Save the selected rows (a folder included) as a reusable material asset"},
    {int(PaintLayerAssetType::Layer),
     "LAYER",
     0,
     "Smart Layer",
     "Save one row or folder as a reusable layer asset"},
    {int(PaintLayerAssetType::Mask),
     "MASK",
     0,
     "Smart Mask",
     "Save the selected row's mask stack as a reusable mask asset"},
    {0, nullptr, 0, nullptr, nullptr},
};

/** Whether \a row hangs on the stack itself; corrections and mask items do not. */
bool row_is_stack_row(Material &ma, const MaterialPaintLayer *row)
{
  return row != nullptr && BKE_paint_layers_owner_list(ma, *row) != nullptr;
}

/** The selected rows that are rows of the stack, bottom to top, without rows nested in a selected
 * folder. Falls back to the active row when nothing is selected. */
Vector<const MaterialPaintLayer *> selected_stack_rows(Material &ma,
                                                       SpaceOutliner &space_outliner)
{
  Vector<int> ordinals;
  stack_selected_ordinals_get(space_outliner, ordinals);
  if (!ordinals.is_empty()) {
    stack_ordinals_drop_covered_descendants(space_outliner, ordinals);
  }

  Vector<const MaterialPaintLayer *> rows;
  for (const int ordinal : ordinals) {
    const MaterialPaintLayer *row = paint_description_row_for_ordinal(ma, ordinal);
    if (row_is_stack_row(ma, row) && !rows.contains(row)) {
      rows.append(row);
    }
  }
  if (rows.is_empty()) {
    if (const MaterialPaintLayer *active = BKE_paint_layers_active_layer_get(ma)) {
      if (row_is_stack_row(ma, active)) {
        rows.append(active);
      }
    }
  }
  return rows;
}

/** The one row whose mask the operator saves, or null. */
const MaterialPaintLayer *selected_mask_row(const StackReadContext &ctx,
                                            Material &ma,
                                            SpaceOutliner &space_outliner,
                                            const Span<const MaterialPaintLayer *> rows)
{
  if (!rows.is_empty()) {
    return (rows.size() == 1 && rows[0]->mask_stack.first != nullptr) ? rows[0] : nullptr;
  }
  const int active_ordinal = outliner_stack_active_ordinal_get(ctx, space_outliner);
  const MaterialPaintLayer *active = paint_description_row_for_ordinal(ma, active_ordinal);
  if (row_is_stack_row(ma, active) && active->mask_stack.first != nullptr) {
    return active;
  }
  return nullptr;
}

/**
 * The rows the operator saves, or an empty vector after reporting why not. \a type decides which
 * selection is valid: a mask needs one masked row, a layer exactly one row or folder.
 */
Vector<const MaterialPaintLayer *> smart_asset_save_rows(bContext *C,
                                                         wmOperator *op,
                                                         Material &material,
                                                         SpaceOutliner &space_outliner,
                                                         const PaintLayerAssetType type)
{
  Vector<const MaterialPaintLayer *> rows = selected_stack_rows(material, space_outliner);
  if (type == PaintLayerAssetType::Mask) {
    const StackReadContext ctx = outliner_stack_read_context(*C);
    const MaterialPaintLayer *mask_row = selected_mask_row(
        ctx, material, space_outliner, rows);
    if (mask_row == nullptr) {
      BKE_report(
          op->reports, RPT_ERROR, "Select one layer with a mask to save as a Smart Mask");
      return {};
    }
    if (BKE_paint_layers_mask_branch_has_anchor(*mask_row)) {
      BKE_report(op->reports,
                 RPT_ERROR,
                 "A Smart Mask cannot contain Anchor Point rows; remove them from the mask first");
      return {};
    }
    rows.clear();
    rows.append(mask_row);
  }
  if (rows.is_empty()) {
    BKE_report(op->reports, RPT_ERROR, "Select a layer or a folder to save");
    return {};
  }
  if (type == PaintLayerAssetType::Layer && rows.size() != 1) {
    BKE_report(op->reports, RPT_ERROR, "Select one layer or folder to save as a Smart Layer");
    return {};
  }
  return rows;
}

/**
 * Fill both preview sizes with a flat, pre-multiplied grey. Rendering a Smart Mask's material would
 * only show its white holder row, so a neutral swatch says more about the asset than a misleading
 * render does.
 */
void smart_mask_preview_fill(ID &id)
{
  PreviewImage *preview = BKE_previewimg_id_ensure(&id);
  if (preview == nullptr) {
    return;
  }
  /* #BKE_previewimg_ensure() only materializes deferred thumbnails, so a generated preview has to
   * allocate its pixels here. #BKE_previewimg_clear() leaves #PRV_CHANGED set, which makes the next
   * icon draw upload the rect. #PRV_USER_EDITED keeps the icon machinery from rendering over it. */
  BKE_previewimg_clear(preview);

  const uint gray = 0x80u | (0x80u << 8) | (0x80u << 16) | (0xFFu << 24);
  const eIconSizes sizes[2] = {ICON_SIZE_ICON, ICON_SIZE_PREVIEW};
  for (const eIconSizes size : sizes) {
    const int render_size = ui::icon_preview_to_render_size(size);
    if (render_size <= 0) {
      continue;
    }
    preview->w[size] = uint(render_size);
    preview->h[size] = uint(render_size);
    preview->rect[size] = MEM_new_array_zeroed<uint>(size_t(render_size) * render_size,
                                                     "smart_mask_preview");
    std::fill_n(preview->rect[size], size_t(render_size) * render_size, gray);
    preview->flag[size] |= (PRV_CHANGED | PRV_USER_EDITED);
  }
}

/**
 * Render the preview now instead of starting the background job #asset::generate_preview() uses in
 * the UI. A preview job holds the ID it renders (see #ED_preview_icon_job()), but an asset written
 * to a library file is freed right after the write. Rendering in place also makes the preview part
 * of the written file.
 */
void smart_asset_preview_render_sync(const bContext *C, Material &asset)
{
  if (!ED_preview_id_render_is_supported(&asset.id)) {
    return;
  }
  ED_preview_kill_jobs_for_id(CTX_wm_manager(C), &asset.id);
  if (PreviewImage *preview = BKE_previewimg_id_get(&asset.id)) {
    BKE_previewimg_clear(preview);
  }
  ui::icon_render_id(C, nullptr, &asset.id, ICON_SIZE_PREVIEW, false);
}

/** Warn about everything the saved branch carries along or loses; none of it blocks the save. */
void smart_asset_portability_report(wmOperator &op, const PaintLayerAssetPortability &portability)
{
  if (portability.foreign_reader_num > 0) {
    BKE_reportf(op.reports,
                RPT_WARNING,
                "%d Anchor Point Reader(s) point outside the saved layers and were cleared",
                portability.foreign_reader_num);
  }
  if (portability.external_image_num > 0) {
    BKE_reportf(op.reports,
                RPT_WARNING,
                "%d image(s) are linked to files; pack them or the asset will not move with the "
                "library",
                portability.external_image_num);
  }
  if (portability.movie_image_num > 0) {
    BKE_reportf(op.reports,
                RPT_WARNING,
                "%d image(s) of a sequence/movie type were saved without pixels",
                portability.movie_image_num);
  }
  if (portability.material_source_num > 0) {
    BKE_reportf(op.reports,
                RPT_WARNING,
                "%d layer(s) read other materials; those materials are copied into the asset",
                portability.material_source_num);
  }
  if (portability.object_mask_num > 0) {
    BKE_report(op.reports, RPT_WARNING, "Object masks are not saved");
  }
}

/**
 * What saving \a rows would carry, or nothing after reporting why the save cannot happen. Both
 * #smart_asset_save_invoke() and #smart_asset_save_exec() run this, so the dialog never opens on a
 * branch the save would refuse.
 */
std::optional<PaintLayerAssetPortability> smart_asset_portability_check(
    wmOperator &op,
    const Material &material,
    const Span<const MaterialPaintLayer *> rows,
    const PaintLayerAssetType type)
{
  PaintLayerAssetPortability portability = BKE_paint_layers_asset_portability_check(
      material, rows, type);
  if (portability.blocking()) {
    BKE_report(op.reports, RPT_ERROR, "Curve Patch effects cannot be saved in a Smart asset");
    return std::nullopt;
  }
  return portability;
}

/** The target library of a save, resolved before anything is created. */
struct SaveLibrary {
  AssetLibraryReference library_ref;
  const bUserAssetLibrary *user_library = nullptr;
  asset_system::AssetLibrary *library = nullptr;
};

/**
 * Resolve the operator's library property, or nothing after reporting. A missing or read-only
 * library must not leave a half-prepared asset behind.
 */
std::optional<SaveLibrary> resolve_save_library(bContext *C, wmOperator *op)
{
  Main &bmain = *CTX_data_main(C);
  SaveLibrary resolved;
  resolved.library_ref = asset::library_reference_from_enum_value(
      RNA_enum_get(op->ptr, "asset_library_reference"));
  if (resolved.library_ref.type == ASSET_LIBRARY_CUSTOM) {
    resolved.user_library = BKE_preferences_asset_library_find_from_ref(
        &U, &resolved.library_ref);
    if (resolved.user_library == nullptr) {
      BKE_report(op->reports, RPT_ERROR, "The selected asset library is missing");
      return std::nullopt;
    }
  }
  else if (resolved.library_ref.type != ASSET_LIBRARY_LOCAL) {
    /* Only the current file and custom on-disk libraries can be written to. */
    BKE_report(op->reports, RPT_ERROR, "The selected asset library cannot hold new assets");
    return std::nullopt;
  }
  resolved.library = AS_asset_library_load(&bmain, resolved.library_ref);
  if (resolved.library == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "Failed to load asset library");
    return std::nullopt;
  }
  return resolved;
}

/** The name a save uses when the user leaves the field empty: the type label and the first row. */
std::string smart_asset_default_name(const PaintLayerAssetType type,
                                     const MaterialPaintLayer &row)
{
  char name[MAX_ID_NAME - 2];
  SNPRINTF(name, "%s %s", BKE_paint_layers_asset_type_label(type), row.name);
  return name;
}

/** Finish a save into the current file. */
wmOperatorStatus smart_asset_store_local(bContext *C,
                                         wmOperator *op,
                                         Material &asset,
                                         const PaintLayerAssetType type,
                                         const size_t row_num)
{
  WM_main_add_notifier(NC_ID | NA_ADDED, &asset.id);
  WM_main_add_notifier(NC_ASSET | ND_ASSET_LIST | NA_ADDED, nullptr);
  WM_event_add_notifier(C, NC_MATERIAL | ND_SHADING, &asset.id);
  WM_file_tag_modified();

  BKE_reportf(op->reports,
              RPT_INFO,
              "Saved %s '%s' from %d row(s)",
              BKE_paint_layers_asset_type_label(type),
              asset.id.name + 2,
              int(row_num));
  return OPERATOR_FINISHED;
}

/**
 * Finish a save into a custom on-disk library file and free the temporary local copy. The preview
 * must already be rendered into \a asset: the file copy takes it from there.
 */
wmOperatorStatus smart_asset_store_in_library(bContext *C,
                                              wmOperator *op,
                                              Material &asset,
                                              const SaveLibrary &save_library,
                                              const StringRefNull name,
                                              const PaintLayerAssetType type,
                                              const size_t row_num)
{
  Main &bmain = *CTX_data_main(C);
  AssetWeakReference asset_ref;
  const std::optional<std::string> final_full_asset_filepath = bke::asset_edit_id_save_as(
      bmain, asset.id, name, *save_library.user_library, asset_ref, *op->reports);
  if (!final_full_asset_filepath) {
    BKE_reportf(op->reports,
                RPT_ERROR,
                "Could not write %s '%s' to the asset library",
                BKE_paint_layers_asset_type_label(type),
                name.c_str());
    BKE_id_free(&bmain, &asset);
    return OPERATOR_CANCELLED;
  }
  save_library.library->catalog_service().write_to_disk(*final_full_asset_filepath);

  /* The library file is the asset now; the local copy only existed to be written. */
  BKE_id_free(&bmain, &asset);

  asset::refresh_asset_library(C, save_library.library_ref);
  WM_main_add_notifier(NC_ASSET | ND_ASSET_LIST | NA_ADDED, nullptr);

  BKE_reportf(op->reports,
              RPT_INFO,
              "Saved %s '%s' from %d row(s) to %s",
              BKE_paint_layers_asset_type_label(type),
              name.c_str(),
              int(row_num),
              final_full_asset_filepath->c_str());
  return OPERATOR_FINISHED;
}

bool smart_asset_save_poll(bContext *C)
{
  if (!stack_mode_poll(C)) {
    return false;
  }
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const StackReadContext ctx = outliner_stack_read_context(*C);
  ID *owner = outliner_stack_owner_get(ctx, *space_outliner);
  return owner != nullptr && GS(owner->name) == ID_MA &&
         stack_source_for_space(*space_outliner)->is_editable(*owner);
}

wmOperatorStatus smart_asset_save_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const StackReadContext ctx = outliner_stack_read_context(*C);
  ID *owner = outliner_stack_owner_get(ctx, *space_outliner);
  if (owner == nullptr || GS(owner->name) != ID_MA) {
    return OPERATOR_CANCELLED;
  }
  Material &material = *id_cast<Material *>(owner);
  Main &bmain = *CTX_data_main(C);

  const PaintLayerAssetType asset_type = PaintLayerAssetType(RNA_enum_get(op->ptr, "type"));

  const Vector<const MaterialPaintLayer *> rows = smart_asset_save_rows(
      C, op, material, *space_outliner, asset_type);
  if (rows.is_empty()) {
    return OPERATOR_CANCELLED;
  }
  const Span<const MaterialPaintLayer *> row_span(rows.data(), rows.size());

  /* The invoke already refuses a blocking branch; this keeps a direct exec honest. */
  const std::optional<PaintLayerAssetPortability> portability = smart_asset_portability_check(
      *op, material, row_span, asset_type);
  if (!portability) {
    return OPERATOR_CANCELLED;
  }

  const std::optional<SaveLibrary> save_library = resolve_save_library(C, op);
  if (!save_library) {
    return OPERATOR_CANCELLED;
  }

  char name[MAX_ID_NAME - 2];
  RNA_string_get(op->ptr, "name", name);
  if (name[0] == '\0') {
    const std::string default_name = smart_asset_default_name(asset_type, *rows[0]);
    BLI_strncpy(name, default_name.c_str(), sizeof(name));
  }

  /* Painted maps are the ones the asset would carry as pixels; a user who does not ask for them
   * gets a small asset whose rows keep the flat value those maps replaced. */
  const PaintLayerImageImportPolicy image_policy =
      RNA_boolean_get(op->ptr, "include_painted_maps") ? PaintLayerImageImportPolicy::Share :
                                                         PaintLayerImageImportPolicy::Strip;
  PaintLayerImportResult import_result{};
  Material *asset = BKE_paint_layers_asset_create(
      bmain, material, row_span, asset_type, name, image_policy, &import_result);
  if (asset == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "Could not save the selected rows as a Smart asset");
    return OPERATOR_CANCELLED;
  }

  /* Mark after the rows are in place: a failed import must not leave an asset behind. mark_id only
   * fails before it creates asset_data, so a plain free is enough here. */
  if (!asset::mark_id(&asset->id)) {
    BKE_report(op->reports, RPT_ERROR, "The saved material could not be marked as an asset");
    BKE_id_free(&bmain, asset);
    return OPERATOR_CANCELLED;
  }

  char catalog[MAX_NAME];
  RNA_string_get(op->ptr, "catalog", catalog);
  if (catalog[0] == '\0') {
    STRNCPY(catalog, BKE_paint_layers_asset_default_catalog(asset_type));
  }
  const asset_system::AssetCatalog &catalog_entry = asset::library_ensure_catalogs_in_path(
      *save_library->library, asset_system::AssetCatalogPath::from_user_input(catalog));
  BKE_paint_layers_asset_metadata_apply(*asset,
                                        asset_type,
                                        RNA_string_get(op->ptr, "description"),
                                        catalog_entry.catalog_id,
                                        catalog_entry.simple_name);
  /* The exposed-parameters list mirrors into the asset metadata the same way; its first build ran
   * before the material was an asset, so there was nothing to mirror into yet. */
  BKE_paint_layers_asset_params_expose(*asset);
  smart_asset_portability_report(*op, *portability);
  if (import_result.stripped_maps > 0) {
    BKE_reportf(op->reports,
                RPT_WARNING,
                "%d painted map(s) were saved as flat values; enable \"Include Painted Maps\" to "
                "keep their pixels",
                import_result.stripped_maps);
  }

  /* Synchronous: the asset has no user yet, so nothing else would ever build its stack for the
   * preview. The operator's OPTYPE_UNDO step is pushed after exec and includes the new material,
   * as it does for the other operators here that create an ID. */
  BKE_paint_layers_regenerate(bmain, *asset);
  if (asset_type == PaintLayerAssetType::Mask) {
    smart_mask_preview_fill(asset->id);
  }
  else if (save_library->library_ref.type == ASSET_LIBRARY_LOCAL) {
    /* A local asset lives on, so its preview may be built by a background job. */
    asset::generate_preview(C, &asset->id);
  }
  else {
    /* The library asset is freed right after the write; render before that. */
    smart_asset_preview_render_sync(C, *asset);
  }

  if (save_library->library_ref.type == ASSET_LIBRARY_LOCAL) {
    return smart_asset_store_local(C, op, *asset, asset_type, rows.size());
  }
  return smart_asset_store_in_library(
      C, op, *asset, *save_library, name, asset_type, rows.size());
}

const EnumPropertyItem *rna_asset_library_reference_itemf(bContext * /*C*/,
                                                          PointerRNA * /*ptr*/,
                                                          PropertyRNA * /*prop*/,
                                                          bool *r_free)
{
  const EnumPropertyItem *items = asset::library_reference_to_rna_enum_itemf(
      /*include_readonly=*/false,
      /*include_current_file=*/true,
      /*include_remote_libraries=*/false,
      /*include_separate_online_essentials=*/false);
  *r_free = items != nullptr;
  return items;
}

void rna_asset_catalog_search_fn(const bContext *C,
                                 PointerRNA *ptr,
                                 PropertyRNA * /*prop*/,
                                 const char *edit_text,
                                 FunctionRef<void(StringPropertySearchVisitParams)> visit_fn)
{
  asset::visit_library_catalogs_catalog_for_search(
      *CTX_data_main(C), asset::get_asset_library_ref_from_opptr(*ptr), edit_text, visit_fn);
}

wmOperatorStatus smart_asset_save_invoke(bContext *C, wmOperator *op, const wmEvent * /*event*/)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const StackReadContext ctx = outliner_stack_read_context(*C);
  ID *owner = outliner_stack_owner_get(ctx, *space_outliner);
  if (owner == nullptr || GS(owner->name) != ID_MA) {
    return OPERATOR_CANCELLED;
  }
  Material &material = *id_cast<Material *>(owner);
  const PaintLayerAssetType asset_type = PaintLayerAssetType(RNA_enum_get(op->ptr, "type"));

  /* Validate the selection before the dialog opens, so a wrong one reports instead of asking. */
  const Vector<const MaterialPaintLayer *> rows = smart_asset_save_rows(
      C, op, material, *space_outliner, asset_type);
  if (rows.is_empty()) {
    return OPERATOR_CANCELLED;
  }
  const Span<const MaterialPaintLayer *> row_span(rows.data(), rows.size());

  /* A blocking branch is refused before the user fills the dialog, not after. */
  if (!smart_asset_portability_check(*op, material, row_span, asset_type)) {
    return OPERATOR_CANCELLED;
  }

  if (!RNA_struct_property_is_set_ex(op->ptr, "name", false)) {
    RNA_string_set(
        op->ptr, "name", smart_asset_default_name(asset_type, *rows[0]).c_str());
  }
  if (!RNA_struct_property_is_set_ex(op->ptr, "asset_library_reference", false)) {
    /* Saving into the current file stays the default unless the user picks a library. */
    const AssetLibraryReference library_ref = asset_system::current_file_library_reference();
    RNA_enum_set(op->ptr,
                 "asset_library_reference",
                 asset::library_reference_to_enum_value(&library_ref));
  }
  if (!RNA_struct_property_is_set_ex(op->ptr, "catalog", false)) {
    RNA_string_set(op->ptr, "catalog", BKE_paint_layers_asset_default_catalog(asset_type));
  }
  if (!RNA_struct_property_is_set_ex(op->ptr, "include_painted_maps", false)) {
    /* A mask is its pixels, so a Smart Mask keeps them unless the user says otherwise. */
    RNA_boolean_set(op->ptr, "include_painted_maps", asset_type == PaintLayerAssetType::Mask);
  }

  return WM_operator_props_dialog_popup(C, op, 400, std::nullopt, IFACE_("Save"));
}

/* -------------------------------------------------------------------- */
/** \name Apply Smart Mask
 * \{ */

enum eSmartMaskApplyMode : int8_t {
  SMART_MASK_REPLACE = 0,
  SMART_MASK_ADD = 1,
};

const EnumPropertyItem smart_mask_apply_mode_items[] = {
    {SMART_MASK_REPLACE,
     "REPLACE",
     0,
     "Replace Mask",
     "Replace the target row's mask with the Smart Mask's"},
    {SMART_MASK_ADD,
     "ADD",
     0,
     "Add Corrections",
     "Lay the Smart Mask's corrections over the target row's mask"},
    {0, nullptr, 0, nullptr, nullptr},
};

bool smart_mask_apply_poll(bContext *C)
{
  /* Everything comes from the operator's own properties. The dialog that opened from the drop can
   * outlive the Outliner context, so this poll must not require one. */
  return CTX_data_main(C) != nullptr;
}

wmOperatorStatus smart_mask_apply_invoke(bContext *C, wmOperator *op, const wmEvent * /*event*/)
{
  return WM_operator_props_dialog_popup(C, op, 400, std::nullopt, IFACE_("Apply Smart Mask"));
}

wmOperatorStatus smart_mask_apply_exec(bContext *C, wmOperator *op)
{
  Main &bmain = *CTX_data_main(C);
  Material *asset = id_cast<Material *>(
      BKE_libblock_find_session_uid(&bmain, ID_MA, RNA_int_get(op->ptr, "asset_uid")));
  if (asset == nullptr || BKE_paint_layers_asset_type_get(*asset) != PaintLayerAssetType::Mask) {
    BKE_report(op->reports, RPT_ERROR, "The Smart Mask asset is gone");
    return OPERATOR_CANCELLED;
  }
  Material *material = id_cast<Material *>(
      BKE_libblock_find_session_uid(&bmain, ID_MA, RNA_int_get(op->ptr, "owner_uid")));
  if (material == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "The target material is gone");
    return OPERATOR_CANCELLED;
  }
  if (!ID_IS_EDITABLE(&material->id) || ID_IS_OVERRIDE_LIBRARY(&material->id)) {
    BKE_report(op->reports, RPT_ERROR, "The layered material is linked or overridden");
    return OPERATOR_CANCELLED;
  }

  char marker_str[UUID_STRING_SIZE];
  RNA_string_get(op->ptr, "layer", marker_str);
  bUUID marker;
  if (marker_str[0] == '\0' || !BLI_uuid_parse_string(&marker, marker_str)) {
    BKE_report(op->reports, RPT_ERROR, "The target layer is gone");
    return OPERATOR_CANCELLED;
  }
  MaterialPaintLayer *target = BKE_paint_layers_find(*material, marker);
  if (target == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "The target layer is gone");
    return OPERATOR_CANCELLED;
  }

  /* The asset's own predicate reports why it cannot land (no mask, Anchor Point rows). */
  const char *reason = nullptr;
  if (!BKE_paint_layers_asset_can_insert(*material, *asset, target, PaintLayerPlace::Above, &reason))
  {
    BKE_report(op->reports, RPT_ERROR, reason != nullptr ? reason : "Could not apply the Smart Mask");
    return OPERATOR_CANCELLED;
  }

  const bool add_corrections = RNA_enum_get(op->ptr, "mode") == SMART_MASK_ADD;
  PaintLayerImportResult result;
  if (!BKE_paint_layers_asset_insert(
          bmain, *material, *asset, target, PaintLayerPlace::Above, &result, add_corrections))
  {
    BKE_report(op->reports, RPT_ERROR, "Could not apply the Smart Mask");
    return OPERATOR_CANCELLED;
  }
  /* The dialog is the one point a Smart Mask is applied from, so this is where the drop becomes an
   * instance: the record names the target row and the items this application laid on its mask, so
   * a reload and Make Unique can find them again. */
  BKE_paint_layers_asset_instance_record_mask(
      *material, *asset, *target, add_corrections, result.imported_mask_items.as_span());

  /* The operator's #OPTYPE_UNDO flag pushes the undo step: the drop that opened the dialog pushes
   * none, and a second push here would record the edit twice. */
  const int ordinal = paint_layers_ordinal_of(*material, result.first);
  if (SpaceOutliner *space_outliner = CTX_wm_space_outliner(C)) {
    stack_row_edit_finished(*C, *space_outliner, material->id, ordinal, true);
  }
  WM_event_add_notifier(C, NC_MATERIAL | ND_SHADING, &material->id);
  return OPERATOR_FINISHED;
}

/** \} */

}  // namespace

void OUTLINER_OT_stack_layer_smart_mask_apply(wmOperatorType *ot)
{
  ot->name = "Apply Smart Mask";
  ot->idname = "OUTLINER_OT_stack_layer_smart_mask_apply";
  ot->description = "Apply a Smart Mask's mask to a layer, replacing it or adding its corrections";
  ot->exec = smart_mask_apply_exec;
  ot->invoke = smart_mask_apply_invoke;
  ot->poll = smart_mask_apply_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* Session UIDs are unsigned; they travel through the signed property bit for bit. */
  PropertyRNA *prop = RNA_def_int(ot->srna,
                                  "asset_uid",
                                  0,
                                  INT32_MIN,
                                  INT32_MAX,
                                  "Asset",
                                  "Session UID of the Smart Mask's material",
                                  INT32_MIN,
                                  INT32_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN);
  prop = RNA_def_int(ot->srna,
                     "owner_uid",
                     0,
                     INT32_MIN,
                     INT32_MAX,
                     "Owner",
                     "Session UID of the target material",
                     INT32_MIN,
                     INT32_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN);
  prop = RNA_def_string(
      ot->srna, "layer", nullptr, UUID_STRING_SIZE, "Layer", "Marker of the target row");
  RNA_def_property_flag(prop, PROP_HIDDEN);
  RNA_def_enum(ot->srna,
               "mode",
               smart_mask_apply_mode_items,
               SMART_MASK_REPLACE,
               "Mode",
               "Replace the target mask, or add the Smart Mask's corrections over it");
}



void OUTLINER_OT_stack_layer_smart_asset_save(wmOperatorType *ot)
{
  ot->name = "Save as Smart Asset";
  ot->idname = "OUTLINER_OT_stack_layer_smart_asset_save";
  ot->description = "Save the selected rows, folder or mask as a reusable Smart asset";
  ot->exec = smart_asset_save_exec;
  ot->invoke = smart_asset_save_invoke;
  ot->poll = smart_asset_save_poll;
  /* No #OPTYPE_REGISTER: repeating the operator would create a second asset, and its properties
   * (a name, a catalog) do not describe an adjustable result. The undo step is still pushed. */
  ot->flag = OPTYPE_UNDO;

  PropertyRNA *prop = RNA_def_enum(ot->srna,
                                   "type",
                                   smart_asset_save_type_items,
                                   int(PaintLayerAssetType::Material),
                                   "Type",
                                   "What part of the stack the asset holds");
  /* Chosen by the menu entry, not by the dialog. */
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
  ot->prop = RNA_def_string(ot->srna,
                            "name",
                            nullptr,
                            MAX_ID_NAME - 2,
                            "Name",
                            "Asset name; empty derives one from the selected row");
  prop = RNA_def_string(ot->srna,
                        "catalog",
                        nullptr,
                        MAX_NAME,
                        "Catalog",
                        "Asset Browser catalog path; empty uses the type's default");
  RNA_def_property_string_search_func_runtime(
      prop, rna_asset_catalog_search_fn, PROP_STRING_SEARCH_SUGGESTION);
  prop = RNA_def_property(ot->srna, "asset_library_reference", PROP_ENUM, PROP_NONE);
  RNA_def_enum_funcs(prop, rna_asset_library_reference_itemf);
  RNA_def_property_enum_default(prop, ASSET_LIBRARY_LOCAL);
  RNA_def_property_ui_text(prop, "Library", "Asset library used to store the new asset");
  prop = RNA_def_boolean(ot->srna,
                         "include_painted_maps",
                         false,
                         "Include Painted Maps",
                         "Save painted maps with their pixels instead of flat "
                         "values; textures assigned to Fill rows and "
                         "corrections are always saved");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  RNA_def_string(ot->srna,
                 "description",
                 nullptr,
                 0,
                 "Description",
                 "Asset description; empty uses the default text");
}

/* -------------------------------------------------------------------- */
/** \name Smart-asset instance operators
 *
 * The panel of the imported smart assets (the Layer Material tab) calls these, one section per
 * instance: a reload pulls that instance's rows fresh from the asset it came from, and make unique
 * ends that instance's binding. Both take the instance's id through a hidden property -- without
 * it there is no telling which drop the caller means -- and work on whatever material the context
 * holds, so the outliner is only their registration home.
 * \{ */

static Material *smart_asset_instance_from_context(bContext *C)
{
  Material *ma = static_cast<Material *>(
      CTX_data_pointer_get_type(C, "material", RNA_Material).data);
  if (ma == nullptr || !BKE_paint_layers_asset_instances_any(*ma)) {
    return nullptr;
  }
  return ma;
}

static bool smart_asset_instance_poll(bContext *C)
{
  if (smart_asset_instance_from_context(C) == nullptr) {
    CTX_wm_operator_poll_msg_set(C, "No imported smart asset on this material");
    return false;
  }
  return true;
}

/** The instance id the operator property names, or 0 after reporting that it names none. */
static int smart_asset_instance_id_from_op(wmOperator *op, Material &ma)
{
  const int instance_id = RNA_int_get(op->ptr, "instance_id");
  if (BKE_paint_layers_asset_instance_find(ma, instance_id) == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "No smart-asset instance with that id on this material");
    return 0;
  }
  return instance_id;
}

static wmOperatorStatus smart_asset_reload_exec(bContext *C, wmOperator *op)
{
  Material *ma = smart_asset_instance_from_context(C);
  Main *bmain = CTX_data_main(C);
  if (ma == nullptr || bmain == nullptr) {
    return OPERATOR_CANCELLED;
  }
  const int instance_id = smart_asset_instance_id_from_op(op, *ma);
  if (instance_id == 0) {
    return OPERATOR_CANCELLED;
  }

  const PaintLayerAssetReloadResult result = BKE_paint_layers_asset_reload(*bmain, *ma, instance_id);
  switch (result) {
    case PaintLayerAssetReloadResult::Reloaded: {
      WM_event_add_notifier(C, NC_MATERIAL | ND_SHADING, &ma->id);
      const std::string label = BKE_paint_layers_asset_instance_label(*ma, instance_id);
      BKE_reportf(
          op->reports, RPT_INFO, "Reloaded the rows of smart asset '%s'", label.c_str());
      return OPERATOR_FINISHED;
    }
    case PaintLayerAssetReloadResult::NoSource:
      BKE_report(op->reports, RPT_ERROR, "This material has no smart-asset import to reload");
      break;
    case PaintLayerAssetReloadResult::SourceMissing:
      BKE_report(
          op->reports, RPT_ERROR, "The smart asset's source material is not in this file");
      break;
    case PaintLayerAssetReloadResult::ImportFailed:
      BKE_report(op->reports, RPT_ERROR, "The source has nothing to import any more");
      break;
  }
  return OPERATOR_CANCELLED;
}

void OUTLINER_OT_stack_layer_smart_asset_reload(wmOperatorType *ot)
{
  ot->name = "Reload Smart Asset";
  ot->idname = "OUTLINER_OT_stack_layer_smart_asset_reload";
  ot->description =
      "Replace the rows this material imported from a smart asset with fresh copies of the "
      "asset, keeping the exposed parameter values changed on this instance";
  ot->exec = smart_asset_reload_exec;
  ot->poll = smart_asset_instance_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  PropertyRNA *prop = RNA_def_int(ot->srna,
                                  "instance_id",
                                  0,
                                  INT32_MIN,
                                  INT32_MAX,
                                  "Instance",
                                  "Id of the smart-asset instance to reload",
                                  INT32_MIN,
                                  INT32_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
}

static wmOperatorStatus smart_asset_make_unique_exec(bContext *C, wmOperator *op)
{
  Material *ma = smart_asset_instance_from_context(C);
  if (ma == nullptr) {
    return OPERATOR_CANCELLED;
  }
  const int instance_id = smart_asset_instance_id_from_op(op, *ma);
  if (instance_id == 0) {
    return OPERATOR_CANCELLED;
  }
  /* The parameter values are the rows' own by now; this only removes the labels of this
   * instance's rows, so the material stops following its source there while the other instances
   * keep theirs. */
  BKE_paint_layers_asset_params_unbind(*ma, instance_id);
  BKE_paint_layers_tag_edited(*ma);
  WM_event_add_notifier(C, NC_MATERIAL | ND_SHADING, &ma->id);
  return OPERATOR_FINISHED;
}

void OUTLINER_OT_stack_layer_smart_asset_make_unique(wmOperatorType *ot)
{
  ot->name = "Make Smart Asset Unique";
  ot->idname = "OUTLINER_OT_stack_layer_smart_asset_make_unique";
  ot->description =
      "End the binding to the imported smart asset; the parameter values stay as they are and no "
      "longer follow the asset";
  ot->exec = smart_asset_make_unique_exec;
  ot->poll = smart_asset_instance_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  PropertyRNA *prop = RNA_def_int(ot->srna,
                                  "instance_id",
                                  0,
                                  INT32_MIN,
                                  INT32_MAX,
                                  "Instance",
                                  "Id of the smart-asset instance to make unique",
                                  INT32_MIN,
                                  INT32_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
}

/** \} */

}  // namespace blender::ed::outliner
