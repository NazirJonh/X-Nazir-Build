/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>

#include "MEM_guardedalloc.h"

#include "BLI_ghash.h"
#include "BLI_listbase.h"
#include "BLI_math_color.h"
#include "BLI_math_vector.h"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "AS_asset_library.hh"
#include "AS_asset_representation.hh"

#include "IMB_interp.hh"

#include "DNA_brush_types.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_object_enums.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_space_types.h"

#include "BKE_asset_edit.hh"
#include "BKE_brush.hh"
#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_paint.hh"
#include "BKE_paint_material_sync.hh"
#include "BKE_paint_types.hh"
#include "BKE_report.hh"

#include "ED_asset_image_utils.hh"
#include "ED_asset_list.hh"
#include "ED_asset_menu_utils.hh"
#include "ED_image.hh"
#include "ED_paint.hh"
#include "ED_screen.hh"

#include "WM_api.hh"
#include "WM_keymap.hh"
#include "WM_toolsystem.hh"
#include "WM_types.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"
#include "RNA_prototypes.hh"

#include "IMB_colormanagement.hh"

#include "paint_curve_intern.hh"
#include "paint_curve_patch_edit_intern.hh"
#include "paint_image_curve_patch_edit.hh"
#include "paint_intern.hh"

#include "curves/sculpt_intern.hh"
#include "mesh/paint_hide.hh"
#include "mesh/paint_mask.hh"
#include "mesh/paint_material_attribute.hh"
#include "mesh/sculpt_intern.hh"

namespace blender {

static wmOperatorStatus brush_scale_size_exec(bContext *C, wmOperator *op)
{
  Paint *paint = BKE_paint_get_active_from_context(C);
  Brush *brush = BKE_paint_brush(paint);
  float scalar = RNA_float_get(op->ptr, "scalar");

  /* Grease Pencil brushes in Paint mode do not use unified size. */
  const bool use_unified_size = !(brush && brush->gpencil_settings &&
                                  brush->ob_mode == OB_MODE_PAINT_GREASE_PENCIL);

  if (brush) {
    /* Pixel diameter. */
    {
      const int old_size = (use_unified_size) ? BKE_brush_size_get(paint, brush) : brush->size;
      int size = int(scalar * old_size);

      if (abs(old_size - size) < U.pixelsize) {
        if (scalar > 1) {
          size += U.pixelsize;
        }
        else if (scalar < 1) {
          size -= U.pixelsize;
        }
      }

      if (use_unified_size) {
        BKE_brush_size_set(paint, brush, size);
      }
      else {
        brush->size = max_ii(size, 1);
        BKE_brush_tag_unsaved_changes(brush);
      }
    }

    /* Unprojected diameter. */
    {
      float unprojected_size = scalar * (use_unified_size ?
                                             BKE_brush_unprojected_size_get(paint, brush) :
                                             brush->unprojected_size);

      unprojected_size = std::max(unprojected_size, 0.001f);

      if (use_unified_size) {
        BKE_brush_unprojected_size_set(paint, brush, unprojected_size);
      }
      else {
        brush->unprojected_size = unprojected_size;
        BKE_brush_tag_unsaved_changes(brush);
      }
    }

    WM_main_add_notifier(NC_BRUSH | NA_EDITED, brush);
  }

  return OPERATOR_FINISHED;
}

static void BRUSH_OT_scale_size(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Scale Sculpt/Paint Brush Size";
  ot->description = "Change brush size by a scalar";
  ot->idname = "BRUSH_OT_scale_size";

  /* API callbacks. */
  ot->exec = brush_scale_size_exec;

  /* flags */
  ot->flag = 0;

  RNA_def_float(ot->srna, "scalar", 1, 0, 2, "Scalar", "Factor to scale brush size by", 0, 2);
}

/***** Stencil Control *****/

enum StencilControlMode {
  STENCIL_TRANSLATE,
  STENCIL_SCALE,
  STENCIL_ROTATE,
};

enum StencilTextureMode {
  STENCIL_PRIMARY = 0,
  STENCIL_SECONDARY = 1,
};

enum StencilConstraint {
  STENCIL_CONSTRAINT_X = 1,
  STENCIL_CONSTRAINT_Y = 2,
};

struct StencilControlData {
  float init_mouse[2];
  float init_spos[2];
  float init_sdim[2];
  float init_rot;
  float init_angle;
  float lenorig;
  float area_size[2];
  StencilControlMode mode;
  StencilConstraint constrain_mode;
  /** We are tweaking mask or color stencil. */
  int mask;
  Brush *br;
  float *dim_target;
  float *rot_target;
  float *pos_target;
  short launch_event;
};

static bool brush_primary_stencil_mapping(const Brush *br)
{
  if (br == nullptr) {
    return false;
  }
  if (br->mtex.brush_map_mode == MTEX_MAP_MODE_STENCIL) {
    return true;
  }
  return br->material_paint != nullptr &&
         br->material_paint->shared_source_mapping.brush_map_mode == MTEX_MAP_MODE_STENCIL;
}

static void stencil_set_target(StencilControlData *scd)
{
  Brush *br = scd->br;
  float mdiff[2];
  if (scd->mask) {
    copy_v2_v2(scd->init_sdim, br->mask_stencil_dimension);
    copy_v2_v2(scd->init_spos, br->mask_stencil_pos);
    scd->init_rot = br->mask_mtex.rot;

    scd->dim_target = br->mask_stencil_dimension;
    scd->rot_target = &br->mask_mtex.rot;
    scd->pos_target = br->mask_stencil_pos;

    sub_v2_v2v2(mdiff, scd->init_mouse, br->mask_stencil_pos);
  }
  else if (br->material_paint != nullptr &&
           br->material_paint->shared_source_mapping.brush_map_mode == MTEX_MAP_MODE_STENCIL &&
           br->mtex.brush_map_mode != MTEX_MAP_MODE_STENCIL)
  {
    /* PBR sources share mapping; position/scale live on #Brush.stencil_* (same fields 3D
     * #DirectSampleLayout reads) while rotation is #shared_source_mapping.rot. */
    copy_v2_v2(scd->init_sdim, br->stencil_dimension);
    copy_v2_v2(scd->init_spos, br->stencil_pos);
    scd->init_rot = br->material_paint->shared_source_mapping.rot;

    scd->dim_target = br->stencil_dimension;
    scd->rot_target = &br->material_paint->shared_source_mapping.rot;
    scd->pos_target = br->stencil_pos;

    sub_v2_v2v2(mdiff, scd->init_mouse, br->stencil_pos);
  }
  else {
    copy_v2_v2(scd->init_sdim, br->stencil_dimension);
    copy_v2_v2(scd->init_spos, br->stencil_pos);
    scd->init_rot = br->mtex.rot;

    scd->dim_target = br->stencil_dimension;
    scd->rot_target = &br->mtex.rot;
    scd->pos_target = br->stencil_pos;

    sub_v2_v2v2(mdiff, scd->init_mouse, br->stencil_pos);
  }

  scd->lenorig = len_v2(mdiff);

  scd->init_angle = atan2f(mdiff[1], mdiff[0]);
}

static wmOperatorStatus stencil_control_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  Paint *paint = BKE_paint_get_active_from_context(C);
  Brush *br = BKE_paint_brush(paint);
  const float mvalf[2] = {float(event->mval[0]), float(event->mval[1])};
  ARegion *region = CTX_wm_region(C);
  StencilControlData *scd;
  int mask = RNA_enum_get(op->ptr, "texmode");

  if (mask) {
    if (br->mask_mtex.brush_map_mode != MTEX_MAP_MODE_STENCIL) {
      return OPERATOR_CANCELLED;
    }
  }
  else {
    if (!brush_primary_stencil_mapping(br)) {
      return OPERATOR_CANCELLED;
    }
  }

  scd = MEM_new_uninitialized<StencilControlData>(__func__);
  scd->mask = mask;
  scd->br = br;

  copy_v2_v2(scd->init_mouse, mvalf);

  stencil_set_target(scd);

  scd->mode = StencilControlMode(RNA_enum_get(op->ptr, "mode"));
  scd->launch_event = WM_userdef_event_type_from_keymap_type(event->type);
  scd->area_size[0] = region->winx;
  scd->area_size[1] = region->winy;

  op->customdata = scd;
  WM_event_add_modal_handler(C, op);

  return OPERATOR_RUNNING_MODAL;
}

static void stencil_restore(StencilControlData *scd)
{
  copy_v2_v2(scd->dim_target, scd->init_sdim);
  copy_v2_v2(scd->pos_target, scd->init_spos);
  *scd->rot_target = scd->init_rot;
}

