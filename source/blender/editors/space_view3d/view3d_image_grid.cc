/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spview3d
 */

#include "BLI_listbase.h"
#include "BLI_math_base.h"

#include "DNA_ID.h"
#include "DNA_brush_types.h"
#include "DNA_image_types.h"
#include "DNA_texture_types.h"
#include "DNA_space_enums.h"
#include "DNA_space_types.h"
#include "DNA_windowmanager_types.h"

#include "AS_asset_catalog_path.hh"
#include "AS_asset_catalog_tree.hh"
#include "AS_asset_library.hh"

#include "BKE_asset.hh"
#include "BKE_brush.hh"
#include "BKE_context.hh"
#include "BKE_idtype.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_screen.hh"
#include "BKE_main_idmap.hh"
#include "BKE_paint.hh"
#include "BKE_preferences.h"
#include "BKE_screen.hh"
#include "BKE_texture.h"

#include "BLT_translation.hh"

#include "ED_asset_image_library.hh"
#include "ED_asset_import.hh"
#include "ED_asset_library.hh"
#include "ED_asset_list.hh"
#include "ED_asset_mark_clear.hh"
#include "ED_asset_menu_utils.hh"
#include "ED_asset_shelf.hh"
#include "ED_screen.hh"
#include "ED_view3d.hh"

#include "BLI_string_utf8.h"

#include "DNA_userdef_types.h"

#include "MEM_guardedalloc.h"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"
#include "RNA_prototypes.hh"

#include "UI_interface.hh"
#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"
#include "UI_tree_view.hh"

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
  return clamp_i(stored ? stored : 1, 1, 16);
}

