/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 */

#include "BKE_context.hh"
#include "BKE_idtype.hh"
#include "BKE_linestyle.h"
#include "BKE_scene.hh"

#include "BLI_listbase.hh"
#include "BLI_string_utf8.hh"

#include "BLT_translation.hh"

#include "MEM_guardedalloc.h"

#include "DNA_brush_types.h"
#include "DNA_light_types.h"
#include "DNA_material_types.h"
#include "DNA_texture_types.h"
#include "DNA_world_types.h"

#include <cstdlib>  /* For rand() */

#include "GPU_immediate.hh"
#include "GPU_state.hh"

#include "ED_render.hh"
#include "ED_screen.hh"

#include "RNA_access.hh"

#include "WM_api.hh"

#include "UI_interface.hh"
#include "UI_interface_layout.hh"

#include "interface_template_preview_brush.hh"

namespace blender::ui {

void template_preview(Layout *layout,
                      bContext *C,
                      ID *id,
                      bool show_buttons,
                      ID *parent,
                      MTex *slot,
                      const char *preview_id)
{
  Material *ma = nullptr;
  short *pr_texture = nullptr;

  char _preview_id[sizeof(uiPreview::preview_id)];

  if (id && !ELEM(GS(id->name), ID_MA, ID_TE, ID_WO, ID_LA, ID_LS)) {
    RNA_warning("Expected ID of type material, texture, light, world or line style");
    return;
  }

  /* decide what to render */
  ID *pid = id;
  ID *pparent = nullptr;

  if (id && (GS(id->name) == ID_TE)) {
    if (parent && (GS(parent->name) == ID_MA)) {
      pr_texture = &(id_cast<Material *>(parent))->pr_texture;
    }
    else if (parent && (GS(parent->name) == ID_WO)) {
      pr_texture = &(id_cast<World *>(parent))->pr_texture;
    }
    else if (parent && (GS(parent->name) == ID_LA)) {
      pr_texture = &(id_cast<Light *>(parent))->pr_texture;
    }
    else if (parent && (GS(parent->name) == ID_LS)) {
      pr_texture = &(id_cast<FreestyleLineStyle *>(parent))->pr_texture;
    }

    if (pr_texture) {
      if (*pr_texture == TEX_PR_OTHER) {
        pid = parent;
      }
      else if (*pr_texture == TEX_PR_BOTH) {
        pparent = parent;
      }
    }
  }

  if (!preview_id || (preview_id[0] == '\0')) {
    /* If no identifier given, generate one from ID type. */
    SNPRINTF_UTF8(_preview_id, "uiPreview_%s", BKE_idtype_idcode_to_name(GS(id->name)));
    preview_id = _preview_id;
  }

  /* Find or add the uiPreview to the current Region. */
  ARegion *region = CTX_wm_region(C);
  uiPreview *ui_preview = static_cast<uiPreview *>(
      BLI_findstring(&region->ui_previews, preview_id, offsetof(uiPreview, preview_id)));

  if (!ui_preview) {
    ui_preview = MEM_new<uiPreview>(__func__);
    STRNCPY_UTF8(ui_preview->preview_id, preview_id);
    ui_preview->height = short(UI_UNIT_Y * 7.6f);
    ui_preview->id_session_uid = pid->session_uid;
    ui_preview->tag = UI_PREVIEW_TAG_DIRTY;
    BLI_addtail(&region->ui_previews, ui_preview);
  }
  else if (ui_preview->id_session_uid != pid->session_uid) {
    ui_preview->id_session_uid = pid->session_uid;
    ui_preview->tag |= UI_PREVIEW_TAG_DIRTY;
  }

  if (ui_preview->height < UI_UNIT_Y) {
    ui_preview->height = UI_UNIT_Y;
  }
  else if (ui_preview->height > UI_UNIT_Y * 50) { /* Rather high upper limit, yet not insane! */
    ui_preview->height = UI_UNIT_Y * 50;
  }

  /* layout */
  Block *block = layout->block();
  Layout *row = &layout->row(false);
  Layout *col = &row->column(false);

  /* add preview */
  uiDefBut(
      block, ButtonType::Extra, "", 0, 0, UI_UNIT_X * 10, ui_preview->height, pid, 0.0, 0.0, "");
  button_func_drawextra_set(block,
                            [pid, pparent, slot, ui_preview](const bContext *C, rcti *rect) {
                              ED_preview_draw(C, pid, pparent, slot, ui_preview, rect);
                            });
  uiDefIconButV(block,
                ButtonType::Grip,
                ICON_GRIP,
                0,
                0,
                UI_UNIT_X * 10,
                short(UI_UNIT_Y * 0.3f),
                &ui_preview->height,
                UI_UNIT_Y,
                UI_UNIT_Y * 50.0f,
                "");

  /* add buttons */
  if (pid && show_buttons) {
    if (GS(pid->name) == ID_MA || (pparent && GS(pparent->name) == ID_MA)) {
      if (GS(pid->name) == ID_MA) {
        ma = id_cast<Material *>(pid);
      }
      else {
        ma = id_cast<Material *>(pparent);
      }

      /* Create RNA Pointer */
      PointerRNA material_ptr = RNA_id_pointer_create(&ma->id);

      col = &row->column(true);
      col->scale_x_set(1.5);
      col->prop(&material_ptr, "preview_render_type", ITEM_R_EXPAND, "", ICON_NONE);

      /* EEVEE preview file has baked lighting so use_preview_world has no effect,
       * just hide the option until this feature is supported. */
      if (!BKE_scene_uses_blender_eevee(CTX_data_scene(C))) {
        col->separator();
        col->prop(&material_ptr, "use_preview_world", UI_ITEM_NONE, "", ICON_WORLD);
      }
    }

    if (pr_texture) {
      /* Create RNA Pointer */
      PointerRNA texture_ptr = RNA_id_pointer_create(id);

      layout->row(true);
      Button *but = uiDefButV(block,
                              ButtonType::Row,
                              IFACE_("Texture"),
                              0,
                              0,
                              UI_UNIT_X * 10,
                              UI_UNIT_Y,
                              pr_texture,
                              10,
                              TEX_PR_TEXTURE,
                              "");
      button_func_set(but, [](bContext &C) {
        WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING_PREVIEW, nullptr);
      });
      if (GS(parent->name) == ID_MA) {
        but = uiDefButV(block,
                        ButtonType::Row,
                        IFACE_("Material"),
                        0,
                        0,
                        UI_UNIT_X * 10,
                        UI_UNIT_Y,
                        pr_texture,
                        10,
                        TEX_PR_OTHER,
                        "");
        button_func_set(but, [](bContext &C) {
          WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING_PREVIEW, nullptr);
        });
      }
      else if (GS(parent->name) == ID_LA) {
        but = uiDefButV(block,
                        ButtonType::Row,
                        CTX_IFACE_(BLT_I18NCONTEXT_ID_LIGHT, "Light"),
                        0,
                        0,
                        UI_UNIT_X * 10,
                        UI_UNIT_Y,
                        pr_texture,
                        10,
                        TEX_PR_OTHER,
                        "");
        button_func_set(but, [](bContext &C) {
          WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING_PREVIEW, nullptr);
        });
      }
      else if (GS(parent->name) == ID_WO) {
        but = uiDefButV(block,
                        ButtonType::Row,
                        CTX_IFACE_(BLT_I18NCONTEXT_ID_WORLD, "World"),
                        0,
                        0,
                        UI_UNIT_X * 10,
                        UI_UNIT_Y,
                        pr_texture,
                        10,
                        TEX_PR_OTHER,
                        "");
        button_func_set(but, [](bContext &C) {
          WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING_PREVIEW, nullptr);
        });
      }
      else if (GS(parent->name) == ID_LS) {
        but = uiDefButV(block,
                        ButtonType::Row,
                        IFACE_("Line Style"),
                        0,
                        0,
                        UI_UNIT_X * 10,
                        UI_UNIT_Y,
                        pr_texture,
                        10,
                        TEX_PR_OTHER,
                        "");
        button_func_set(but, [](bContext &C) {
          WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING_PREVIEW, nullptr);
        });
      }
      but = uiDefButV(block,
                      ButtonType::Row,
                      IFACE_("Both"),
                      0,
                      0,
                      UI_UNIT_X * 10,
                      UI_UNIT_Y,
                      pr_texture,
                      10,
                      TEX_PR_BOTH,
                      "");
      button_func_set(but, [](bContext &C) {
        WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING_PREVIEW, nullptr);
      });

      /* Alpha button for texture preview */
      if (*pr_texture != TEX_PR_OTHER) {
        row = &layout->row(false);
        row->prop(&texture_ptr, "use_preview_alpha", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      }
    }
  }
}