static void stencil_control_cancel(bContext * /*C*/, wmOperator *op)
{
  StencilControlData *scd = static_cast<StencilControlData *>(op->customdata);
  stencil_restore(scd);
  MEM_delete(scd);
}

static void stencil_control_calculate(StencilControlData *scd, const int mval[2])
{
#define PIXEL_MARGIN 5

  float mdiff[2];
  const float mvalf[2] = {float(mval[0]), float(mval[1])};
  switch (scd->mode) {
    case STENCIL_TRANSLATE:
      sub_v2_v2v2(mdiff, mvalf, scd->init_mouse);
      add_v2_v2v2(scd->pos_target, scd->init_spos, mdiff);
      CLAMP(scd->pos_target[0],
            -scd->dim_target[0] + PIXEL_MARGIN,
            scd->area_size[0] + scd->dim_target[0] - PIXEL_MARGIN);

      CLAMP(scd->pos_target[1],
            -scd->dim_target[1] + PIXEL_MARGIN,
            scd->area_size[1] + scd->dim_target[1] - PIXEL_MARGIN);
      BKE_brush_tag_unsaved_changes(scd->br);

      break;
    case STENCIL_SCALE: {
      float len, factor;
      sub_v2_v2v2(mdiff, mvalf, scd->pos_target);
      len = len_v2(mdiff);
      factor = len / scd->lenorig;
      copy_v2_v2(mdiff, scd->init_sdim);
      if (scd->constrain_mode != STENCIL_CONSTRAINT_Y) {
        mdiff[0] = factor * scd->init_sdim[0];
      }
      if (scd->constrain_mode != STENCIL_CONSTRAINT_X) {
        mdiff[1] = factor * scd->init_sdim[1];
      }
      clamp_v2(mdiff, 5.0f, 10000.0f);
      copy_v2_v2(scd->dim_target, mdiff);
      BKE_brush_tag_unsaved_changes(scd->br);
      break;
    }
    case STENCIL_ROTATE: {
      float angle;
      sub_v2_v2v2(mdiff, mvalf, scd->pos_target);
      angle = atan2f(mdiff[1], mdiff[0]);
      angle = scd->init_rot + angle - scd->init_angle;
      if (angle < 0.0f) {
        angle += float(2 * M_PI);
      }
      if (angle > float(2 * M_PI)) {
        angle -= float(2 * M_PI);
      }
      *scd->rot_target = angle;
      BKE_brush_tag_unsaved_changes(scd->br);
      break;
    }
  }
#undef PIXEL_MARGIN
}

static wmOperatorStatus stencil_control_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  StencilControlData *scd = static_cast<StencilControlData *>(op->customdata);

  if (event->type == scd->launch_event && event->val == KM_RELEASE) {
    MEM_delete(scd);
    WM_event_add_notifier(C, NC_WINDOW, nullptr);
    return OPERATOR_FINISHED;
  }

  switch (event->type) {
    case MOUSEMOVE:
      stencil_control_calculate(scd, event->mval);
      break;
    case EVT_ESCKEY:
      if (event->val == KM_PRESS) {
        stencil_control_cancel(C, op);
        WM_event_add_notifier(C, NC_WINDOW, nullptr);
        return OPERATOR_CANCELLED;
      }
      break;
    case EVT_XKEY:
      if (event->val == KM_PRESS) {

        if (scd->constrain_mode == STENCIL_CONSTRAINT_X) {
          scd->constrain_mode = StencilConstraint(0);
        }
        else {
          scd->constrain_mode = STENCIL_CONSTRAINT_X;
        }

        stencil_control_calculate(scd, event->mval);
      }
      break;
    case EVT_YKEY:
      if (event->val == KM_PRESS) {
        if (scd->constrain_mode == STENCIL_CONSTRAINT_Y) {
          scd->constrain_mode = StencilConstraint(0);
        }
        else {
          scd->constrain_mode = STENCIL_CONSTRAINT_Y;
        }

        stencil_control_calculate(scd, event->mval);
      }
      break;
    default:
      break;
  }

  ED_region_tag_redraw(CTX_wm_region(C));

  return OPERATOR_RUNNING_MODAL;
}

static bool stencil_control_poll(bContext *C)
{
  PaintMode mode = BKE_paintmode_get_active_from_context(C);

  Paint *paint;
  Brush *br;

  if (!ed::sculpt_paint::paint_supports_texture(mode)) {
    return false;
  }

  paint = BKE_paint_get_active_from_context(C);
  br = BKE_paint_brush(paint);
  return (br && (brush_primary_stencil_mapping(br) ||
                 br->mask_mtex.brush_map_mode == MTEX_MAP_MODE_STENCIL));
}

