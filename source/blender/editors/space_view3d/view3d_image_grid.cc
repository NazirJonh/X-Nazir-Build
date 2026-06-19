/* SPDX-FileCopyrightText: 2026 Blender Authors
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

#include "UI_interface.hh"
#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"
#include "UI_tree_view.hh"

#include "WM_api.hh"
#include "WM_toolsystem.hh"
#include "WM_types.hh"

#include "view3d_intern.hh"

namespace blender::ed::view3d {

void image_grid_notify_change(bContext &C, const bool is_mask_slot)
{
  View3D *v3d = CTX_wm_view3d(&C);
  if (!v3d) {
    return;
  }

  ImageGridUIState &state = image_grid_state_get(*v3d, is_mask_slot);
  ed::asset::list::storage_fetch(&state.filter.lib_ref, &C);
  WM_event_add_notifier(&C, NC_ASSET | ND_ASSET_LIST, nullptr);
  WM_event_add_notifier(&C, NC_ID | NA_EDITED, nullptr);

  ARegion *region = CTX_wm_region(&C);
  if (region) {
    ED_region_tag_redraw(region);
    ED_region_tag_refresh_ui(region);
  }
}

int image_grid_effective_rows(const View3D &v3d, const bool is_mask_slot)
{
  const int stored = is_mask_slot ? v3d.image_grid_mask_rows : v3d.image_grid_rows;
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

int image_grid_max_scroll_row(const ImageGridUIState &state,
                              const View3D &v3d,
                              const bool is_mask_slot)
{
  const int effective_rows = image_grid_effective_rows(v3d, is_mask_slot);
  const int total_rows = (state.viewport.cached_cols > 0) ?
                             int(ceil(float(state.viewport.cached_item_count) /
                                      float(state.viewport.cached_cols))) :
                             effective_rows;
  return max_ii(0, total_rows - effective_rows);
}

void image_grid_clamp_scroll_row(ImageGridUIState &state,
                                 const View3D &v3d,
                                 const bool is_mask_slot)
{
  state.viewport.scroll_row = clamp_i(
      state.viewport.scroll_row, 0, image_grid_max_scroll_row(state, v3d, is_mask_slot));
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

static bool image_grid_scroll_under_mouse(const ARegion *region,
                                        const int xy[2],
                                        const View3D &v3d,
                                        bool *r_is_mask_slot)
{
  if (ui::region_scroll_button_under_mouse(
          region, xy, &image_grid_state_get(v3d, false).viewport.scroll_row))
  {
    *r_is_mask_slot = false;
    return true;
  }
  if (ui::region_scroll_button_under_mouse(
          region, xy, &image_grid_state_get(v3d, true).viewport.scroll_row))
  {
    *r_is_mask_slot = true;
    return true;
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

bool image_grid_wheel_poll(bContext *C, const wmEvent *event, ARegion *region)
{
  if (!ELEM(event->type, WHEELUPMOUSE, WHEELDOWNMOUSE) || event->modifier) {
    return false;
  }
  View3D *v3d = CTX_wm_view3d(C);
  if (!v3d || !region) {
    return false;
  }
  bool is_mask_slot = false;
  if (!image_grid_mouse_over(region, event->xy, &is_mask_slot) &&
      !image_grid_scroll_under_mouse(region, event->xy, *v3d, &is_mask_slot))
  {
    return false;
  }
  ImageGridUIState &state = image_grid_state_get(*v3d, is_mask_slot);
  return image_grid_max_scroll_row(state, *v3d, is_mask_slot) > 0;
}

int handle_image_grid_wheel_event(bContext *C, const wmEvent *event, ARegion *region)
{
  if (!image_grid_wheel_poll(C, event, region)) {
    return WM_UI_HANDLER_CONTINUE;
  }
  wmOperatorType *ot = WM_operatortype_find("VIEW3D_OT_image_grid_scroll", true);
  if (!ot) {
    return WM_UI_HANDLER_CONTINUE;
  }
  bool is_mask_slot = false;
  if (!image_grid_mouse_over(region, event->xy, &is_mask_slot)) {
    image_grid_scroll_under_mouse(region, event->xy, *CTX_wm_view3d(C), &is_mask_slot);
  }
  PointerRNA op_ptr = WM_operator_properties_create_ptr(ot);
  const int delta = (event->type == WHEELUPMOUSE) ? -1 : 1;
  RNA_int_set(&op_ptr, "delta", delta);
  RNA_boolean_set(&op_ptr, "use_mask_slot", is_mask_slot);
  WM_operator_name_call_ptr(C, ot, wm::OpCallContext::ExecDefault, &op_ptr, nullptr);
  WM_operator_properties_free(&op_ptr);
  /* Popovers use a temporary region; CTX_wm_region() still points at the opener (e.g. tool header). */
  ED_region_tag_redraw(region);
  ED_region_tag_refresh_ui(region);
  return WM_UI_HANDLER_BREAK;
}

