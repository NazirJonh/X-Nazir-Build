/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spview3d
 */

#include "BLI_listbase.h"
#include "BLI_math_base.h"
#include "BLI_string.h"

#include "DNA_ID.h"
#include "DNA_brush_types.h"
#include "DNA_image_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_enums.h"
#include "DNA_space_types.h"
#include "DNA_texture_types.h"
#include "DNA_windowmanager_types.h"

#include "AS_asset_catalog_path.hh"
#include "AS_asset_catalog_tree.hh"
#include "AS_asset_library.hh"

#include "BKE_asset.hh"
#include "BKE_asset_edit.hh"
#include "BKE_brush.hh"
#include "BKE_context.hh"
#include "BKE_idtype.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_paint.hh"
#include "BKE_preferences.h"
#include "BKE_screen.hh"
#include "BKE_texture.h"

#include "BLT_translation.hh"

#include "ED_asset_image_utils.hh"
#include "ED_asset_import.hh"
#include "ED_asset_library.hh"
#include "ED_asset_list.hh"
#include "ED_asset_mark_clear.hh"
#include "ED_asset_menu_utils.hh"
#include "ED_asset_shelf.hh"
#include "ED_image_grid.hh"
#include "ED_screen.hh"

#include "BLI_string_utf8.h"

#include "DNA_scene_types.h"
#include "DNA_userdef_types.h"

#include "BKE_global.hh"

#include "ED_image.hh"
#include "ED_render.hh"
#include "ED_undo.hh"

#include "MEM_guardedalloc.h"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"
#include "RNA_prototypes.hh"

#include "UI_grid_view.hh"
#include "UI_interface.hh"
#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"
#include "UI_tree_view.hh"

#include "WM_api.hh"
#include "WM_toolsystem.hh"
#include "WM_types.hh"

#include "view3d_intern.hh"

namespace blender::ed::image_grid {

void image_grid_notify_change(bContext &C, const bool is_mask_slot)
{
  const std::optional<ed::image_grid::ImageGridOwner> owner = ed::image_grid::image_grid_owner_from_context(
      C);
  if (!owner) {
    return;
  }

  ImageGridUIState &state = ed::image_grid::image_grid_state_get(*owner, is_mask_slot);
  ed::asset::list::storage_fetch(&state.filter.lib_ref, &C);
  if (state.filter.lib_ref.type == ASSET_LIBRARY_ALL) {
    /* #storage_fetch() above only warms the built-in merged All list, not each real library's own
     * #AssetList that #image_grid_all_mode_libraries() needs -- see #image_grid_fetch_all_mode_libraries(). */
    image_grid_fetch_all_mode_libraries(C);
  }
  WM_event_add_notifier(&C, NC_ASSET | ND_ASSET_LIST, nullptr);
  WM_event_add_notifier(&C, NC_ID | NA_EDITED, nullptr);

  ARegion *region = CTX_wm_region(&C);
  if (region) {
    ED_region_tag_redraw(region);
    ED_region_tag_refresh_ui(region);
  }
}

/**
 * Whether \a region is a popover. The pre-button handlers receive the popup's #RGN_TYPE_TEMPORARY
 * region (see #region_pre_button_handlers_call with #uiPopupBlockHandle::region), while
 * #CTX_wm_region_popup is not yet set at that point — so detect from the region, not the context.
 */
static bool image_grid_region_is_popover(const ARegion *region)
{
  return region && region->regiontype == RGN_TYPE_TEMPORARY;
}

bool image_grid_slot_is_mask(const PointerRNA &texture_slot_ptr)
{
  if (!texture_slot_ptr.data || !texture_slot_ptr.owner_id) {
    return false;
  }
  if (GS(texture_slot_ptr.owner_id->name) != ID_BR) {
    return false;
  }
  const Brush *brush = id_cast<const Brush *>(texture_slot_ptr.owner_id);
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
      const bUserAssetLibrary *user_library = BKE_preferences_asset_library_find_from_ref(
          &U, &lib_ref);
      if (user_library && user_library->name[0]) {
        return user_library->name;
      }
      return IFACE_("Asset Library");
    }
    default:
      return IFACE_("Asset Library");
  }
}

const char *image_grid_library_selector_label(const ImageGridUIState &state)
{
  switch (state.filter.catalog_mode) {
    case ImageGridCatalogMode::Recent:
      return IFACE_("Recent");
    case ImageGridCatalogMode::Favorites:
      return IFACE_("Favorites");
    case ImageGridCatalogMode::All:
    case ImageGridCatalogMode::CatalogPath:
      break;
  }
  return image_grid_library_ui_name(state.filter.lib_ref);
}

static bool image_grid_scroll_under_mouse(const ARegion *region,
                                          const int xy[2],
                                          const ed::image_grid::ImageGridOwner owner,
                                          bool *r_is_mask_slot)
{
  /* The grid's scroll position now lives in the shared session registry; hit-test the scrollbar of
   * each slot's session for the host (sidebar/popover) the cursor is in. */
  const bool is_popover = image_grid_region_is_popover(region);
  for (const bool is_mask : {false, true}) {
    if (ui::grid_view_session_scroll_button_under_mouse(
            region, xy, ed::image_grid::image_grid_session_id(owner, is_mask, is_popover)))
    {
      *r_is_mask_slot = is_mask;
      return true;
    }
  }
  return false;
}

static bool image_grid_mouse_over(const ARegion *region, const int xy[2], bool *r_is_mask_slot)
{
  if (ui::region_view_has_idname_at(region, xy, 0, "image_asset_grid_mask") ||
      ui::region_view_item_has_idname_at(region, xy, "image_asset_grid_mask"))
  {
    *r_is_mask_slot = true;
    return true;
  }
  if (ui::region_view_has_idname_at(region, xy, 0, "image_asset_grid") ||
      ui::region_view_item_has_idname_at(region, xy, "image_asset_grid"))
  {
    *r_is_mask_slot = false;
    return true;
  }
  return false;
}

/* -------------------------------------------------------------------- */
/** \name Focus Active Texture (numpad-period hotkey)
 * \{ */

/** The image assigned to the active paint brush's main or mask texture slot, or null. */
static const Image *image_grid_active_slot_image(bContext &C, const bool is_mask_slot)
{
  Paint *paint = BKE_paint_get_active_from_context(&C);
  if (!paint) {
    return nullptr;
  }
  const Brush *brush = BKE_paint_brush(paint);
  if (!brush) {
    return nullptr;
  }
  const MTex &mtex = is_mask_slot ? brush->mask_mtex : brush->mtex;
  if (mtex.tex && mtex.tex->type == TEX_IMAGE) {
    return mtex.tex->ima;
  }
  return nullptr;
}

