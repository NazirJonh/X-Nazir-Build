/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spview3d
 *
 * Image grid context-menu operators: mark/clear, assign catalog, copy/move to libraries.
 */

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_userdef_types.h"

#include "AS_asset_library.hh"
#include "AS_asset_representation.hh"

#include "BLI_fileops.h"
#include "BLI_path_utils.hh"
#include "BLI_string.h"
#include "BLI_string_utils.hh"
#include "BLI_string_utf8.h"

#include "BKE_asset.hh"
#include "BKE_context.hh"
#include "BKE_global.hh"
#include "BKE_idtype.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_preferences.h"
#include "BKE_report.hh"

#include "BLT_translation.hh"

#include "ED_asset.hh"
#include "ED_asset_image_library.hh"
#include "ED_asset_image_utils.hh"
#include "ED_asset_library.hh"
#include "ED_asset_list.hh"
#include "ED_asset_mark_clear.hh"
#include "ED_undo.hh"
#include "ED_view3d.hh"

#include "MEM_guardedalloc.h"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_prototypes.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "view3d_intern.hh"

namespace blender::ed::view3d {

/* -------------------------------------------------------------------- */
/** \name Shared Helpers
 * \{ */

static bool image_grid_is_mask_slot_from_op(const bContext *C, wmOperator *op)
{
  if (RNA_struct_property_is_set(op->ptr, "use_mask_slot")) {
    return RNA_boolean_get(op->ptr, "use_mask_slot");
  }
  return image_grid_is_mask_slot_from_context(*C);
}

static void image_grid_operation_finish(bContext &C,
                                        const AssetLibraryReference &focus_library,
                                        const StringRefNull focus_identifier,
                                        const bool is_mask_slot)
{
  View3D *v3d = CTX_wm_view3d(&C);
  if (!v3d) {
    return;
  }

  ImageGridUIState &state = image_grid_state_get(*v3d, is_mask_slot);
  if (state.filter.lib_ref.type != focus_library.type ||
      state.filter.lib_ref.custom_library_index != focus_library.custom_library_index)
  {
    const AssetLibraryReference old_lib_ref = state.filter.lib_ref;
    image_grid_catalog_swap_library(state, old_lib_ref, focus_library);
    state.filter.lib_ref = focus_library;
    image_grid_catalog_commit_active(state);
  }

  if (!focus_identifier.is_empty()) {
    state.viewport.scroll_row = 0;
    image_grid_request_scroll_to_asset(state, focus_identifier);
  }
  image_grid_pending_clear(state);
  image_grid_state_persist_to_view3d(*v3d, state, is_mask_slot);
  ed::asset::list::storage_fetch(&focus_library, &C);
  image_grid_prepare_browse_shelf(C, state, "VIEW3D_AST_image_texture");
  image_grid_notify_change(C, is_mask_slot);
}

static Image *image_grid_image_from_session_uid(Main *bmain, wmOperator *op)
{
  if (!RNA_struct_property_is_set(op->ptr, "image_session_uid")) {
    return nullptr;
  }
  const uint32_t session_uid = uint32_t(RNA_int_get(op->ptr, "image_session_uid"));
  if (session_uid == MAIN_ID_SESSION_UID_UNSET) {
    return nullptr;
  }
  ID *id = BKE_libblock_find_session_uid(bmain, ID_IM, session_uid);
  if (!id || GS(id->name) != ID_IM) {
    return nullptr;
  }
  return id_cast<Image *>(id);
}

static asset_system::AssetRepresentation *image_grid_asset_from_op(bContext &C, wmOperator &op)
{
  if (!RNA_struct_property_is_set(op.ptr, "asset_identifier")) {
    return nullptr;
  }
  char identifier[FILE_MAX_LIBEXTRA];
  RNA_string_get(op.ptr, "asset_identifier", identifier);
  if (identifier[0] == '\0') {
    return nullptr;
  }

  const AssetLibraryReference lib_ref = ed::asset::library_reference_from_enum_value(
      RNA_enum_get(op.ptr, "asset_library_reference"));

  asset_system::AssetRepresentation *found = nullptr;
  ed::asset::list::storage_fetch(&lib_ref, &C);
  ed::asset::list::iterate(lib_ref, [&](asset_system::AssetRepresentation &asset) {
    if (asset.get_id_type() != ID_IM) {
      return true;
    }
    if (asset.library_relative_identifier() != identifier) {
      return true;
    }
    found = &asset;
    return false;
  });
  return found;
}

static Image *image_grid_resolve_context_image(bContext &C, wmOperator &op)
{
  Main *bmain = CTX_data_main(&C);
  if (Image *ima = image_grid_image_from_session_uid(bmain, &op)) {
    return ima;
  }

  if (asset_system::AssetRepresentation *asset = image_grid_asset_from_op(C, op)) {
    if (ID *local_id = asset->local_id()) {
      if (GS(local_id->name) == ID_IM) {
        return id_cast<Image *>(local_id);
      }
    }
    if (!asset->full_path().empty()) {
      return BKE_image_load_exists(bmain, asset->full_path().c_str());
    }
  }

  ID *id = static_cast<ID *>(CTX_data_pointer_get_type(&C, "id", RNA_ID).data);
  if (id && GS(id->name) == ID_IM) {
    return id_cast<Image *>(id);
  }
  return nullptr;
}

static bool image_grid_image_source_abs_path(Main *bmain,
                                             Image &image,
                                             char r_abs[FILE_MAX])
{
  if (!BKE_image_has_filepath(&image)) {
    return false;
  }
  BKE_image_user_file_path(nullptr, &image, r_abs);
  if (r_abs[0] == '\0') {
    return false;
  }
  BLI_path_abs(r_abs, BKE_main_blendfile_path(bmain));
  return BLI_is_file(r_abs);
}

static bool image_grid_asset_source_abs_path(const asset_system::AssetRepresentation &asset,
                                             char r_abs[FILE_MAX])
{
  const std::string path = asset.full_path();
  if (path.empty() || !BLI_is_file(path.c_str())) {
    return false;
  }
  BLI_strncpy(r_abs, path.c_str(), FILE_MAX);
  return true;
}

static const EnumPropertyItem *rna_image_grid_catalog_library_itemf(bContext * /*C*/,
                                                                    PointerRNA * /*ptr*/,
                                                                    PropertyRNA * /*prop*/,
                                                                    bool *r_free)
{
  const EnumPropertyItem *items = ed::asset::library_reference_to_rna_enum_itemf(
      true, true, false, false);
  if (!items) {
    *r_free = false;
    return nullptr;
  }
  *r_free = true;
  return items;
}

static const EnumPropertyItem *rna_image_grid_writable_library_itemf(bContext * /*C*/,
                                                                     PointerRNA * /*ptr*/,
                                                                     PropertyRNA * /*prop*/,
                                                                     bool *r_free)
{
  const EnumPropertyItem *items = ed::asset::custom_libraries_rna_enum_itemf();
  if (!items) {
    *r_free = false;
    return nullptr;
  }
  *r_free = true;
  return items;
}

static void image_grid_visit_catalogs_for_search(const bContext *C,
                                               PointerRNA *ptr,
                                               PropertyRNA * /*prop*/,
                                               const char *edit_text,
                                               FunctionRef<void(StringPropertySearchVisitParams)>
                                                   visit_fn)
{
  ed::asset::visit_library_catalogs_catalog_for_search(
      *CTX_data_main(C),
      ed::asset::get_asset_library_ref_from_opptr(*ptr),
      edit_text,
      visit_fn);
}

static wmOperatorStatus image_grid_relocate_invoke(bContext *C,
                                                   wmOperator *op,
                                                   const char *title)
{
  if (!RNA_struct_property_is_set(op->ptr, "asset_library_reference")) {
    std::optional<AssetLibraryReference> dest = ed::asset::get_user_library_ref_for_save(nullptr);
    if (!dest) {
      BKE_report(op->reports, RPT_ERROR, "No writable on-disk asset library found");
      return OPERATOR_CANCELLED;
    }
    RNA_enum_set(op->ptr,
                 "asset_library_reference",
                 ed::asset::library_reference_to_enum_value(&*dest));
  }

  RNA_boolean_set(op->ptr, "use_mask_slot", image_grid_is_mask_slot_from_context(*C));

  return WM_operator_props_dialog_popup(C, op, 400, IFACE_(title), IFACE_("OK"));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Assign Catalog (Current File)
 * \{ */

static bool image_grid_assign_catalog_poll(bContext *C)
{
  const ID *id = static_cast<const ID *>(CTX_data_pointer_get_type_silent(C, "id", RNA_ID).data);
  if (!id || GS(id->name) != ID_IM) {
    return false;
  }
  return !ID_IS_LINKED(id);
}

static wmOperatorStatus image_grid_assign_catalog_exec(bContext *C, wmOperator *op)
{
  Image *image = image_grid_resolve_context_image(*C, *op);
  if (!image) {
    return OPERATOR_CANCELLED;
  }
  if (ID_IS_LINKED(&image->id)) {
    BKE_report(op->reports, RPT_ERROR, "Cannot edit catalog of linked images");
    return OPERATOR_CANCELLED;
  }

  char catalog_path_c[MAX_NAME];
  RNA_string_get(op->ptr, "catalog_path", catalog_path_c);
  if (catalog_path_c[0] == '\0') {
    BKE_report(op->reports, RPT_ERROR, "Catalog path is required");
    return OPERATOR_CANCELLED;
  }

  if (!image->id.asset_data) {
    if (!ed::asset::image_mark_as_asset(image)) {
      BKE_report(op->reports, RPT_ERROR, "Image cannot be marked as an asset");
      return OPERATOR_CANCELLED;
    }
    ed::asset::generate_preview(C, &image->id);
  }

  asset_system::AssetLibrary *library = AS_asset_library_load(
      CTX_data_main(C), asset_system::current_file_library_reference());
  if (!library) {
    return OPERATOR_CANCELLED;
  }

  const asset_system::AssetCatalogPath catalog_path =
      asset_system::AssetCatalogPath::from_user_input(catalog_path_c);
  const asset_system::AssetCatalog &catalog = ed::asset::library_ensure_catalogs_in_path(
      *library, catalog_path);
  BKE_asset_metadata_catalog_id_set(
      image->id.asset_data, catalog.catalog_id, catalog.simple_name.c_str());

  WM_event_add_notifier(C, NC_ASSET | NA_EDITED, nullptr);
  WM_event_add_notifier(C, NC_ID | NA_EDITED, nullptr);

  const bool is_mask_slot = image_grid_is_mask_slot_from_op(C, op);
  image_grid_operation_finish(
      *C, asset_system::current_file_library_reference(), image->id.name + 2, is_mask_slot);

  ED_undo_push_op(C, op);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus image_grid_assign_catalog_invoke(bContext *C,
                                                         wmOperator *op,
                                                         const wmEvent * /*event*/)
{
  const AssetLibraryReference local_ref = asset_system::current_file_library_reference();
  RNA_enum_set(op->ptr,
               "asset_library_reference",
               ed::asset::library_reference_to_enum_value(&local_ref));
  RNA_boolean_set(op->ptr, "use_mask_slot", image_grid_is_mask_slot_from_context(*C));
  return WM_operator_props_dialog_popup(
      C, op, 400, IFACE_("Assign to Catalog"), IFACE_("Assign"));
}

}  // namespace blender::ed::view3d

namespace blender {

using namespace ed::view3d;

void VIEW3D_OT_image_grid_assign_catalog(wmOperatorType *ot)
{
  ot->name = "Assign Image to Catalog";
  ot->description = "Assign the image asset to a catalog in the current file library";
  ot->idname = "VIEW3D_OT_image_grid_assign_catalog";

  ot->exec = image_grid_assign_catalog_exec;
  ot->invoke = image_grid_assign_catalog_invoke;
  ot->poll = image_grid_assign_catalog_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  PropertyRNA *prop = RNA_def_property(ot->srna, "asset_library_reference", PROP_ENUM, PROP_NONE);
  RNA_def_enum_funcs(prop, rna_image_grid_catalog_library_itemf);
  RNA_def_property_flag(prop, PROP_SKIP_SAVE | PROP_HIDDEN);

  prop = RNA_def_string(
      ot->srna, "catalog_path", nullptr, MAX_NAME, "Catalog", "Catalog path in the current file");
  RNA_def_property_string_search_func_runtime(
      prop, image_grid_visit_catalogs_for_search, PROP_STRING_SEARCH_SUGGESTION);

  prop = RNA_def_boolean(ot->srna,
                         "use_mask_slot",
                         false,
                         "Mask Texture Slot",
                         "Update the mask texture grid state");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE | PROP_HIDDEN);

  prop = RNA_def_int(ot->srna,
                     "image_session_uid",
                     int(MAIN_ID_SESSION_UID_UNSET),
                     INT32_MIN,
                     INT32_MAX,
                     "Image Session UID",
                     "",
                     INT32_MIN,
                     INT32_MAX);
  RNA_def_property_flag(prop, PROP_SKIP_SAVE | PROP_HIDDEN);

  prop = RNA_def_string(ot->srna,
                        "asset_identifier",
                        nullptr,
                        FILE_MAX_LIBEXTRA,
                        "Asset Identifier",
                        "");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE | PROP_HIDDEN);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Copy / Move to Library
 * \{ */

static bool image_grid_copy_move_poll(bContext *C)
{
  const ID *id = static_cast<const ID *>(CTX_data_pointer_get_type_silent(C, "id", RNA_ID).data);
  if (id && GS(id->name) == ID_IM) {
    if (ID_IS_LINKED(id)) {
      return false;
    }
    return true;
  }
  /* Asset-only context from grid items is validated in exec. */
  return id == nullptr;
}

static bool image_grid_export_file_to_library(Main *bmain,
                                              wmOperator &op,
                                              const char *src_abs,
                                              const bUserAssetLibrary &dest_lib,
                                              const char *catalog_path_c,
                                              const bool do_move,
                                              char r_focus_identifier[FILE_MAX])
{
  asset_system::AssetLibrary *library = AS_asset_library_load(
      bmain, ed::asset::user_library_to_library_ref(dest_lib));
  if (!library) {
    BKE_report(op.reports, RPT_ERROR, "Failed to load destination asset library");
    return false;
  }

  const asset_system::AssetCatalogPath catalog_path =
      asset_system::AssetCatalogPath::from_user_input(catalog_path_c);
  const asset_system::AssetCatalog &catalog = ed::asset::library_ensure_catalogs_in_path(
      *library, catalog_path);

  if (!ed::asset::image_library_catalog_directory_ensure(dest_lib.dirpath,
                                                         catalog.path.c_str()))
  {
    BKE_report(op.reports, RPT_ERROR, "Failed to create catalog directory");
    return false;
  }

  char filename[FILE_MAX];
  BLI_path_split_file_part(src_abs, filename, sizeof(filename));

  char dest_relative[FILE_MAX];
  const char *catalog_path_str = catalog.path.c_str();
  const bool catalog_is_root = catalog_path_str[0] == '\0' ||
                               STREQ(catalog_path_str, ed::asset::IMAGE_LIBRARY_ROOT_CATALOG_PATH);
  if (catalog_is_root) {
    BLI_strncpy(dest_relative, filename, sizeof(dest_relative));
  }
  else {
    BLI_path_join(dest_relative, sizeof(dest_relative), catalog_path_str, filename);
  }
  for (char &c : dest_relative) {
    if (c == SEP) {
      c = '/';
    }
  }

  char dest_abs[FILE_MAX];
  BLI_path_join(dest_abs, sizeof(dest_abs), dest_lib.dirpath, dest_relative);
  BLI_path_normalize(dest_abs);

  if (BLI_path_cmp(src_abs, dest_abs) == 0) {
    BLI_strncpy(r_focus_identifier, dest_relative, FILE_MAX);
    return true;
  }

  if (BLI_exists(dest_abs)) {
    char unique_name[FILE_MAX];
    BLI_uniquename_cb(
        [&](const StringRef check_name) {
          char test_relative[FILE_MAX];
          if (catalog_is_root) {
            BLI_strncpy(test_relative, check_name.data(), sizeof(test_relative));
          }
          else {
            BLI_path_join(
                test_relative, sizeof(test_relative), catalog_path_str, check_name.data());
          }
          for (char &c : test_relative) {
            if (c == SEP) {
              c = '/';
            }
          }
          char test_abs[FILE_MAX];
          BLI_path_join(test_abs, sizeof(test_abs), dest_lib.dirpath, test_relative);
          return BLI_exists(test_abs);
        },
        filename,
        '.',
        unique_name,
        sizeof(unique_name));

    if (catalog_is_root) {
      BLI_strncpy(dest_relative, unique_name, sizeof(dest_relative));
    }
    else {
      BLI_path_join(dest_relative, sizeof(dest_relative), catalog_path_str, unique_name);
    }
    BLI_path_join(dest_abs, sizeof(dest_abs), dest_lib.dirpath, dest_relative);
    BLI_path_normalize(dest_abs);
  }

  if (!BLI_file_ensure_parent_dir_exists(dest_abs)) {
    BKE_report(op.reports, RPT_ERROR, "Failed to create destination directory");
    return false;
  }

  if (do_move) {
    if (BLI_rename(src_abs, dest_abs) != 0) {
      BKE_report(op.reports, RPT_ERROR, "Failed to move image file");
      return false;
    }
  }
  else {
    if (BLI_copy(src_abs, dest_abs) != 0) {
      BKE_report(op.reports, RPT_ERROR, "Failed to copy image file");
      return false;
    }
  }

  ed::asset::image_library_scan_and_index(dest_lib.dirpath, library);
  ed::asset::image_library_invalidate_cached_previews(dest_lib.dirpath);
  BLI_strncpy(r_focus_identifier, dest_relative, FILE_MAX);
  return true;
}

static wmOperatorStatus image_grid_copy_move_exec(bContext *C, wmOperator *op, const bool do_move)
{
  Main *bmain = CTX_data_main(C);

  const bUserAssetLibrary *dest_lib = ed::asset::get_asset_library_from_opptr(*op->ptr);
  if (!dest_lib || !dest_lib->dirpath[0]) {
    BKE_report(op->reports, RPT_ERROR, "Invalid destination asset library");
    return OPERATOR_CANCELLED;
  }
  if ((dest_lib->flag & ASSET_LIBRARY_USE_REMOTE_URL) || !BLI_is_dir(dest_lib->dirpath)) {
    BKE_report(op->reports, RPT_ERROR, "Destination is not an on-disk image asset library");
    return OPERATOR_CANCELLED;
  }

  char catalog_path_c[MAX_NAME];
  RNA_string_get(op->ptr, "catalog_path", catalog_path_c);
  if (catalog_path_c[0] == '\0') {
    BKE_report(op->reports, RPT_ERROR, "Catalog path is required");
    return OPERATOR_CANCELLED;
  }

  const AssetLibraryReference dest_ref = ed::asset::user_library_to_library_ref(*dest_lib);
  const bool is_mask_slot = image_grid_is_mask_slot_from_op(C, op);

  if (asset_system::AssetRepresentation *asset = image_grid_asset_from_op(*C, *op)) {
    const char *library_root = ed::asset::image_library_editable_root_from_asset_library(
        asset->owner_asset_library());
    if (ed::asset::image_library_asset_is_movable_on_disk(*asset) && do_move && library_root &&
        STREQ(library_root, dest_lib->dirpath))
    {
      asset_system::AssetLibrary *library = AS_asset_library_load(bmain, dest_ref);
      if (!library) {
        return OPERATOR_CANCELLED;
      }

      const asset_system::AssetCatalogPath catalog_path =
          asset_system::AssetCatalogPath::from_user_input(catalog_path_c);
      const asset_system::AssetCatalog &catalog = ed::asset::library_ensure_catalogs_in_path(
          *library, catalog_path);

      if (!ed::asset::image_library_assign_image_to_catalog(library_root,
                                                          *library,
                                                          asset->library_relative_identifier(),
                                                          catalog.catalog_id))
      {
        BKE_report(op->reports, RPT_ERROR, "Failed to move image in the library");
        return OPERATOR_CANCELLED;
      }

      BKE_asset_metadata_catalog_id_set(
          &asset->get_metadata(), catalog.catalog_id, catalog.simple_name.c_str());
      const std::string asset_full_path = asset->full_path();
      asset->owner_asset_library().remove_asset(*asset);

      char focus_id[FILE_MAX];
      BLI_path_split_file_part(asset_full_path.c_str(), focus_id, sizeof(focus_id));
      char moved_relative[FILE_MAX];
      if (catalog.path.c_str()[0] == '\0' ||
          STREQ(catalog.path.c_str(), ed::asset::IMAGE_LIBRARY_ROOT_CATALOG_PATH))
      {
        BLI_strncpy(moved_relative, focus_id, sizeof(moved_relative));
      }
      else {
        BLI_path_join(
            moved_relative, sizeof(moved_relative), catalog.path.c_str(), focus_id);
      }
      for (char &c : moved_relative) {
        if (c == SEP) {
          c = '/';
        }
      }

      ed::asset::list::library_refresh(C, &dest_ref);
      image_grid_operation_finish(*C, dest_ref, moved_relative, is_mask_slot);
      ED_undo_push_op(C, op);
      return OPERATOR_FINISHED;
    }
  }

  Image *image = image_grid_resolve_context_image(*C, *op);
  if (!image) {
    BKE_report(op->reports, RPT_ERROR, "No image found for this operation");
    return OPERATOR_CANCELLED;
  }

  char src_abs[FILE_MAX];
  if (!image_grid_image_source_abs_path(bmain, *image, src_abs)) {
    if (asset_system::AssetRepresentation *asset = image_grid_asset_from_op(*C, *op)) {
      if (!image_grid_asset_source_abs_path(*asset, src_abs)) {
        BKE_report(op->reports, RPT_ERROR, "Image has no file path on disk");
        return OPERATOR_CANCELLED;
      }
    }
    else {
      BKE_report(op->reports, RPT_ERROR, "Image has no file path on disk");
      return OPERATOR_CANCELLED;
    }
  }

  char focus_identifier[FILE_MAX];
  if (!image_grid_export_file_to_library(
          bmain, *op, src_abs, *dest_lib, catalog_path_c, do_move, focus_identifier))
  {
    return OPERATOR_CANCELLED;
  }

  ed::asset::list::library_refresh(C, &dest_ref);
  image_grid_operation_finish(*C, dest_ref, focus_identifier, is_mask_slot);

  ED_undo_push_op(C, op);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus image_grid_copy_to_library_exec(bContext *C, wmOperator *op)
{
  return image_grid_copy_move_exec(C, op, false);
}

static wmOperatorStatus image_grid_move_to_library_exec(bContext *C, wmOperator *op)
{
  return image_grid_copy_move_exec(C, op, true);
}

static wmOperatorStatus image_grid_copy_to_library_invoke(bContext *C,
                                                          wmOperator *op,
                                                          const wmEvent * /*event*/)
{
  return image_grid_relocate_invoke(C, op, "Copy to Library");
}

static wmOperatorStatus image_grid_move_to_library_invoke(bContext *C,
                                                          wmOperator *op,
                                                          const wmEvent * /*event*/)
{
  return image_grid_relocate_invoke(C, op, "Move to Library");
}

static void image_grid_copy_move_ot_common(wmOperatorType *ot, const bool do_move)
{
  if (do_move) {
    ot->name = "Move Image to Library";
    ot->description =
        "Move the image file into a catalog of an on-disk image asset library, or re-catalog an "
        "existing on-disk asset";
    ot->idname = "VIEW3D_OT_image_grid_move_to_library";
    ot->exec = image_grid_move_to_library_exec;
    ot->invoke = image_grid_move_to_library_invoke;
  }
  else {
    ot->name = "Copy Image to Library";
    ot->description = "Copy the image file into a catalog of an on-disk image asset library";
    ot->idname = "VIEW3D_OT_image_grid_copy_to_library";
    ot->exec = image_grid_copy_to_library_exec;
    ot->invoke = image_grid_copy_to_library_invoke;
  }

  ot->poll = image_grid_copy_move_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  PropertyRNA *prop = RNA_def_property(ot->srna, "asset_library_reference", PROP_ENUM, PROP_NONE);
  RNA_def_enum_funcs(prop, rna_image_grid_writable_library_itemf);
  RNA_def_property_ui_text(prop, "Library", "Destination on-disk image asset library");

  prop = RNA_def_string(
      ot->srna, "catalog_path", nullptr, MAX_NAME, "Catalog", "Catalog in the destination library");
  RNA_def_property_string_search_func_runtime(
      prop, image_grid_visit_catalogs_for_search, PROP_STRING_SEARCH_SUGGESTION);

  prop = RNA_def_boolean(ot->srna,
                         "use_mask_slot",
                         false,
                         "Mask Texture Slot",
                         "Update the mask texture grid state");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE | PROP_HIDDEN);

  prop = RNA_def_int(ot->srna,
                     "image_session_uid",
                     int(MAIN_ID_SESSION_UID_UNSET),
                     INT32_MIN,
                     INT32_MAX,
                     "Image Session UID",
                     "",
                     INT32_MIN,
                     INT32_MAX);
  RNA_def_property_flag(prop, PROP_SKIP_SAVE | PROP_HIDDEN);

  prop = RNA_def_string(ot->srna,
                        "asset_identifier",
                        nullptr,
                        FILE_MAX_LIBEXTRA,
                        "Asset Identifier",
                        "");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE | PROP_HIDDEN);
}

void VIEW3D_OT_image_grid_copy_to_library(wmOperatorType *ot)
{
  image_grid_copy_move_ot_common(ot, false);
}

void VIEW3D_OT_image_grid_move_to_library(wmOperatorType *ot)
{
  image_grid_copy_move_ot_common(ot, true);
}

/** \} */

}  // namespace blender