/* -------------------------------------------------------------------- */
/** \name Drag Scroll (pen / tablet LMB drag)
 * \{ */

int handle_image_grid_drag_scroll_event(bContext *C, const wmEvent *event, ARegion *region)
{
  View3D *v3d = CTX_wm_view3d(C);
  if (!v3d || !region) {
    return WM_UI_HANDLER_CONTINUE;
  }

  /* One drag gesture per View3D; state lives in #View3D_Runtime (see #image_grid_drag_scroll_state)
   * rather than a process-global, so independent viewports never share it. */
  ImageGridDragScrollState &drag = image_grid_drag_scroll_state(*v3d);

  if (event->type == LEFTMOUSE && event->val == KM_PRESS) {
    /* Reset on any new LMB press so a missed release never leaves stale state. */
    drag = {};
    bool is_mask = false;
    if (image_grid_mouse_over(region, event->xy, &is_mask)) {
      const ImageGridUIState &state = image_grid_state_get(*v3d, is_mask);
      if (image_grid_max_scroll_row(state, *v3d, is_mask) > 0) {
        drag.active = true;
        drag.is_mask_slot = is_mask;
        drag.start_y = event->xy[1];
        drag.last_y = event->xy[1];
      }
    }
    /* Always continue so grid items still receive the press for click-selection. */
    return WM_UI_HANDLER_CONTINUE;
  }

  if (event->type == LEFTMOUSE && event->val == KM_RELEASE) {
    if (drag.active) {
      const bool was_dragging = drag.dragging;
      drag = {};
      /* Consume the release after a drag to prevent item activation. */
      return was_dragging ? WM_UI_HANDLER_BREAK : WM_UI_HANDLER_CONTINUE;
    }
    return WM_UI_HANDLER_CONTINUE;
  }

  if (event->type == MOUSEMOVE && drag.active) {
    const int dy = event->xy[1] - drag.last_y;
    drag.last_y = event->xy[1];

    if (!drag.dragging) {
      /* Enter drag mode once the cursor travels more than 8 px from the press origin. */
      if (abs(event->xy[1] - drag.start_y) >= 8) {
        drag.dragging = true;
      }
    }

    if (drag.dragging) {
      /* Phone UX: drag up (dy > 0 in Blender Y-up coords) → content scrolls up → later rows
       * appear. Scroll by sub-row pixel amounts for smooth motion: accumulate into a combined
       * pixel position and split it back into whole rows plus a sub-row offset. */
      ImageGridUIState &state = image_grid_state_get(*v3d, drag.is_mask_slot);
      const int preview_size = image_grid_preview_size_get(*v3d);
      const int tile_h = max_ii(1, ui::preview_tile_size_y_no_label(preview_size));
      const int max_scroll_px = image_grid_max_scroll_row(state, *v3d, drag.is_mask_slot) * tile_h;

      int total_px = state.viewport.scroll_row * tile_h + state.viewport.scroll_offset_px;
      total_px = clamp_i(total_px + dy, 0, max_scroll_px);
      state.viewport.scroll_row = total_px / tile_h;
      state.viewport.scroll_offset_px = total_px % tile_h;

      /* User dragged manually — dismiss pending focus so the grid does not snap back. */
      image_grid_focus_clear(state.viewport);
      const int drag_rows = clamp_i(
          int(divide_ceil_u(uint(state.viewport.grip_pixel_height), uint(tile_h))), 1, 16);
      image_grid_viewport_store_scroll_for_layout(
          state.viewport,
          clamp_i(state.viewport.cached_cols > 0 ? state.viewport.cached_cols : 1, 1, 16),
          drag_rows);

      ED_region_tag_redraw(region);
      ED_region_tag_refresh_ui(region);
      return WM_UI_HANDLER_BREAK;
    }
  }

  return WM_UI_HANDLER_CONTINUE;
}

