/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spview3d
 */

#include "BLI_listbase.h"
#include "BLI_math_base.h"

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_space_enums.h"
#include "DNA_space_types.h"
#include "DNA_windowmanager_types.h"

#include "AS_asset_catalog_path.hh"
#include "AS_asset_library.hh"

#include "BKE_asset.hh"
#include "BKE_context.hh"
#include "BKE_idtype.hh"
#include "BKE_lib_id.hh"
#include "BKE_screen.hh"

#include "BLT_translation.hh"

#include "ED_asset_library.hh"
#include "ED_asset_list.hh"
#include "ED_asset_mark_clear.hh"
#include "ED_screen.hh"

#include "DNA_userdef_types.h"

#include "MEM_guardedalloc.h"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"
#include "RNA_prototypes.hh"

#include "UI_interface.hh"
#include "UI_interface_c.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "view3d_intern.hh"

namespace blender::ed::view3d {

void image_grid_notify_change(bContext &C)
{
  View3D *v3d = CTX_wm_view3d(&C);
  if (!v3d) {
    return;
  }

  ImageGridUIState &state = image_grid_state_get(*v3d);
  ed::asset::list::storage_fetch(&state.lib_ref, &C);
  WM_event_add_notifier(&C, NC_ASSET | ND_ASSET_LIST, nullptr);
  WM_event_add_notifier(&C, NC_ID | NA_EDITED, nullptr);

  ARegion *region = CTX_wm_region(&C);
  if (region) {
    ED_region_tag_redraw(region);
    ED_region_tag_refresh_ui(region);
  }
}

int image_grid_effective_rows(const View3D &v3d)
{
  const int stored = v3d.image_grid_rows;
  return clamp_i(stored ? stored : 3, 1, 16);
}

int image_grid_max_scroll_row(const ImageGridUIState &state, const View3D &v3d)
{
  const int effective_rows = image_grid_effective_rows(v3d);
  const int total_rows = (state.cached_cols > 0) ?
                             int(ceil(float(state.cached_item_count) / float(state.cached_cols))) :
                             effective_rows;
  return max_ii(0, total_rows - effective_rows);
}

void image_grid_clamp_scroll_row(ImageGridUIState &state, const View3D &v3d)
{
  state.scroll_row = clamp_i(state.scroll_row, 0, image_grid_max_scroll_row(state, v3d));
}

bool image_grid_wheel_poll(bContext *C, const wmEvent *event)
{
  if (!ELEM(event->type, WHEELUPMOUSE, WHEELDOWNMOUSE) || event->modifier) {
    return false;
  }
  View3D *v3d = CTX_wm_view3d(C);
  ARegion *region = CTX_wm_region(C);
  if (!v3d || !region) {
    return false;
  }
  if (!ui::region_view_has_idname_at(region, event->xy, 0, "image_asset_grid")) {
    return false;
  }
  ImageGridUIState &state = image_grid_state_get(*v3d);
  return image_grid_max_scroll_row(state, *v3d) > 0;
}

int handle_image_grid_wheel_event(bContext *C, const wmEvent *event, ARegion * /*region*/)
{
  if (!image_grid_wheel_poll(C, event)) {
    return WM_UI_HANDLER_CONTINUE;
  }
  wmOperatorType *ot = WM_operatortype_find("VIEW3D_OT_image_grid_scroll", true);
  if (!ot) {
    return WM_UI_HANDLER_CONTINUE;
  }
  PointerRNA op_ptr = WM_operator_properties_create_ptr(ot);
  const int delta = (event->type == WHEELUPMOUSE) ? -1 : 1;
  RNA_int_set(&op_ptr, "delta", delta);
  WM_operator_name_call_ptr(C, ot, wm::OpCallContext::ExecDefault, &op_ptr, nullptr);
  WM_operator_properties_free(&op_ptr);
  return WM_UI_HANDLER_BREAK;
}

}  // namespace blender::ed::view3d