static void BRUSH_OT_stencil_control(wmOperatorType *ot)
{
  static const EnumPropertyItem stencil_control_items[] = {
      {STENCIL_TRANSLATE, "TRANSLATION", 0, "Translation", ""},
      {STENCIL_SCALE, "SCALE", 0, "Scale", ""},
      {STENCIL_ROTATE, "ROTATION", 0, "Rotation", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const EnumPropertyItem stencil_texture_items[] = {
      {STENCIL_PRIMARY, "PRIMARY", 0, "Primary", ""},
      {STENCIL_SECONDARY, "SECONDARY", 0, "Secondary", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };
  /* identifiers */
  ot->name = "Stencil Brush Control";
  ot->description = "Control the stencil brush";
  ot->idname = "BRUSH_OT_stencil_control";

  /* API callbacks. */
  ot->invoke = stencil_control_invoke;
  ot->modal = stencil_control_modal;
  ot->cancel = stencil_control_cancel;
  ot->poll = stencil_control_poll;

  /* flags */
  ot->flag = 0;

  PropertyRNA *prop;
  prop = RNA_def_enum(ot->srna, "mode", stencil_control_items, STENCIL_TRANSLATE, "Tool", "");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
  prop = RNA_def_enum(ot->srna, "texmode", stencil_texture_items, STENCIL_PRIMARY, "Tool", "");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
}

static wmOperatorStatus stencil_fit_image_aspect_exec(bContext *C, wmOperator *op)
{
  Paint *paint = BKE_paint_get_active_from_context(C);
  Brush *br = BKE_paint_brush(paint);
  bool use_scale = RNA_boolean_get(op->ptr, "use_scale");
  bool use_repeat = RNA_boolean_get(op->ptr, "use_repeat");
  bool do_mask = RNA_boolean_get(op->ptr, "mask");
  Tex *tex = nullptr;
  MTex *mtex = nullptr;
  MTex material_mtex_storage = {};
  if (br) {
    if (do_mask) {
      mtex = &br->mask_mtex;
    }
    else if (br->mtex.tex != nullptr) {
      mtex = &br->mtex;
    }
    else if (br->material_paint != nullptr) {
      const PaintModeSettings &mode_settings = CTX_data_tool_settings(C)->paint_mode;
      if (BKE_paint_material_preview_mtex_get(*br->material_paint,
                                              mode_settings,
                                              paint->visible_material_channels,
                                              material_mtex_storage))
      {
        mtex = &material_mtex_storage;
      }
    }
    tex = mtex != nullptr ? mtex->tex : nullptr;
  }

  if (tex && tex->type == TEX_IMAGE && tex->ima) {
    float aspx, aspy;
    Image *ima = tex->ima;
    float orig_area, stencil_area, factor;
    ED_image_get_uv_aspect(ima, nullptr, &aspx, &aspy);

    if (use_scale) {
      aspx *= mtex->size[0];
      aspy *= mtex->size[1];
    }

    if (use_repeat && tex->extend == TEX_REPEAT) {
      aspx *= tex->xrepeat;
      aspy *= tex->yrepeat;
    }

    orig_area = fabsf(aspx * aspy);

    if (do_mask) {
      stencil_area = fabsf(br->mask_stencil_dimension[0] * br->mask_stencil_dimension[1]);
    }
    else {
      stencil_area = fabsf(br->stencil_dimension[0] * br->stencil_dimension[1]);
    }

    factor = sqrtf(stencil_area / orig_area);

    if (do_mask) {
      br->mask_stencil_dimension[0] = fabsf(factor * aspx);
      br->mask_stencil_dimension[1] = fabsf(factor * aspy);
    }
    else {
      br->stencil_dimension[0] = fabsf(factor * aspx);
      br->stencil_dimension[1] = fabsf(factor * aspy);
    }
    BKE_brush_tag_unsaved_changes(br);
  }

  WM_event_add_notifier(C, NC_WINDOW, nullptr);

  return OPERATOR_FINISHED;
}

static void BRUSH_OT_stencil_fit_image_aspect(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Image Aspect";
  ot->description =
      "When using an image texture, adjust the stencil size to fit the image aspect ratio";
  ot->idname = "BRUSH_OT_stencil_fit_image_aspect";

  /* API callbacks. */
  ot->exec = stencil_fit_image_aspect_exec;
  ot->poll = stencil_control_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_boolean(ot->srna, "use_repeat", true, "Use Repeat", "Use repeat mapping values");
  RNA_def_boolean(ot->srna, "use_scale", true, "Use Scale", "Use texture scale values");
  RNA_def_boolean(
      ot->srna, "mask", false, "Modify Mask Stencil", "Modify either the primary or mask stencil");
}

static wmOperatorStatus stencil_reset_transform_exec(bContext *C, wmOperator *op)
{
  Paint *paint = BKE_paint_get_active_from_context(C);
  Brush *br = BKE_paint_brush(paint);
  bool do_mask = RNA_boolean_get(op->ptr, "mask");

  if (!br) {
    return OPERATOR_CANCELLED;
  }

  if (do_mask) {
    br->mask_stencil_pos[0] = 256;
    br->mask_stencil_pos[1] = 256;

    br->mask_stencil_dimension[0] = 256;
    br->mask_stencil_dimension[1] = 256;

    br->mask_mtex.rot = 0;
  }
  else {
    br->stencil_pos[0] = 256;
    br->stencil_pos[1] = 256;

    br->stencil_dimension[0] = 256;
    br->stencil_dimension[1] = 256;

    br->mtex.rot = 0;
  }

  BKE_brush_tag_unsaved_changes(br);
  WM_event_add_notifier(C, NC_WINDOW, nullptr);

  return OPERATOR_FINISHED;
}

static wmOperatorStatus material_paint_brush_ensure_exec(bContext *C, wmOperator *op)
{
  Paint *paint = BKE_paint_get_active_from_context(C);
  Brush *brush = paint ? BKE_paint_brush(paint) : nullptr;
  if (brush == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "No active paint brush");
    return OPERATOR_CANCELLED;
  }
  BKE_brush_material_paint_ensure(brush);

  /* #BKE_brush_material_paint_ensure seeds Base Color to white because it has no #Paint to resolve
   * the effective (unified or per-brush) color from. Pull the current color in here, otherwise the
   * Base Color swatch contradicts the brush's own color swatch until the user next edits it. */
  BKE_brush_material_paint_base_color_sync_to_channel(paint, brush);

  /* Sync after the channel settings exist, so the paired editor adopts a brush that is already set
   * up. No-op when sync is off or the canvas is not Material. */
  BKE_paint_material_brush_sync(CTX_data_scene(C), paint);

  WM_event_add_notifier(C, NC_BRUSH | NA_EDITED, brush);
  return OPERATOR_FINISHED;
}

/**
 * Make the active brush local when it is a linked asset, returning the brush to configure.
 *
 * A linked ID may not reference a local one (#BKE_id_can_use_id, which #RNA_property_pointer_poll
 * enforces for the UI), so a linked brush cannot be given a local source #Material -- local
 * materials are not even offered in the selector. The restriction is not cosmetic: brushes live
 * outside memfile undo, whose pointer remap skips linked IDs, so such a reference would dangle
 * after an undo step and could not be written to the .blend. Same reasoning, and same remedy, as
 * #brush_ensure_local_for_texture in `image_grid_ops.cc`.
 *
 * \return the new local copy, or \a brush unchanged when it is already local.
 */
static Brush *material_paint_brush_ensure_local(bContext *C,
                                                Paint *paint,
                                                Brush &brush,
                                                ReportList *reports)
{
  if (!ID_IS_LINKED(&brush)) {
    return &brush;
  }

  Main &bmain = *CTX_data_main(C);
  Brush *local_brush = id_cast<Brush *>(bke::asset_edit_id_ensure_local(bmain, brush.id));
  if (local_brush == nullptr || local_brush == &brush) {
    return &brush;
  }

  /* Plain #BKE_paint_brush_set rather than the `_synced` variant: this is the same brush made
   * local, not a user-facing brush switch, so running the PBR preset snapshot/apply round trip
   * would only risk clobbering the very state being carried over. Matches the texture path. */
  if (paint != nullptr) {
    BKE_paint_brush_set(paint, local_brush);
    /* The tool brush bindings, not #Paint.brush, are what re-activates a brush on entering the
     * mode again. Without this the linked asset comes back on the next mode change and takes the
     * source mode with it, which #BKE_paint_brush_set alone does not prevent. */
    WM_toolsystem_brush_bindings_update_from_active(C, paint);
  }
  /* Reported rather than done silently: the active brush is being swapped for a copy, which the
   * user would otherwise only notice later by the missing library icon. */
  BKE_reportf(reports,
              RPT_INFO,
              "Brush \"%s\" made local: a linked brush cannot reference a source material",
              local_brush->id.name + 2);
  return local_brush;
}

static wmOperatorStatus material_paint_source_mode_set_exec(bContext *C, wmOperator *op)
{
  Paint *paint = BKE_paint_get_active_from_context(C);
  Brush *brush = paint ? BKE_paint_brush(paint) : nullptr;
  if (brush == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "No active paint brush");
    return OPERATOR_CANCELLED;
  }

  const int mode = RNA_enum_get(op->ptr, "mode");
  if (mode == BRUSH_MATERIAL_PAINT_SOURCE_MATERIAL) {
    /* Before the mode is stored, so the panel that draws next already has a brush whose selector
     * can list local materials. */
    brush = material_paint_brush_ensure_local(C, paint, *brush, op->reports);
  }

  BKE_brush_material_paint_ensure(brush);
  brush->material_paint->source_mode = char(mode);

  Scene *scene = CTX_data_scene(C);
  /* The scene-side preset is what #paint_brush_update_from_asset_reference applies when the brush
   * is re-activated -- on re-entering the mode, or after an undo step restores the scene. Leaving
   * it stale is what silently put the brush back on Maps. */
  if (scene != nullptr) {
    BKE_paint_material_brush_preset_snapshot(*scene, *brush);
  }

  /* Same reasoning as #material_paint_brush_ensure_exec: the paired editor must not be left on a
   * different source mode. No-op when sync is off or the canvas is not Material. */
  BKE_paint_material_brush_sync(scene, paint);

  BKE_brush_tag_unsaved_changes(brush);
  WM_event_add_notifier(C, NC_BRUSH | NA_EDITED, brush);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus material_channel_value_invert_exec(bContext *C, wmOperator *op)
{
  Paint *paint = BKE_paint_get_active_from_context(C);
  Brush *brush = paint ? BKE_paint_brush(paint) : nullptr;
  if (brush == nullptr || brush->material_paint == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "No active material paint brush");
    return OPERATOR_CANCELLED;
  }

  const eMaterialPaintChannel channel_id = eMaterialPaintChannel(RNA_enum_get(op->ptr, "channel"));
  BrushMaterialPaintChannel &channel = brush->material_paint->channels[channel_id];

  const Scene *scene = CTX_data_scene(C);
  const PaintModeSettings &mode_settings = scene->toolsettings->paint_mode;
  const float2 range = BKE_paint_material_channel_range(mode_settings, channel_id);
  channel.value[0] = BKE_paint_material_value_invert(range[0], range[1], channel.value[0]);

  BKE_brush_tag_unsaved_changes(brush);
  WM_event_add_notifier(C, NC_BRUSH | NA_EDITED, brush);
  return OPERATOR_FINISHED;
}

void PAINT_OT_material_channel_value_invert(wmOperatorType *ot)
{
  ot->name = "Invert Value";
  ot->idname = "PAINT_OT_material_channel_value_invert";
  ot->description = "Invert this channel's value within its range (min + max - value)";
  ot->exec = material_channel_value_invert_exec;
  ot->poll = ED_operator_object_active_editable;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  ot->prop = RNA_def_enum(ot->srna,
                          "channel",
                          rna_enum_material_paint_channel_items,
                          PAINT_MATERIAL_CHANNEL_METALLIC,
                          "Channel",
                          "Material paint channel to invert");
}

static wmOperatorStatus material_channel_source_clear_exec(bContext *C, wmOperator *op)
{
  Paint *paint = BKE_paint_get_active_from_context(C);
  Brush *brush = paint ? BKE_paint_brush(paint) : nullptr;
  if (brush == nullptr || brush->material_paint == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "No active material paint brush");
    return OPERATOR_CANCELLED;
  }

  const eMaterialPaintChannel channel_id = eMaterialPaintChannel(RNA_enum_get(op->ptr, "channel"));
  BrushMaterialPaintChannel &channel = brush->material_paint->channels[channel_id];

  /* Drop the whole Tex, not just its image: this also recovers a source left in a broken state
   * (e.g. its image was deleted from Main), which the source_image pointer alone cannot express
   * since it already reads as unset. */
  if (channel.source_mtex.tex != nullptr) {
    id_us_min(&channel.source_mtex.tex->id);
    channel.source_mtex.tex = nullptr;
  }

  BKE_brush_tag_unsaved_changes(brush);
  WM_event_add_notifier(C, NC_BRUSH | NA_EDITED, brush);
  return OPERATOR_FINISHED;
}