int image_grid_preview_size_get(const View3D &v3d)
{
  const int stored = v3d.image_grid_preview_size;
  if (stored >= 24) {
    return stored;
  }
  return ASSET_SHELF_PREVIEW_SIZE_DEFAULT;
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

bool image_grid_slot_is_mask(const PointerRNA &texture_slot_ptr)
{
  if (!texture_slot_ptr.data || !texture_slot_ptr.owner_id) {
    return false;
  }
  if (GS(texture_slot_ptr.owner_id->name) != ID_BR) {
    return false;
  }
  const Brush *brush = reinterpret_cast<const Brush *>(texture_slot_ptr.owner_id);
  return texture_slot_ptr.data == &brush->mask_mtex;
}

const char *image_grid_library_ui_name(const AssetLibraryReference &lib_ref)
{
  switch (lib_ref.type) {
    case ASSET_LIBRARY_ALL:
      return IFACE_("All Libraries");
    case ASSET_LIBRARY_LOCAL:
      return IFACE_("Current File");
    case ASSET_LIBRARY_ESSENTIALS:
      return IFACE_("Essentials");
    case ASSET_LIBRARY_ONLINE_ESSENTIALS:
      return IFACE_("Online Essentials");
    case ASSET_LIBRARY_CUSTOM: {
      const bUserAssetLibrary *user_library = BKE_preferences_asset_library_find_index(
          &U, lib_ref.custom_library_index);
      if (user_library && user_library->name[0]) {
        return user_library->name;
      }
      return IFACE_("Asset Library");
    }
    default:
      return IFACE_("Asset Library");
  }
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

namespace blender::ed::view3d {

/**
 * BrushTextureSlot.texture is a #Tex (ID_TE), while the grid lists #Image (ID_IM) assets.
 * Find or create an image texture datablock for assignment.
 */
static Tex *brush_texture_for_image(Main &bmain, Image &image, const ID *owner_id)
{
  ID *id;
  FOREACH_MAIN_ID_BEGIN (&bmain, id) {
    if (GS(id->name) != ID_TE) {
      continue;
    }
    Tex *tex = reinterpret_cast<Tex *>(id);
    if (tex->type == TEX_IMAGE && tex->ima == &image) {
      return tex;
    }
  }
  FOREACH_MAIN_ID_END;

  Tex *tex = BKE_texture_add(&bmain, image.id.name + 2);
  BKE_texture_type_set(tex, TEX_IMAGE);
  /* Register the image as a user of this texture — #texture_foreach_id uses
   * #IDWALK_CB_USER for `tex->ima`, so the reference must be counted. */
  tex->ima = &image;
  id_us_plus(&image.id);

  if (owner_id) {
    BKE_id_move_to_same_lib(bmain, tex->id, *owner_id);
  }

  return tex;
}

static void brush_mtex_slot_set_texture(MTex *mtex, Tex *tex, Brush *brush)
{
  if (mtex->tex) {
    id_us_min(&mtex->tex->id);
  }
  mtex->tex = tex;
  if (tex) {
    id_us_plus(&tex->id);
  }
  BKE_brush_tag_unsaved_changes(brush);
}

}  // namespace blender::ed::view3d

namespace blender {

using namespace ed::view3d;

static const EnumPropertyItem *rna_image_grid_library_itemf(bContext *C,
                                                            PointerRNA *ptr,
                                                            PropertyRNA *prop,
                                                            bool *r_free);

/* -------------------------------------------------------------------- */
/** \name Assign Brush Texture
 * \{ */

static bool image_grid_id_is_valid(Main &bmain, const ID *id)
{
  if (!id) {
    return false;
  }
  IDNameLib_Map *id_map = BKE_main_idmap_create(
      &bmain, true, nullptr, MAIN_IDMAP_TYPE_UID | MAIN_IDMAP_TYPE_NAME);
  const bool is_valid = BKE_main_idmap_lookup_id(id_map, id) != nullptr;
  BKE_main_idmap_destroy(id_map);
  return is_valid;
}

static Image *image_grid_resolve_image(bContext &C, wmOperator &op)
{
  Main *bmain = CTX_data_main(&C);

  if (RNA_struct_property_is_set(op.ptr, "image_session_uid")) {
    const uint32_t session_uid = uint32_t(RNA_int_get(op.ptr, "image_session_uid"));
    if (session_uid == MAIN_ID_SESSION_UID_UNSET) {
      return nullptr;
    }
    ID *id = BKE_libblock_find_session_uid(bmain, ID_IM, session_uid);
    Image *image = reinterpret_cast<Image *>(id);
    if (!image || !image_grid_id_is_valid(*bmain, &image->id)) {
      return nullptr;
    }
    return image;
  }

  if (!RNA_struct_property_is_set(op.ptr, "asset_identifier")) {
    return nullptr;
  }

  char identifier[FILE_MAX_LIBEXTRA];
  RNA_string_get(op.ptr, "asset_identifier", identifier);
  if (identifier[0] == '\0') {
    return nullptr;
  }

  const int lib_enum = RNA_enum_get(op.ptr, "asset_library_reference");
  const AssetLibraryReference lib_ref = ed::asset::library_reference_from_enum_value(
      lib_enum);

  Image *found_image = nullptr;
  ed::asset::list::storage_fetch(&lib_ref, &C);
  ed::asset::list::iterate(lib_ref, [&](asset_system::AssetRepresentation &asset) {
    if (asset.get_id_type() != ID_IM) {
      return true;
    }
    if (asset.library_relative_identifier() != identifier) {
      return true;
    }

    if (ID *local_id = asset.local_id()) {
      found_image = id_cast<Image *>(local_id);
    }
    else if (!asset.full_library_path().empty()) {
      ID *imported_id = ed::asset::asset_local_id_ensure_imported(*bmain, asset);
      if (imported_id && GS(imported_id->name) == ID_IM) {
        found_image = id_cast<Image *>(imported_id);
      }
    }
    else {
      found_image = BKE_image_load_exists(bmain, asset.full_path().c_str());
    }
    return false;
  });

  if (found_image && !image_grid_id_is_valid(*bmain, &found_image->id)) {
    return nullptr;
  }
  return found_image;
}


static wmOperatorStatus image_grid_assign_texture_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);

  const uint32_t brush_session_uid = uint32_t(RNA_int_get(op->ptr, "brush_session_uid"));
  if (brush_session_uid == MAIN_ID_SESSION_UID_UNSET) {
    return OPERATOR_CANCELLED;
  }
  Brush *brush = reinterpret_cast<Brush *>(
      BKE_libblock_find_session_uid(bmain, ID_BR, brush_session_uid));
  if (!brush) {
    return OPERATOR_CANCELLED;
  }

  const bool use_mask_slot = RNA_boolean_get(op->ptr, "use_mask_slot");
  MTex *mtex = use_mask_slot ? &brush->mask_mtex : &brush->mtex;

  Image *image = image_grid_resolve_image(*C, *op);
  if (!image) {
    return OPERATOR_CANCELLED;
  }

  Tex *tex = brush_texture_for_image(*bmain, *image, &brush->id);
  if (!tex) {
    return OPERATOR_CANCELLED;
  }

  /* Same refcount rules as #set_current_brush_texture() — avoid RNA pointer set, which
   * double-counts users and breaks memfile undo refcount recompute. */
  brush_mtex_slot_set_texture(mtex, tex, brush);

  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  BKE_paint_invalidate_overlay_tex(*bmain, scene, view_layer, tex);

  WM_event_add_notifier(C, NC_BRUSH, brush);
  WM_event_add_notifier(C, NC_ID | NA_EDITED, nullptr);
  return OPERATOR_FINISHED;
}

void VIEW3D_OT_image_grid_assign_texture(wmOperatorType *ot)
{
  ot->name = "Assign Image Grid Texture";
  ot->description = "Assign an image asset as the brush texture";
  ot->idname = "VIEW3D_OT_image_grid_assign_texture";

  ot->exec = image_grid_assign_texture_exec;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  PropertyRNA *prop = RNA_def_int(ot->srna,
                                  "brush_session_uid",
                                  int(MAIN_ID_SESSION_UID_UNSET),
                                  INT32_MIN,
                                  INT32_MAX,
                                  "Brush Session UID",
                                  "Session UID of the brush to assign the texture to",
                                  INT32_MIN,
                                  INT32_MAX);
  RNA_def_property_flag(prop, PROP_SKIP_SAVE | PROP_HIDDEN);

  prop = RNA_def_int(ot->srna,
                     "image_session_uid",
                     int(MAIN_ID_SESSION_UID_UNSET),
                     INT32_MIN,
                     INT32_MAX,
                     "Image Session UID",
                     "Session UID of a local image; when unset, use asset library properties",
                     INT32_MIN,
                     INT32_MAX);
  RNA_def_property_flag(prop, PROP_SKIP_SAVE | PROP_HIDDEN);

  RNA_def_boolean(ot->srna,
                  "use_mask_slot",
                  false,
                  "Mask Texture Slot",
                  "Assign to the mask texture slot instead of the main texture slot");

  prop = RNA_def_property(ot->srna, "asset_library_reference", PROP_ENUM, PROP_NONE);
  RNA_def_enum_funcs(prop, rna_image_grid_library_itemf);
  RNA_def_property_ui_text(prop, "Library", "Asset library of the image asset");

  RNA_def_string(ot->srna,
                 "asset_identifier",
                 nullptr,
                 FILE_MAX_LIBEXTRA,
                 "Asset Identifier",
                 "Library-relative asset identifier when the image is not local");
}

static wmOperatorStatus image_shelf_activate_asset_exec(bContext *C, wmOperator *op)
{
  PointerRNA target_ptr = CTX_data_pointer_get(C, "image_grid_target");
  if (!target_ptr.data || !target_ptr.owner_id || GS(target_ptr.owner_id->name) != ID_BR) {
    BKE_report(op->reports, RPT_ERROR, "No brush target in context");
    return OPERATOR_CANCELLED;
  }

  Main *bmain = CTX_data_main(C);
  Brush *brush = reinterpret_cast<Brush *>(
      BKE_libblock_find_session_uid(bmain, ID_BR, target_ptr.owner_id->session_uid));
  if (!brush) {
    return OPERATOR_CANCELLED;
  }

  /* Reconstruct the asset weak reference from operator properties set by the asset shelf. */
  AssetWeakReference weak_ref{};
  weak_ref.asset_library_type = eAssetLibraryType(RNA_enum_get(op->ptr, "asset_library_type"));
  weak_ref.asset_library_identifier = RNA_string_get_alloc(
      op->ptr, "asset_library_identifier", nullptr, 0, nullptr);
  weak_ref.relative_asset_identifier = RNA_string_get_alloc(
      op->ptr, "relative_asset_identifier", nullptr, 0, nullptr);

  /* Search for the asset in the shelf's current library first. The shelf fetches its specific
   * library (e.g. a custom image library), which may not yet appear in the combined ALL-library
   * view when that combined fetch hasn't been separately requested. */
  const asset_system::AssetRepresentation *asset = nullptr;
  AssetLibraryReference shelf_lib_ref{};
  bool resolved_from_shelf = false;
  if (AssetShelfType *shelf_type = ed::asset::shelf::type_find_from_idname(
          "VIEW3D_AST_image_texture"))
  {
    if (AssetShelf *shelf = ed::asset::shelf::popup_shelf_get_or_create(*C, *shelf_type)) {
      shelf_lib_ref = shelf->settings.asset_library_reference;
      ed::asset::list::iterate(
          shelf->settings.asset_library_reference,
          [&](asset_system::AssetRepresentation &a) {
            if (a.make_weak_reference() == weak_ref) {
              asset = &a;
              resolved_from_shelf = true;
              return false;
            }
            return true;
          });
    }
  }

  /* Fallback: search the combined all-library view (standard path for .blend library assets). */
  if (!asset) {
    asset = ed::asset::find_asset_from_weak_ref(*C, weak_ref, op->reports);
  }

  if (!asset || asset->get_id_type() != ID_IM) {
    if (asset) {
      BKE_report(op->reports, RPT_ERROR, "Selected asset is not an image");
    }
    return OPERATOR_CANCELLED;
  }

  /* Resolve image: prefer already-loaded local ID, then import from .blend library, then
   * load from disk for direct-file assets stored outside a .blend library. */
  Image *image = nullptr;
  if (ID *local_id = asset->local_id()) {
    image = id_cast<Image *>(local_id);
  }
  else if (!asset->full_library_path().empty()) {
    if (ID *imported_id = ed::asset::asset_local_id_ensure_imported(*bmain, *asset)) {
      if (GS(imported_id->name) == ID_IM) {
        image = id_cast<Image *>(imported_id);
      }
    }
  }
  else {
    image = BKE_image_load_exists(bmain, asset->full_path().c_str());
  }

  if (!image) {
    BKE_report(op->reports, RPT_ERROR, "Could not load image asset");
    return OPERATOR_CANCELLED;
  }

  const bool use_mask_slot = image_grid_slot_is_mask(target_ptr);
  MTex *mtex = use_mask_slot ? &brush->mask_mtex : &brush->mtex;

  Tex *tex = brush_texture_for_image(*bmain, *image, &brush->id);
  if (!tex) {
    return OPERATOR_CANCELLED;
  }

  brush_mtex_slot_set_texture(mtex, tex, brush);

  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  BKE_paint_invalidate_overlay_tex(*bmain, scene, view_layer, tex);

  WM_event_add_notifier(C, NC_BRUSH, brush);
  WM_event_add_notifier(C, NC_ID | NA_EDITED, nullptr);

  if (resolved_from_shelf) {
    View3D *v3d = CTX_wm_view3d(C);
    if (v3d) {
      ImageGridUIState &state = image_grid_state_get(*v3d);
      const std::optional<std::string> catalog_path = image_grid_catalog_path_for_asset(
          *asset, shelf_lib_ref);
      image_grid_request_scroll_to_asset(state, asset->library_relative_identifier());
      if (image_grid_asset_is_visible_in_state(state, shelf_lib_ref, catalog_path)) {
        image_grid_pending_clear(state);
        /* Do not call image_grid_notify_change while the browse popover is open: it would
         * trigger a full grid rebuild (storage_fetch + refresh_ui) on every asset click.
         * NC_BRUSH and NC_ID notifiers sent above already schedule a redraw, on which
         * image_grid_apply_focus_scroll will update scroll_row without a rebuild. */
      }
      else {
        image_grid_pending_schedule_from_asset(state,
                                               shelf_lib_ref,
                                               catalog_path,
                                               asset->library_relative_identifier());
      }
    }
  }

  return OPERATOR_FINISHED;
}

void VIEW3D_OT_image_shelf_activate_asset(wmOperatorType *ot)
{
  ot->name = "Activate Image Asset";
  ot->description = "Assign the selected image asset to the brush texture slot";
  ot->idname = "VIEW3D_OT_image_shelf_activate_asset";
  ot->exec = image_shelf_activate_asset_exec;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  ed::asset::operator_asset_reference_props_register(*ot->srna);
}

/** \} */

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
  if (path_buf[0] == '\0') {
    /* Empty path means "show all" — clear the filter set. */
    state.enabled_catalog_paths.clear();
  }
  else {
    /* Toggle the path in the enabled set. */
    const std::string path(path_buf);
    if (!state.enabled_catalog_paths.remove(path)) {
      state.enabled_catalog_paths.add(path);
    }
  }
  image_grid_catalog_commit_active(state);
  state.scroll_row = 0;
  image_grid_pending_clear(state);

  image_grid_state_persist_to_view3d(*v3d, state);
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
  if (enum_value == ed::asset::library_reference_to_enum_value(&state.lib_ref)) {
    return OPERATOR_CANCELLED;
  }

  const AssetLibraryReference old_lib_ref = state.lib_ref;
  image_grid_catalog_swap_library(state, old_lib_ref, new_ref);
  state.scroll_row = 0;
  image_grid_pending_clear(state);

  image_grid_state_persist_to_view3d(*v3d, state);
  image_grid_prepare_browse_shelf(*C, state, "VIEW3D_AST_image_texture");

  image_grid_notify_change(*C);
  return OPERATOR_FINISHED;
}