/** Filtered-list identifier of \a image (asset identifier or blend-image name), empty if absent.
 */
static std::string image_grid_filtered_identifier_for_image(bContext &C,
                                                            const ImageGridUIState &state,
                                                            const Image &image)
{
  Main *bmain = CTX_data_main(&C);
  std::string identifier;
  image_grid_foreach_filtered_item(
      *bmain, state, [&](const ImageGridFilteredItem &item, int /*filtered_index*/) -> bool {
        const bool matches = item.image ? (item.image == &image) :
                                          image_grid_asset_represents_image(*item.asset, image);
        if (matches) {
          identifier = item.image ? std::string(item.image->id.name + 2) :
                                    std::string(item.asset->library_relative_identifier());
          return false;
        }
        return true;
      });
  return identifier;
}

/** Request every grid layout (N-Panel and Texture popover) to scroll-center on the slot's
 * currently assigned texture. Returns false when there is nothing to focus. */
static bool image_grid_focus_active_texture(bContext &C, const bool is_mask_slot)
{
  const std::optional<ed::image_grid::ImageGridOwner> owner =
      ed::image_grid::image_grid_owner_from_context(C);
  if (!owner) {
    return false;
  }
  const Image *active_image = image_grid_active_slot_image(C, is_mask_slot);
  if (!active_image) {
    return false;
  }
  ImageGridUIState &state = ed::image_grid::image_grid_state_get(*owner, is_mask_slot);
  const std::string identifier = image_grid_filtered_identifier_for_image(C, state, *active_image);
  if (identifier.empty()) {
    return false;
  }
  /* Reset every layout's applied flag so both grids re-center on their next redraw. The block
   * listener refreshes each grid block on #NC_ASSET, so the popover and the N-Panel both re-run
   * #build_image_grid and apply the centered focus scroll. */
  image_grid_request_scroll_to_asset(state, identifier);
  image_grid_notify_change(C, is_mask_slot);
  return true;
}

int handle_image_grid_focus_active_event(bContext *C, const wmEvent *event, ARegion *region)
{
  if (event->type != EVT_PADPERIOD || event->val != KM_PRESS || event->modifier) {
    return WM_UI_HANDLER_CONTINUE;
  }
  const std::optional<ed::image_grid::ImageGridOwner> owner =
      ed::image_grid::image_grid_owner_from_context(*C);
  if (!owner || !region) {
    return WM_UI_HANDLER_CONTINUE;
  }
  /* Only react when the cursor is over a grid (or its scrollbar), matching the wheel handler. */
  bool is_mask_slot = false;
  if (!image_grid_mouse_over(region, event->xy, &is_mask_slot) &&
      !image_grid_scroll_under_mouse(region, event->xy, *owner, &is_mask_slot))
  {
    return WM_UI_HANDLER_CONTINUE;
  }
  if (!image_grid_focus_active_texture(*C, is_mask_slot)) {
    return WM_UI_HANDLER_CONTINUE;
  }
  ED_region_tag_redraw(region);
  ED_region_tag_refresh_ui(region);
  return WM_UI_HANDLER_BREAK;
}

void image_grid_auto_focus_on_brush_change(bContext &C, const bool is_mask_slot)
{
  const std::optional<ed::image_grid::ImageGridOwner> owner = ed::image_grid::image_grid_owner_from_context(
      C);
  if (!owner) {
    return;
  }
  Paint *paint = BKE_paint_get_active_from_context(&C);
  const Brush *brush = paint ? BKE_paint_brush(paint) : nullptr;
  const uint32_t brush_uid = brush ? brush->id.session_uid : 0;

  ImageGridUIState &state = ed::image_grid::image_grid_state_get(*owner, is_mask_slot);
  if (brush_uid == state.viewport.last_auto_focus_brush_uid) {
    return;
  }

  if (!brush) {
    state.viewport.last_auto_focus_brush_uid = brush_uid;
    return;
  }
  const MTex &mtex = is_mask_slot ? brush->mask_mtex : brush->mtex;
  if (!mtex.tex || mtex.tex->type != TEX_IMAGE || !mtex.tex->ima) {
    state.viewport.last_auto_focus_brush_uid = brush_uid;
    return;
  }

  /* Defer until the library finishes loading; NC_ASSET will retrigger a redraw. */
  if (!ed::asset::list::library_get_once_available(state.filter.lib_ref)) {
    return;
  }

  state.viewport.last_auto_focus_brush_uid = brush_uid;
  const std::string identifier = image_grid_filtered_identifier_for_image(
      C, state, *mtex.tex->ima);
  if (!identifier.empty()) {
    image_grid_request_scroll_to_asset(state, identifier);
  }
}

/** \} */

int image_grid_effective_rows(const ImageGridOwner owner, const bool is_mask_slot)
{
  const int stored = owner.slot_dna(is_mask_slot).rows;
  return clamp_i(stored ? stored : 1, 1, 16);
}

int image_grid_preview_size_get(const ImageGridOwner owner)
{
  const int stored = owner.preview_size_dna();
  if (stored >= 24) {
    return stored;
  }
  return ASSET_SHELF_PREVIEW_SIZE_DEFAULT;
}

/**
 * Image paint expects a plain image texture (#TEX_IMAGE without a node tree or color ramp).
 * Reusing other blocks from the file can crash in #ntreeTexBeginExecTree or
 * #BKE_colorband_evaluate.
 */
static bool brush_texture_is_plain_image(const Tex *tex)
{
  if (!tex || tex->type != TEX_IMAGE || tex->use_nodes || tex->nodetree != nullptr) {
    return false;
  }
  if (tex->flag & TEX_COLORBAND) {
    return false;
  }
  return true;
}

/**
 * BrushTextureSlot.texture is a #Tex (ID_TE), while the grid lists #Image (ID_IM) assets.
 * Find or create an image texture datablock for assignment.
 */