void PAINT_OT_material_channel_source_clear(wmOperatorType *ot)
{
  ot->name = "Clear Source";
  ot->idname = "PAINT_OT_material_channel_source_clear";
  ot->description = "Remove this channel's source texture, including one left in a broken state";
  ot->exec = material_channel_source_clear_exec;
  ot->poll = ED_operator_object_active_editable;
  /* Not registered: this is a picker button, so the redo panel it would raise ("Adjust Last
   * Operation") is noise. Undo still works, which gates on #OPTYPE_UNDO alone. */
  ot->flag = OPTYPE_UNDO | OPTYPE_INTERNAL;

  ot->prop = RNA_def_enum(ot->srna,
                          "channel",
                          rna_enum_material_paint_channel_items,
                          PAINT_MATERIAL_CHANNEL_METALLIC,
                          "Channel",
                          "Material paint channel whose source texture should be cleared");
}

/**
 * Image the activated grid tile stands for. The local grid (#UIGrid) can only hand over a single
 * string, so it passes the image name in `identifier`; the asset grid instead sets the standard
 * asset reference properties, exactly as the image texture shelf does
 * (see #image_shelf_activate_asset_exec).
 */
static Image *material_channel_source_image_from_op(bContext *C,
                                                    wmOperator *op,
                                                    const StringRef image_name_in)
{
  Main *bmain = CTX_data_main(C);

  const std::string image_name = image_name_in;
  if (!image_name.empty()) {
    Image *image = reinterpret_cast<Image *>(
        BKE_libblock_find_name(bmain, ID_IM, image_name.c_str()));
    if (image == nullptr) {
      BKE_report(op->reports, RPT_ERROR, "Image not found");
    }
    return image;
  }

  if (!ed::asset::operator_asset_reference_props_is_set(*op->ptr)) {
    BKE_report(op->reports, RPT_ERROR, "No image given");
    return nullptr;
  }

  /* Reported only if every lookup below fails: the "All Libraries" search is expected to come up
   * empty whenever that combined list has not been fetched, which is the normal state when the
   * grid only ever fetched the one library it browses. */
  const asset_system::AssetRepresentation *asset =
      ed::asset::operator_asset_reference_props_get_asset_from_all_library(*C, *op->ptr, nullptr);

  if (asset == nullptr) {
    /* Search each library on its own, the way the image texture shelf falls back to the library
     * it actually fetched (see #image_shelf_activate_asset_exec). */
    AssetWeakReference weak_ref{};
    weak_ref.asset_library_type = eAssetLibraryType(RNA_enum_get(op->ptr, "asset_library_type"));
    weak_ref.asset_library_identifier = RNA_string_get_alloc(
        op->ptr, "asset_library_identifier", nullptr, 0, nullptr);
    weak_ref.relative_asset_identifier = RNA_string_get_alloc(
        op->ptr, "relative_asset_identifier", nullptr, 0, nullptr);

    for (const AssetLibraryReference &library_ref : asset_system::all_valid_asset_library_refs()) {
      ed::asset::list::storage_fetch(&library_ref, C);
      ed::asset::list::iterate(library_ref, [&](asset_system::AssetRepresentation &candidate) {
        if (candidate.make_weak_reference() == weak_ref) {
          asset = &candidate;
          return false;
        }
        return true;
      });
      if (asset != nullptr) {
        break;
      }
    }
  }

  if (asset == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "Asset not found");
    return nullptr;
  }
  if (asset->get_id_type() != ID_IM) {
    BKE_report(op->reports, RPT_ERROR, "Selected asset is not an image");
    return nullptr;
  }

  /* Links or appends the asset when it is not already in this file. */
  Image *image = ed::asset::resolve_image_from_asset(*bmain, *asset);
  if (image == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "Could not load image asset");
  }
  return image;
}

static wmOperatorStatus material_channel_source_image_set_exec(bContext *C, wmOperator *op)
{
  Paint *paint = BKE_paint_get_active_from_context(C);
  Brush *brush = paint ? BKE_paint_brush(paint) : nullptr;
  if (brush == nullptr || brush->material_paint == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "No active material paint brush");
    return OPERATOR_CANCELLED;
  }

  /* Neither grid can pass the channel along with the item through the button system, so it travels
   * with the item instead: the local grid puts it in front of the image name ("<CHANNEL>/<image>"),
   * the asset grid sets `context_id`. The layout context is only a fallback for menus, which keep
   * their context store alive. */
  const std::string identifier = RNA_string_get(op->ptr, "identifier");
  const size_t separator = identifier.find('/');

  std::string channel_id;
  std::string image_name = identifier;
  if (separator != std::string::npos) {
    channel_id = identifier.substr(0, separator);
    image_name = identifier.substr(separator + 1);
  }
  else {
    channel_id = RNA_string_get(op->ptr, "context_id");
  }

  std::optional<StringRefNull> channel_name;
  if (!channel_id.empty()) {
    channel_name = StringRefNull(channel_id);
  }
  else {
    channel_name = CTX_data_string_get(C, "material_paint_channel_id");
  }

  PointerRNA channel_ptr = PointerRNA_NULL;
  int channel_value = 0;
  if (channel_name && RNA_enum_value_from_id(rna_enum_material_paint_channel_items,
                                             channel_name->c_str(),
                                             &channel_value))
  {
    channel_ptr = RNA_pointer_create_discrete(
        &brush->id,
        RNA_BrushMaterialPaintChannel,
        &brush->material_paint->channels[eMaterialPaintChannel(channel_value)]);
  }
  else {
    /* Menus keep the layout context store alive, so the pointer still works there. */
    channel_ptr = CTX_data_pointer_get_type(
        C, "material_paint_channel", RNA_BrushMaterialPaintChannel);
  }
  if (channel_ptr.data == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "No material paint channel given");
    return OPERATOR_CANCELLED;
  }

  Image *image = material_channel_source_image_from_op(C, op, image_name);
  if (image == nullptr) {
    return OPERATOR_CANCELLED;
  }

  /* Assign through RNA so the source_image setter's Tex wrapper creation, user counts and
   * non-color default all stay in one place. */
  PointerRNA target_ptr = channel_ptr;
  PointerRNA image_ptr = RNA_id_pointer_create(&image->id);
  RNA_pointer_set(&target_ptr, "source_image", image_ptr);

  /* The setter can decline (e.g. the Tex wrapper could not be created), which would otherwise
   * leave the click looking like it did nothing at all. */
  if (RNA_pointer_get(&target_ptr, "source_image").data != &image->id) {
    BKE_reportf(op->reports,
                RPT_ERROR,
                "Could not assign \"%s\" to this material paint channel",
                image->id.name + 2);
    return OPERATOR_CANCELLED;
  }

  BKE_brush_tag_unsaved_changes(brush);
  WM_event_add_notifier(C, NC_BRUSH | NA_EDITED, brush);
  return OPERATOR_FINISHED;
}

void PAINT_OT_material_channel_source_image_set(wmOperatorType *ot)
{
  ot->name = "Set Source Image";
  ot->idname = "PAINT_OT_material_channel_source_image_set";
  ot->description = "Use the picked image as this channel's source texture";
  ot->exec = material_channel_source_image_set_exec;
  ot->poll = ED_operator_object_active_editable;
  /* Not registered: activating a grid tile is a picker click, not an operation worth re-running
   * from the redo panel, whose properties (a grid identifier or an asset reference) mean nothing
   * out of that context. Undo still works, which gates on #OPTYPE_UNDO alone. */
  ot->flag = OPTYPE_UNDO | OPTYPE_INTERNAL;

  ed::asset::operator_asset_reference_props_register(*ot->srna);

  /* Both properties below describe one click and must never be inherited from the previous run:
   * every invocation sets only the pair its own picker uses, and #WM_operator_last_properties_init
   * fills in whatever the caller left unset. A stale `identifier` left by a local-grid click would
   * send an asset-grid click down the image-name branch, which never reads the asset reference at
   * all. #PROP_SKIP_SAVE keeps them out of that store, as it does for the asset reference
   * properties registered above (see #operator_asset_reference_props_register). */

  /* Filled in by the asset grid from its `activate_context_id`; see #AssetGridItem::on_activate. */
  PropertyRNA *prop = RNA_def_string(ot->srna,
                                     "context_id",
                                     nullptr,
                                     0,
                                     "Context ID",
                                     "Material paint channel this assignment applies to");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);

  ot->prop = RNA_def_string(ot->srna,
                            "identifier",
                            nullptr,
                            0,
                            "Identifier",
                            "Local grid item, as \"<channel>/<image name>\". When empty, the "
                            "asset reference properties are used instead");
  RNA_def_property_flag(ot->prop, PROP_HIDDEN | PROP_SKIP_SAVE);
}