/** \} */

}  // namespace blender::ed::view3d

namespace blender::ed::view3d {

/**
 * Image paint expects a plain image texture (#TEX_IMAGE without a node tree or color ramp).
 * Reusing other blocks from the file can crash in #ntreeTexBeginExecTree or #BKE_colorband_evaluate.
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
    Tex *tex = reinterpret_cast<Tex *>(id);
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
 * IDs (#read_undo_remap_noundo_data), so a linked brush referencing a local #Tex would dangle after
 * an undo step (and the reference could not be saved into the .blend either). Make the brush local
 * before assignment so both sides of the reference share the undo domain, then re-activate the
 * now-local brush as the paint's active brush.
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
  Brush *local_brush = reinterpret_cast<Brush *>(bke::asset_edit_id_ensure_local(bmain, brush.id));
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

/* -------------------------------------------------------------------- */
/** \name Asset→Image Resolution and Brush Assignment Helpers
 * \{ */

/**
 * Resolve an asset representation to a loaded #Image using the canonical three-step order:
 * 1. Already-loaded local ID.
 * 2. Import from a .blend library via #asset_local_id_ensure_imported.
 * 3. Load from disk for direct-file assets stored outside a .blend library.
 *
 * Returns nullptr when none of the steps succeeds.
 */
static Image *image_grid_resolve_image_from_asset(Main &bmain,
                                                  const asset_system::AssetRepresentation &asset)
{
  if (ID *local_id = asset.local_id()) {
    return id_cast<Image *>(local_id);
  }
  if (!asset.full_library_path().empty()) {
    ID *imported_id = ed::asset::asset_local_id_ensure_imported(bmain, asset);
    if (imported_id && GS(imported_id->name) == ID_IM) {
      return id_cast<Image *>(imported_id);
    }
    return nullptr;
  }
  return BKE_image_load_exists(&bmain, asset.full_path().c_str());
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

  /* Assigning a local image texture makes the brush a local datablock (in place, keeping its name).
   * Update the tool brush bindings to reference this local brush, otherwise a later
   * #WM_toolsystem_refresh_active (run from #ed_undo_step_post on every undo) re-resolves the active
   * brush from the unchanged asset weak-reference and re-links a fresh, texture-less copy of the
   * source asset, silently dropping the texture. */
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

/** \} */

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
    return reinterpret_cast<Image *>(id);
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

  Image *found_image = nullptr;
  ed::asset::list::storage_fetch(&lib_ref, &C);
  ed::asset::list::iterate(lib_ref, [&](asset_system::AssetRepresentation &asset) {
    if (asset.get_id_type() != ID_IM) {
      return true;
    }
    if (asset.library_relative_identifier() != identifier) {
      return true;
    }

    found_image = image_grid_resolve_image_from_asset(*bmain, asset);
    return false;
  });

  /* All resolution paths in #image_grid_resolve_image_from_asset return an #Image owned by
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
  Brush *brush = reinterpret_cast<Brush *>(
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
  if (View3D *v3d = CTX_wm_view3d(C)) {
    ImageGridUIState &state = image_grid_state_get(*v3d, use_mask_slot);
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
     * instead of re-centering (which would make it jump). Other grids still center on the asset.
     * The clicked grid was the most recently drawn one, so #cached_cols holds its column count and
     * #grip_pixel_height its visible height. */
    const int preview_size = image_grid_preview_size_get(*v3d);
    const int tile_h = max_ii(1, ui::preview_tile_size_y_no_label(preview_size));
    const int source_rows = clamp_i(
        int(divide_ceil_u(uint(state.viewport.grip_pixel_height), uint(tile_h))), 1, 16);
    const int source_cols = max_ii(1, state.viewport.cached_cols);
    image_grid_focus_mark_applied(state.viewport, source_cols, source_rows);

    /* Refresh the UI (not just redraw) on all grids so build_image_grid re-runs and applies the
     * focus scroll. The NC_BRUSH / NC_ID notifiers sent above only tag a redraw. */
    image_grid_notify_change(*C, use_mask_slot);
  }

