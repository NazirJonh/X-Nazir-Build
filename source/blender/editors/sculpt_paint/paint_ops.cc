/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "MEM_guardedalloc.h"

#include "BLI_ghash.h"
#include "BLI_listbase.h"
#include "BLI_math_color.h"
#include "BLI_math_vector.h"
#include "BLI_utildefines.h"

#include "IMB_interp.hh"

#include "DNA_brush_types.h"
#include "DNA_object_enums.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

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
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  ot->prop = RNA_def_enum(ot->srna,
                          "channel",
                          rna_enum_material_paint_channel_items,
                          PAINT_MATERIAL_CHANNEL_METALLIC,
                          "Channel",
                          "Material paint channel whose source texture should be cleared");
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
  const int created = BKE_paint_material_images_ensure_writable(
      *bmain, *ob, brush_paint, mode_settings, paint->visible_material_channels);
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

static const EnumPropertyItem brush_override_group_items[] = {
    {int(BrushOverrideGroup::FaceSets), "FACE_SETS", 0, "Face Sets", ""},
    {int(BrushOverrideGroup::Stroke), "STROKE", 0, "Stroke", ""},
    {int(BrushOverrideGroup::Falloff), "FALLOFF", 0, "Falloff", ""},
    {0, nullptr, 0, nullptr, nullptr},
};

static bool *brush_override_group_flag(Paint *paint, const BrushOverrideGroup group)
{
  switch (group) {
    case BrushOverrideGroup::FaceSets:
      return &paint->runtime->override_face_sets;
    case BrushOverrideGroup::Stroke:
      return &paint->runtime->override_stroke;
    case BrushOverrideGroup::Falloff:
      return &paint->runtime->override_falloff;
  }
  BLI_assert_unreachable();
  return nullptr;
}

static bool brush_group_override_toggle_poll(bContext *C)
{
  const Paint *paint = BKE_paint_get_active_from_context(C);
  return paint != nullptr && paint->runtime != nullptr &&
         BKE_paint_brush_for_read(paint) != nullptr;
}

static wmOperatorStatus brush_group_override_toggle_invoke(bContext *C,
                                                           wmOperator *op,
                                                           const wmEvent *event)
{
  Main *bmain = CTX_data_main(C);
  Paint *paint = BKE_paint_get_active_from_context(C);
  const BrushOverrideGroup group = BrushOverrideGroup(RNA_enum_get(op->ptr, "group"));

  if (event->modifier & KM_ALT) {
    if (!BKE_paint_brush_group_reset_from_asset(
            bmain, CTX_data_scene(C), paint, group, op->reports))
    {
      return OPERATOR_CANCELLED;
    }
    WM_main_add_notifier(NC_BRUSH | NA_EDITED, nullptr);
    WM_main_add_notifier(NC_TEXTURE | ND_NODES, nullptr);
    return OPERATOR_FINISHED;
  }

  bool *flag = brush_override_group_flag(paint, group);
  *flag = !*flag;
  WM_main_add_notifier(NC_BRUSH | NA_EDITED, nullptr);
  return OPERATOR_FINISHED;
}

void PAINT_OT_brush_group_override_toggle(wmOperatorType *ot)
{
  ot->name = "Toggle Brush Group Override";
  ot->description =
      "Keep this group of brush settings when switching brushes during this session. "
      "Alt click reverts the group to the values stored in the brush asset";
  ot->idname = "PAINT_OT_brush_group_override_toggle";

  ot->invoke = brush_group_override_toggle_invoke;
  ot->poll = brush_group_override_toggle_poll;

  ot->flag = OPTYPE_INTERNAL;

  RNA_def_enum(ot->srna,
               "group",
               brush_override_group_items,
               int(BrushOverrideGroup::Stroke),
               "Group",
               "Which group of brush settings to toggle");
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
  WM_operatortype_append(PAINT_OT_brush_group_override_toggle);
  WM_operatortype_append(PAINT_OT_material_paint_brush_ensure);
  WM_operatortype_append(PAINT_OT_material_paint_images_ensure);
  WM_operatortype_append(PAINT_OT_material_paint_brush_sync);
  WM_operatortype_append(PAINT_OT_material_channel_value_invert);
  WM_operatortype_append(PAINT_OT_material_channel_source_clear);
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
  WM_modalkeymap_assign(keymap, "GREASE_PENCIL_OT_brush_stroke");
  WM_modalkeymap_assign(keymap, "GREASE_PENCIL_OT_sculpt_paint");
  WM_modalkeymap_assign(keymap, "GREASE_PENCIL_OT_weight_brush_stroke");
  WM_modalkeymap_assign(keymap, "GREASE_PENCIL_OT_vertex_brush_stroke");
  WM_modalkeymap_assign(keymap, "SCULPT_CURVES_OT_brush_stroke");

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