/* -------------------------------------------------------------------- */
/** \name Cycle Material Paint Canvas
 *
 * Steps the Image Editor's shown image (#SpaceImage.image) through the active material's texture
 * paint slots, in node order. Only available while the Image Editor is in Material (PBR) paint
 * mode. Bound to `C` / `Shift-C` in the Image Paint keymap.
 * \{ */

static bool material_canvas_cycle_poll(bContext *C)
{
  const SpaceImage *sima = CTX_wm_space_image(C);
  if (sima == nullptr || sima->mode != SI_MODE_PAINT) {
    return false;
  }
  const Scene *scene = CTX_data_scene(C);
  if (scene == nullptr ||
      scene->toolsettings->imapaint.mode != IMAGEPAINT_MODE_MATERIAL)
  {
    return false;
  }
  const Object *ob = CTX_data_active_object(C);
  return ob != nullptr && ob->actcol > 0;
}

static wmOperatorStatus material_canvas_cycle_exec(bContext *C, wmOperator *op)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  Scene *scene = CTX_data_scene(C);
  Object *ob = CTX_data_active_object(C);
  Material *ma = ob ? BKE_object_material_get(ob, ob->actcol) : nullptr;
  if (sima == nullptr || scene == nullptr || ma == nullptr) {
    BKE_report(op->reports, RPT_WARNING, "No active material paint canvas");
    return OPERATOR_CANCELLED;
  }

  /* Rebuild the paint-slot cache so it reflects the current node tree (the Image Editor never
   * calls this itself, unlike entering texture paint mode). */
  BKE_texpaint_slot_refresh_cache(scene, ma, ob);

  const blender::Vector<Image *> images = BKE_texpaint_slot_canvas_images(ma);
  if (images.is_empty()) {
    BKE_report(op->reports, RPT_WARNING, "Active material has no paintable image slots");
    return OPERATOR_CANCELLED;
  }
  const bool keep_view = RNA_boolean_get(op->ptr, "keep_view");

  if (images.size() == 1) {
    /* Nothing to cycle to; still assign so an out-of-material canvas snaps back. */
    if (sima->image != images[0]) {
      ED_space_image_set_ex(CTX_data_main(C), sima, images[0], keep_view);
      WM_event_add_notifier(C, NC_SPACE | ND_SPACE_IMAGE, nullptr);
    }
    return OPERATOR_FINISHED;
  }

  const bool reverse = RNA_boolean_get(op->ptr, "reverse");
  const int count = int(images.size());
  const int current = int(images.first_index_of_try(sima->image));
  int next;
  if (current < 0) {
    next = reverse ? count - 1 : 0;
  }
  else {
    next = (current + (reverse ? -1 : 1) + count) % count;
  }

  ED_space_image_set_ex(CTX_data_main(C), sima, images[next], keep_view);
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_IMAGE, nullptr);
  return OPERATOR_FINISHED;
}

void PAINT_OT_material_canvas_cycle(wmOperatorType *ot)
{
  ot->name = "Cycle Material Paint Canvas";
  ot->idname = "PAINT_OT_material_canvas_cycle";
  ot->description =
      "Show the next texture paint slot of the active material in the Image Editor (PBR paint)";
  ot->exec = material_canvas_cycle_exec;
  ot->poll = material_canvas_cycle_poll;
  ot->flag = OPTYPE_REGISTER;

  RNA_def_boolean(
      ot->srna, "reverse", false, "Reverse", "Step to the previous slot instead of the next");
  RNA_def_boolean(ot->srna,
                  "keep_view",
                  true,
                  "Keep View",
                  "Preserve the current zoom and pan instead of adopting the next image's "
                  "remembered view");
}

/** \} */

void PAINT_OT_material_paint_source_mode_set(wmOperatorType *ot)
{
  /* Mirrors #prop_source_mode_items in `rna_brush.cc`; the panel draws its own labels, so these
   * exist only to carry the value. */
  static const EnumPropertyItem mode_items[] = {
      {BRUSH_MATERIAL_PAINT_SOURCE_MAPS, "MAPS", 0, "Maps", ""},
      {BRUSH_MATERIAL_PAINT_SOURCE_MATERIAL, "MATERIAL", 0, "Material", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  ot->name = "Set Material Paint Source Mode";
  ot->idname = "PAINT_OT_material_paint_source_mode_set";
  ot->description =
      "Choose where the brush takes its channel textures from, making the brush local first when "
      "a source material requires it";
  ot->exec = material_paint_source_mode_set_exec;
  ot->poll = ED_operator_object_active_editable;
  /* No #OPTYPE_UNDO, matching #IMAGE_GRID_OT_assign_texture, the other operator that makes a brush
   * local. Brushes are outside memfile undo (their persistence goes through
   * #BKE_brush_tag_unsaved_changes), so pushing an undo step that both creates a brush ID and
   * swaps the active one is what leaves the reference dangling on the next undo. */
  ot->flag = OPTYPE_REGISTER;

  ot->prop = RNA_def_enum(ot->srna,
                          "mode",
                          mode_items,
                          BRUSH_MATERIAL_PAINT_SOURCE_MAPS,
                          "Mode",
                          "Where the brush takes its channel textures from");
}

void PAINT_OT_material_paint_brush_ensure(wmOperatorType *ot)
{
  ot->name = "Enable Material Paint Channels for Brush";
  ot->idname = "PAINT_OT_material_paint_brush_ensure";
  ot->description = "Initialize per-channel material paint values for the active brush";
  ot->exec = material_paint_brush_ensure_exec;
  ot->poll = ED_operator_object_active_editable;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static wmOperatorStatus material_paint_images_ensure_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr || ob->type != OB_MESH) {
    BKE_report(op->reports, RPT_ERROR, "Active object must be a mesh");
    return OPERATOR_CANCELLED;
  }

  Paint *paint = BKE_paint_get_active_from_context(C);
  Brush *brush = paint ? BKE_paint_brush(paint) : nullptr;
  if (brush == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "No active paint brush");
    return OPERATOR_CANCELLED;
  }
  BKE_brush_material_paint_ensure(brush);
  if (brush->material_paint == nullptr) {
    return OPERATOR_CANCELLED;
  }

  Scene *scene = CTX_data_scene(C);
  PaintModeSettings &mode_settings = scene->toolsettings->paint_mode;
  Main *bmain = CTX_data_main(C);
  const BrushMaterialPaint &brush_paint = *brush->material_paint;
  const PaintMaterialImagesEnsureResult ensure_result = BKE_paint_material_images_ensure_writable(
      *bmain, *ob, brush_paint, mode_settings, paint->visible_material_channels);
  const int created = ensure_result.created;
  if (ensure_result.conflicting_layer_ids) {
    BKE_report(op->reports,
               RPT_WARNING,
               "Enabled channels already belong to different paint layers; "
               "the new maps were put in a new layer");
  }
  int missing = 0;

  for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
    if (!info.supports_image_paint) {
      continue;
    }
    if (!BKE_paint_material_channel_writes_to_target(
            brush_paint, mode_settings, paint->visible_material_channels, info.channel))
    {
      continue;
    }
    Image *image = nullptr;
    ImageUser *iuser = nullptr;
    if (!BKE_paint_principled_channel_image_get(*ob, info.channel, &image, &iuser, &mode_settings))
    {
      missing++;
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "%s channel has no paintable image texture on the active material",
                  info.ui_name);
    }
  }

  if (created == 0 && missing == 0) {
    BKE_report(op->reports, RPT_INFO, "All enabled channels already have image maps");
  }
  else if (created > 0) {
    BKE_reportf(op->reports, RPT_INFO, "Created %d material paint image map(s)", created);
  }

  ED_space_image_paint_auto_select_material_canvas(bmain, ob);

  WM_event_add_notifier(C, NC_MATERIAL | ND_SHADING, nullptr);
  WM_event_add_notifier(C, NC_NODE | NA_EDITED, nullptr);
  return (created > 0 || missing == 0) ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
}