  return OPERATOR_FINISHED;
}

void VIEW3D_OT_image_grid_assign_texture(wmOperatorType *ot)
{
  ot->name = "Assign Image Grid Texture";
  ot->description = "Assign an image asset as the brush texture";
  ot->idname = "VIEW3D_OT_image_grid_assign_texture";

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
      shelf_lib_ref = image_texture_shelf->settings.asset_library_reference;
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

  Image *image = image_grid_resolve_image_from_asset(*bmain, *asset);
  if (!image) {
    BKE_report(op->reports, RPT_ERROR, "Could not load image asset");
    return OPERATOR_CANCELLED;
  }

  Brush *target_brush = brush_ensure_local_for_texture(*C, *brush);
  if (!image_grid_assign_image_to_brush(*C, *target_brush, *image, use_mask_slot)) {
    return OPERATOR_CANCELLED;
  }

  if (resolved_from_shelf && image_texture_shelf) {
    View3D *v3d = CTX_wm_view3d(C);
    if (v3d) {
      ImageGridUIState &state = image_grid_state_get(*v3d, use_mask_slot);
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
        image_grid_pending_schedule_from_asset(
            state, shelf_lib_ref, shelf_catalog_path, asset->library_relative_identifier());
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

  const bool is_mask_slot = image_grid_is_mask_slot_from_context(*C);
  ImageGridUIState &state = image_grid_state_get(*v3d, is_mask_slot);
  if (path_buf[0] == '\0') {
    /* Empty path means "show all" — clear the filter set. */
    state.filter.enabled_catalog_paths.clear();
  }
  else {
    /* Toggle the path in the enabled set. */
    const std::string path(path_buf);
    if (!state.filter.enabled_catalog_paths.remove(path)) {
      state.filter.enabled_catalog_paths.add(path);
    }
  }
  image_grid_catalog_commit_active(state);
  state.viewport.scroll_row = 0;
  image_grid_pending_clear(state);

  image_grid_state_persist_to_view3d(*v3d, state, is_mask_slot);
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
  const AssetLibraryReference new_ref = ed::asset::library_reference_from_enum_value(enum_value);

  const bool is_mask_slot = image_grid_is_mask_slot_from_context(*C);
  ImageGridUIState &state = image_grid_state_get(*v3d, is_mask_slot);
  if (enum_value == ed::asset::library_reference_to_enum_value(&state.filter.lib_ref)) {
    return OPERATOR_CANCELLED;
  }

  const AssetLibraryReference old_lib_ref = state.filter.lib_ref;
  image_grid_catalog_swap_library(state, old_lib_ref, new_ref);
  image_grid_catalog_commit_active(state);
  state.viewport.scroll_row = 0;
  image_grid_pending_clear(state);

  image_grid_state_persist_to_view3d(*v3d, state, is_mask_slot);
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

  WM_event_add_notifier(C, NC_ASSET | NA_ADDED, nullptr);
  WM_event_add_notifier(C, NC_ID | NA_EDITED, nullptr);
  image_grid_notify_change(*C, image_grid_is_mask_slot_from_context(*C));

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
/** \name New Texture / Open Image (Brush Texture Grid)
 * \{ */

struct ImageGridBrushTarget {
  PointerRNA target_ptr;
};

static bool image_grid_brush_target_poll(bContext *C)
{
  if (!CTX_wm_view3d(C)) {
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

static void image_grid_after_image_opened(bContext &C,
                                          View3D &v3d,
                                          Image &image,
                                          const bool is_mask_slot)
{
  ImageGridUIState &state = image_grid_state_get(v3d, is_mask_slot);
  const AssetLibraryReference local_ref = asset_system::current_file_library_reference();

  const AssetLibraryReference old_lib_ref = state.filter.lib_ref;
  if (state.filter.lib_ref.type != ASSET_LIBRARY_LOCAL) {
    image_grid_catalog_swap_library(state, old_lib_ref, local_ref);
    state.filter.lib_ref = local_ref;
    image_grid_catalog_commit_active(state);
  }

  state.viewport.scroll_row = 0;
  image_grid_request_scroll_to_asset(state, image.id.name + 2);
  image_grid_pending_clear(state);

  image_grid_state_persist_to_view3d(v3d, state, is_mask_slot);
  ed::asset::list::storage_fetch(&local_ref, &C);
  image_grid_prepare_browse_shelf(C, state, "VIEW3D_AST_image_texture");
  image_grid_notify_change(C, is_mask_slot);
}

static bool image_grid_assign_target_image(bContext &C, Image &image, wmOperator *op)
{
  PointerRNA target_ptr;
  Brush *brush = nullptr;
  if (!image_grid_brush_target_resolve(&C, op, &target_ptr, &brush)) {
    return false;
  }
  const bool use_mask_slot = image_grid_slot_is_mask(target_ptr);
  Brush *target_brush = brush_ensure_local_for_texture(C, *brush);
  if (!image_grid_assign_image_to_brush(C, *target_brush, image, use_mask_slot)) {
    return false;
  }
  View3D *v3d = CTX_wm_view3d(&C);
  if (v3d) {
    image_grid_after_image_opened(C, *v3d, image, use_mask_slot);
  }
  return true;
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

  bool linked_id_created = false;
  id_us_min(&tex->id);

  if (target_ptr.owner_id) {
    BKE_id_move_to_same_lib(*bmain, tex->id, *target_ptr.owner_id);
    linked_id_created = ID_IS_LINKED(&tex->id);
  }

  PointerRNA idptr = RNA_id_pointer_create(&tex->id);
  RNA_property_pointer_set(&target_ptr, tex_prop, idptr, nullptr);
  RNA_property_update(C, &target_ptr, tex_prop);

  BKE_brush_tag_unsaved_changes(brush);

  const bool use_mask_slot = image_grid_slot_is_mask(target_ptr);
  View3D *v3d = CTX_wm_view3d(C);
  if (v3d) {
    if (tex->type == TEX_IMAGE && tex->ima) {
      image_grid_after_image_opened(*C, *v3d, *tex->ima, use_mask_slot);
    }
    else {
      ImageGridUIState &state = image_grid_state_get(*v3d, use_mask_slot);
      const AssetLibraryReference local_ref = asset_system::current_file_library_reference();
      if (state.filter.lib_ref.type != ASSET_LIBRARY_LOCAL) {
        const AssetLibraryReference old_lib_ref = state.filter.lib_ref;
        image_grid_catalog_swap_library(state, old_lib_ref, local_ref);
        state.filter.lib_ref = local_ref;
        image_grid_catalog_commit_active(state);
      }
      state.viewport.scroll_row = 0;
      image_grid_pending_clear(state);
      image_grid_state_persist_to_view3d(*v3d, state, use_mask_slot);
      ed::asset::list::storage_fetch(&local_ref, C);
      image_grid_prepare_browse_shelf(*C, state, "VIEW3D_AST_image_texture");
      image_grid_notify_change(*C, use_mask_slot);
    }
  }

  if (!linked_id_created) {
    ED_undo_push_op(C, op);
  }

  WM_event_add_notifier(C, NC_TEXTURE | NA_ADDED, tex);
  WM_event_add_notifier(C, NC_BRUSH, brush);

  return OPERATOR_FINISHED;
}

void VIEW3D_OT_image_grid_new(wmOperatorType *ot)
{
  ot->name = "New Texture for Brush";
  ot->description =
      "Add a new texture to the brush slot (same as texture.new), switching the grid to the "
      "current file";
  ot->idname = "VIEW3D_OT_image_grid_new";

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
  ED_undo_push_op(C, op);
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

void VIEW3D_OT_image_grid_open(wmOperatorType *ot)
{
  ot->name = "Open Image for Brush Texture";
  ot->description =
      "Open an image from disk, assign it to the brush texture slot, and show it in the grid";
  ot->idname = "VIEW3D_OT_image_grid_open";

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

void VIEW3D_OT_image_grid_browse_assets(wmOperatorType *ot)
{
  ot->name = "Browse Image Assets";
  ot->description = "Open or focus the Asset Browser filtered to images";
  ot->idname = "VIEW3D_OT_image_grid_browse_assets";

  ot->exec = image_grid_browse_assets_exec;

  ot->flag = OPTYPE_REGISTER;

  PropertyRNA *prop = RNA_def_property(ot->srna, "asset_library_reference", PROP_ENUM, PROP_NONE);
  RNA_def_enum_funcs(prop, rna_image_grid_library_itemf);
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
  const bool is_mask_slot = RNA_boolean_get(op->ptr, "use_mask_slot");
  ImageGridUIState &state = image_grid_state_get(*v3d, is_mask_slot);
  const int delta = RNA_int_get(op->ptr, "delta");
  image_grid_clamp_scroll_row(state, *v3d, is_mask_slot);
  state.viewport.scroll_row = clamp_i(
      state.viewport.scroll_row + delta, 0, image_grid_max_scroll_row(state, *v3d, is_mask_slot));
  /* Mouse-wheel scrolls in whole rows; align the sub-row offset to the row boundary. */
  state.viewport.scroll_offset_px = 0;
  /* User scrolled manually — dismiss any pending focus-to-asset request so the grid does not
   * snap back to the previously selected asset on the next redraw. */
  image_grid_focus_clear(state.viewport);
  const int preview_size = image_grid_preview_size_get(*v3d);
  const int tile_h = max_ii(1, ui::preview_tile_size_y_no_label(preview_size));
  const int scroll_rows = clamp_i(
      int(divide_ceil_u(uint(state.viewport.grip_pixel_height), uint(tile_h))), 1, 16);
  image_grid_viewport_store_scroll_for_layout(
      state.viewport,
      clamp_i(state.viewport.cached_cols > 0 ? state.viewport.cached_cols : 1, 1, 16),
      scroll_rows);

  ARegion *region = CTX_wm_region_popup(C);
  if (!region) {
    region = CTX_wm_region(C);
  }
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
  RNA_def_boolean(ot->srna,
                  "use_mask_slot",
                  false,
                  "Mask Texture Slot",
                  "Scroll the mask texture grid instead of the main texture grid");
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
  const ImageGridUIState &state = image_grid_state_get(*v3d, false);
  return state.filter.lib_ref.type != ASSET_LIBRARY_LOCAL;
}

static wmOperatorStatus image_grid_refresh_library_exec(bContext *C, wmOperator * /*op*/)
{
  View3D *v3d = CTX_wm_view3d(C);
  if (!v3d) {
    return OPERATOR_CANCELLED;
  }

  ImageGridUIState &state = image_grid_state_get(*v3d, false);

  if (state.filter.lib_ref.type == ASSET_LIBRARY_CUSTOM) {
    const bUserAssetLibrary *user_lib = BKE_preferences_asset_library_find_index(
        &U, state.filter.lib_ref.custom_library_index);
    if (user_lib && !(user_lib->flag & ASSET_LIBRARY_USE_REMOTE_URL) && user_lib->dirpath[0]) {
      ed::asset::image_library_scan_and_index(user_lib->dirpath);
      ed::asset::image_library_invalidate_cached_previews(user_lib->dirpath);
    }
  }

  ed::asset::list::clear(&state.filter.lib_ref, C);
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

}  // namespace blender
