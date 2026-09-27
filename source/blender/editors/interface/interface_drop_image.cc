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
#include "DNA_material_types.h"
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
#include "BLI_function_ref.hh"
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
#include "BKE_global.hh"
#include "BKE_icons.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_main_invariants.hh"
#include "BKE_material.hh"
#include "BKE_paint.hh"
#include "BKE_report.hh"
#include "BKE_screen.hh"
#include "BKE_texture.h"
#include "BKE_wm_runtime.hh"

#include "AS_asset_library.hh"
#include "AS_asset_representation.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"

#include "ED_asset_import.hh"
#include "ED_asset_image_utils.hh"
#include "ED_asset_menu_utils.hh"
#include "ED_paint.hh"
#include "ED_render.hh"
#include "ED_screen.hh"
#include "ED_undo.hh"

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
/** \name Shared Drag Helpers
 * \{ */

/**
 * The ID type an Asset Browser multi-select drag item resolves to, without importing or resolving
 * anything. External (library) items are typed by their asset representation, local items by the
 * data-block itself.
 */
ID_Type drag_asset_list_item_idtype(const wmDragAssetListItem &item)
{
  return item.is_external ?
             item.asset_data.external_info->asset->get_id_type() :
             (item.asset_data.local_id ? GS(item.asset_data.local_id->name) : ID_Type(0));
}

/** First item of an Asset Browser multi-select drag matching any of \a idtypes, or null. */
static const wmDragAssetListItem *drag_asset_list_first_of(const wmDrag &drag,
                                                           const Span<const ID_Type> idtypes)
{
  const ListBaseT<wmDragAssetListItem> *asset_drags = WM_drag_asset_list_get(&drag);
  if (!asset_drags) {
    return nullptr;
  }
  for (const wmDragAssetListItem &item : *asset_drags) {
    if (idtypes.contains(drag_asset_list_item_idtype(item))) {
      return &item;
    }
  }
  return nullptr;
}

/** Number of items of \a idtype an Asset Browser multi-select drag carries. */
static int drag_asset_list_count_of(const wmDrag &drag, const ID_Type idtype)
{
  const ListBaseT<wmDragAssetListItem> *asset_drags = WM_drag_asset_list_get(&drag);
  if (!asset_drags) {
    return 0;
  }
  int count = 0;
  for (const wmDragAssetListItem &item : *asset_drags) {
    if (drag_asset_list_item_idtype(item) == idtype) {
      count++;
    }
  }
  return count;
}

/** True when \a id can be edited in place (a local ID, or an asset-editable linked one). */
static bool paint_texture_target_owner_editable(const ID *id)
{
  return id != nullptr && ID_IS_EDITABLE(id) && !ID_IS_OVERRIDE_LIBRARY(id);
}

/** True when \a owner (linked data) cannot reference the local \a tex at all. */
static bool paint_texture_linked_owner_refuses_local_tex(const ID *owner, const Tex &tex)
{
  return ID_IS_LINKED(owner) && !ID_IS_LINKED(&tex.id);
}

/** The dragged #Texture data-block, when the drag carries one as a local ID (no imports). */
static const Tex *drag_local_texture(const wmDrag &drag)
{
  const ID *id = WM_drag_get_local_ID(const_cast<wmDrag *>(&drag), ID_TE);
  return id ? id_cast<const Tex *>(id) : nullptr;
}

/**
 * The Asset Browser starts a drag for the item under the cursor together with a
 * #WM_DRAG_ASSET_LIST carrying the whole selection, and #drop_target_apply_drop() applies
 * whichever of the two comes first. The item drag is a #WM_DRAG_ASSET for an external asset but a
 * #WM_DRAG_ID for an asset stored in the current file (see #button_drag_start), so both defer.
 * Targets that take the whole selection (multi-image drops) defer the item drag to its list
 * sibling when one is present and matches \a predicate. A #WM_DRAG_ID started elsewhere (the
 * Outliner, ...) gets an empty list sibling, which no predicate matches, so it stays as is.
 * Walks the window manager's active drags: no #bContext is available in #drop_tooltip, where this
 * is called too.
 */
static const wmDrag &drag_prefer_asset_list_sibling(
    const wmDrag &drag, FunctionRef<bool(const wmDrag &asset_list_drag)> predicate)
{
  if (!ELEM(drag.type, WM_DRAG_ASSET, WM_DRAG_ID)) {
    return drag;
  }
  const wmWindowManager *wm = static_cast<const wmWindowManager *>(G_MAIN->wm.first);
  if (!wm) {
    return drag;
  }
  for (const wmDrag &other : wm->runtime->drags) {
    if (other.type == WM_DRAG_ASSET_LIST && predicate(other)) {
      return other;
    }
  }
  return drag;
}

/** \} */

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

/**
 * Find the top-most button at the event position that \a predicate accepts, without filtering for
 * normal mouse interaction. Asset Browser drags can leave the target button outside the regular
 * interactive hit-test path, so drop-target lookups test the event coordinates directly.
 */
static const ui::Button *find_button_at(const ARegion *region,
                                        const wmEvent *event,
                                        FunctionRef<bool(const ui::Button &but)> predicate)
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
      /* A grayed out slot must not accept a drop: the layouts that disable one (a linked brush,
       * for instance) rely on that being the only way in, and the operator behind the drop would
       * just report an error. */
      if (but.flag & (ui::BUT_DISABLED | ui::UI_HIDDEN)) {
        continue;
      }
      if (predicate(but)) {
        return &but;
      }
    }
  }
  return nullptr;
}

static const ui::Button *find_texture_slot_button_at(const ARegion *region,
                                                     const wmEvent *event,
                                                     const Brush *brush,
                                                     bool *r_use_mask_slot)
{
  return find_button_at(region, event, [&](const ui::Button &but) {
    /* Grid tiles (#template_asset_image_grid) already get their own drop target from
     * #AbstractViewItem::create_drop_target() (see #ImageGridDropTarget), which is what actually
     * runs the assignment for them. Do not also claim them here, or hovering a tile shows two
     * overlapping "Assign ... to the brush texture slot" tooltips for the two competing targets.
     * This drop target stays responsible for the New/Open/Browse row and the legacy
     * #template_ID_preview widget, neither of which are view items. */
    if (but.type == ui::ButtonType::ViewItem) {
      return false;
    }
    return determine_texture_slot_type(&but, brush, r_use_mask_slot);
  });
}