void PAINT_OT_material_paint_images_ensure(wmOperatorType *ot)
{
  ot->name = "Create PBR Paint Maps";
  ot->idname = "PAINT_OT_material_paint_images_ensure";
  ot->description =
      "Create missing Image Texture nodes on the active material's Principled BSDF for enabled "
      "PBR Paint channels";
  ot->exec = material_paint_images_ensure_exec;
  ot->poll = ED_operator_object_active_editable;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static bool material_paint_brush_sync_poll(bContext *C)
{
  if (!ED_operator_object_active_editable(C)) {
    return false;
  }
  /* Texture Paint shares Image Paint's brush with the Image Editor. Sync from that mode would
   * overwrite an already configured Image Editor: Paint / Sculpt pair. */
  const Object *ob = CTX_data_active_object(C);
  if (ob != nullptr && (ob->mode & OB_MODE_TEXTURE_PAINT)) {
    return false;
  }
  return true;
}

enum eMaterialPaintBrushSyncDirection {
  MATERIAL_PAINT_BRUSH_SYNC_IMAGE_TO_SCULPT = 0,
  MATERIAL_PAINT_BRUSH_SYNC_SCULPT_TO_IMAGE = 1,
};

static const EnumPropertyItem material_paint_brush_sync_direction_items[] = {
    {MATERIAL_PAINT_BRUSH_SYNC_IMAGE_TO_SCULPT,
     "IMAGE_TO_SCULPT",
     0,
     "Image Editor to Sculpt Mode",
     "Use the Image Editor Paint brush and settings in Sculpt Mode"},
    {MATERIAL_PAINT_BRUSH_SYNC_SCULPT_TO_IMAGE,
     "SCULPT_TO_IMAGE",
     0,
     "Sculpt Mode to Image Editor",
     "Use the Sculpt Mode brush and settings in the Image Editor"},
    {0, nullptr, 0, nullptr, nullptr},
};

static wmOperatorStatus material_paint_brush_sync_exec(bContext *C, wmOperator *op)
{
  Scene *scene = CTX_data_scene(C);
  ToolSettings *ts = scene->toolsettings;
  if (ts->sculpt == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "Sculpt Mode paint is not available");
    return OPERATOR_CANCELLED;
  }
  Paint *sculpt_paint = &ts->sculpt->paint;
  Paint *image_paint = &ts->imapaint.paint;

  const int direction = RNA_enum_get(op->ptr, "direction");
  const bool image_to_sculpt = direction == MATERIAL_PAINT_BRUSH_SYNC_IMAGE_TO_SCULPT;
  Paint *source = image_to_sculpt ? image_paint : sculpt_paint;
  Paint *destination = image_to_sculpt ? sculpt_paint : image_paint;

  Brush *source_brush = BKE_paint_brush(source);
  if (source_brush == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "The source editor has no active brush to sync from");
    return OPERATOR_CANCELLED;
  }

  /* This is a one-shot copy: it deliberately bypasses the automatic sync flag and, just as
   * deliberately, leaves that flag alone so the two editors stay independent afterwards. Make the
   * source brush valid for the receiving mode before assigning it. */
  BLI_assert(destination->runtime != nullptr);
  if (!BKE_paint_can_use_brush(destination, source_brush)) {
    source_brush->ob_mode |= destination->runtime->ob_mode;
    BKE_brush_tag_unsaved_changes(source_brush);
    BKE_reportf(op->reports,
                RPT_INFO,
                "Enabled this paint mode on brush \"%s\" so it could be synced here",
                source_brush->id.name + 2);
  }

  if (!BKE_paint_material_brush_sync_directional(scene, source, destination)) {
    BKE_report(op->reports, RPT_ERROR, "Sync Brush requires the Material canvas in both editors");
    return OPERATOR_CANCELLED;
  }

  /* Keep the receiving editor's tool and its brush bindings pointing at the brush that was just
   * assigned; only possible for the editor the operator was called from. */
  if (destination == BKE_paint_get_active_from_context(C)) {
    WM_toolsystem_activate_brush_and_tool(C, destination, source_brush);
  }

  WM_event_add_notifier(C, NC_BRUSH | NA_SELECTED, source_brush);
  WM_event_add_notifier(C, NC_SCENE | ND_TOOLSETTINGS, nullptr);
  return OPERATOR_FINISHED;
}

void PAINT_OT_material_paint_brush_sync(wmOperatorType *ot)
{
  ot->name = "Sync Brush";
  ot->idname = "PAINT_OT_material_paint_brush_sync";
  ot->description =
      "Copy the brush and PBR paint settings once between Sculpt Mode and the Image Editor, "
      "without turning on automatic sync";
  ot->exec = material_paint_brush_sync_exec;
  ot->poll = material_paint_brush_sync_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  ot->prop = RNA_def_enum(ot->srna,
                          "direction",
                          material_paint_brush_sync_direction_items,
                          0,
                          "Direction",
                          "Choose which editor provides the brush and PBR paint settings");
}

static void BRUSH_OT_stencil_reset_transform(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Reset Transform";
  ot->description = "Reset the stencil transformation to the default";
  ot->idname = "BRUSH_OT_stencil_reset_transform";

  /* API callbacks. */
  ot->exec = stencil_reset_transform_exec;
  ot->poll = stencil_control_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_boolean(
      ot->srna, "mask", false, "Modify Mask Stencil", "Modify either the primary or mask stencil");
}

/* -------------------------------------------------------------------- */
/** \name Curve Patch Texture Slots
 * \{ */

static bool curve_patch_texture_slot_poll(bContext *C)
{
  const Paint *paint = BKE_paint_get_active_from_context(C);
  if (paint == nullptr) {
    return false;
  }
  const Brush *brush = BKE_paint_brush_for_read(paint);
  /* Curve Patch is sculpt-only, so the slots it draws from are meaningless anywhere else. Without
   * this the operators run in vertex, weight and texture paint, editing a list nothing reads. */
  return brush != nullptr && (brush->ob_mode & OB_MODE_SCULPT) != 0 &&
         bke::brush::supports_curve_patch(*brush);
}

static wmOperatorStatus curve_patch_texture_slot_add_exec(bContext *C, wmOperator * /*op*/)
{
  Paint *paint = BKE_paint_get_active_from_context(C);
  Brush *brush = BKE_paint_brush(paint);

  BKE_brush_curve_patch_texture_slot_add(*brush);

  /* Brushes are assets: without this the edit looks saved and is lost on reload. Every RNA setter
   * on this same sub-struct tags it, so the operators have to as well. */
  BKE_brush_tag_unsaved_changes(brush);
  WM_event_add_notifier(C, NC_BRUSH | NA_EDITED, brush);
  return OPERATOR_FINISHED;
}

