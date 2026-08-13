/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Texture drop functionality for UI elements.
 * Handles drag and drop operations for textures and images onto UI buttons.
 */

#include <cstdlib>
#include <memory>
#include <optional>

#include "DNA_ID.h"
#include "DNA_brush_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_texture_types.h"
#include "DNA_userdef_types.h"
#include "DNA_windowmanager_types.h"

#include "MEM_guardedalloc.h"

#include "BLI_fileops.hh"
#include "BLI_listbase.h"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_path_utils.hh"
#include "BLI_rect.h"
#include "BLI_string.h"
#include "BLI_string_ref.hh"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "BLT_translation.hh"

#include "BKE_brush.hh"
#include "BKE_context.hh"
#include "BKE_icons.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_main_invariants.hh"
#include "BKE_paint.hh"
#include "BKE_report.hh"
#include "BKE_screen.hh"
#include "BKE_texture.h"

#include "AS_asset_library.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"

#include "ED_asset_import.hh"
#include "ED_asset_image_utils.hh"
#include "ED_paint.hh"
#include "ED_render.hh"
#include "ED_screen.hh"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"
#include "IMB_colormanagement.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"
#include "RNA_prototypes.hh"
#include "RNA_types.hh"

#include "UI_interface.hh"
#include "UI_interface_c.hh"
#include "UI_view2d.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "wm_window.hh"

#include <fmt/format.h>

