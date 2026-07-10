/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Texture drop functionality for UI elements.
 * Handles drag and drop operations for textures and images onto UI buttons.
 */

#include <cstdio>
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
#include "BKE_texture.h"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"

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
 * Identify whether \a but targets one of the active brush's texture slots and, if so, which one.
 *
 * The slot is drawn with `template_ID_preview(brush.texture_slot, "texture")`, whose droppable
 * widget is a *block* button that carries no RNA pointer/property of its own. Instead the owner
 * pointer and property name live in its context store, keyed `"template_id_ptr"` /
 * `"template_id_prop"` (mirrors #context_active_but_prop_get_templateID). Primary vs mask is told
 * apart by matching the slot data against the brush's `mtex` / `mask_mtex`.
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
  const PointerRNA *slot_ptr = CTX_store_ptr_lookup(but->context, "template_id_ptr");
  const std::optional<StringRefNull> prop_name = CTX_store_string_lookup(but->context,
                                                                         "template_id_prop");
  if (!slot_ptr || !prop_name) {
    return false;
  }
  /* The owner must be a texture slot (#BrushTextureSlot derives from #TextureSlot). */
  if (!slot_ptr->type || !RNA_struct_is_a(slot_ptr->type, RNA_TextureSlot)) {
    return false;
  }
  /* ... and the property must be a pointer to a #Texture. */
  PointerRNA ptr_copy = *slot_ptr;
  PropertyRNA *prop = RNA_struct_find_property(&ptr_copy, prop_name->c_str());
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
static bool DROP_IMAGE_set_preview_for_drag(wmDrag *drag, int max_size)
{
  if (!drag) {
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
      DROP_IMAGE_set_preview_for_drag(drag, 128);
    }
  }
  else if (drag->type == WM_DRAG_ID) {
    ID *id = WM_drag_get_local_ID(drag, ID_IM);
    if (id) {
      DROP_IMAGE_set_preview_for_drag(drag, 128);
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

 public:
  BrushTextureSlotDropTarget(bool use_mask_slot) : use_mask_slot_(use_mask_slot) {}

  bool can_drop(const wmDrag &drag, const char ** /*r_disabled_hint*/) const override
  {
    /* Local image data-block or image asset (imported on drop). */
    if (WM_drag_is_ID_type(&drag, ID_IM)) {
      return true;
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
    /* Resolve a local image, importing the asset first if the drag came from the asset browser. */
    if (const ID *image_id = WM_drag_get_local_ID_or_import_from_asset(C, &drag, ID_IM)) {
      RNA_int_set(&props, "session_uid", int(image_id->session_uid));
    }
    else if (drag.type == WM_DRAG_PATH) {
      if (const char *path = WM_drag_get_single_path(&drag)) {
        RNA_string_set(&props, "filepath", path);
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
  const ui::Button *but = ui::but_find_mouse_over(region, event);
  if (!but) {
    return nullptr;
  }
  bool use_mask_slot = false;
  if (!determine_texture_slot_type(but, brush, &use_mask_slot)) {
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
  return target->can_drop(*drag, &disabled_hint);
}

static std::string brush_texture_drop_tooltip(bContext *C,
                                              wmDrag *drag,
                                              const int /*xy*/[2],
                                              wmDropBox * /*drop*/)
{
  const ARegion *region = CTX_wm_region(C);
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
  return ui::drop_target_tooltip(*region, *target, *drag, *event);
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
   * Interface" map lives in #SPACE_EMPTY and is never tagged visible, so it would never fire.
   * A global handler triggers unconditionally when a path drag starts - including files dragged in
   * from the OS - so the texture preview shows as soon as the cursor enters the window. */
  WM_drag_global_prefetch_handler_add(WM_DRAG_PATH, [](bContext &C, wmDrag &drag) {
    DROP_IMAGE_drag_start_callback(&C, &drag);
  });
}

}