void template_brush_stroke_preview(Layout *layout,
                                   bContext *C,
                                   PointerRNA *brush_ptr,
                                   float angle,
                                   float spacing,
                                   const char *preview_id,
                                   bool show_grip)
{
  char _preview_id[sizeof(uiPreview::preview_id)];

  if (!brush_ptr || !brush_ptr->data) {
    RNA_warning("Expected valid brush pointer for brush stroke preview");
    return;
  }

  if (!preview_id || (preview_id[0] == '\0')) {
    SNPRINTF_UTF8(_preview_id, "uiBrushStrokePreview_%p", brush_ptr->data);
    preview_id = _preview_id;
  }

  ARegion *region = CTX_wm_region(C);
  uiPreview *ui_preview = static_cast<uiPreview *>(
      BLI_findstring(&region->ui_previews, preview_id, offsetof(uiPreview, preview_id)));

  if (!ui_preview) {
    ui_preview = MEM_new<uiPreview>(__func__);
    STRNCPY_UTF8(ui_preview->preview_id, preview_id);
    ui_preview->height = short(UI_UNIT_Y * 4.0f);
    ui_preview->tag = UI_PREVIEW_TAG_DIRTY;
    BLI_addtail(&region->ui_previews, ui_preview);
  }
  else {
    ui_preview->tag |= UI_PREVIEW_TAG_DIRTY;
  }

  if (ui_preview->height < UI_UNIT_Y) {
    ui_preview->height = UI_UNIT_Y;
  }
  else if (ui_preview->height > UI_UNIT_Y * 10) {
    ui_preview->height = UI_UNIT_Y * 10;
  }

  Block *block = layout->block();
  Layout *col = &layout->column(false);

  /* The preview button reads the current brush parameters at draw time. Both `angle` and
   * `spacing` are captured by value: the panel is rebuilt (and this template re-evaluated) on
   * every brush property change, so the captured snapshot always reflects the latest values. */
  void *brush_data = brush_ptr->data;
  uiDefBut(block,
           ButtonType::Extra,
           "",
           0,
           0,
           UI_UNIT_X * 50,
           ui_preview->height,
           brush_data,
           0.0,
           0.0,
           "Brush Stroke Preview");
  button_func_drawextra_set(block, [brush_data, angle, spacing](const bContext *C, rcti *rect) {
    ED_brush_stroke_preview_draw(C, brush_data, angle, spacing, rect);
  });

  if (show_grip) {
    uiDefIconButV(block,
                  ButtonType::Grip,
                  ICON_GRIP,
                  0,
                  0,
                  UI_UNIT_X * 20,
                  short(UI_UNIT_Y * 0.3f),
                  &ui_preview->height,
                  UI_UNIT_Y * 4.0f,
                  UI_UNIT_Y * 5.0f,
                  "Resize brush stroke preview");
  }

  Layout *row = &col->row(true);

  PointerRNA texture_slot_ptr = RNA_pointer_get(brush_ptr, "texture_slot");
  if (!RNA_pointer_is_null(&texture_slot_ptr)) {
    bool use_random = RNA_boolean_get(&texture_slot_ptr, "use_random");
    if (use_random) {
      row->prop(&texture_slot_ptr, "random_angle", UI_ITEM_NONE, "Random Angle", ICON_NONE);
    }
    else {
      row->prop(&texture_slot_ptr, "angle", UI_ITEM_NONE, "Angle", ICON_NONE);
    }
  }
  row->prop(brush_ptr, "spacing", UI_ITEM_NONE, "Spacing", ICON_NONE);
}

}  // namespace blender::ui