#include "interface_drop_image.hh"
#include "interface_intern.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Brush Texture Slot Detection
 * \{ */

/**
 * Check whether \a slot_ptr/\a propname refer to one of the brush's texture slots and, if so,
 * which one. Primary vs mask is told apart by matching the slot data against the brush's `mtex` /
 * `mask_mtex`.
 *
 * \param r_use_mask_slot: Set to true for the mask slot, false for the primary slot.
 * \return true if the pointer/property refer to one of the brush's texture slots.
 */
static bool texture_slot_matches_brush(const PointerRNA *slot_ptr,
                                       const StringRefNull propname,
                                       const Brush *brush,
                                       bool *r_use_mask_slot)
{
  /* The owner must be a texture slot (#BrushTextureSlot derives from #TextureSlot). */
  if (!slot_ptr->type || !RNA_struct_is_a(slot_ptr->type, RNA_TextureSlot)) {
    return false;
  }
  /* ... and the property must be a pointer to a #Texture. */
  PointerRNA ptr_copy = *slot_ptr;
  PropertyRNA *prop = RNA_struct_find_property(&ptr_copy, propname.c_str());
  if (!prop || RNA_property_type(prop) != PROP_POINTER ||
      RNA_property_pointer_type(&ptr_copy, prop) != RNA_Texture)
  {
    return false;
  }
  /* Primary vs mask slot: match the slot data against the active brush. */
  const void *slot_data = slot_ptr->data;
  if (slot_data == &brush->mask_mtex) {
    *r_use_mask_slot = true;
    return true;
  }
  if (slot_data == &brush->mtex) {
    *r_use_mask_slot = false;
    return true;
  }
  return false;
}

/**
 * Identify whether \a but targets one of the active brush's texture slots and, if so, which one.
 *
 * The owner pointer (and, for the legacy widget, the property name) live in the button's context
 * store, under one of two keys depending on which widget draws the slot:
 * - `"template_id_ptr"` / `"template_id_prop"` for the legacy single-preview
 *   #template_ID/#template_ID_preview widget (mirrors #context_active_but_prop_get_templateID).
 * - `"image_grid_target"` for the current #template_asset_image_grid widget (used when the brush
 *   texture slot display mode is 'ASSET_GRID'); the property is implicitly "texture", since the
 *   widget is only ever used for that one property, so there is no separate name key.
 *
 * \param r_use_mask_slot: Set to true for the mask slot, false for the primary slot.
 * \return true if the button is one of the brush's texture slots.
 */
static bool determine_texture_slot_type(const ui::Button *but,
                                        const Brush *brush,
                                        bool *r_use_mask_slot)
{
  if (!but || !brush || !r_use_mask_slot || !but->context) {
    return false;
  }
  if (const PointerRNA *slot_ptr = CTX_store_ptr_lookup(but->context, "template_id_ptr")) {
    if (const std::optional<StringRefNull> prop_name = CTX_store_string_lookup(
            but->context, "template_id_prop"))
    {
      return texture_slot_matches_brush(slot_ptr, *prop_name, brush, r_use_mask_slot);
    }
  }
  if (const PointerRNA *slot_ptr = CTX_store_ptr_lookup(but->context, "image_grid_target")) {
    return texture_slot_matches_brush(slot_ptr, "texture", brush, r_use_mask_slot);
  }
  return false;
}

/** Find the texture-slot button without filtering for normal mouse interaction. Asset Browser drags
 * can leave the preview button outside the regular interactive hit-test path. */
static const ui::Button *find_texture_slot_button_at(const ARegion *region,
                                                     const wmEvent *event,
                                                     const Brush *brush,
                                                     bool *r_use_mask_slot)
{
  if (!region || !event || !region->runtime) {
    return nullptr;
  }

  for (ui::Block &block : region->runtime->uiblocks) {
    float x = float(event->xy[0]);
    float y = float(event->xy[1]);
    ui::window_to_block_fl(region, &block, &x, &y);
    for (ui::Button &but : block.buttons() | std::ranges::views::reverse) {
      if (!ui::button_contains_pt(&but, x, y)) {
        continue;
      }
      /* Grid tiles (#template_asset_image_grid) already get their own drop target from
       * #AbstractViewItem::create_drop_target() (see #ImageGridDropTarget), which is what actually
       * runs the assignment for them. Do not also claim them here, or hovering a tile shows two
       * overlapping "Assign ... to the brush texture slot" tooltips for the two competing targets.
       * This drop target stays responsible for the New/Open/Browse row and the legacy
       * #template_ID_preview widget, neither of which are view items. */
      if (but.type == ui::ButtonType::ViewItem) {
        continue;
      }
      if (determine_texture_slot_type(&but, brush, r_use_mask_slot)) {
        return &but;
      }
    }
  }
  return nullptr;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Drag Preview
 * \{ */

/**
 * Sets image preview for drag operation.
 * Loads and scales image preview, then attaches it to drag operation.
 *
 * @param drag Pointer to wmDrag structure
 * @param max_size Maximum size for the preview (default 128px)
 * @return true if preview was successfully set, false otherwise
 */
static bool DROP_IMAGE_set_preview_for_drag(bContext *C, wmDrag *drag, int max_size)
{
  if (!C || !drag) {
    return false;
  }

  /* Load image preview using functions from interface_drop_image_feedback.cc */
  ImBuf *preview_imb = nullptr;

  if (drag->type == WM_DRAG_PATH) {
    /* For image files */
    const char *filepath = WM_drag_get_single_path(drag);
    if (filepath) {
      preview_imb = DROP_IMAGE_load_and_scale_preview(filepath, max_size);
    }
  }
  else if (drag->type == WM_DRAG_ID) {
    /* For ID images */
    ID *id = WM_drag_get_local_ID(drag, ID_IM);
    if (id) {
      Image *image = (Image *)id;
      preview_imb = DROP_IMAGE_load_and_scale_preview_from_id(image, max_size);
    }
  }
  else if (drag->type == WM_DRAG_ASSET) {
    /* Resolve image assets so the drag preview is also available when dragging from the Asset
     * Browser. This follows the same resolution path used by the image drop target below. */
    wmDragAsset *asset_drag = WM_drag_get_asset_data(drag, ID_IM);
    if (asset_drag) {
      Image *image = ed::asset::resolve_image_from_asset(*CTX_data_main(C), *asset_drag->asset);
      if (image) {
        preview_imb = DROP_IMAGE_load_and_scale_preview_from_id(image, max_size);
      }
    }
  }
  else if (drag->type == WM_DRAG_ASSET_LIST) {
    /* Asset Browser creates a paired asset-list drag for multi-selection. Use its first image
     * item for the visual preview; the window manager may keep this drag functional while drawing
     * only the paired single-asset drag. */
    const ListBaseT<wmDragAssetListItem> *asset_items = WM_drag_asset_list_get(drag);
    if (asset_items) {
      for (const wmDragAssetListItem &item : *asset_items) {
        const ID_Type id_type = item.is_external ?
                                    item.asset_data.external_info->asset->get_id_type() :
                                    (item.asset_data.local_id ? GS(item.asset_data.local_id->name) :
                                                                 ID_Type(0));
        if (id_type != ID_IM) {
          continue;
        }
        Image *image = item.is_external ?
                           ed::asset::resolve_image_from_asset(
                               *CTX_data_main(C), *item.asset_data.external_info->asset) :
                           id_cast<Image *>(item.asset_data.local_id);
        if (image) {
          preview_imb = DROP_IMAGE_load_and_scale_preview_from_id(image, max_size);
        }
        break;
      }
    }
  }

  if (!preview_imb) {
    return false;
  }

  /* Set preview for drag operation. The buffer was created just for this drag, so hand ownership
   * to the drag which frees it in #WM_drag_free. */
  WM_event_drag_image(drag, preview_imb, 1.0f, true);

  return true;
}

/**
 * Callback function for drag start event.
 * Sets up image preview for drag operations when drag starts.
 * This is called by the dropbox system when drag operation begins.
 *
 * @param C Blender context
 * @param drag Drag operation data
 */
static void DROP_IMAGE_drag_start_callback(bContext *C, wmDrag *drag)
{
  if (!C || !drag) {
    return;
  }

  /* Only set preview if it's not already set */
  if (drag->imb) {
    return;
  }

  /* Check if this is an image/texture drag operation */
  if (drag->type == WM_DRAG_PATH) {
    const char *path = WM_drag_get_single_path(drag);
    if (path && BLI_path_extension_check_array(path, imb_ext_image)) {
      DROP_IMAGE_set_preview_for_drag(C, drag, 128);
    }
  }
  else if (drag->type == WM_DRAG_ASSET) {
    DROP_IMAGE_set_preview_for_drag(C, drag, 128);
  }
  else if (drag->type == WM_DRAG_ASSET_LIST) {
    DROP_IMAGE_set_preview_for_drag(C, drag, 128);
  }
  else if (drag->type == WM_DRAG_ID) {
    ID *id = WM_drag_get_local_ID(drag, ID_IM);
    if (id) {
      DROP_IMAGE_set_preview_for_drag(C, drag, 128);
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Brush Texture Slot Drop Target
 * \{ */

/**
 * Drop target for a single brush texture slot (primary or mask). Accepts image data-blocks, image
 * assets and image files, wrapping the image in a texture that is assigned to the slot via
 * #BRUSH_OT_texture_slot_assign_image.
 */
class BrushTextureSlotDropTarget : public ui::DropTargetInterface {
  /** True for the mask texture slot, false for the primary slot. */
  bool use_mask_slot_;

  /**
   * The Asset Browser starts both a #WM_DRAG_ASSET (single item) and a #WM_DRAG_ASSET_LIST drag
   * simultaneously for the same interaction (multi-select support); #drop_target_apply_drop()
   * requires every drag in that shared list to pass #can_drop(), and applies whichever one is
   * first. Handle both so the outcome does not depend on which one happens to be first.
   */
  static const wmDragAssetListItem *first_image_item_in_list(const wmDrag &drag)
  {
    const ListBaseT<wmDragAssetListItem> *asset_drags = WM_drag_asset_list_get(&drag);
    if (!asset_drags) {
      return nullptr;
    }
    for (const wmDragAssetListItem &item : *asset_drags) {
      const ID_Type item_idtype = item.is_external ?
                                      item.asset_data.external_info->asset->get_id_type() :
                                      (item.asset_data.local_id ?
                                           GS(item.asset_data.local_id->name) :
                                           ID_Type(0));
      if (item_idtype == ID_IM) {
        return &item;
      }
    }
    return nullptr;
  }

  static Image *resolve_asset_image(bContext *C, const wmDrag &drag)
  {
    if (drag.type == WM_DRAG_ASSET) {
      wmDragAsset *asset_drag = WM_drag_get_asset_data(&drag, ID_IM);
      if (!asset_drag) {
        return nullptr;
      }
      Image *image = ed::asset::resolve_image_from_asset(*CTX_data_main(C), *asset_drag->asset);
      if (image && !image->id.asset_data) {
        ed::asset::image_mark_as_asset(image);
      }
      return image;
    }
    if (drag.type == WM_DRAG_ASSET_LIST) {
      const wmDragAssetListItem *item = first_image_item_in_list(drag);
      if (!item) {
        return nullptr;
      }
      if (item->is_external) {
        Image *image = ed::asset::resolve_image_from_asset(
            *CTX_data_main(C), *item->asset_data.external_info->asset);
        if (image && !image->id.asset_data) {
          ed::asset::image_mark_as_asset(image);
        }
        return image;
      }
      return id_cast<Image *>(item->asset_data.local_id);
    }
    return nullptr;
  }

 public:
  BrushTextureSlotDropTarget(bool use_mask_slot) : use_mask_slot_(use_mask_slot) {}

  bool can_drop(bContext & /*C*/, const wmDrag &drag, const char ** /*r_disabled_hint*/) const override
  {
    /* Local image ID, or an image-typed #WM_DRAG_ASSET. #WM_drag_is_ID_type covers both
     * #WM_DRAG_ID and #WM_DRAG_ASSET (via #WM_drag_get_asset_data with ID_IM); assets of any
     * other type are rejected. */
    if (WM_drag_is_ID_type(&drag, ID_IM)) {
      return true;
    }
    /* Multi-select from the Asset Browser is a separate #WM_DRAG_ASSET_LIST drag. Every drag in
     * the shared list must pass this check; accept the list when it contains at least one image. */
    if (drag.type == WM_DRAG_ASSET_LIST) {
      return first_image_item_in_list(drag) != nullptr;
    }
    /* Image file from the file browser or the OS. */
    if (drag.type == WM_DRAG_PATH) {
      const char *path = WM_drag_get_single_path(&drag);
      return path && BLI_path_extension_check_array(path, imb_ext_image);
    }
    return false;
  }

  std::string drop_tooltip(const ui::DragInfo &drag_info) const override
  {
    const std::string image_name = WM_drag_get_item_name(
        const_cast<wmDrag *>(&drag_info.drag_data));
    if (use_mask_slot_) {
      return fmt::format(fmt::runtime(TIP_("Assign {} to the brush mask texture slot")), image_name);
    }
    return fmt::format(fmt::runtime(TIP_("Assign {} to the brush texture slot")), image_name);
  }

  bool on_drop(bContext *C, const ui::DragInfo &drag_info) const override
  {
    const wmDrag &drag = drag_info.drag_data;

    PointerRNA props = WM_operator_properties_create("BRUSH_OT_texture_slot_assign_image");
    /* Resolve a local image, importing the asset first if the drag came from the asset browser.
     * #WM_drag_get_local_ID_or_import_from_asset() only handles #WM_DRAG_ID / #WM_DRAG_ASSET, so
     * #WM_DRAG_ASSET_LIST falls through to #resolve_asset_image() below. */
    const ID *image_id = WM_drag_get_local_ID_or_import_from_asset(C, &drag, ID_IM);
    if (!image_id && ELEM(drag.type, WM_DRAG_ASSET, WM_DRAG_ASSET_LIST)) {
      if (Image *image = resolve_asset_image(C, drag)) {
        image_id = &image->id;
      }
    }
    if (image_id) {
      RNA_int_set(&props, "session_uid", int(image_id->session_uid));
    }
    else if (drag.type == WM_DRAG_PATH) {
      if (const char *path = WM_drag_get_single_path(&drag)) {
        /* Image assets from an on-disk Asset Browser library are dragged as paths rather than
         * WM_DRAG_ASSET. Load the image here so it can be kept in the current file as an asset and
         * assigned through the same ID-based path as blend-library assets. */
        Image *image = BKE_image_load_exists(CTX_data_main(C), path, nullptr);
        if (image) {
          id_us_min(&image->id);
          if (!image->id.asset_data) {
            ed::asset::image_mark_as_asset(image);
            WM_event_add_notifier(C, NC_ASSET | NA_EDITED, nullptr);
            WM_event_add_notifier(C, NC_ID | NA_EDITED, &image->id);
          }
          RNA_int_set(&props, "session_uid", int(image->id.session_uid));
        }
        else {
          RNA_string_set(&props, "filepath", path);
        }
      }
    }
    RNA_boolean_set(&props, "use_mask_slot", use_mask_slot_);
    RNA_boolean_set(&props, "replace_existing", true);

    /* Invoke (not exec): a packed dropped image on an occupied slot opens a popup menu, which
     * returns #OPERATOR_INTERFACE - still a successful, accepted drop. */
    const wmOperatorStatus status = WM_operator_name_call(C,
                                                          "BRUSH_OT_texture_slot_assign_image",
                                                          wm::OpCallContext::InvokeDefault,
                                                          &props,
                                                          &drag_info.event);
    WM_operator_properties_free(&props);
    return (status & (OPERATOR_FINISHED | OPERATOR_INTERFACE)) != 0;
  }
};

std::unique_ptr<ui::DropTargetInterface> brush_texture_slot_drop_target_get(bContext *C,
                                                                             const ARegion *region,
                                                                             const wmEvent *event)
{
  if (!C || !region || !event) {
    return nullptr;
  }
  const Brush *brush = BKE_paint_brush(BKE_paint_get_active_from_context(C));
  if (!brush) {
    return nullptr;
  }
  /* This is only ever called while a drag is in progress (#brush_texture_drop_poll,
   * #brush_texture_drop_tooltip), where the regular mouse-over hit-test and the "active button"
   * both become unreliable: mid-drag, #but_find_mouse_over frequently misses the target widget
   * entirely, and #context_active_but_get falls back to whichever button last had regular
   * interactive focus in the region - which can be a stale, position-independent button (e.g. a
   * popover trigger) that has nothing to do with the current cursor position. Always hit-test
   * directly against the event coordinates instead. */
  bool use_mask_slot = false;
  const ui::Button *but = find_texture_slot_button_at(region, event, brush, &use_mask_slot);
  if (!but) {
    return nullptr;
  }
  return std::make_unique<BrushTextureSlotDropTarget>(use_mask_slot);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Texture Drop Registration
 * \{ */

static bool brush_texture_drop_poll(bContext *C, wmDrag *drag, const wmEvent *event)
{
  const ARegion *region = CTX_wm_region(C);
  if (!region) {
    return false;
  }
  std::unique_ptr<ui::DropTargetInterface> target = ui::region_but_find_drop_target_at(
      C, region, event);
  if (!target) {
    return false;
  }
  const char *disabled_hint = nullptr;
  return target->can_drop(*C, *drag, &disabled_hint);
}

static std::string brush_texture_drop_tooltip(bContext *C,
                                              wmDrag *drag,
                                              const int /*xy*/[2],
                                              wmDropBox * /*drop*/)
{
  ARegion *region = CTX_wm_region(C);
  const wmWindow *win = CTX_wm_window(C);
  if (!region || !win) {
    return {};
  }
  const wmEvent *event = win->runtime->eventstate;
  std::unique_ptr<ui::DropTargetInterface> target = ui::region_but_find_drop_target_at(
      C, region, event);
  if (!target) {
    return {};
  }
  return ui::drop_target_tooltip(*C, *region, *target, *drag, *event);
}

/**
 * Register the brush texture-slot image/texture drop box.
 *
 * The "User Interface" drop-box map is attached to every region that hosts UI widgets
 * (see #ED_region_add_handlers / #ED_KEYMAP_UI), so a single registration here covers the
 * brush texture panels in all editors - no per-editor registration is needed.
 */
void DROP_IMAGE_register_dropboxes()
{
  ListBaseT<wmDropBox> *lb = WM_dropboxmap_find("User Interface", SPACE_EMPTY, RGN_TYPE_WINDOW);
  if (!lb) {
    return;
  }

  WM_dropbox_add(lb,
                 "UI_OT_button_drop",
                 brush_texture_drop_poll,
                 nullptr,
                 WM_drag_free_imported_drag_ID,
                 brush_texture_drop_tooltip);

  /* Attach the drag preview through a *global* prefetch handler rather than the drop-box'
   * #on_drag_start. The latter only runs for drop-boxes in a visible area/region, but the "User
   * Interface" map lives in #SPACE_EMPTY and is never tagged visible, so it would never fire. */
  for (const eWM_DragDataType drag_type : {
           WM_DRAG_PATH, WM_DRAG_ID, WM_DRAG_ASSET, WM_DRAG_ASSET_LIST}) {
    WM_drag_global_prefetch_handler_add(drag_type, [](bContext &C, wmDrag &drag) {
      DROP_IMAGE_drag_start_callback(&C, &drag);
    });
  }
}

}