void VIEW3D_OT_image_grid_set_library(wmOperatorType *ot)
{
  ot->name = "Set Image Grid Library";
  ot->description = "Set the asset library used by the image grid";
  ot->idname = "VIEW3D_OT_image_grid_set_library";

  ot->exec = image_grid_set_library_exec;

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
  if (!state.enabled_catalog_paths.is_empty()) {
    asset_system::AssetLibrary *library = ed::asset::list::library_get_once_available(
        asset_system::current_file_library_reference());
    if (library) {
      /* Assign the asset to the first enabled catalog path. If multiple catalogs are
       * enabled, the first one encountered in set iteration is used. */
      const std::string &path_str = *state.enabled_catalog_paths.begin();
      const asset_system::AssetCatalogPath cat_path =
          asset_system::AssetCatalogPath::from_user_input(path_str.c_str());
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

/* -------------------------------------------------------------------- */
/** \name Refresh Image Grid Library
 * \{ */

static bool image_grid_refresh_library_poll(bContext *C)
{
  const View3D *v3d = CTX_wm_view3d(C);
  if (!v3d) {
    return false;
  }
  const ImageGridUIState &state = image_grid_state_get(*v3d);
  return state.lib_ref.type != ASSET_LIBRARY_LOCAL;
}

static wmOperatorStatus image_grid_refresh_library_exec(bContext *C, wmOperator * /*op*/)
{
  View3D *v3d = CTX_wm_view3d(C);
  if (!v3d) {
    return OPERATOR_CANCELLED;
  }

  ImageGridUIState &state = image_grid_state_get(*v3d);

  if (state.lib_ref.type == ASSET_LIBRARY_CUSTOM) {
    const bUserAssetLibrary *user_lib = BKE_preferences_asset_library_find_index(
        &U, state.lib_ref.custom_library_index);
    if (user_lib && !(user_lib->flag & ASSET_LIBRARY_USE_REMOTE_URL) && user_lib->dirpath[0]) {
      ed::asset::image_library_scan_and_index(user_lib->dirpath);
      ed::asset::image_library_invalidate_cached_previews(user_lib->dirpath);
    }
  }

  ed::asset::list::clear(&state.lib_ref, C);
  WM_event_add_notifier(C, NC_ASSET | ND_ASSET_LIST_READING, nullptr);
  return OPERATOR_FINISHED;
}

void VIEW3D_OT_image_grid_refresh_library(wmOperatorType *ot)
{
  ot->name = "Refresh Image Grid Library";
  ot->description =
      "Rescan the image library for added, moved or deleted files and reload the image grid";
  ot->idname = "VIEW3D_OT_image_grid_refresh_library";

  ot->exec = image_grid_refresh_library_exec;
  ot->poll = image_grid_refresh_library_poll;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Catalog Selector Popover
 * \{ */

/** Tree view listing catalogs of the current image-grid library. Individual catalogs can be
 * enabled or disabled via checkboxes. An empty selection means "show all". */
class ImageGridCatalogSelectorTree : public ui::AbstractTreeView {
  const bContext &C_;
  ed::view3d::ImageGridUIState &state_;
  /* Full catalog tree shared from the library's catalog service. Using a shared_ptr avoids
   * copying the tree (which has raw parent pointers) and ensures the data stays alive. */
  std::shared_ptr<const asset_system::AssetCatalogTree> catalog_tree_;

 public:
  class AllItem;
  class Item;

  ImageGridCatalogSelectorTree(const bContext &C,
                                ed::view3d::ImageGridUIState &state,
                                const asset_system::AssetLibrary &library)
      : C_(C), state_(state)
  {
    /* Use the full catalog tree of the library rather than a filtered tree built from
     * catalog IDs of loaded assets. The filtered approach shows nothing when assets have
     * no catalog assignment (nil UUID), which is the common case after a plain
     * "Mark as Asset" without assigning a catalog path. Showing ALL registered catalogs
     * lets the user navigate to any catalog regardless of current asset assignments. */
    catalog_tree_ = library.catalog_service().catalog_tree();
  }

  void build_tree() override;

  void update_enabled_catalogs_from_items(bContext &C);

  static Item &build_catalog_items_recursive(
      ui::TreeViewOrItem &parent,
      const asset_system::AssetCatalogTreeItem &catalog_item,
      ed::view3d::ImageGridUIState &state);

  /** Activatable item that clears the catalog filter (shows all assets). */
  class AllItem : public ui::BasicTreeViewItem {
    ed::view3d::ImageGridUIState &state_;

   public:
    AllItem(ed::view3d::ImageGridUIState &state)
        : ui::BasicTreeViewItem(IFACE_("All")), state_(state)
    {
      this->set_on_activate_fn([this](bContext &C, ui::BasicTreeViewItem & /*item*/) {
        state_.enabled_catalog_paths.clear();
        ed::view3d::image_grid_catalog_commit_active(state_);
        state_.scroll_row = 0;
        ed::view3d::image_grid_notify_change(C);
      });
      this->set_is_active_fn(
          [this]() -> bool { return state_.enabled_catalog_paths.is_empty(); });
    }
  };

  /** Checkbox item for an individual catalog path. */
  class Item : public ui::BasicTreeViewItem {
    const asset_system::AssetCatalogTreeItem &catalog_item_;
    /* Is the catalog path enabled in this redraw? Set on construction, updated by the UI (which
     * gets a pointer to it). The UI needs it as char. */
    char catalog_path_enabled_ = false;

   public:
    Item(const asset_system::AssetCatalogTreeItem &catalog_item,
         ed::view3d::ImageGridUIState &state)
        : ui::BasicTreeViewItem(catalog_item.get_name()),
          catalog_item_(catalog_item),
          catalog_path_enabled_(
              state.enabled_catalog_paths.contains(catalog_item.catalog_path().str()) ? 1 : 0)
    {
      disable_activatable();
    }

    bool is_catalog_path_enabled() const
    {
      return catalog_path_enabled_ != 0;
    }

    bool has_enabled_in_subtree()
    {
      bool has_enabled = false;
      foreach_item_recursive(
          [&has_enabled](const ui::AbstractTreeViewItem &abstract_item) {
            const Item *item = dynamic_cast<const Item *>(&abstract_item);
            if (item && item->is_catalog_path_enabled()) {
              has_enabled = true;
            }
          },
          IterOptions::SkipFiltered);
      return has_enabled;
    }

    asset_system::AssetCatalogPath catalog_path() const
    {
      return catalog_item_.catalog_path();
    }

    void build_row(ui::Layout &row) override
    {
      ImageGridCatalogSelectorTree &tree =
          dynamic_cast<ImageGridCatalogSelectorTree &>(get_tree_view());
      ui::Block *block = row.block();

      row.emboss_set(ui::EmbossType::Emboss);

      ui::Layout &subrow = row.row(false);
      subrow.active_set(catalog_path_enabled_);
      subrow.label(catalog_item_.get_name(), ICON_NONE);
      ui::block_layout_set_current(block, &row);

      ui::Button *toggle_but = uiDefButV(block,
                                         ui::ButtonType::Checkbox,
                                         "",
                                         0,
                                         0,
                                         UI_UNIT_X,
                                         UI_UNIT_Y,
                                         &catalog_path_enabled_,
                                         0,
                                         0,
                                         TIP_("Toggle catalog visibility in the image grid"));
      ui::button_func_set(toggle_but, [&tree](bContext &C) {
        tree.update_enabled_catalogs_from_items(C);
      });
      if (!is_catalog_path_enabled() && has_enabled_in_subtree()) {
        ui::button_drawflag_enable(toggle_but, ui::BUT_INDETERMINATE);
      }
      ui::button_flag_disable(toggle_but, ui::BUT_UNDO);
    }
  };
};

void ImageGridCatalogSelectorTree::update_enabled_catalogs_from_items(bContext &C)
{
  state_.enabled_catalog_paths.clear();
  foreach_item([this](ui::AbstractTreeViewItem &view_item) {
    const Item *item = dynamic_cast<const Item *>(&view_item);
    if (item && item->is_catalog_path_enabled()) {
      state_.enabled_catalog_paths.add(item->catalog_path().str());
    }
  });
  ed::view3d::image_grid_catalog_commit_active(state_);
  state_.scroll_row = 0;
  ed::view3d::image_grid_pending_clear(state_);

  if (View3D *v3d = CTX_wm_view3d(&C)) {
    ed::view3d::image_grid_state_persist_to_view3d(*v3d, state_);
  }

  ed::view3d::image_grid_notify_change(C);
}

void ImageGridCatalogSelectorTree::build_tree()
{
  AllItem &all_item = add_tree_item<AllItem>(state_);
  all_item.uncollapse_by_default();

  if (!catalog_tree_ || catalog_tree_->is_empty()) {
    return;
  }

  catalog_tree_->foreach_root_item(
      [this](const asset_system::AssetCatalogTreeItem &cat_item) {
        Item &item = build_catalog_items_recursive(*this, cat_item, state_);
        item.uncollapse_by_default();
      });
}

ImageGridCatalogSelectorTree::Item &ImageGridCatalogSelectorTree::build_catalog_items_recursive(
    ui::TreeViewOrItem &parent,
    const asset_system::AssetCatalogTreeItem &catalog_item,
    ed::view3d::ImageGridUIState &state)
{
  Item &item = parent.add_tree_item<Item>(catalog_item, state);

  catalog_item.foreach_child([&](const asset_system::AssetCatalogTreeItem &child) {
    build_catalog_items_recursive(item, child, state);
  });

  return item;
}

static void image_grid_display_panel_draw(const bContext *C, Panel *panel)
{
  View3D *v3d = CTX_wm_view3d(C);
  if (!v3d) {
    return;
  }

  ui::Layout &layout = *panel->layout;
  layout.use_property_split_set(true);
  layout.use_property_decorate_set(false);

  PointerRNA v3d_ptr = RNA_pointer_create_discrete(nullptr, RNA_SpaceView3D, v3d);
  layout.prop(&v3d_ptr, "image_grid_preview_size", UI_ITEM_NONE, IFACE_("Size"), ICON_NONE);
}

void image_grid_display_panel_register()
{
  if (WM_paneltype_find("VIEW3D_PT_image_grid_display", true)) {
    return;
  }

  PanelType *pt = MEM_new_zeroed<PanelType>(__func__);
  STRNCPY_UTF8(pt->idname, "VIEW3D_PT_image_grid_display");
  STRNCPY_UTF8(pt->label, N_("Display Settings"));
  STRNCPY_UTF8(pt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  pt->description = N_("Adjust display settings for the image grid");
  pt->draw = image_grid_display_panel_draw;
  WM_paneltype_add(pt);
}

static void image_grid_catalog_selector_draw(const bContext *C, Panel *panel)
{
  View3D *v3d = CTX_wm_view3d(C);
  if (!v3d) {
    return;
  }

  ed::view3d::ImageGridUIState &state = image_grid_state_get(*v3d);

  ui::Layout &layout = *panel->layout;
  layout.operator_context_set(wm::OpCallContext::InvokeDefault);

  /* Catalog tree. */
  ed::asset::list::storage_fetch(&state.lib_ref, C);

  const asset_system::AssetLibrary *library = ed::asset::list::library_get_once_available(
      state.lib_ref);
  if (!library) {
    layout.label(IFACE_("Loading\xe2\x80\xa6"), ICON_NONE);
    return;
  }

  ui::Block *block = layout.block();
  ui::AbstractTreeView *tree_view = ui::block_add_view(
      *block,
      "image_grid_catalog_selector",
      std::make_unique<ImageGridCatalogSelectorTree>(*C, state, *library));
  ui::TreeViewBuilder::build_tree_view(*C, *tree_view, layout);
}

void image_grid_catalog_selector_panel_register()
{
  if (WM_paneltype_find("VIEW3D_PT_image_grid_catalog_selector", true)) {
    return;
  }

  PanelType *pt = MEM_new_zeroed<PanelType>(__func__);
  STRNCPY_UTF8(pt->idname, "VIEW3D_PT_image_grid_catalog_selector");
  STRNCPY_UTF8(pt->label, N_("Catalog Selector"));
  STRNCPY_UTF8(pt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  pt->description = N_("Select the asset library and catalog to display in the image grid");
  pt->draw = image_grid_catalog_selector_draw;
  pt->listener = ed::asset::list::asset_reading_region_listen_fn;
  WM_paneltype_add(pt);
}

/** \} */

}  // namespace blender