static bool material_slot_matches_brush(const PointerRNA *slot_ptr,
                                        const StringRefNull propname,
                                        const Brush *brush)
{
  if (!slot_ptr->type || !RNA_struct_is_a(slot_ptr->type, RNA_BrushMaterialPaint)) {
    return false;
  }
  if (propname != "source_material") {
    return false;
  }
  if (!brush || !brush->material_paint || slot_ptr->data != brush->material_paint) {
    return false;
  }
  return true;
}

static bool determine_material_slot_type(const ui::Button *but, const Brush *brush)
{
  if (!but || !brush || !but->context) {
    return false;
  }
  /* The empty slot's labelled drop button carries the browser context; the assigned row's name
   * button carries the standard template-ID one. Same two-step lookup as
   * #image_id_browser_button_target, for the same reason. */
  const PointerRNA *slot_ptr = CTX_store_ptr_lookup(but->context, "id_browser_ptr");
  std::optional<StringRefNull> prop_name = CTX_store_string_lookup(but->context,
                                                                   "id_browser_prop");
  if (!slot_ptr || !prop_name) {
    slot_ptr = CTX_store_ptr_lookup(but->context, "template_id_ptr");
    prop_name = CTX_store_string_lookup(but->context, "template_id_prop");
  }
  if (!slot_ptr || !prop_name) {
    return false;
  }
  return material_slot_matches_brush(slot_ptr, *prop_name, brush);
}