namespace blender {

using namespace ed::view3d;

/* -------------------------------------------------------------------- */
/** \name Set Catalog
 * \{ */

static wmOperatorStatus image_grid_set_catalog_exec(bContext *C, wmOperator *op)
{
  View3D *v3d = CTX_wm_view3d(C);
  if (!v3d) {
    return OPERATOR_CANCELLED;
  }

  char path_buf[MAX_NAME];
  RNA_string_get(op->ptr, "catalog_path", path_buf);

  ImageGridUIState &state = image_grid_state_get(*v3d);
  state.active_catalog_path = path_buf;
  state.scroll_row = 0;

  image_grid_notify_change(*C);
  return OPERATOR_FINISHED;
}

void VIEW3D_OT_image_grid_set_catalog(wmOperatorType *ot)
{
  ot->name = "Set Image Grid Catalog";
  ot->description = "Set the active asset catalog filter for the image grid";
  ot->idname = "VIEW3D_OT_image_grid_set_catalog";

  ot->exec = image_grid_set_catalog_exec;

  ot->flag = OPTYPE_REGISTER;

  RNA_def_string(ot->srna, "catalog_path", nullptr, MAX_NAME, "Catalog Path", "");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Set Library
 * \{ */

static const EnumPropertyItem *rna_image_grid_library_itemf(bContext * /*C*/,
                                                            PointerRNA * /*ptr*/,
                                                            PropertyRNA * /*prop*/,
                                                            bool *r_free)
{
  const EnumPropertyItem *items = ed::asset::library_reference_to_rna_enum_itemf(
      /*include_readonly=*/true,
      /*include_current_file=*/true,
      /*include_remote_libraries=*/false,
      /*include_separate_online_essentials=*/false);
  if (!items) {
    *r_free = false;
    return nullptr;
  }

  *r_free = true;
  return items;
}

static wmOperatorStatus image_grid_set_library_exec(bContext *C, wmOperator *op)
{
  View3D *v3d = CTX_wm_view3d(C);
  if (!v3d) {
    return OPERATOR_CANCELLED;
  }

  const int enum_value = RNA_enum_get(op->ptr, "asset_library_reference");
  const AssetLibraryReference new_ref = ed::asset::library_reference_from_enum_value(
      enum_value);

  ImageGridUIState &state = image_grid_state_get(*v3d);
  state.lib_ref = new_ref;
  state.active_catalog_path.clear();
  state.scroll_row = 0;

  image_grid_notify_change(*C);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus image_grid_set_library_invoke(bContext *C,
                                                    wmOperator *op,
                                                    const wmEvent *event)
{
  ImageGridUIState &state = image_grid_state_get_from_context(*C);
  if (!RNA_struct_property_is_set_ex(op->ptr, "asset_library_reference", false)) {
    RNA_enum_set(op->ptr,
                 "asset_library_reference",
                 ed::asset::library_reference_to_enum_value(&state.lib_ref));
  }
  return WM_enum_search_invoke(C, op, event);
}

void VIEW3D_OT_image_grid_set_library(wmOperatorType *ot)
{
  ot->name = "Set Image Grid Library";
  ot->description = "Set the asset library used by the image grid";
  ot->idname = "VIEW3D_OT_image_grid_set_library";

  ot->exec = image_grid_set_library_exec;
  ot->invoke = image_grid_set_library_invoke;

  ot->flag = OPTYPE_REGISTER;

  PropertyRNA *prop = RNA_def_property(ot->srna, "asset_library_reference", PROP_ENUM, PROP_NONE);
  RNA_def_enum_funcs(prop, rna_image_grid_library_itemf);
  RNA_def_property_ui_text(prop, "Library", "Asset library to browse");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Mark Asset
 * \{ */

static bool image_grid_mark_asset_poll(bContext *C)
{
  const ID *id = static_cast<const ID *>(CTX_data_pointer_get_type_silent(C, "id", RNA_ID).data);
  if (!id || GS(id->name) != ID_IM) {
    return false;
  }
  return BKE_id_can_be_asset(id) && id->asset_data == nullptr;
}

static wmOperatorStatus image_grid_mark_asset_exec(bContext *C, wmOperator * /*op*/)
{
  ID *id = static_cast<ID *>(CTX_data_pointer_get_type(C, "id", RNA_ID).data);
  if (!id || GS(id->name) != ID_IM) {
    return OPERATOR_CANCELLED;
  }
  if (!BKE_id_can_be_asset(id)) {
    return OPERATOR_CANCELLED;
  }

  if (!ed::asset::mark_id(id)) {
    return OPERATOR_CANCELLED;
  }

  ed::asset::generate_preview(C, id);

  ImageGridUIState &state = image_grid_state_get_from_context(*C);
  if (!state.active_catalog_path.empty()) {
    asset_system::AssetLibrary *library = ed::asset::list::library_get_once_available(
        asset_system::current_file_library_reference());
    if (library) {
      const asset_system::AssetCatalogPath cat_path =
          asset_system::AssetCatalogPath::from_user_input(state.active_catalog_path.c_str());
      const asset_system::AssetCatalog &catalog = ed::asset::library_ensure_catalogs_in_path(
          *library, cat_path);
      BKE_asset_metadata_catalog_id_set(
          id->asset_data, catalog.catalog_id, catalog.simple_name.c_str());
    }
  }

  WM_event_add_notifier(C, NC_ASSET | NA_ADDED, nullptr);
  WM_event_add_notifier(C, NC_ID | NA_EDITED, nullptr);
  image_grid_notify_change(*C);

  return OPERATOR_FINISHED;
}

void VIEW3D_OT_image_grid_mark_asset(wmOperatorType *ot)
{
  ot->name = "Mark Image as Asset";
  ot->description = "Mark the image as an asset for use in the image grid";
  ot->idname = "VIEW3D_OT_image_grid_mark_asset";

  ot->exec = image_grid_mark_asset_exec;
  ot->poll = image_grid_mark_asset_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Browse Assets
 * \{ */

static void image_grid_configure_asset_browser(SpaceFile &sfile,
                                               const AssetLibraryReference &library_ref)
{
  sfile.browse_mode = FILE_BROWSE_MODE_ASSETS;

  UserDef U_default = {};
  FileAssetSelectParams *asset_params = sfile.asset_params;
  if (!asset_params) {
    asset_params = MEM_new<FileAssetSelectParams>(__func__);
    sfile.asset_params = asset_params;
    asset_params->base_params.details_flags = U_default.file_space_data.details_flags;
    asset_params->import_method = FILE_ASSET_IMPORT_FOLLOW_PREFS;
    asset_params->import_flags = FILE_ASSET_IMPORT_INSTANCE_COLLECTIONS_ON_LINK;
  }

  asset_params->asset_library_ref = library_ref;

  FileSelectParams *params = &asset_params->base_params;
  params->filter_id = FILTER_ID_IM;
  params->flag |= FILE_ASSETS_ONLY;
}

static bool image_grid_focus_asset_browser(bContext &C, const AssetLibraryReference &library_ref)
{
  wmWindowManager *wm = CTX_wm_manager(&C);
  for (wmWindow &win : wm->windows) {
    bScreen *screen = WM_window_get_active_screen(&win);
    for (ScrArea &area : screen->areabase) {
      if (area.spacetype != SPACE_FILE) {
        continue;
      }
      SpaceFile *sfile = reinterpret_cast<SpaceFile *>(area.spacedata.first);
      if (sfile->browse_mode != FILE_BROWSE_MODE_ASSETS) {
        continue;
      }

      image_grid_configure_asset_browser(*sfile, library_ref);
      ed::asset::list::storage_fetch(&library_ref, &C);

      CTX_wm_window_set(&C, &win);
      WM_window_set_active_screen(&win, nullptr, screen);
      CTX_wm_area_set(&C, &area);

      ED_area_tag_redraw(&area);
      ED_area_tag_refresh(&area);
      return true;
    }
  }
  return false;
}

static wmOperatorStatus image_grid_browse_assets_exec(bContext *C, wmOperator *op)
{
  const int enum_value = RNA_enum_get(op->ptr, "asset_library_reference");
  const AssetLibraryReference library_ref = ed::asset::library_reference_from_enum_value(
      enum_value);

  if (image_grid_focus_asset_browser(*C, library_ref)) {
    return OPERATOR_FINISHED;
  }

  ScrArea *area = CTX_wm_area(C);
  if (!area) {
    return OPERATOR_CANCELLED;
  }

  ED_area_newspace(C, area, SPACE_FILE, false);
  SpaceFile *sfile = reinterpret_cast<SpaceFile *>(area->spacedata.first);
  image_grid_configure_asset_browser(*sfile, library_ref);
  ed::asset::list::storage_fetch(&library_ref, C);
  ED_area_tag_redraw(area);
  ED_area_tag_refresh(area);

  return OPERATOR_FINISHED;
}

static const EnumPropertyItem *rna_image_grid_browse_library_itemf(bContext * /*C*/,
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

void VIEW3D_OT_image_grid_browse_assets(wmOperatorType *ot)
{
  ot->name = "Browse Image Assets";
  ot->description = "Open or focus the Asset Browser filtered to images";
  ot->idname = "VIEW3D_OT_image_grid_browse_assets";

  ot->exec = image_grid_browse_assets_exec;

  ot->flag = OPTYPE_REGISTER;

  PropertyRNA *prop = RNA_def_property(ot->srna, "asset_library_reference", PROP_ENUM, PROP_NONE);
  RNA_def_enum_funcs(prop, rna_image_grid_browse_library_itemf);
  RNA_def_property_ui_text(prop, "Library", "Asset library to browse");
}

/* -------------------------------------------------------------------- */
/** \name Scroll Grid
 * \{ */

static wmOperatorStatus image_grid_scroll_exec(bContext *C, wmOperator *op)
{
  View3D *v3d = CTX_wm_view3d(C);
  if (!v3d) {
    return OPERATOR_CANCELLED;
  }
  ImageGridUIState &state = image_grid_state_get(*v3d);
  const int delta = RNA_int_get(op->ptr, "delta");
  image_grid_clamp_scroll_row(state, *v3d);
  state.scroll_row = clamp_i(state.scroll_row + delta, 0, image_grid_max_scroll_row(state, *v3d));

  ARegion *region = CTX_wm_region(C);
  if (region) {
    ED_region_tag_redraw(region);
    ED_region_tag_refresh_ui(region);
  }
  return OPERATOR_FINISHED;
}

void VIEW3D_OT_image_grid_scroll(wmOperatorType *ot)
{
  ot->name = "Image Grid Scroll";
  ot->idname = "VIEW3D_OT_image_grid_scroll";
  ot->description = "Scroll the image asset grid by rows";
  ot->exec = image_grid_scroll_exec;
  ot->poll = [](bContext *C) { return CTX_wm_view3d(C) != nullptr; };
  RNA_def_int(ot->srna, "delta", 0, -100, 100, "Delta", "Rows to scroll", -16, 16);
}

/** \} */

}  // namespace blender