static Tex *brush_texture_for_image(Main &bmain, Image &image)
{
  ID *id;
  FOREACH_MAIN_ID_BEGIN (&bmain, id) {
    if (GS(id->name) != ID_TE) {
      continue;
    }
    Tex *tex = id_cast<Tex *>(id);
    if (!brush_texture_is_plain_image(tex)) {
      continue;
    }
    if (tex->ima == &image) {
      return tex;
    }
  }
  FOREACH_MAIN_ID_END;

  Tex *tex = BKE_texture_add(&bmain, image.id.name + 2);
  BKE_texture_type_set(tex, TEX_IMAGE);
  tex->flag &= ~TEX_COLORBAND;
  /* Register the image as a user of this texture — #texture_foreach_id uses
   * #IDWALK_CB_USER for `tex->ima`, so the reference must be counted. */
  tex->ima = &image;
  id_us_plus(&image.id);

  /* #BKE_texture_add already left one user; the sole caller assigns #tex into an #MTex slot via
   * #brush_mtex_slot_set_texture, which adds the real reference with #id_us_plus, so compensate. */
  id_us_min(&tex->id);

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

/**
 * The #Tex created for an #Image is a local, undo-managed datablock, but an editable asset brush
 * is linked and belongs to the no-undo domain. The memfile-undo no-undo pointer remap skips linked
 * IDs (#read_undo_remap_noundo_data), so a linked brush referencing a local #Tex would dangle
 * after an undo step (and the reference could not be saved into the .blend either). Make the brush
 * local before assignment so both sides of the reference share the undo domain, then re-activate
 * the now-local brush as the paint's active brush.
 *
 * Returns the brush to assign the texture to: the new local copy, or \a brush unchanged when it is
 * already local.
 */
static Brush *brush_ensure_local_for_texture(bContext &C, Brush &brush)
{
  if (!ID_IS_LINKED(&brush)) {
    return &brush;
  }

  Main &bmain = *CTX_data_main(&C);
  Brush *local_brush = id_cast<Brush *>(bke::asset_edit_id_ensure_local(bmain, brush.id));
  if (local_brush == nullptr || local_brush == &brush) {
    return &brush;
  }

  /* Keep the active paint brush in sync with the datablock that now owns the texture. The tool
   * brush bindings are updated later in #image_grid_assign_image_to_brush, once the texture has
   * been assigned (which may make the brush local in place). */
  if (Paint *paint = BKE_paint_get_active_from_context(&C)) {
    BKE_paint_brush_set(paint, local_brush);
  }
  return local_brush;
}

/**
 * Find or create a #Tex wrapping \a image, assign it to the appropriate #MTex slot of \a brush,
 * invalidate paint overlays, and send #NC_BRUSH / #NC_ID notifiers.
 *
 * Returns true on success; false when no #Tex could be created (extremely rare).
 */
static bool image_grid_assign_image_to_brush(bContext &C,
                                             Brush &brush,
                                             Image &image,
                                             const bool use_mask_slot)
{
  Main &bmain = *CTX_data_main(&C);
  MTex *mtex = use_mask_slot ? &brush.mask_mtex : &brush.mtex;

  Tex *tex = brush_texture_for_image(bmain, image);
  if (!tex) {
    return false;
  }

  brush_mtex_slot_set_texture(mtex, tex, &brush);

  Scene *scene = CTX_data_scene(&C);
  ViewLayer *view_layer = CTX_data_view_layer(&C);
  BKE_paint_invalidate_overlay_tex(bmain, scene, view_layer, tex);

  /* Assigning a local image texture makes the brush a local datablock (in place, keeping its
   * name). Update the tool brush bindings to reference this local brush, otherwise a later
   * #WM_toolsystem_refresh_active (run from #ed_undo_step_post on every undo) re-resolves the
   * active brush from the unchanged asset weak-reference and re-links a fresh, texture-less copy
   * of the source asset, silently dropping the texture. */
  if (Paint *paint = BKE_paint_get_active_from_context(&C)) {
    if (paint->brush == &brush) {
      WM_toolsystem_activate_brush_and_tool(&C, paint, &brush);
    }
  }

  /* Push a visible memfile undo step that captures the texture assignment.  Using skip=false
   * (the default) means the undo system treats this as a normal landing point: undoing all paint
   * strokes above it stops here rather than falling through to an older memfile that predates the
   * assignment.  Setting is_memfile_undo_written=false instead (previous approach) deferred the
   * push to WITH_GLOBAL_UNDO_ENSURE_UPDATED at the first stroke, but that mechanism inserts a
   * skip=true baseline, which BKE_undosys_step_load_data_ex skips over when looking for a
   * non-skip landing point — causing it to decode the pre-texture memfile and erase the texture
   * on the very first Ctrl+Z. */
  ED_undo_memfile_push(&C, "Assign Brush Texture");

  WM_event_add_notifier(&C, NC_BRUSH, &brush);
  WM_event_add_notifier(&C, NC_ID | NA_EDITED, nullptr);
  return true;
}

/** Switch the grid to the current-file library (if it was showing another one) and scroll-focus
 * \a image, so an image just opened/assigned is visible regardless of the previous filter. */
static void image_grid_after_image_opened(bContext &C,
                                          const ed::image_grid::ImageGridOwner owner,
                                          Image &image,
                                          const bool is_mask_slot)
{
  ImageGridUIState &state = ed::image_grid::image_grid_state_get(owner, is_mask_slot);
  const AssetLibraryReference local_ref = asset_system::current_file_library_reference();

  const AssetLibraryReference old_lib_ref = state.filter.lib_ref;
  if (state.filter.lib_ref.type != ASSET_LIBRARY_LOCAL) {
    image_grid_catalog_swap_library(state, old_lib_ref, local_ref);
    state.filter.lib_ref = local_ref;
    image_grid_catalog_commit_active(state);
  }

  ed::image_grid::image_grid_reset_scroll(owner, is_mask_slot);
  image_grid_request_scroll_to_asset(state, image.id.name + 2);
  image_grid_pending_clear(state);

  ed::image_grid::image_grid_state_persist(owner, state, is_mask_slot);
  ed::asset::list::storage_fetch(&local_ref, &C);
  image_grid_prepare_browse_shelf(C, state, "VIEW3D_AST_image_texture");
  image_grid_notify_change(C, is_mask_slot);
}

/**
 * Core of assigning \a image to the brush texture slot identified by \a target_ptr: localize a
 * linked brush, wrap the image in a #Tex, assign it to the slot, then switch the grid to the
 * current-file library and scroll-focus the image. Shared by the Open/New-image operators (via
 * #image_grid_assign_target_image) and by dropping an image onto the grid
 * (#image_grid_assign_dropped_image).
 */
static bool image_grid_assign_image_to_slot(bContext &C,
                                            const PointerRNA &target_ptr,
                                            Brush &brush,
                                            Image &image)
{
  const bool use_mask_slot = image_grid_slot_is_mask(target_ptr);
  Brush *target_brush = brush_ensure_local_for_texture(C, brush);
  if (!image_grid_assign_image_to_brush(C, *target_brush, image, use_mask_slot)) {
    return false;
  }
  if (const std::optional<ed::image_grid::ImageGridOwner> owner =
          ed::image_grid::image_grid_owner_from_context(C))
  {
    image_grid_after_image_opened(C, *owner, image, use_mask_slot);
  }
  return true;
}

bool image_grid_assign_dropped_image(bContext &C, const PointerRNA &target_ptr, Image &image)
{
  if (!target_ptr.owner_id || GS(target_ptr.owner_id->name) != ID_BR) {
    return false;
  }
  Main *bmain = CTX_data_main(&C);
  Brush *brush = id_cast<Brush *>(
      BKE_libblock_find_session_uid(bmain, ID_BR, target_ptr.owner_id->session_uid));
  if (!brush) {
    return false;
  }
  return image_grid_assign_image_to_slot(C, target_ptr, *brush, image);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Set Library
 * \{ */

bool image_grid_set_library(bContext &C,
                            ed::image_grid::ImageGridOwner owner,
                            const bool is_mask_slot,
                            const AssetLibraryReference &new_ref)
{
  ImageGridUIState &state = ed::image_grid::image_grid_state_get(owner, is_mask_slot);

  const bool in_membership = state.filter.catalog_mode == ImageGridCatalogMode::Recent ||
                             state.filter.catalog_mode == ImageGridCatalogMode::Favorites;
  const bool same_lib = ed::asset::library_reference_to_enum_value(&new_ref) ==
                        ed::asset::library_reference_to_enum_value(&state.filter.lib_ref);
  if (same_lib && !in_membership) {
    return false;
  }

  const AssetLibraryReference old_lib_ref = state.filter.lib_ref;
  /* Same library while in Recent/Favorites: exit membership and restore that library's saved
   * catalog filter via #image_grid_catalog_swap_library (skips committing empty membership paths). */
  image_grid_catalog_swap_library(state, old_lib_ref, new_ref);
  image_grid_catalog_commit_active(state);
  ed::image_grid::image_grid_reset_scroll(owner, is_mask_slot);
  image_grid_pending_clear(state);

  ed::image_grid::image_grid_state_persist(owner, state, is_mask_slot);
  image_grid_prepare_browse_shelf(C, state, "VIEW3D_AST_image_texture");

  image_grid_notify_change(C);
  return true;
}

/** \} */

}  // namespace blender::ed::image_grid

namespace blender {

using namespace ed::image_grid;

static const EnumPropertyItem *rna_image_grid_library_itemf(bContext *C,
                                                            PointerRNA *ptr,
                                                            PropertyRNA *prop,
                                                            bool *r_free);

/* -------------------------------------------------------------------- */
/** \name Assign Brush Texture
 * \{ */

static Image *image_grid_resolve_image(bContext &C, wmOperator &op)
{
  Main *bmain = CTX_data_main(&C);

  if (RNA_struct_property_is_set(op.ptr, "image_session_uid")) {
    const uint32_t session_uid = uint32_t(RNA_int_get(op.ptr, "image_session_uid"));
    if (session_uid == MAIN_ID_SESSION_UID_UNSET) {
      return nullptr;
    }
    /* #BKE_libblock_find_session_uid only returns IDs present in `bmain`, so no extra
     * existence check is needed. */
    ID *id = BKE_libblock_find_session_uid(bmain, ID_IM, session_uid);
    return id_cast<Image *>(id);
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
  const AssetLibraryReference lib_ref = ed::asset::library_reference_from_enum_value(lib_enum);

  /* The clicked tile was drawn from this library's asset list, so the list is normally already
   * loaded. Avoid #storage_fetch_blocking()'s synchronous library scan on the UI thread for the
   * rare case it isn't: kick off a background fetch instead and let the user retry the click. */
  if (!ed::asset::list::is_loaded(&lib_ref)) {
    ed::asset::list::storage_fetch(&lib_ref, &C);
    BKE_report(op.reports, RPT_WARNING, "Asset library is still loading, try again shortly");
    return nullptr;
  }

  Image *found_image = nullptr;
  ed::asset::list::iterate(lib_ref, [&](asset_system::AssetRepresentation &asset) {
    if (asset.get_id_type() != ID_IM) {
      return true;
    }
    if (asset.library_relative_identifier() != identifier) {
      return true;
    }

    found_image = ed::asset::resolve_image_from_asset(*bmain, asset);
    return false;
  });

  /* All resolution paths in #ed::asset::resolve_image_from_asset return an #Image owned by
   * `bmain` (local, imported, or loaded from disk), so the result is already valid. */
  return found_image;
}

static wmOperatorStatus image_grid_assign_texture_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);

  const uint32_t brush_session_uid = uint32_t(RNA_int_get(op->ptr, "brush_session_uid"));
  if (brush_session_uid == MAIN_ID_SESSION_UID_UNSET) {
    return OPERATOR_CANCELLED;
  }
  Brush *brush = id_cast<Brush *>(
      BKE_libblock_find_session_uid(bmain, ID_BR, brush_session_uid));
  if (!brush) {
    return OPERATOR_CANCELLED;
  }

  Image *image = image_grid_resolve_image(*C, *op);
  if (!image) {
    return OPERATOR_CANCELLED;
  }

  /* Assigning a local texture to a linked asset brush creates a cross-domain reference the undo
   * system cannot remap, so localize the brush first. */
  Brush *target_brush = brush_ensure_local_for_texture(*C, *brush);

  const bool use_mask_slot = RNA_boolean_get(op->ptr, "use_mask_slot");
  if (!image_grid_assign_image_to_brush(*C, *target_brush, *image, use_mask_slot)) {
    return OPERATOR_CANCELLED;
  }

  /* Scroll the *other* grid layout (e.g. the N-Panel sidebar when clicking in the Texture popover)
   * to the freshly assigned asset, centered. */
  if (const std::optional<ed::image_grid::ImageGridOwner> owner_opt =
          ed::image_grid::image_grid_owner_from_context(*C))
  {
    const ed::image_grid::ImageGridOwner owner = *owner_opt;
    ImageGridUIState &state = ed::image_grid::image_grid_state_get(owner, use_mask_slot);
    char asset_identifier[FILE_MAX_LIBEXTRA];
    asset_identifier[0] = '\0';
    if (RNA_struct_property_is_set(op->ptr, "asset_identifier")) {
      RNA_string_get(op->ptr, "asset_identifier", asset_identifier);
    }
    const std::string focus_identifier = (asset_identifier[0] != '\0') ?
                                             std::string(asset_identifier) :
                                             std::string(image->id.name + 2);
    image_grid_request_scroll_to_asset(state, focus_identifier);

    /* The grid the user clicked in already shows the asset where they clicked, so keep its scroll
     * instead of re-centering (which would make it jump); other grids still center on it. Mark the
     * clicked grid's layout (keyed by column count) as already focused so its next build skips the
     * re-center. The column count comes from the clicked grid's session. The popover flag comes
     * from the grid view (an operator property), not #CTX_wm_region_popup: this runs from the
     * item-activation callback during button handling, where #region_popup is not yet set — a false
     * negative would mark the N-Panel's bucket applied and stop it scrolling to the new asset. */
    const bool is_popover = RNA_boolean_get(op->ptr, "is_popover");
    const int source_cols = max_ii(
        1,
        ui::grid_view_session_cols(
            ed::image_grid::image_grid_session_id(owner, use_mask_slot, is_popover)));
    image_grid_focus_mark_applied(state.viewport, source_cols, /*rows*/ 0);

    if (is_popover) {
      /* The Texture popover stays open across picks (#AbstractView::set_popup_keep_open in
       * #build_image_grid), so it must not be rebuilt mid-interaction: #image_grid_notify_change
       * does a #storage_fetch + #ED_region_tag_refresh_ui, and rebuilding an open popover on every
       * asset click is the Stage-4 failure mode. The #NC_BRUSH / #NC_ID notifiers already sent by
       * #image_grid_assign_image_to_brush schedule the redraw on which #image_grid_apply_focus_scroll
       * updates the other grids' scroll without a rebuild (same approach as the browse popover in
       * #image_shelf_activate_asset_exec). */
    }
    else {
      /* Refresh the UI (not just redraw) on all grids so build_image_grid re-runs and applies the
       * focus scroll. The NC_BRUSH / NC_ID notifiers sent above only tag a redraw. */
      image_grid_notify_change(*C, use_mask_slot);
    }
  }

  return OPERATOR_FINISHED;
}

void IMAGE_GRID_OT_assign_texture(wmOperatorType *ot)
{
  ot->name = "Assign Image Grid Texture";
  ot->description = "Assign an image asset as the brush texture";
  ot->idname = "IMAGE_GRID_OT_assign_texture";

  ot->exec = image_grid_assign_texture_exec;

  ot->flag = OPTYPE_REGISTER;

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

  prop = RNA_def_boolean(
      ot->srna,
      "is_popover",
      false,
      "From Popover",
      "The grid the item was clicked in is the Texture popover, not the N-Panel");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE | PROP_HIDDEN);

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

/**
 * Resolve the brush texture slot from operator properties (set when the asset tile is built) or,
 * as a fallback, from the `image_grid_target` context member.
 */
static bool image_shelf_activate_resolve_brush(bContext *C,
                                               wmOperator *op,
                                               Brush **r_brush,
                                               bool *r_use_mask_slot)
{
  Main *bmain = CTX_data_main(C);

  const uint32_t brush_session_uid = uint32_t(RNA_int_get(op->ptr, "brush_session_uid"));
  if (brush_session_uid != MAIN_ID_SESSION_UID_UNSET) {
    Brush *brush = id_cast<Brush *>(
        BKE_libblock_find_session_uid(bmain, ID_BR, brush_session_uid));
    if (!brush) {
      return false;
    }
    *r_brush = brush;
    *r_use_mask_slot = RNA_boolean_get(op->ptr, "use_mask_slot");
    return true;
  }

  const PointerRNA target_ptr = CTX_data_pointer_get(C, "image_grid_target");
  if (target_ptr.data && target_ptr.owner_id && GS(target_ptr.owner_id->name) == ID_BR) {
    Brush *brush = id_cast<Brush *>(
        BKE_libblock_find_session_uid(bmain, ID_BR, target_ptr.owner_id->session_uid));
    if (!brush) {
      return false;
    }
    *r_brush = brush;
    *r_use_mask_slot = image_grid_slot_is_mask(target_ptr);
    return true;
  }

  PointerRNA paint_target_ptr;
  if (image_grid_brush_target_pointer_get(*C, &paint_target_ptr, false)) {
    Brush *brush = id_cast<Brush *>(
        BKE_libblock_find_session_uid(bmain, ID_BR, paint_target_ptr.owner_id->session_uid));
    if (!brush) {
      return false;
    }
    *r_brush = brush;
    *r_use_mask_slot = false;
    return true;
  }
  if (image_grid_brush_target_pointer_get(*C, &paint_target_ptr, true)) {
    Brush *brush = id_cast<Brush *>(
        BKE_libblock_find_session_uid(bmain, ID_BR, paint_target_ptr.owner_id->session_uid));
    if (!brush) {
      return false;
    }
    *r_brush = brush;
    *r_use_mask_slot = true;
    return true;
  }

  BKE_report(op->reports, RPT_ERROR, "No brush target in context");
  return false;
}

static wmOperatorStatus image_shelf_activate_asset_exec(bContext *C, wmOperator *op)
{
  Brush *brush = nullptr;
  bool use_mask_slot = false;
  if (!image_shelf_activate_resolve_brush(C, op, &brush, &use_mask_slot)) {
    return OPERATOR_CANCELLED;
  }

  Main *bmain = CTX_data_main(C);

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
  AssetShelf *image_texture_shelf = nullptr;
  bool resolved_from_shelf = false;
  if (AssetShelfType *shelf_type = ed::asset::shelf::type_find_from_idname(
          "VIEW3D_AST_image_texture"))
  {
    image_texture_shelf = ed::asset::shelf::popup_shelf_get_or_create(*C, *shelf_type);
    if (image_texture_shelf) {
      /* Validated rather than read raw: this reference ends up in #ImageGridUIState::pending and
       * from there in the grid's filter, which fetches it. */
      shelf_lib_ref = ed::asset::shelf::settings_ensure_valid_library_ref(
          image_texture_shelf->settings);
      ed::asset::list::iterate(shelf_lib_ref, [&](asset_system::AssetRepresentation &a) {
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

  Image *image = ed::asset::resolve_image_from_asset(*bmain, *asset);
  if (!image) {
    BKE_report(op->reports, RPT_ERROR, "Could not load image asset");
    return OPERATOR_CANCELLED;
  }

  Brush *target_brush = brush_ensure_local_for_texture(*C, *brush);
  if (!image_grid_assign_image_to_brush(*C, *target_brush, *image, use_mask_slot)) {
    return OPERATOR_CANCELLED;
  }

  ed::asset::shelf::shelf_asset_lists_record_recent(IMAGE_TEXTURE_SHELF_IDNAME, weak_ref);

  if (resolved_from_shelf && image_texture_shelf) {
    if (const std::optional<ed::image_grid::ImageGridOwner> owner =
            ed::image_grid::image_grid_owner_from_context(*C))
    {
      ImageGridUIState &state = ed::image_grid::image_grid_state_get(*owner, use_mask_slot);
      const ImageGridCatalogMode shelf_mode = image_grid_shelf_catalog_mode(
          *image_texture_shelf);
      const std::optional<std::string> shelf_catalog_path = image_grid_catalog_path_from_shelf(
          *image_texture_shelf);
      image_grid_request_scroll_to_asset(state, asset->library_relative_identifier());
      if (image_grid_filter_matches_shelf(state, *image_texture_shelf)) {
        image_grid_pending_clear(state);
        /* Do not call image_grid_notify_change while the browse popover is open: it would
         * trigger a full grid rebuild (storage_fetch + refresh_ui) on every asset click.
         * NC_BRUSH and NC_ID notifiers sent above already schedule a redraw, on which
         * image_grid_apply_focus_scroll will update scroll_row without a rebuild. */
      }
      else {
        image_grid_pending_schedule_from_asset(state,
                                               shelf_lib_ref,
                                               shelf_catalog_path,
                                               asset->library_relative_identifier(),
                                               shelf_mode);
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
  ot->flag = OPTYPE_REGISTER;
  ed::asset::operator_asset_reference_props_register(*ot->srna);

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

  prop = RNA_def_boolean(ot->srna,
                         "use_mask_slot",
                         false,
                         "Mask Texture Slot",
                         "Assign to the mask texture slot instead of the main texture slot");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE | PROP_HIDDEN);
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
      /*include_separate_online_essentials=*/false,
      /*exclude_image_libraries=*/false,
      /*only_image_libraries=*/true);
  if (!items) {
    *r_free = false;
    return nullptr;
  }
  *r_free = true;
  return items;
}

static wmOperatorStatus image_grid_set_library_exec(bContext *C, wmOperator *op)
{
  const std::optional<ed::image_grid::ImageGridOwner> owner_opt =
      ed::image_grid::image_grid_owner_from_context(*C);
  if (!owner_opt) {
    return OPERATOR_CANCELLED;
  }

  const int enum_value = RNA_enum_get(op->ptr, "asset_library_reference");
  const AssetLibraryReference new_ref = ed::asset::library_reference_from_enum_value(enum_value);
  const bool is_mask_slot = image_grid_is_mask_slot_from_context(*C);

  if (!image_grid_set_library(*C, *owner_opt, is_mask_slot, new_ref)) {
    return OPERATOR_CANCELLED;
  }
  return OPERATOR_FINISHED;
}

void IMAGE_GRID_OT_set_library(wmOperatorType *ot)
{
  ot->name = "Set Image Grid Library";
  ot->description = "Set the asset library used by the image grid";
  ot->idname = "IMAGE_GRID_OT_set_library";

  ot->exec = image_grid_set_library_exec;

  ot->flag = OPTYPE_REGISTER;

  PropertyRNA *prop = RNA_def_property(ot->srna, "asset_library_reference", PROP_ENUM, PROP_NONE);
  RNA_def_enum_funcs(prop, rna_image_grid_library_itemf);
  RNA_def_property_ui_text(prop, "Library", "Asset library to browse");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Set Membership (Recent / Favorites)
 * \{ */

static const EnumPropertyItem image_grid_membership_items[] = {
    {int(ImageGridCatalogMode::Recent),
     "RECENT",
     ICON_RECOVER_LAST,
     "Recent",
     "Show recently used image assets"},
    {int(ImageGridCatalogMode::Favorites),
     "FAVORITES",
     ICON_SOLO_ON,
     "Favorites",
     "Show favorited image assets"},
    {0, nullptr, 0, nullptr, nullptr},
};

static wmOperatorStatus image_grid_set_membership_exec(bContext *C, wmOperator *op)
{
  const std::optional<ed::image_grid::ImageGridOwner> owner_opt =
      ed::image_grid::image_grid_owner_from_context(*C);
  if (!owner_opt) {
    return OPERATOR_CANCELLED;
  }

  const ImageGridCatalogMode mode = ImageGridCatalogMode(
      RNA_enum_get(op->ptr, "mode"));
  if (mode != ImageGridCatalogMode::Recent && mode != ImageGridCatalogMode::Favorites) {
    return OPERATOR_CANCELLED;
  }

  const bool is_mask_slot = image_grid_is_mask_slot_from_context(*C);
  const ed::image_grid::ImageGridOwner owner = *owner_opt;
  ImageGridUIState &state = ed::image_grid::image_grid_state_get(owner, is_mask_slot);
  if (state.filter.catalog_mode == mode) {
    return OPERATOR_CANCELLED;
  }

  image_grid_filter_set_membership(state, mode);
  ed::asset::list::storage_fetch(&state.filter.lib_ref, C);
  ed::image_grid::image_grid_reset_scroll(owner, is_mask_slot);
  image_grid_pending_clear(state);

  ed::image_grid::image_grid_state_persist(owner, state, is_mask_slot);
  image_grid_prepare_browse_shelf(*C, state, "VIEW3D_AST_image_texture");

  image_grid_notify_change(*C);
  return OPERATOR_FINISHED;
}

void IMAGE_GRID_OT_set_membership(wmOperatorType *ot)
{
  ot->name = "Set Image Grid Membership";
  ot->description = "Show Recent or Favorites image assets in the image grid";
  ot->idname = "IMAGE_GRID_OT_set_membership";

  ot->exec = image_grid_set_membership_exec;

  ot->flag = OPTYPE_REGISTER;

  PropertyRNA *prop = RNA_def_property(ot->srna, "mode", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, image_grid_membership_items);
  RNA_def_property_ui_text(prop, "Mode", "Recent or Favorites list to show");
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

  const std::optional<ed::image_grid::ImageGridOwner> owner =
      ed::image_grid::image_grid_owner_from_context(*C);
  if (owner) {
    ImageGridUIState &state = ed::image_grid::image_grid_state_get(
        *owner, image_grid_is_mask_slot_from_context(*C));
    if (!state.filter.enabled_catalog_paths.is_empty()) {
      asset_system::AssetLibrary *library = ed::asset::list::library_get_once_available(
          asset_system::current_file_library_reference());
      if (library) {
        /* Assign the asset to the first enabled catalog path. If multiple catalogs are
         * enabled, the first one encountered in set iteration is used. */
        const std::string &path_str = *state.filter.enabled_catalog_paths.begin();
        const asset_system::AssetCatalogPath cat_path =
            asset_system::AssetCatalogPath::from_user_input(path_str.c_str());
        const asset_system::AssetCatalog &catalog = ed::asset::library_ensure_catalogs_in_path(
            *library, cat_path);
        BKE_asset_metadata_catalog_id_set(
            id->asset_data, catalog.catalog_id, catalog.simple_name.c_str());
      }
    }
  }

  WM_event_add_notifier(C, NC_ASSET | NA_ADDED, nullptr);
  WM_event_add_notifier(C, NC_ID | NA_EDITED, nullptr);
  image_grid_notify_change(*C, image_grid_is_mask_slot_from_context(*C));

  return OPERATOR_FINISHED;
}

void IMAGE_GRID_OT_mark_asset(wmOperatorType *ot)
{
  ot->name = "Mark Image as Asset";
  ot->description = "Mark the image as an asset for use in the image grid";
  ot->idname = "IMAGE_GRID_OT_mark_asset";

  ot->exec = image_grid_mark_asset_exec;
  ot->poll = image_grid_mark_asset_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name New Texture / Open Image (Brush Texture Grid)
 * \{ */

struct ImageGridBrushTarget {
  PointerRNA target_ptr;
};

static bool image_grid_brush_target_poll(bContext *C)
{
  if (!ed::image_grid::image_grid_owner_from_context(*C)) {
    return false;
  }
  const PointerRNA target_ptr = CTX_data_pointer_get(C, "image_grid_target");
  if (!target_ptr.data || !target_ptr.owner_id || GS(target_ptr.owner_id->name) != ID_BR) {
    CTX_wm_operator_poll_msg_set(C, "No brush texture slot in context");
    return false;
  }
  Main *bmain = CTX_data_main(C);
  if (!BKE_libblock_find_session_uid(bmain, ID_BR, target_ptr.owner_id->session_uid)) {
    CTX_wm_operator_poll_msg_set(C, "Brush not found");
    return false;
  }
  return true;
}

static bool image_grid_brush_target_resolve(bContext *C,
                                            wmOperator *op,
                                            PointerRNA *r_target_ptr,
                                            Brush **r_brush)
{
  PointerRNA target_ptr = CTX_data_pointer_get(C, "image_grid_target");
  if (!target_ptr.data && op != nullptr && op->customdata != nullptr) {
    target_ptr = static_cast<ImageGridBrushTarget *>(op->customdata)->target_ptr;
  }
  if (!target_ptr.data || !target_ptr.owner_id || GS(target_ptr.owner_id->name) != ID_BR) {
    return false;
  }
  Main *bmain = CTX_data_main(C);
  Brush *brush = id_cast<Brush *>(
      BKE_libblock_find_session_uid(bmain, ID_BR, target_ptr.owner_id->session_uid));
  if (!brush) {
    return false;
  }
  *r_target_ptr = target_ptr;
  *r_brush = brush;
  return true;
}

static bool image_grid_assign_target_image(bContext &C, Image &image, wmOperator *op)
{
  PointerRNA target_ptr;
  Brush *brush = nullptr;
  if (!image_grid_brush_target_resolve(&C, op, &target_ptr, &brush)) {
    return false;
  }
  return image_grid_assign_image_to_slot(C, target_ptr, *brush, image);
}

static void image_grid_open_cancel(bContext * /*C*/, wmOperator *op)
{
  if (op->customdata) {
    MEM_delete(static_cast<ImageGridBrushTarget *>(op->customdata));
    op->customdata = nullptr;
  }
}

static Image *image_grid_load_first_image(Main *bmain, wmOperator *op)
{
  const bool use_udim = RNA_boolean_get(op->ptr, "use_udim_detecting");
  const StringRefNull blendfile_path = BKE_main_blendfile_path(bmain);
  ListBaseT<ImageFrameRange> ranges = ED_image_filesel_detect_sequences(
      blendfile_path, blendfile_path, op, use_udim);

  Image *result = nullptr;
  for (ImageFrameRange &range : ranges) {
    if (!result) {
      bool exists = false;
      Image *ima = BKE_image_load_exists_in_lib(bmain, nullptr, range.filepath, &exists);
      if (!ima) {
        BKE_reportf(op->reports, RPT_ERROR, "Cannot read '%s'", range.filepath);
      }
      else {
        /* #BKE_image_load_exists_in_lib takes a loan on the user count regardless of whether the
         * block was newly created or already existed; the sole consumer
         * (#image_grid_assign_target_image, via #brush_texture_for_image) always counts the real
         * reference with #id_us_plus, so release the loan here unconditionally. */
        id_us_min(&ima->id);
        if (exists) {
          STRNCPY(ima->filepath, range.filepath);
        }
        else if (ima->source == IMA_SRC_FILE) {
          if (range.udims_detected && range.udim_tiles.first) {
            ima->source = IMA_SRC_TILED;
            ImageTile *first_tile = static_cast<ImageTile *>(ima->tiles.first);
            first_tile->tile_number = range.offset;
            for (LinkData &node : range.udim_tiles) {
              BKE_image_add_tile(ima, POINTER_AS_INT(node.data), nullptr);
            }
          }
          else if (range.length > 1) {
            ima->source = IMA_SRC_SEQUENCE;
          }
        }
        result = ima;
      }
    }
    range.frames.free_no_destruct();
    range.udim_tiles.free_no_destruct();
  }
  ranges.free_no_destruct();
  return result;
}

static wmOperatorStatus image_grid_new_exec(bContext *C, wmOperator *op)
{
  PointerRNA target_ptr;
  Brush *brush = nullptr;
  if (!image_grid_brush_target_resolve(C, op, &target_ptr, &brush)) {
    return OPERATOR_CANCELLED;
  }

  PropertyRNA *tex_prop = RNA_struct_find_property(&target_ptr, "texture");
  if (!tex_prop || RNA_property_type(tex_prop) != PROP_POINTER) {
    return OPERATOR_CANCELLED;
  }

  Main *bmain = CTX_data_main(C);
  Tex *tex = static_cast<Tex *>(RNA_property_pointer_get(&target_ptr, tex_prop).data);

  /* Same behavior as #TEXTURE_OT_new. */
  if (tex) {
    tex = id_cast<Tex *>(BKE_id_copy(bmain, &tex->id));
  }
  else {
    tex = BKE_texture_add(bmain, DATA_("Texture"));
  }

  id_us_min(&tex->id);

  if (target_ptr.owner_id) {
    BKE_id_move_to_same_lib(*bmain, tex->id, *target_ptr.owner_id);
  }

  PointerRNA idptr = RNA_id_pointer_create(&tex->id);
  RNA_property_pointer_set(&target_ptr, tex_prop, idptr, nullptr);
  RNA_property_update(C, &target_ptr, tex_prop);

  BKE_brush_tag_unsaved_changes(brush);

  const bool use_mask_slot = image_grid_slot_is_mask(target_ptr);
  if (const std::optional<ed::image_grid::ImageGridOwner> owner_opt =
          ed::image_grid::image_grid_owner_from_context(*C))
  {
    if (tex->type == TEX_IMAGE && tex->ima) {
      image_grid_after_image_opened(*C, *owner_opt, *tex->ima, use_mask_slot);
    }
    else {
      const ed::image_grid::ImageGridOwner owner = *owner_opt;
      ImageGridUIState &state = ed::image_grid::image_grid_state_get(owner, use_mask_slot);
      const AssetLibraryReference local_ref = asset_system::current_file_library_reference();
      if (state.filter.lib_ref.type != ASSET_LIBRARY_LOCAL) {
        const AssetLibraryReference old_lib_ref = state.filter.lib_ref;
        image_grid_catalog_swap_library(state, old_lib_ref, local_ref);
        state.filter.lib_ref = local_ref;
        image_grid_catalog_commit_active(state);
      }
      ed::image_grid::image_grid_reset_scroll(owner, use_mask_slot);
      image_grid_pending_clear(state);
      ed::image_grid::image_grid_state_persist(owner, state, use_mask_slot);
      ed::asset::list::storage_fetch(&local_ref, C);
      image_grid_prepare_browse_shelf(*C, state, "VIEW3D_AST_image_texture");
      image_grid_notify_change(*C, use_mask_slot);
    }
  }

  WM_event_add_notifier(C, NC_TEXTURE | NA_ADDED, tex);
  WM_event_add_notifier(C, NC_BRUSH, brush);

  return OPERATOR_FINISHED;
}

void IMAGE_GRID_OT_new(wmOperatorType *ot)
{
  ot->name = "New Texture for Brush";
  ot->description =
      "Add a new texture to the brush slot (same as texture.new), switching the grid to the "
      "current file";
  ot->idname = "IMAGE_GRID_OT_new";

  ot->exec = image_grid_new_exec;
  ot->poll = image_grid_brush_target_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static wmOperatorStatus image_grid_open_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  Image *ima = image_grid_load_first_image(bmain, op);
  if (!ima) {
    return OPERATOR_CANCELLED;
  }

  ED_preview_kill_jobs(CTX_wm_manager(C), bmain);
  BKE_image_signal(bmain, ima, nullptr, IMA_SIGNAL_RELOAD);
  WM_event_add_notifier(C, NC_IMAGE | NA_EDITED, ima);

  if (!image_grid_assign_target_image(*C, *ima, op)) {
    image_grid_open_cancel(C, op);
    return OPERATOR_CANCELLED;
  }

  image_grid_open_cancel(C, op);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus image_grid_open_invoke(bContext *C,
                                               wmOperator *op,
                                               const wmEvent * /*event*/)
{
  if (!image_grid_brush_target_poll(C)) {
    return OPERATOR_CANCELLED;
  }

  ImageGridBrushTarget *target = MEM_new<ImageGridBrushTarget>(__func__);
  target->target_ptr = CTX_data_pointer_get(C, "image_grid_target");
  op->customdata = target;

  if (RNA_struct_property_is_set(op->ptr, "filepath")) {
    return image_grid_open_exec(C, op);
  }

  RNA_string_set(op->ptr, "filepath", U.textudir);
  WM_event_add_fileselect(C, op);
  return OPERATOR_RUNNING_MODAL;
}

void IMAGE_GRID_OT_open(wmOperatorType *ot)
{
  ot->name = "Open Image for Brush Texture";
  ot->description =
      "Open an image from disk, assign it to the brush texture slot, and show it in the grid";
  ot->idname = "IMAGE_GRID_OT_open";

  ot->exec = image_grid_open_exec;
  ot->invoke = image_grid_open_invoke;
  ot->cancel = image_grid_open_cancel;
  ot->poll = image_grid_brush_target_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  PropertyRNA *prop = RNA_def_boolean(
      ot->srna, "allow_path_tokens", true, "", "Allow the path to contain substitution tokens");
  RNA_def_property_flag(prop, PROP_HIDDEN);

  WM_operator_properties_filesel(ot,
                                 FILE_TYPE_FOLDER | FILE_TYPE_IMAGE | FILE_TYPE_MOVIE,
                                 FILE_SPECIAL,
                                 FILE_OPENFILE,
                                 WM_FILESEL_FILEPATH | WM_FILESEL_DIRECTORY | WM_FILESEL_FILES |
                                     WM_FILESEL_RELPATH,
                                 FILE_DEFAULTDISPLAY,
                                 FILE_SORT_DEFAULT);

  RNA_def_boolean(
      ot->srna,
      "use_sequence_detection",
      true,
      "Detect Sequences",
      "Automatically detect animated sequences in selected images (based on file names)");
  RNA_def_boolean(ot->srna,
                  "use_udim_detecting",
                  true,
                  "Detect UDIMs",
                  "Detect selected UDIM files and load all matching tiles");
}

/** \} */

}  // namespace blender