static void BRUSH_OT_curve_patch_texture_slot_add(wmOperatorType *ot)
{
  ot->name = "Add Curve Patch Texture";
  ot->description = "Add a texture slot to the brush's Curve Patch texture list";
  ot->idname = "BRUSH_OT_curve_patch_texture_slot_add";

  ot->exec = curve_patch_texture_slot_add_exec;
  ot->poll = curve_patch_texture_slot_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static wmOperatorStatus curve_patch_texture_slot_remove_exec(bContext *C, wmOperator * /*op*/)
{
  Paint *paint = BKE_paint_get_active_from_context(C);
  Brush *brush = BKE_paint_brush(paint);

  BrushCurvePatchTextureSlot *slot = static_cast<BrushCurvePatchTextureSlot *>(
      BLI_findlink(&brush->curve_patch.texture_slots, brush->curve_patch.texture_active_index));
  if (slot == nullptr || !BKE_brush_curve_patch_texture_slot_remove(*brush, *slot)) {
    return OPERATOR_CANCELLED;
  }

  BKE_brush_tag_unsaved_changes(brush);
  WM_event_add_notifier(C, NC_BRUSH | NA_EDITED, brush);
  return OPERATOR_FINISHED;
}

static void BRUSH_OT_curve_patch_texture_slot_remove(wmOperatorType *ot)
{
  ot->name = "Remove Curve Patch Texture";
  ot->description = "Remove the active texture slot from the brush's Curve Patch texture list";
  ot->idname = "BRUSH_OT_curve_patch_texture_slot_remove";

  ot->exec = curve_patch_texture_slot_remove_exec;
  ot->poll = curve_patch_texture_slot_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static wmOperatorStatus curve_patch_texture_slot_move_exec(bContext *C, wmOperator *op)
{
  Paint *paint = BKE_paint_get_active_from_context(C);
  Brush *brush = BKE_paint_brush(paint);

  BrushCurvePatchTextureSlot *slot = static_cast<BrushCurvePatchTextureSlot *>(
      BLI_findlink(&brush->curve_patch.texture_slots, brush->curve_patch.texture_active_index));
  if (slot == nullptr) {
    return OPERATOR_CANCELLED;
  }

  const int direction = RNA_enum_get(op->ptr, "type");
  /* `0` is admitted because the `type` enum's DEFAULT is 0 and no item carries that value: running
   * the operator without setting `type` (F3 search, a bare `bpy.ops` call) would otherwise abort a
   * debug build. `BLI_listbase_link_move()` early-returns on a zero step. Same reasoning, and the
   * same admitted value, as `palette_color_move_exec()`. */
  BLI_assert(ELEM(direction, -1, 0, 1));
  if (BLI_listbase_link_move(&brush->curve_patch.texture_slots, slot, direction)) {
    brush->curve_patch.texture_active_index += direction;
    BKE_brush_tag_unsaved_changes(brush);
    WM_event_add_notifier(C, NC_BRUSH | NA_EDITED, brush);
  }

  return OPERATOR_FINISHED;
}

static void BRUSH_OT_curve_patch_texture_slot_move(wmOperatorType *ot)
{
  static const EnumPropertyItem slot_move[] = {
      {-1, "UP", 0, "Up", ""},
      {1, "DOWN", 0, "Down", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  ot->name = "Move Curve Patch Texture";
  ot->description = "Move the active texture slot up or down in the list";
  ot->idname = "BRUSH_OT_curve_patch_texture_slot_move";

  ot->exec = curve_patch_texture_slot_move_exec;
  ot->poll = curve_patch_texture_slot_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  ot->prop = RNA_def_enum(ot->srna, "type", slot_move, 0, "Type", "");
}

/** \} */

/**************************** registration **********************************/

void ED_operatormacros_paint()
{
  wmOperatorType *ot;
  wmOperatorTypeMacro *otmacro;

  ot = WM_operatortype_append_macro("PAINTCURVE_OT_add_point_slide",
                                    "Add Curve Point and Slide",
                                    "Add new curve point and slide it",
                                    OPTYPE_REGISTER);
  ot->description = "Add new curve point and slide it";
  WM_operatortype_macro_define(ot, "PAINTCURVE_OT_add_point");
  otmacro = WM_operatortype_macro_define(ot, "PAINTCURVE_OT_slide");
  RNA_boolean_set(otmacro->ptr, "align", true);
  RNA_boolean_set(otmacro->ptr, "select", false);

  ot = WM_operatortype_append_macro("PAINTCURVE_OT_duplicate_move",
                                    "Duplicate Curve Spline and Move",
                                    "Duplicate selected paint curve splines and move them",
                                    OPTYPE_UNDO | OPTYPE_REGISTER);
  WM_operatortype_macro_define(ot, "PAINTCURVE_OT_duplicate");
  otmacro = WM_operatortype_macro_define(ot, "TRANSFORM_OT_translate");
  RNA_boolean_set(otmacro->ptr, "use_proportional_edit", false);
  RNA_boolean_set(otmacro->ptr, "mirror", false);
}

void ED_operatortypes_paint()
{
  /* palette */
  using namespace blender::ed::sculpt_paint;
  WM_operatortype_append(PALETTE_OT_new);
  WM_operatortype_append(PALETTE_OT_color_add);
  WM_operatortype_append(PALETTE_OT_color_delete);

  WM_operatortype_append(PALETTE_OT_extract_from_image);
  WM_operatortype_append(PALETTE_OT_sort);
  WM_operatortype_append(PALETTE_OT_color_move);
  WM_operatortype_append(PALETTE_OT_join);

  /* paint curve */
  WM_operatortype_append(PAINTCURVE_OT_new);
  WM_operatortype_append(PAINTCURVE_OT_add_point);
  WM_operatortype_append(PAINTCURVE_OT_insert_or_add_point);
  WM_operatortype_append(PAINTCURVE_OT_new_spline);
  WM_operatortype_append(PAINTCURVE_OT_delete_point);
  WM_operatortype_append(PAINTCURVE_OT_clear);
  WM_operatortype_append(PAINTCURVE_OT_duplicate);
  WM_operatortype_append(PAINTCURVE_OT_select);
  WM_operatortype_append(PAINTCURVE_OT_slide);
  WM_operatortype_append(PAINTCURVE_OT_slide_radius);
  WM_operatortype_append(PAINTCURVE_OT_draw);
  WM_operatortype_append(PAINTCURVE_OT_from_curve_object);
  WM_operatortype_append(PAINTCURVE_OT_to_curve_object);
  WM_operatortype_append(PAINTCURVE_OT_separate_to_curve_object);
  WM_operatortype_append(PAINTCURVE_OT_cursor);
  WM_operatortype_append(PAINTCURVE_OT_sculpt_pick);
  WM_operatortype_append(PAINTCURVE_OT_handle_type_set);
  WM_operatortype_append(PAINTCURVE_OT_split);
  WM_operatortype_append(PAINTCURVE_OT_make_segment);
  WM_operatortype_append(PAINTCURVE_OT_select_linked);
  WM_operatortype_append(PAINTCURVE_OT_toggle_cyclic);
  WM_operatortype_append(PAINTCURVE_OT_context_menu);

  /* brush */
  WM_operatortype_append(BRUSH_OT_scale_size);
  WM_operatortype_append(BRUSH_OT_stencil_control);
  WM_operatortype_append(BRUSH_OT_stencil_fit_image_aspect);
  WM_operatortype_append(BRUSH_OT_stencil_reset_transform);
  WM_operatortype_append(BRUSH_OT_curve_patch_texture_slot_add);
  WM_operatortype_append(BRUSH_OT_curve_patch_texture_slot_remove);
  WM_operatortype_append(BRUSH_OT_curve_patch_texture_slot_move);
  WM_operatortype_append(BRUSH_OT_asset_activate);
  WM_operatortype_append(BRUSH_OT_asset_save_as);
  WM_operatortype_append(BRUSH_OT_asset_edit_metadata);
  WM_operatortype_append(BRUSH_OT_asset_load_preview);
  WM_operatortype_append(BRUSH_OT_asset_delete);
  WM_operatortype_append(BRUSH_OT_asset_save);
  WM_operatortype_append(BRUSH_OT_asset_revert);

  /* image */
  WM_operatortype_append(PAINT_OT_texture_paint_toggle);
  WM_operatortype_append(PAINT_OT_image_paint);
  WM_operatortype_append(image::curve_patch::edit::PAINT_OT_image_curve_patch_edit);
  WM_operatortype_append(image::curve_patch::edit::PAINT_OT_image_curve_patch_handle_type_set);
  WM_operatortype_append(image::curve_patch::edit::PAINT_OT_image_curve_patch_delete_point);
  WM_operatortype_append(image::curve_patch::edit::PAINT_OT_image_curve_patch_toggle_cyclic);
  WM_operatortype_append(image::curve_patch::edit::PAINT_OT_image_curve_patch_switch_direction);
  WM_operatortype_append(PAINT_OT_sample_color);
  WM_operatortype_append(PAINT_OT_grab_clone);
  WM_operatortype_append(PAINT_OT_project_image);
  WM_operatortype_append(PAINT_OT_image_from_view);
  WM_operatortype_append(PAINT_OT_brush_colors_flip);
  WM_operatortype_append(PAINT_OT_material_paint_brush_ensure);
  WM_operatortype_append(PAINT_OT_material_paint_source_mode_set);
  WM_operatortype_append(PAINT_OT_material_paint_images_ensure);
  WM_operatortype_append(PAINT_OT_material_paint_brush_sync);
  WM_operatortype_append(PAINT_OT_material_channel_value_invert);
  WM_operatortype_append(PAINT_OT_material_channel_source_clear);
  WM_operatortype_append(PAINT_OT_material_channel_source_image_set);
  WM_operatortype_append(PAINT_OT_material_canvas_cycle);
  WM_operatortype_append(PAINT_OT_add_texture_paint_slot);
  WM_operatortype_append(PAINT_OT_add_simple_uvs);

  /* texture assignment */
  WM_operatortype_append(BRUSH_OT_texture_slot_assign_image);

  /* weight */
  WM_operatortype_append(PAINT_OT_weight_paint_toggle);
  WM_operatortype_append(PAINT_OT_weight_paint);
  WM_operatortype_append(PAINT_OT_weight_set);
  WM_operatortype_append(PAINT_OT_weight_from_bones);
  WM_operatortype_append(PAINT_OT_weight_gradient);
  WM_operatortype_append(PAINT_OT_weight_sample);
  WM_operatortype_append(PAINT_OT_weight_sample_group);

  /* uv */
  WM_operatortype_append(SCULPT_OT_uv_sculpt_grab);
  WM_operatortype_append(SCULPT_OT_uv_sculpt_relax);
  WM_operatortype_append(SCULPT_OT_uv_sculpt_pinch);

  /* vertex selection */
  WM_operatortype_append(PAINT_OT_vert_select_all);
  WM_operatortype_append(PAINT_OT_vert_select_ungrouped);
  WM_operatortype_append(PAINT_OT_vert_select_hide);
  WM_operatortype_append(PAINT_OT_vert_select_linked);
  WM_operatortype_append(PAINT_OT_vert_select_linked_pick);
  WM_operatortype_append(PAINT_OT_vert_select_more);
  WM_operatortype_append(PAINT_OT_vert_select_less);
  WM_operatortype_append(PAINT_OT_vert_select_loop);

  /* vertex */
  WM_operatortype_append(PAINT_OT_vertex_paint_toggle);
  WM_operatortype_append(PAINT_OT_vertex_paint);
  WM_operatortype_append(PAINT_OT_vertex_color_set);
  WM_operatortype_append(PAINT_OT_vertex_color_smooth);

  WM_operatortype_append(PAINT_OT_vertex_color_brightness_contrast);
  WM_operatortype_append(PAINT_OT_vertex_color_hsv);
  WM_operatortype_append(PAINT_OT_vertex_color_invert);
  WM_operatortype_append(PAINT_OT_vertex_color_levels);
  WM_operatortype_append(PAINT_OT_vertex_color_from_weight);

  /* face-select */
  WM_operatortype_append(PAINT_OT_face_select_linked);
  WM_operatortype_append(PAINT_OT_face_select_linked_pick);
  WM_operatortype_append(PAINT_OT_face_select_all);
  WM_operatortype_append(PAINT_OT_face_select_more);
  WM_operatortype_append(PAINT_OT_face_select_less);
  WM_operatortype_append(PAINT_OT_face_select_hide);
  WM_operatortype_append(PAINT_OT_face_select_loop);

  WM_operatortype_append(PAINT_OT_face_vert_reveal);

  /* material attributes (Poly Paint) */
  WM_operatortype_append(PAINT_OT_material_attribute_add);
  WM_operatortype_append(PAINT_OT_material_attribute_remove);

  /* partial visibility */
  WM_operatortype_append(hide::PAINT_OT_hide_show_all);
  WM_operatortype_append(hide::PAINT_OT_hide_show_masked);
  WM_operatortype_append(hide::PAINT_OT_hide_show);
  WM_operatortype_append(hide::PAINT_OT_hide_show_lasso_gesture);
  WM_operatortype_append(hide::PAINT_OT_hide_show_line_gesture);
  WM_operatortype_append(hide::PAINT_OT_hide_show_polyline_gesture);
  WM_operatortype_append(hide::PAINT_OT_visibility_invert);
  WM_operatortype_append(hide::PAINT_OT_visibility_filter);

  /* paint masking */
  WM_operatortype_append(mask::PAINT_OT_mask_flood_fill);
  WM_operatortype_append(mask::PAINT_OT_mask_lasso_gesture);
  WM_operatortype_append(mask::PAINT_OT_mask_box_gesture);
  WM_operatortype_append(mask::PAINT_OT_mask_line_gesture);
  WM_operatortype_append(mask::PAINT_OT_mask_polyline_gesture);

  /* image selection */
  WM_operatortype_append(PAINT_OT_image_select_all);
  WM_operatortype_append(PAINT_OT_image_select_none);
  WM_operatortype_append(PAINT_OT_image_select_box);
  WM_operatortype_append(PAINT_OT_image_select_lasso);
  WM_operatortype_append(PAINT_OT_image_select_polyline);
  WM_operatortype_append(PAINT_OT_image_select_circle);
  WM_operatortype_append(PAINT_OT_image_select_invert);
  WM_operatortype_append(PAINT_OT_image_select_move);
  WM_operatortype_append(PAINT_OT_image_select_move_confirm);
  WM_operatortype_append(PAINT_OT_image_select_move_cancel);
  WM_operatortype_append(PAINT_OT_image_select_move_undo_step);
  WM_operatortype_append(PAINT_OT_image_select_copy);
  WM_operatortype_append(PAINT_OT_image_select_paste);
  WM_operatortype_append(PAINT_OT_image_select_transform);
  WM_operatortype_append(PAINT_OT_image_select_transform_confirm);
  WM_operatortype_append(PAINT_OT_image_select_transform_cancel);
  WM_operatortype_append(PAINT_OT_image_select_transform_drag);

  WM_operatortype_append(PAINT_OT_image_select_gradient);
  WM_operatortype_append(PAINT_OT_image_select_gradient_apply);
  WM_operatortype_append(PAINT_OT_image_select_gradient_cancel);

  WM_operatortype_append(PAINT_OT_image_select_warp);
  WM_operatortype_append(PAINT_OT_image_select_warp_confirm);
  WM_operatortype_append(PAINT_OT_image_select_warp_cancel);
  WM_operatortype_append(PAINT_OT_image_select_warp_undo_step);

  image_paint_clipboard_ensure_atexit_handler();
}

void ED_keymap_paint(wmKeyConfig *keyconf)
{
  using namespace blender::ed::sculpt_paint;
  wmKeyMap *keymap;

  keymap = WM_keymap_ensure(keyconf, "Paint Curve", SPACE_EMPTY, RGN_TYPE_WINDOW);
  keymap->poll = paint_curve_poll;
  {
    KeyMapItem_Params params{};
    params.type = EVT_DKEY;
    params.value = KM_PRESS;
    params.modifier = KM_SHIFT;
    params.direction = KM_ANY;
    WM_keymap_add_item(keymap, "PAINTCURVE_OT_duplicate_move", &params);
  }
  paintcurve_slide_modal_keymap(keyconf);
  curve_patch_edit_modal_keymap(keyconf);

  /* Sculpt mode */
  keymap = WM_keymap_ensure(keyconf, "Sculpt", SPACE_EMPTY, RGN_TYPE_WINDOW);
  keymap->poll = sculpt_mode_poll;

  /* Vertex Paint mode */
  keymap = WM_keymap_ensure(keyconf, "Vertex Paint", SPACE_EMPTY, RGN_TYPE_WINDOW);
  keymap->poll = vertex_paint_mode_poll;

  /* Weight Paint mode */
  keymap = WM_keymap_ensure(keyconf, "Weight Paint", SPACE_EMPTY, RGN_TYPE_WINDOW);
  keymap->poll = weight_paint_mode_poll;

  /* Weight paint's Vertex Selection Mode. */
  keymap = WM_keymap_ensure(
      keyconf, "Paint Vertex Selection (Weight, Vertex)", SPACE_EMPTY, RGN_TYPE_WINDOW);
  keymap->poll = vert_paint_poll;

  /* Image/Texture Paint mode */
  keymap = WM_keymap_ensure(keyconf, "Image Paint", SPACE_EMPTY, RGN_TYPE_WINDOW);
  keymap->poll = image_texture_paint_poll;

  /* face-mask mode */
  keymap = WM_keymap_ensure(
      keyconf, "Paint Face Mask (Weight, Vertex, Texture)", SPACE_EMPTY, RGN_TYPE_WINDOW);
  keymap->poll = facemask_paint_poll;

  /* paint stroke */
  keymap = paint_stroke_modal_keymap(keyconf);
  WM_modalkeymap_assign(keymap, "SCULPT_OT_brush_stroke");
  WM_modalkeymap_assign(keymap, "PAINT_OT_vertex_paint");
  WM_modalkeymap_assign(keymap, "PAINT_OT_weight_paint");
  WM_modalkeymap_assign(keymap, "PAINT_OT_image_paint");

  /* Curves Sculpt mode. */
  keymap = WM_keymap_ensure(keyconf, "Sculpt Curves", SPACE_EMPTY, RGN_TYPE_WINDOW);
  keymap->poll = curves_sculpt_poll;
  {
    KeyMapItem_Params params{};
    params.type = EVT_DKEY;
    params.value = KM_PRESS;
    params.modifier = KM_SHIFT;
    params.direction = KM_ANY;
    WM_keymap_add_item(keymap, "PAINTCURVE_OT_duplicate_move", &params);
  }

  /* sculpt expand. */
  expand::modal_keymap(keyconf);

  /* Image paint floating selection (move / transform / warp). */
  image_select_floating_modal_keymap(keyconf);
}

}  // namespace blender