static const ui::Button *find_material_slot_button_at(const ARegion *region,
                                                      const wmEvent *event,
                                                      const Brush *brush)
{
  return find_button_at(region, event, [&](const ui::Button &but) {
    if (but.type == ui::ButtonType::ViewItem) {
      return false;
    }
    return determine_material_slot_type(&but, brush);
  });
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
        if (drag_asset_list_item_idtype(item) != ID_IM) {
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
    return drag_asset_list_first_of(drag, {ID_IM});
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

  /* The dragged item's name is already drawn as the drag's own label, so it is not repeated. */
  std::string drop_tooltip(const ui::DragInfo & /*drag_info*/) const override
  {
    if (use_mask_slot_) {
      return TIP_("Assign to the brush mask texture slot");
    }
    return TIP_("Assign to the brush texture slot");
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

/**
 * Drop target for the active brush's source material slot. Accepts Material data-blocks and
 * Material assets, assigning the material as the brush's PBR paint source via
 * #PAINT_OT_material_paint_source_material_set.
 */
class BrushMaterialSlotDropTarget : public ui::DropTargetInterface {
  static const wmDragAssetListItem *first_material_item_in_list(const wmDrag &drag)
  {
    return drag_asset_list_first_of(drag, {ID_MA});
  }

  static Material *resolve_asset_material(bContext *C, const wmDrag &drag)
  {
    Main &bmain = *CTX_data_main(C);
    if (drag.type == WM_DRAG_ASSET) {
      wmDragAsset *asset_drag = WM_drag_get_asset_data(&drag, ID_MA);
      if (!asset_drag) {
        return nullptr;
      }
      ID *id = ed::asset::asset_local_id_ensure_imported(
          bmain, *asset_drag->asset, 0, ASSET_IMPORT_APPEND_REUSE, std::nullopt, CTX_wm_reports(C));
      return (id && GS(id->name) == ID_MA) ? id_cast<Material *>(id) : nullptr;
    }
    if (drag.type == WM_DRAG_ASSET_LIST) {
      const wmDragAssetListItem *item = first_material_item_in_list(drag);
      if (!item) {
        return nullptr;
      }
      if (item->is_external) {
        ID *id = ed::asset::asset_local_id_ensure_imported(
            bmain,
            *item->asset_data.external_info->asset,
            0,
            ASSET_IMPORT_APPEND_REUSE,
            std::nullopt,
            CTX_wm_reports(C));
        return (id && GS(id->name) == ID_MA) ? id_cast<Material *>(id) : nullptr;
      }
      return id_cast<Material *>(item->asset_data.local_id);
    }
    return nullptr;
  }

 public:
  BrushMaterialSlotDropTarget() = default;

  bool can_drop(bContext & /*C*/, const wmDrag &drag, const char ** /*r_disabled_hint*/) const override
  {
    if (WM_drag_is_ID_type(&drag, ID_MA)) {
      return true;
    }
    if (drag.type == WM_DRAG_ASSET_LIST) {
      return first_material_item_in_list(drag) != nullptr;
    }
    return false;
  }

  std::string drop_tooltip(const ui::DragInfo &drag_info) const override
  {
    const std::string material_name = WM_drag_get_item_name(
        const_cast<wmDrag *>(&drag_info.drag_data));
    return fmt::format(fmt::runtime(TIP_("Assign material \"{}\" to brush")), material_name);
  }

  bool on_drop(bContext *C, const ui::DragInfo &drag_info) const override
  {
    const wmDrag &drag = drag_info.drag_data;

    const bool is_asset_drag = ELEM(drag.type, WM_DRAG_ASSET, WM_DRAG_ASSET_LIST);
    Material *ma = is_asset_drag ? resolve_asset_material(C, drag) :
                                   id_cast<Material *>(WM_drag_get_local_ID(&drag, ID_MA));
    if (!ma) {
      /* Nothing usable came out of the drag (a failed asset import, most likely). Reporting is up
       * to whatever failed; calling the operator without a material would only add a second,
       * misleading "No material specified" error. */
      return false;
    }

    PointerRNA props = WM_operator_properties_create("PAINT_OT_material_paint_source_material_set");
    RNA_int_set(&props, "session_uid", int(ma->id.session_uid));
    RNA_string_set(&props, "name", ma->id.name + 2);

    const wmOperatorStatus status = WM_operator_name_call(
        C,
        "PAINT_OT_material_paint_source_material_set",
        wm::OpCallContext::ExecDefault,
        &props,
        nullptr);
    WM_operator_properties_free(&props);
    return (status & OPERATOR_FINISHED) != 0;
  }
};

std::unique_ptr<ui::DropTargetInterface> brush_material_slot_drop_target_get(
    bContext *C, const ARegion *region, const wmEvent *event)
{
  if (!C || !region || !event) {
    return nullptr;
  }
  const Brush *brush = BKE_paint_brush(BKE_paint_get_active_from_context(C));
  if (!brush) {
    return nullptr;
  }
  const ui::Button *but = find_material_slot_button_at(region, event, brush);
  if (!but) {
    return nullptr;
  }
  return std::make_unique<BrushMaterialSlotDropTarget>();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Texture Drop Registration
 * \{ */

/** Resolve the dragged #Tex: a local #WM_DRAG_ID, an imported #WM_DRAG_ASSET, or the first
 * texture of a multi-select #WM_DRAG_ASSET_LIST. Null when the drag carries no texture. */
static Tex *resolve_drag_texture(bContext *C, const wmDrag &drag)
{
  if (ID *id = WM_drag_get_local_ID_or_import_from_asset(C, &drag, ID_TE)) {
    return (GS(id->name) == ID_TE) ? id_cast<Tex *>(id) : nullptr;
  }
  if (drag.type == WM_DRAG_ASSET_LIST) {
    const wmDragAssetListItem *item = drag_asset_list_first_of(drag, {ID_TE});
    if (!item) {
      return nullptr;
    }
    if (item->is_external) {
      ID *id = ed::asset::asset_local_id_ensure_imported(*CTX_data_main(C),
                                                         *item->asset_data.external_info->asset,
                                                         /*flags*/ 0,
                                                         /*import_method*/ std::nullopt,
                                                         /*instantiate_context*/ std::nullopt,
                                                         CTX_wm_reports(C));
      return (id && GS(id->name) == ID_TE) ? id_cast<Tex *>(id) : nullptr;
    }
    return id_cast<Tex *>(item->asset_data.local_id);
  }
  return nullptr;
}

Tex *ED_paint_texture_wrap_image_for_owner(Main *bmain,
                                           Tex *current,
                                           const ID *owner,
                                           Image *image,
                                           bool image_has_extra_user)
{
  BLI_assert(image != nullptr);
  Tex *tex = BKE_texture_image_wrap_for_slot(bmain, current, image);

  /* The new texture follows the owner's library, whatever it is: a linked (asset-editable) brush
   * can never reference a local texture, and #BKE_id_can_use_id makes the reference-counted
   * setter silently refuse the assignment otherwise. Same treatment as #paint_assign_image_exec
   * (#BRUSH_OT_texture_slot_assign_image); a no-op for a local owner. */
  if (owner != nullptr) {
    BKE_id_move_to_same_lib(*bmain, tex->id, *owner);
  }

  /* A linked texture must not reference a local image either: a freshly loaded image simply
   * moves into the texture's library (its load reference travels with it), while a pre-existing
   * local image is copied -- moving it would leave its session_uid present both locally
   * (referenced by earlier undo steps) and linked, crashing the next undo. */
  if (ID_IS_LINKED(&tex->id) && !ID_IS_LINKED(&image->id)) {
    if (image_has_extra_user) {
      BKE_id_move_to_same_lib(*bmain, image->id, tex->id);
    }
    else {
      Image *copy = id_cast<Image *>(BKE_id_copy(bmain, &image->id));
      /* #BKE_id_copy leaves one user; hand the wrap's image reference over to the copy. */
      id_us_min(&copy->id);
      id_us_min(&image->id);
      tex->ima = copy;
      id_us_plus(&copy->id);
      BKE_id_move_to_same_lib(*bmain, copy->id, tex->id);
    }
  }

  if (image_has_extra_user) {
    /* A freshly loaded image's creation reference is superseded by the texture's own. */
    id_us_min(&image->id);
  }
  return tex;
}

void ED_paint_texture_assignment_finalize(bContext *C,
                                          ID *owner,
                                          Tex *tex,
                                          const bool created,
                                          const bool refresh_preview)
{
  DEG_id_tag_update(&tex->id, ID_RECALC_SHADING);

  WM_event_add_notifier(C, NC_TEXTURE | (created ? NA_ADDED : NA_EDITED), tex);
  if (owner && GS(owner->name) == ID_BR) {
    Brush *brush = id_cast<Brush *>(owner);
    BKE_brush_tag_unsaved_changes(brush);
    WM_event_add_notifier(C, NC_BRUSH | NA_EDITED, brush);
  }

  /* Refresh the texture preview/icon for immediate visual feedback. */
  if (refresh_preview) {
    DROP_IMAGE_update_texture_preview_smart(C, CTX_data_main(C), tex, true);
  }
}

Tex *ED_paint_texture_property_assign_image(bContext *C,
                                            const PointerRNA &target_ptr,
                                            PropertyRNA *target_prop,
                                            Image *image,
                                            const bool image_has_extra_user)
{
  BLI_assert(image != nullptr);
  Main *bmain = CTX_data_main(C);
  PointerRNA ptr = target_ptr;

  /* The wrapping path takes real refcount side effects before the assignment; a non-editable
   * (linked / library-override) owner can never take them (the generated setter refuses it
   * silently), so don't leave them dangling. */
  if (ptr.owner_id && !paint_texture_target_owner_editable(ptr.owner_id)) {
    return nullptr;
  }

  Tex *old_tex = static_cast<Tex *>(RNA_property_pointer_get(&ptr, target_prop).data);
  Tex *tex = ED_paint_texture_wrap_image_for_owner(
      bmain, old_tex, ptr.owner_id, image, image_has_extra_user);

  if (tex != old_tex && (RNA_property_flag(target_prop) & PROP_ID_REFCOUNT)) {
    /* The generated setter takes its own user on the new texture; #BKE_texture_add left the one
     * the assigning slot owns. */
    id_us_min(&tex->id);
  }

  RNA_property_pointer_set(&ptr, target_prop, RNA_id_pointer_create(&tex->id), nullptr);
  RNA_property_update(C, &ptr, target_prop);

  /* The browser row shows the texture's preview: a retarget swapped the image on the SAME
   * texture, so without regenerating its preview the row keeps showing the previous image and
   * the replacement looks like a no-op. Same refresh as #assign_image_finish. */
  ED_paint_texture_assignment_finalize(C, ptr.owner_id, tex, tex != old_tex);
  return tex;
}

/**
 * Drop target for the labelled Image #template_ID_browser control. The button's context identifies
 * the target property, so the same target works for every specialized image browser template.
 */
class ImageIDBrowserDropTarget : public ui::DropTargetInterface {
  PointerRNA target_ptr_;
  PropertyRNA *target_prop_;
  /**
   * True when the target property wants a #Texture (a brush Curve Patch texture slot, the Face Set
   * color texture, ...): dragged images are wrapped into a #TEX_IMAGE texture through
   * #ED_paint_texture_property_assign_image, and #Texture data-blocks are assigned directly.
   */
  bool wrap_in_texture_;

  /** First image item of an Asset Browser multi-select drag, or null. */
  static const wmDragAssetListItem *first_image_item_in_list(const wmDrag &drag)
  {
    return drag_asset_list_first_of(drag, {ID_IM});
  }

  /** True when the target property is a PBR paint channel's source image
   * (#BrushMaterialPaintChannel.source_image), which can take a whole matching batch of images. */
  bool targets_paint_channel() const
  {
    return !wrap_in_texture_ && target_ptr_.type &&
           RNA_struct_is_a(target_ptr_.type, RNA_BrushMaterialPaintChannel);
  }

  /** All image file paths carried by a path drag, absolute and normalized. */
  static Vector<std::string> drag_image_paths(const wmDrag &drag)
  {
    Vector<std::string> image_paths;
    if (drag.type != WM_DRAG_PATH) {
      return image_paths;
    }
    for (const std::string &path : WM_drag_get_paths(&drag)) {
      if (!BLI_path_extension_check_array(path.c_str(), imb_ext_image)) {
        continue;
      }
      char abs[FILE_MAX];
      BLI_strncpy(abs, path.c_str(), sizeof(abs));
      BLI_path_abs(abs, BKE_main_blendfile_path_from_global());
      BLI_path_normalize(abs);
      image_paths.append(abs);
    }
    return image_paths;
  }

  /**
   * The Asset Browser starts a #WM_DRAG_ASSET for the item under the cursor together with a
   * #WM_DRAG_ASSET_LIST carrying the whole selection, and #drop_target_apply_drop() applies
   * whichever of the two comes first. A paint channel takes the whole selection, so the
   * single-asset drag defers to its list sibling.
   */
  static const wmDrag &paint_channel_effective_drag(const wmDrag &drag)
  {
    return drag_prefer_asset_list_sibling(
        drag, [](const wmDrag &asset_list_drag) {
          return drag_asset_list_first_of(asset_list_drag, {ID_IM}) != nullptr;
        });
  }

  /** Number of image assets carried by an asset/asset-list drag, without resolving them. */
  static int drag_asset_image_count(const wmDrag &drag)
  {
    if (drag.type == WM_DRAG_ASSET) {
      return WM_drag_get_asset_data(&drag, ID_IM) ? 1 : 0;
    }
    if (drag.type == WM_DRAG_ASSET_LIST) {
      return drag_asset_list_count_of(drag, ID_IM);
    }
    return 0;
  }

  static void add_local_image_item(PointerRNA &props, const ID &image_id)
  {
    PointerRNA itemptr{};
    RNA_collection_add(&props, "images", &itemptr);
    RNA_string_set(&itemptr, "name", image_id.name + 2);
  }

  /**
   * Add an "images" entry for an image asset without importing it: the operator only brings it
   * into the file once the user confirms, and not at all for rows the user unticks.
   */
  static void add_asset_image_item(PointerRNA &props, const asset_system::AssetRepresentation &asset)
  {
    if (const ID *local_id = asset.local_id()) {
      add_local_image_item(props, *local_id);
      return;
    }
    PointerRNA itemptr{};
    RNA_collection_add(&props, "images", &itemptr);
    if (asset.full_library_path().empty()) {
      /* An on-disk image asset is just its file, see #ed::asset::resolve_image_from_asset(). */
      RNA_string_set(&itemptr, "filepath", asset.full_path().c_str());
      return;
    }
    ed::asset::operator_asset_reference_props_set(asset, itemptr);
    RNA_string_set(&itemptr, "name", asset.get_name().c_str());
  }

  static void add_drag_asset_image_items(PointerRNA &props, const wmDrag &drag)
  {
    if (drag.type == WM_DRAG_ASSET) {
      if (const wmDragAsset *asset_drag = WM_drag_get_asset_data(&drag, ID_IM)) {
        add_asset_image_item(props, *asset_drag->asset);
      }
      return;
    }
    if (drag.type != WM_DRAG_ASSET_LIST) {
      return;
    }
    const ListBaseT<wmDragAssetListItem> *asset_drags = WM_drag_asset_list_get(&drag);
    if (!asset_drags) {
      return;
    }
    for (const wmDragAssetListItem &item : *asset_drags) {
      if (item.is_external) {
        const asset_system::AssetRepresentation &asset = *item.asset_data.external_info->asset;
        if (asset.get_id_type() == ID_IM) {
          add_asset_image_item(props, asset);
        }
      }
      else if (item.asset_data.local_id && GS(item.asset_data.local_id->name) == ID_IM) {
        add_local_image_item(props, *item.asset_data.local_id);
      }
    }
  }

  int paint_channel_drag_image_count(const wmDrag &drag) const
  {
    const wmDrag &effective_drag = paint_channel_effective_drag(drag);
    return int(drag_image_paths(effective_drag).size()) + drag_asset_image_count(effective_drag);
  }

  /**
   * Hand a multi-image drop on a PBR paint channel to
   * #PAINT_OT_material_paint_channels_assign_images: the operator's dialog prefills a channel for
   * every file through the Preferences name-matching map types and assigns them on confirm, so one
   * drop fills the whole PBR set (Base Color, Normal, ...).
   */
  bool drop_to_channel_assign_operator(bContext *C, const ui::DragInfo &drag_info) const
  {
    const wmDrag &drag = paint_channel_effective_drag(drag_info.drag_data);

    PointerRNA props = WM_operator_properties_create(
        "PAINT_OT_material_paint_channels_assign_images");
    for (const std::string &path : drag_image_paths(drag)) {
      PointerRNA itemptr{};
      RNA_collection_add(&props, "images", &itemptr);
      RNA_string_set(&itemptr, "filepath", path.c_str());
    }
    add_drag_asset_image_items(props, drag);
    RNA_boolean_set(&props, "use_name_matching", true);
    /* The channel belongs to this brush, which is not necessarily the active one. */
    if (target_ptr_.owner_id && GS(target_ptr_.owner_id->name) == ID_BR) {
      RNA_int_set(&props, "brush_session_uid", int(target_ptr_.owner_id->session_uid));
    }

    const wmOperatorStatus status = WM_operator_name_call(
        C,
        "PAINT_OT_material_paint_channels_assign_images",
        wm::OpCallContext::InvokeDefault,
        &props,
        &drag_info.event);
    WM_operator_properties_free(&props);
    /* The confirm dialog keeps the operator running modal until the user accepts it. */
    return (status & (OPERATOR_FINISHED | OPERATOR_RUNNING_MODAL | OPERATOR_INTERFACE)) != 0;
  }

 public:
  ImageIDBrowserDropTarget(const PointerRNA &target_ptr, PropertyRNA *target_prop)
      : target_ptr_(target_ptr),
        target_prop_(target_prop),
        /* #RNA_property_pointer_type takes a mutable #PointerRNA; #target_ptr_ is the copy it
         * reads. */
        wrap_in_texture_(RNA_property_pointer_type(&target_ptr_, target_prop) == RNA_Texture)
  {
  }

  bool can_drop(bContext & /*C*/, const wmDrag &drag, const char **r_disabled_hint) const override
  {
    if (wrap_in_texture_) {
      /* A texture target belongs to a brush (Curve Patch slots, Face Set color map); the
       * assignment edits that brush, so linked data must not take it. */
      if (target_ptr_.owner_id && !paint_texture_target_owner_editable(target_ptr_.owner_id)) {
        *r_disabled_hint = TIP_("Cannot edit linked brush data");
        return false;
      }
      /* A linked brush can never reference a local texture either (the reference-counted setter
       * refuses it); report that combination instead of silently no-op'ing in #on_drop. */
      if (target_ptr_.owner_id) {
        if (const Tex *tex = drag_local_texture(drag);
            tex && paint_texture_linked_owner_refuses_local_tex(target_ptr_.owner_id, *tex))
        {
          *r_disabled_hint = TIP_("A linked brush cannot use a local texture");
          return false;
        }
      }
      /* Local image or texture ID, or an asset of either type (#WM_drag_is_ID_type covers both
       * #WM_DRAG_ID and #WM_DRAG_ASSET). */
      if (WM_drag_is_ID_type(&drag, ID_IM) || WM_drag_is_ID_type(&drag, ID_TE)) {
        return true;
      }
      if (drag.type == WM_DRAG_ASSET_LIST) {
        return first_image_item_in_list(drag) != nullptr ||
               drag_asset_list_first_of(drag, {ID_TE}) != nullptr;
      }
      if (drag.type == WM_DRAG_PATH) {
        const char *path = WM_drag_get_single_path(&drag);
        return path && BLI_path_extension_check_array(path, imb_ext_image);
      }
      return false;
    }

    if (WM_drag_is_ID_type(&drag, ID_IM)) {
      return true;
    }
    if (drag.type == WM_DRAG_ASSET_LIST) {
      return first_image_item_in_list(drag) != nullptr;
    }
    if (drag.type == WM_DRAG_PATH) {
      /* A paint channel takes a whole batch of images, so any image in the drag qualifies;
       * other image slots keep taking the first path only. */
      if (targets_paint_channel()) {
        return !drag_image_paths(drag).is_empty();
      }
      const char *path = WM_drag_get_single_path(&drag);
      return path && BLI_path_extension_check_array(path, imb_ext_image);
    }
    return false;
  }

  std::string drop_tooltip(const ui::DragInfo &drag_info) const override
  {
    const wmDrag &drag = drag_info.drag_data;
    if (targets_paint_channel()) {
      const int image_count = paint_channel_drag_image_count(drag);
      if (image_count > 1) {
        return fmt::format(
            fmt::runtime(TIP_("Assign {} images to matching material paint channels")),
            image_count);
      }
    }
    /* The dragged item's name is already drawn as the drag's own label, so it is not repeated. */
    if (wrap_in_texture_) {
      return TIP_("Assign to the texture slot");
    }
    return TIP_("Assign to the image slot");
  }

  /** Assign \a tex to the texture target property. The reference-counted generated setter takes
   * its own user, so a drag-resolved texture needs no extra bookkeeping. */
  bool assign_texture(bContext *C, Tex *tex) const
  {
    /* A pre-existing local texture can never be referenced by a linked brush (and moving it into
     * the library would duplicate its session_uid across undo steps): refuse before pushing any
     * undo step. */
    if (target_ptr_.owner_id &&
        paint_texture_linked_owner_refuses_local_tex(target_ptr_.owner_id, *tex))
    {
      return false;
    }
    PointerRNA target_ptr = target_ptr_;
    RNA_property_pointer_set(&target_ptr, target_prop_, RNA_id_pointer_create(&tex->id), nullptr);
    RNA_property_update(C, &target_ptr, target_prop_);
    ED_paint_texture_assignment_finalize(C, target_ptr_.owner_id, tex, false);
    ED_undo_memfile_push(C, "Assign Texture");
    return true;
  }

  /** Wrap \a image into a #TEX_IMAGE texture (retargeting the slot's own image texture, if it has
   * one) and assign it to the texture target property. */
  bool assign_image_wrapped(bContext *C, Image *image, const bool image_has_extra_user) const
  {
    Tex *tex = ED_paint_texture_property_assign_image(
        C, target_ptr_, target_prop_, image, image_has_extra_user);
    if (!tex) {
      return false;
    }
    ED_undo_memfile_push(C, "Assign Texture");
    return true;
  }

  bool on_drop(bContext *C, const ui::DragInfo &drag_info) const override
  {
    const wmDrag &drag = drag_info.drag_data;

    if (targets_paint_channel()) {
      if (paint_channel_drag_image_count(drag) > 1) {
        return drop_to_channel_assign_operator(C, drag_info);
      }
    }

    /* A texture target takes the dragged #Texture directly when the drag carries one. */
    if (wrap_in_texture_) {
      if (Tex *tex = resolve_drag_texture(C, drag)) {
        return assign_texture(C, tex);
      }
    }

    Main *bmain = CTX_data_main(C);
    Image *image = nullptr;
    /* #BKE_image_load_exists hands out an extra user (it either allocates the ID or calls
     * #id_us_plus), which the property setter does not consume. Released after the assignment, so
     * a freshly loaded image is never left at zero users if the setter refuses the value. */
    bool image_has_extra_user = false;

    if (ID *image_id = WM_drag_get_local_ID_or_import_from_asset(C, &drag, ID_IM)) {
      image = id_cast<Image *>(image_id);
    }
    else if (drag.type == WM_DRAG_ASSET) {
      if (wmDragAsset *asset_drag = WM_drag_get_asset_data(&drag, ID_IM)) {
        image = ed::asset::resolve_image_from_asset(*bmain, *asset_drag->asset);
      }
    }
    else if (drag.type == WM_DRAG_ASSET_LIST) {
      if (const wmDragAssetListItem *item = first_image_item_in_list(drag)) {
        image = item->is_external ?
                    ed::asset::resolve_image_from_asset(
                        *bmain, *item->asset_data.external_info->asset) :
                    id_cast<Image *>(item->asset_data.local_id);
      }
    }
    else if (drag.type == WM_DRAG_PATH) {
      if (targets_paint_channel()) {
        /* The first path is not necessarily the image (an odd multi-file drop on a channel).
         * Single-image drops keep using #WM_drag_get_single_path below. */
        const Vector<std::string> image_paths = drag_image_paths(drag);
        if (!image_paths.is_empty()) {
          image = BKE_image_load_exists(bmain, image_paths[0].c_str(), nullptr);
          image_has_extra_user = image != nullptr;
        }
      }
      else if (const char *path = WM_drag_get_single_path(&drag)) {
        image = BKE_image_load_exists(bmain, path, nullptr);
        image_has_extra_user = image != nullptr;
      }
    }

    if (!image) {
      return false;
    }

    if (wrap_in_texture_) {
      if (assign_image_wrapped(C, image, image_has_extra_user)) {
        return true;
      }
      /* The owner refused (a non-editable brush): the load loan was not consumed. */
      if (image_has_extra_user) {
        id_us_min(&image->id);
      }
      return false;
    }

    PointerRNA target_ptr = target_ptr_;
    RNA_property_pointer_set(
        &target_ptr, target_prop_, RNA_id_pointer_create(&image->id), nullptr);
    if (image_has_extra_user && (RNA_property_flag(target_prop_) & PROP_ID_REFCOUNT)) {
      /* The property setter now owns the real image reference. */
      id_us_min(&image->id);
    }
    RNA_property_update(C, &target_ptr, target_prop_);
    ED_undo_memfile_push(C, "Assign Image");
    return true;
  }
};

/**
 * Resolve the Image pointer property a drop-enabled ID-browser button targets. Buttons opt in
 * through the `id_browser_drop_target` context integer, see #template_id_browser.
 */
static bool image_id_browser_button_target(const ui::Button &but,
                                           PointerRNA *r_ptr,
                                           PropertyRNA **r_prop)
{
  if (ui::button_context_int_get(&but, "id_browser_drop_target").value_or(0) == 0) {
    return false;
  }

  const PointerRNA *target_ptr = ui::button_context_ptr_get(&but, "id_browser_ptr", nullptr);
  std::optional<StringRefNull> prop_name = ui::button_context_string_get(&but, "id_browser_prop");
  /* The labelled empty-slot button inherits the browser context. The assigned-image name button
   * uses the standard template-ID context, which identifies its exact PBR channel. */
  if (!target_ptr || !prop_name) {
    target_ptr = ui::button_context_ptr_get(&but, "template_id_ptr", nullptr);
    prop_name = ui::button_context_string_get(&but, "template_id_prop");
  }
  if (!target_ptr || !target_ptr->data || !prop_name) {
    return false;
  }

  PointerRNA ptr = *target_ptr;
  PropertyRNA *prop = RNA_struct_find_property(&ptr, prop_name->c_str());
  /* Image targets assign the dragged image directly; Texture targets wrap it into an image
   * texture instead (see #ImageIDBrowserDropTarget::wrap_in_texture_). */
  const StructRNA *prop_type = prop ? RNA_property_pointer_type(&ptr, prop) : nullptr;
  if (!prop || RNA_property_type(prop) != PROP_POINTER ||
      (prop_type != RNA_Image && prop_type != RNA_Texture))
  {
    return false;
  }

  *r_ptr = ptr;
  *r_prop = prop;
  return true;
}

std::unique_ptr<ui::DropTargetInterface> image_id_browser_drop_target_get(bContext * /*C*/,
                                                                         const ARegion *region,
                                                                         const wmEvent *event)
{
  PointerRNA target_ptr{};
  PropertyRNA *target_prop = nullptr;
  if (!find_button_at(region, event, [&](const ui::Button &but) {
        return image_id_browser_button_target(but, &target_ptr, &target_prop);
      }))
  {
    return nullptr;
  }
  return std::make_unique<ImageIDBrowserDropTarget>(target_ptr, target_prop);
}

/* -------------------------------------------------------------------- */
/** \name Curve Patch Texture List Drop Target
 * \{ */

/**
 * What a button in the Curve Patch STAMPS texture list (#SCULPT_UL_curve_patch_textures) offers
 * as a drop target: the whole list (append one slot per dropped image) or one row (replace that
 * slot's texture). The Python UI publishes the targets through the `curve_patch_texture_list` /
 * `curve_patch_texture_slot` layout context pointers.
 */
struct CurvePatchTextureListTarget {
  Brush *brush = nullptr;
  /** Slot to replace when the drop landed on a row's button; null to append new slots. */
  BrushCurvePatchTextureSlot *replace_slot = nullptr;
  /** The replace slot as an RNA pointer, for the reference-counted `texture` assignment. */
  PointerRNA replace_slot_ptr{};
};

static bool curve_patch_texture_button_target(const ui::Button &but,
                                              CurvePatchTextureListTarget *r_target)
{
  /* The row's own buttons carry the slot context: the drop replaces that slot's texture. */
  if (const PointerRNA *slot_ptr = ui::button_context_ptr_get(&but,
                                                              "curve_patch_texture_slot",
                                                              nullptr))
  {
    if (slot_ptr->type && RNA_struct_is_a(slot_ptr->type, RNA_BrushCurvePatchTextureSlot) &&
        slot_ptr->owner_id && GS(slot_ptr->owner_id->name) == ID_BR)
    {
      Brush *brush = id_cast<Brush *>(slot_ptr->owner_id);
      BrushCurvePatchTextureSlot *slot = static_cast<BrushCurvePatchTextureSlot *>(slot_ptr->data);
      if (slot != nullptr && BLI_findindex(&brush->curve_patch.texture_slots, slot) != -1) {
        r_target->brush = brush;
        r_target->replace_slot = slot;
        r_target->replace_slot_ptr = *slot_ptr;
        return true;
      }
    }
  }
  /* The list frame and the add/remove buttons carry the list context: the drop appends slots. */
  if (const PointerRNA *list_ptr = ui::button_context_ptr_get(&but,
                                                              "curve_patch_texture_list",
                                                              nullptr))
  {
    if (list_ptr->type && RNA_struct_is_a(list_ptr->type, RNA_BrushCurvePatchSettings) &&
        list_ptr->owner_id && GS(list_ptr->owner_id->name) == ID_BR)
    {
      r_target->brush = id_cast<Brush *>(list_ptr->owner_id);
      r_target->replace_slot = nullptr;
      r_target->replace_slot_ptr = {};
      return true;
    }
  }
  return false;
}

/**
 * Drop target for the Curve Patch STAMPS texture list. A multi-select Asset Browser drag or a
 * multi-file drop appends one slot per image (or takes the dragged texture directly); a drop on a
 * single row replaces that slot's texture.
 */
class CurvePatchTextureListDropTarget : public ui::DropTargetInterface {
  CurvePatchTextureListTarget target_;

  /**
   * The Asset Browser starts a #WM_DRAG_ASSET for the item under the cursor together with a
   * #WM_DRAG_ASSET_LIST carrying the whole selection, and #drop_target_apply_drop() applies
   * whichever of the two comes first. This target takes the whole selection, so the single-asset
   * drag defers to its list sibling when one is present (same as
   * #ImageIDBrowserDropTarget::paint_channel_effective_drag).
   */
  static const wmDrag &effective_drag(const wmDrag &drag)
  {
    return drag_prefer_asset_list_sibling(drag, [](const wmDrag &asset_list_drag) {
      return drag_asset_list_first_of(asset_list_drag, {ID_IM, ID_TE}) != nullptr;
    });
  }

  /** Whether the multi-select drag carries at least one image or texture, without resolving any
   * of them (no imports, no file loads). */
  static bool asset_list_has_assignable_item(const wmDrag &drag)
  {
    return drag_asset_list_first_of(drag, {ID_IM, ID_TE}) != nullptr;
  }

  /** Number of slots the drag would fill: images AND textures in an asset list (a mixed
   * selection appends one slot each), images of a path drag, or a single dragged ID. Tooltip
   * only. */
  static int drag_assignable_count(const wmDrag &drag)
  {
    if (drag.type == WM_DRAG_ASSET_LIST) {
      return drag_asset_list_count_of(drag, ID_IM) + drag_asset_list_count_of(drag, ID_TE);
    }
    if (WM_drag_is_ID_type(&drag, ID_IM) || WM_drag_is_ID_type(&drag, ID_TE)) {
      return 1;
    }
    if (drag.type == WM_DRAG_PATH) {
      int count = 0;
      for (const std::string &path : WM_drag_get_paths(&drag)) {
        if (BLI_path_extension_check_array(path.c_str(), imb_ext_image)) {
          count++;
        }
      }
      return count;
    }
    return 0;
  }

  /** An image and where it came from: freshly loaded ones carry an extra #BKE_image_load_exists
   * user that must be released once the slot's texture holds its own reference. */
  struct ResolvedImage {
    Image *image = nullptr;
    bool has_extra_user = false;
  };

  /** Every image carried by the drag, in drag order: a multi-select asset list, then a local ID,
   * then the image files of a multi-file path drop. */
  static Vector<ResolvedImage> resolve_drag_images(bContext *C, const wmDrag &drag)
  {
    Vector<ResolvedImage> images;
    Main *bmain = CTX_data_main(C);

    if (drag.type == WM_DRAG_ASSET_LIST) {
      const ListBaseT<wmDragAssetListItem> *asset_drags = WM_drag_asset_list_get(&drag);
      if (asset_drags) {
        for (const wmDragAssetListItem &item : *asset_drags) {
          if (drag_asset_list_item_idtype(item) != ID_IM) {
            continue;
          }
          /* #resolve_image_from_asset releases its own load reference, see
           * #ed::asset::resolve_image_from_asset. */
          Image *image = item.is_external ?
                             ed::asset::resolve_image_from_asset(
                                 *bmain, *item.asset_data.external_info->asset) :
                             id_cast<Image *>(item.asset_data.local_id);
          if (image) {
            images.append({image, false});
          }
        }
        if (!images.is_empty()) {
          return images;
        }
      }
    }

    if (ID *id = WM_drag_get_local_ID_or_import_from_asset(C, &drag, ID_IM)) {
      if (GS(id->name) == ID_IM) {
        images.append({id_cast<Image *>(id), false});
      }
      return images;
    }

    if (drag.type == WM_DRAG_PATH) {
      for (const std::string &path : WM_drag_get_paths(&drag)) {
        if (!BLI_path_extension_check_array(path.c_str(), imb_ext_image)) {
          continue;
        }
        if (Image *image = BKE_image_load_exists(bmain, path.c_str(), nullptr)) {
          images.append({image, true});
        }
      }
    }
    return images;
  }

  /** Append one slot holding \a tex. Direct DNA assignment: the new slot is the texture's one
   * user, matching #BKE_brush_curve_patch_texture_slot_remove's bookkeeping. */
  BrushCurvePatchTextureSlot *append_slot_with_texture(Main &bmain, Brush &brush, Tex *tex) const
  {
    BrushCurvePatchTextureSlot *slot = BKE_brush_curve_patch_texture_slot_add(brush);
    slot->tex = tex;
    /* A freshly wrapped texture follows the brush's library; a drag-resolved one is already
     * wherever it lives (a linked brush cannot take a local texture: #can_drop refuses that
     * combination for #WM_DRAG_ID drags and #on_drop rejects it). */
    if (!ID_IS_LINKED(&tex->id)) {
      BKE_id_move_to_same_lib(bmain, tex->id, brush.id);
    }
    return slot;
  }

 public:
  explicit CurvePatchTextureListDropTarget(const CurvePatchTextureListTarget &target)
      : target_(target)
  {
  }

  bool can_drop(bContext & /*C*/, const wmDrag &drag, const char **r_disabled_hint) const override
  {
    /* Asset-editable linked brushes (the Essentials brushes, for instance) are editable the same
     * way the slot add/remove operators allow; other linked data is not. */
    if (target_.brush == nullptr || !paint_texture_target_owner_editable(&target_.brush->id)) {
      *r_disabled_hint = TIP_("Cannot edit linked brush data");
      return false;
    }
    if (WM_drag_is_ID_type(&drag, ID_IM)) {
      return true;
    }
    if (WM_drag_is_ID_type(&drag, ID_TE)) {
      /* A pre-existing local texture can never be referenced by a linked brush; report that
       * instead of a silent no-op in #on_drop. */
      const Tex *tex = drag_local_texture(drag);
      if (tex && paint_texture_linked_owner_refuses_local_tex(&target_.brush->id, *tex)) {
        *r_disabled_hint = TIP_("A linked brush cannot use a local texture");
        return false;
      }
      return true;
    }
    if (drag.type == WM_DRAG_ASSET_LIST) {
      return asset_list_has_assignable_item(drag);
    }
    if (drag.type == WM_DRAG_PATH) {
      for (const std::string &path : WM_drag_get_paths(&drag)) {
        if (BLI_path_extension_check_array(path.c_str(), imb_ext_image)) {
          return true;
        }
      }
      return false;
    }
    return false;
  }

  std::string drop_tooltip(const ui::DragInfo &drag_info) const override
  {
    const wmDrag &drag = effective_drag(drag_info.drag_data);

    /* The dragged item's name is already drawn as the drag's own label, so it is not repeated. */
    if (target_.replace_slot) {
      return TIP_("Replace this slot's texture");
    }

    const int count = drag_assignable_count(drag);
    if (count > 1) {
      return fmt::format(fmt::runtime(TIP_("Add {} texture slots")), count);
    }
    return TIP_("Assign to a new texture slot");
  }

  bool on_drop(bContext *C, const ui::DragInfo &drag_info) const override
  {
    const wmDrag &drag = effective_drag(drag_info.drag_data);
    Brush *brush = target_.brush;
    if (brush == nullptr || !paint_texture_target_owner_editable(&brush->id)) {
      return false;
    }
    Main *bmain = CTX_data_main(C);

    /* A dragged #Texture replaces (or fills one new slot with) itself directly. A pre-existing
     * local texture can never be referenced by a linked brush (and moving it into the library
     * would duplicate its session_uid across undo steps), so those combinations do nothing. */
    if (Tex *tex = resolve_drag_texture(C, drag)) {
      if (paint_texture_linked_owner_refuses_local_tex(&brush->id, *tex)) {
        return false;
      }
      if (target_.replace_slot) {
        PointerRNA slot_ptr = target_.replace_slot_ptr;
        PropertyRNA *prop = RNA_struct_find_property(&slot_ptr, "texture");
        if (!prop) {
          return false;
        }
        RNA_property_pointer_set(&slot_ptr, prop, RNA_id_pointer_create(&tex->id), nullptr);
        RNA_property_update(C, &slot_ptr, prop);
        ED_paint_texture_assignment_finalize(C, &brush->id, tex, false);
        ED_undo_memfile_push(C, "Replace Curve Patch Texture");
        return true;
      }
      append_slot_with_texture(*bmain, *brush, tex);
      id_us_plus(&tex->id);
      /* The texture itself already existed (a drag-resolved data-block); the new slot is what was
       * added, which the #NC_BRUSH notifier in the finalize covers. */
      ED_paint_texture_assignment_finalize(C, &brush->id, tex, false);
      ED_undo_memfile_push(C, "Add Curve Patch Texture");
      return true;
    }

    const Vector<ResolvedImage> images = resolve_drag_images(C, drag);
    if (images.is_empty()) {
      return false;
    }

    if (target_.replace_slot) {
      const ResolvedImage &resolved = images[0];
      PointerRNA slot_ptr = target_.replace_slot_ptr;
      PropertyRNA *prop = RNA_struct_find_property(&slot_ptr, "texture");
      if (!prop) {
        return false;
      }
      Tex *tex = ED_paint_texture_property_assign_image(
          C, slot_ptr, prop, resolved.image, resolved.has_extra_user);
      if (!tex) {
        return false;
      }
      ED_undo_memfile_push(C, "Replace Curve Patch Texture");
      return true;
    }

    Vector<Tex *> added_textures;
    for (const ResolvedImage &resolved : images) {
      /* Prepares the texture (and its image) in the brush's library, handling asset-editable
       * linked brushes; the new slot is the texture's one user (#ED_paint_texture_wrap_image_
       * for_owner hands it back with exactly that one user, no extra reference to take). */
      Tex *tex = ED_paint_texture_wrap_image_for_owner(
          bmain, nullptr, &brush->id, resolved.image, resolved.has_extra_user);
      BrushCurvePatchTextureSlot *slot = BKE_brush_curve_patch_texture_slot_add(*brush);
      slot->tex = tex;
      /* The preview is refreshed once for the whole batch below: per-texture updates kill
       * running preview jobs and rebuild dependency relations, cancelling each other out. */
      ED_paint_texture_assignment_finalize(
          C, &brush->id, tex, true, /*refresh_preview=*/false);
      added_textures.append(tex);
    }
    DROP_IMAGE_update_textures_preview_batch(C, bmain, added_textures);
    ED_undo_memfile_push(C, images.size() > 1 ? "Add Curve Patch Textures" :
                                               "Add Curve Patch Texture");
    return true;
  }
};

std::unique_ptr<ui::DropTargetInterface> curve_patch_texture_list_drop_target_get(
    bContext * /*C*/, const ARegion *region, const wmEvent *event)
{
  CurvePatchTextureListTarget target;
  if (!find_button_at(region, event, [&](const ui::Button &but) {
        return curve_patch_texture_button_target(but, &target);
      }))
  {
    return nullptr;
  }
  return std::make_unique<CurvePatchTextureListDropTarget>(target);
}

/** \} */

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
  const bool can_drop = target->can_drop(*C, *drag, &disabled_hint);
  /* Publish the reason a blocked drop is blocked (a linked brush, ...), same as
   * #view_drop_poll. */
  if (disabled_hint) {
    drag->drop_state.disabled_info = disabled_hint;
  }
  return can_drop;
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
