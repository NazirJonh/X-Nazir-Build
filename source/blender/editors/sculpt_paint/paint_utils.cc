/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <unordered_map>

#include "DNA_mesh_types.h"
#include "DNA_object_types.h"

#include "DNA_brush_types.h"
#include "DNA_scene_types.h"

#include "BLI_listbase.h"
#include "BLI_math_color.h"
#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_rect.h"

#include "BLT_translation.hh"

#include "BKE_brush.hh"
#include "BKE_bvhutils.hh"
#include "BKE_colorband.hh"
#include "BKE_colortools.hh"
#include "BKE_context.hh"
#include "BKE_layer.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_sample.hh"
#include "BKE_paint.hh"
#include "BKE_report.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_prototypes.hh"

#include "RE_texture.h"

#include "ED_screen.hh"
#include "ED_select_utils.hh"
#include "ED_view3d.hh"

#include "ED_mesh.hh" /* for face mask functions */

#include "WM_api.hh"
#include "WM_types.hh"

#include "paint_gradient_core.hh"
#include "paint_intern.hh"

namespace blender {

bool paint_convert_bb_to_rect(rcti *rect,
                              const float bb_min[3],
                              const float bb_max[3],
                              const ARegion &region,
                              const RegionView3D &rv3d,
                              const Object &ob)
{
  int i, j, k;

  BLI_rcti_init_minmax(rect);

  /* return zero if the bounding box has non-positive volume */
  if (bb_min[0] > bb_max[0] || bb_min[1] > bb_max[1] || bb_min[2] > bb_max[2]) {
    return false;
  }

  const float4x4 projection = ED_view3d_ob_project_mat_get(&rv3d, &ob);

  for (i = 0; i < 2; i++) {
    for (j = 0; j < 2; j++) {
      for (k = 0; k < 2; k++) {
        float vec[3];
        int proj_i[2];
        vec[0] = i ? bb_min[0] : bb_max[0];
        vec[1] = j ? bb_min[1] : bb_max[1];
        vec[2] = k ? bb_min[2] : bb_max[2];
        /* convert corner to screen space */
        const float2 proj = ED_view3d_project_float_v2_m4(&region, vec, projection);
        /* expand 2D rectangle */

        /* we could project directly to int? */
        proj_i[0] = proj[0];
        proj_i[1] = proj[1];

        BLI_rcti_do_minmax_v(rect, proj_i);
      }
    }
  }

  /* return false if the rectangle has non-positive area */
  return rect->xmin < rect->xmax && rect->ymin < rect->ymax;
}

float paint_calc_object_space_radius(const ViewContext &vc,
                                     const float3 &center,
                                     const float pixel_radius)
{
  Object *ob = vc.obact;
  float delta[3], scale, loc[3];
  const float xy_delta[2] = {pixel_radius, 0.0f};

  mul_v3_m4v3(loc, ob->object_to_world().ptr(), center);

  const float zfac = ED_view3d_calc_zfac(vc.rv3d, loc);
  ED_view3d_win_to_delta(vc.region, xy_delta, zfac, delta);

  scale = fabsf(mat4_to_scale(ob->object_to_world().ptr()));
  scale = (scale == 0.0f) ? 1.0f : scale;

  return len_v3(delta) / scale;
}

bool paint_get_tex_pixel(const MTex *mtex,
                         float u,
                         float v,
                         ImagePool *pool,
                         int thread,
                         /* Return arguments. */
                         float *r_intensity,
                         float r_rgba[4])
{
  const float co[3] = {u, v, 0.0f};
  float intensity;
  const bool has_rgb = RE_texture_evaluate(
      mtex, co, thread, pool, false, false, &intensity, r_rgba);
  *r_intensity = intensity;

  if (!has_rgb) {
    r_rgba[0] = intensity;
    r_rgba[1] = intensity;
    r_rgba[2] = intensity;
    r_rgba[3] = 1.0f;
  }

  return has_rgb;
}

float paint_gradient_value_from_coord(const float coord)
{
  static const std::unique_ptr<ed::sculpt_paint::gradient::Calculator> calculator = []() {
    ed::sculpt_paint::gradient::Params params;
    params.type = ed::sculpt_paint::gradient::Type::Linear;
    params.space = ed::sculpt_paint::gradient::Space::Screen;
    params.start_ss = float2(0.0f, 0.0f);
    params.end_ss = float2(1.0f, 0.0f);
    params.hardness = 1.0f;
    params.clamp_to_range = false;
    params.curve = nullptr;
    return ed::sculpt_paint::gradient::create(params);
  }();

  return calculator->evaluate(float3(coord, 0.0f, 0.0f));
}

void paint_fill_gradient_params_from_brush(const Brush &brush,
                                           const float2 &start_ss,
                                           const float2 &end_ss,
                                           ed::sculpt_paint::gradient::Params &r_params)
{
  r_params.type = (brush.gradient_fill_mode == BRUSH_GRADIENT_LINEAR) ?
                      ed::sculpt_paint::gradient::Type::Linear :
                      ed::sculpt_paint::gradient::Type::Radial;
  r_params.space = ed::sculpt_paint::gradient::Space::Screen;
  r_params.start_ss = start_ss;
  r_params.end_ss = end_ss;
  r_params.hardness = 1.0f;
  r_params.clamp_to_range = false;
  r_params.curve = nullptr;
}

void paint_brush_gradient_color_from_factor(const Brush &brush,
                                            const float factor,
                                            float r_color[4])
{
  BKE_colorband_evaluate(brush.gradient, factor, r_color);
}

float paint_brush_gradient_coord(const Brush &brush, const float distance, const float pressure)
{
  const float gradient_spacing = (brush.gradient_spacing > 0) ? float(brush.gradient_spacing) :
                                                                1.0f;

  switch (brush.gradient_stroke_mode) {
    case BRUSH_GRADIENT_PRESSURE:
      return paint_gradient_value_from_coord(pressure);
    case BRUSH_GRADIENT_SPACING_REPEAT: {
      const float coord = fmod(distance / gradient_spacing, 1.0f);
      return paint_gradient_value_from_coord(coord);
    }
    case BRUSH_GRADIENT_SPACING_CLAMP: {
      const float coord = distance / gradient_spacing;
      return paint_gradient_value_from_coord(coord);
    }
    default:
      return 0.0f;
  }
}

float paint_gradient_finalize_factor(const Brush &brush,
                                     float factor,
                                     const bool clamp_to_range,
                                     const float multiplier)
{
  /* Для Gradient Tools кривая кисти не применяется - градиент уже имеет свою форму,
   * заданную через colorband и параметры калькулятора (hardness, clamp и т.д.).
   * Применение кривой приводит к обнулению валидных факторов и пропускам пикселей. */
  if (brush.flag & BRUSH_USE_GRADIENT) {
    if (clamp_to_range) {
      CLAMP(factor, 0.0f, 1.0f);
    }
    return factor * multiplier;
  }

  /* Для обычных кистей применяем кривую затухания. */
  factor = BKE_brush_curve_strength(&brush, max_ff(factor, 0.0f), 1.0f);
  if (clamp_to_range) {
    CLAMP(factor, 0.0f, 1.0f);
  }
  return factor * multiplier;
}

/* Cache disabled - not effective for gradient tool */
void clear_gradient_project_cache()
{
  /* Disabled - see optimization_plan.md for details */
}

float paint_projected_gradient_factor_with_symmetry(
    const ARegion *region,
    const ed::sculpt_paint::gradient::Calculator &calculator,
    const float3 &position,
    const int symmetry,
    const int8_t radial_symmetry[3])
{
  if (region == nullptr) {
    return 0.0f;
  }

  const auto evaluate_screen_factor = [&](const float3 &sample_position) {
    /* No cache - direct projection */
    float sample_v[3] = {sample_position.x, sample_position.y, sample_position.z};
    float screen_co[2];
    if (ED_view3d_project_float_object(
            region, sample_v, screen_co, V3D_PROJ_TEST_CLIP_BB | V3D_PROJ_TEST_CLIP_NEAR) !=
        V3D_PROJ_RET_OK)
    {
      return 0.0f;
    }

    return calculator.evaluate(float3(screen_co[0], screen_co[1], 0.0f));
  };

  const int symmetry_mask = (symmetry < 0) ? 0 : (symmetry & PAINT_SYMM_AXIS_ALL);

  const auto evaluate_with_mirror = [&](const float3 &sample_position) {
    float result = evaluate_screen_factor(sample_position);
    for (int symm_iter = 1; symm_iter <= symmetry_mask; symm_iter++) {
      if (!ed::sculpt_paint::is_symmetry_iteration_valid(char(symm_iter), char(symmetry_mask))) {
        continue;
      }

      const float3 symm_position = ed::sculpt_paint::symmetry_flip(sample_position,
                                                                   ePaintSymmetryFlags(symm_iter));
      const float symm_factor = evaluate_screen_factor(symm_position);
      result = (result > symm_factor) ? result : symm_factor;
    }
    return result;
  };

  auto rotate_radial = [&](const float3 &co, const int axis, const float angle) {
    const float cs = cosf(angle);
    const float sn = sinf(angle);
    switch (axis) {
      case 0:
        return float3(co.x, co.y * cs - co.z * sn, co.y * sn + co.z * cs);
      case 1:
        return float3(co.x * cs + co.z * sn, co.y, -co.x * sn + co.z * cs);
      default:
        return float3(co.x * cs - co.y * sn, co.x * sn + co.y * cs, co.z);
    }
  };

  /* Single-factor aggregation strategy: take max contribution across symmetry passes.
   * This avoids any additive/double-apply behavior in operator callsites. */
  float factor = evaluate_with_mirror(position);

  if (radial_symmetry != nullptr) {
    constexpr float two_pi = 6.28318530717958647692f;
    for (int axis = 0; axis < 3; axis++) {
      /* Match RNA constraints from Mesh.radial_symmetry (1..64) and guard against corrupted
       * runtime data. */
      const int steps = std::clamp(int(radial_symmetry[axis]), 1, 64);
      for (int i = 1; i < steps; i++) {
        const float angle = two_pi * (float(i) / float(steps));
        const float radial_factor = evaluate_with_mirror(rotate_radial(position, axis, angle));
        factor = (factor > radial_factor) ? factor : radial_factor;
      }
    }
  }

  return factor;
}

/**
 * Variant of paint_projected_gradient_factor_with_symmetry that uses pre-projected
 * screen coordinates instead of doing projection for each point.
 * This is an optimization for batch processing where all positions are projected upfront.
 *
 * Note: For symmetry != 0 or radial_symmetry != nullptr, this falls back to the original
 * function that does per-point projection. For best performance, use symmetry=0.
 */
float paint_projected_gradient_factor_with_preprojected(
    const ed::sculpt_paint::gradient::Calculator &calculator,
    const float2 &screen_co,
    const int symmetry,
    const int8_t radial_symmetry[3])
{
  /* Check for invalid screen coordinates (from failed projection) */
  if (screen_co[0] == 0.0f && screen_co[1] == 0.0f) {
    return 0.0f;
  }

  /* For symmetry cases, fall back to original function - it needs 3D position transformation.
   * radial_symmetry is an int8_t[3] array, always valid (not nullptr).
   * Values of 1 mean symmetry disabled, >1 means enabled. */
  const bool radial_sym_enabled = (radial_symmetry[0] != 1 || radial_symmetry[1] != 1 ||
                                   radial_symmetry[2] != 1);
  if (symmetry != 0 || radial_sym_enabled) {
    /* Return -1 to indicate "use fallback" - the caller will handle this */
    return -1.0f;
  }

  /* No symmetry - simple case, just evaluate the gradient */
  return calculator.evaluate(float3(screen_co[0], screen_co[1], 0.0f));
}

static void paint_brush_gradient_color(const Brush &brush,
                                       const float distance,
                                       const float pressure,
                                       float r_color[4])
{
  const float gradient_coord = paint_brush_gradient_coord(brush, distance, pressure);
  paint_brush_gradient_color_from_factor(brush, gradient_coord, r_color);
}

bool paint_brush_color_varies_during_stroke(const Paint &paint,
                                            const Brush &brush,
                                            const float last_pressure,
                                            const float pressure)
{
  const bool gradient_updates_color = (brush.flag & BRUSH_USE_GRADIENT) &&
                                      (ELEM(brush.gradient_stroke_mode,
                                            BRUSH_GRADIENT_SPACING_REPEAT,
                                            BRUSH_GRADIENT_SPACING_CLAMP) ||
                                       (last_pressure != pressure));

  return gradient_updates_color || BKE_brush_color_jitter_get_settings(&paint, &brush).has_value();
}

void paint_brush_color_get(const Paint *paint,
                           const Brush *br,
                           const std::optional<float3> &initial_hsv_jitter,
                           const bool invert,
                           const float distance,
                           const float pressure,
                           float r_color[3])
{
  if (invert) {
    copy_v3_v3(r_color, BKE_brush_secondary_color_get(paint, br));
  }
  else {
    const std::optional<BrushColorJitterSettings> color_jitter_settings =
        BKE_brush_color_jitter_get_settings(paint, br);
    if (br->flag & BRUSH_USE_GRADIENT) {
      float color_gr[4];
      paint_brush_gradient_color(*br, distance, pressure, color_gr);
      copy_v3_v3(r_color, color_gr);
    }
    else if (color_jitter_settings) {
      /* Perform color jitter with sRGB transfer function. This is inconsistent with other
       * paint modes which do it in linear space. But arguably it's better to do it in the
       * more perceptually uniform color space. */
      float3 color = BKE_brush_color_get(paint, br);
      linearrgb_to_srgb_v3_v3(color, color);
      color = BKE_paint_randomize_color(
          *color_jitter_settings, *initial_hsv_jitter, distance, pressure, color);
      srgb_to_linearrgb_v3_v3(r_color, color);
    }
    else {
      copy_v3_v3(r_color, BKE_brush_color_get(paint, br));
    }
  }
}

void paint_stroke_operator_properties(wmOperatorType *ot)
{
  static const EnumPropertyItem stroke_mode_items[] = {
      {int(BrushStrokeMode::Normal), "NORMAL", 0, "Regular", "Apply brush normally"},
      {int(BrushStrokeMode::Invert),
       "INVERT",
       0,
       "Invert",
       "Invert action of brush for duration of stroke"},
      {0},
  };

  static const EnumPropertyItem temporary_brush_toggle_items[] = {
      {int(BrushSwitchMode::None), "None", 0, "None", "Apply brush normally"},
      {int(BrushSwitchMode::Smooth),
       "SMOOTH",
       0,
       "Smooth",
       "Switch to smooth brush for duration of stroke"},
      {int(BrushSwitchMode::Erase),
       "ERASE",
       0,
       "Erase",
       "Switch to erase brush for duration of stroke"},
      {int(BrushSwitchMode::Mask),
       "MASK",
       0,
       "Mask",
       "Switch to mask brush for duration of stroke"},
      {0},
  };

  PropertyRNA *prop;

  prop = RNA_def_collection_runtime(ot->srna, "stroke", RNA_OperatorStrokeElement, "Stroke", "");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);

  prop = RNA_def_enum(ot->srna,
                      "mode",
                      stroke_mode_items,
                      int(BrushStrokeMode::Normal),
                      "Stroke Mode",
                      "Action taken when a paint stroke is made");
  RNA_def_property_translation_context(prop, BLT_I18NCONTEXT_OPERATOR_DEFAULT);
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);

  prop = RNA_def_enum(ot->srna,
                      "brush_toggle",
                      temporary_brush_toggle_items,
                      int(BrushSwitchMode::None),
                      "Temporary Brush Toggle Type",
                      "Brush to use for duration of stroke");
  RNA_def_property_translation_context(prop, BLT_I18NCONTEXT_OPERATOR_DEFAULT);
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);

  /* TODO: Pen flip logic should likely be combined into the stroke mode logic instead of being
   * an entirely separate concept. */
  prop = RNA_def_boolean(
      ot->srna, "pen_flip", false, "Pen Flip", "Whether a tablet's eraser mode is being used");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
}

void paint_gradient_operator_properties(wmOperatorType *ot,
                                        const int default_type,
                                        const int default_space)
{
  static const EnumPropertyItem gradient_types[] = {
      {WPAINT_GRADIENT_TYPE_LINEAR, "LINEAR", 0, "Linear", ""},
      {WPAINT_GRADIENT_TYPE_RADIAL, "RADIAL", 0, "Radial", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };
  static const EnumPropertyItem gradient_spaces[] = {
      {PAINT_GRADIENT_SPACE_WORLD, "WORLD", 0, "World", "Use world-space coordinates"},
      {PAINT_GRADIENT_SPACE_SCREEN, "SCREEN", 0, "Screen", "Use screen-space coordinates"},
      {PAINT_GRADIENT_SPACE_UV, "UV", 0, "UV", "Use UV-space coordinates"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  PropertyRNA *prop = RNA_def_enum(ot->srna, "type", gradient_types, default_type, "Type", "");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);

  prop = RNA_def_enum(ot->srna, "space", gradient_spaces, default_space, "Space", "");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);

  prop = RNA_def_float(
      ot->srna, "hardness", 1.0f, 0.0f, 1.0f, "Hardness", "Gradient hardness", 0.0f, 1.0f);
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);

  prop = RNA_def_boolean(
      ot->srna, "clamp_to_range", false, "Clamp to Range", "Clamp evaluated factor to [0, 1]");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);

  prop = RNA_def_boolean(
      ot->srna,
      "clip_before_start",
      false,
      "Clip Before Start",
      "For linear gradient: reject pixels before the start point (old behavior). "
      "When off, pixels before start are painted with original color");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
}

/* face-select ops */
static wmOperatorStatus paint_select_linked_exec(bContext *C, wmOperator * /*op*/)
{
  paintface_select_linked(C, CTX_data_active_object(C), nullptr, true);
  ED_region_tag_redraw(CTX_wm_region(C));
  return OPERATOR_FINISHED;
}

void PAINT_OT_face_select_linked(wmOperatorType *ot)
{
  ot->name = "Select Linked";
  ot->description = "Select linked faces";
  ot->idname = "PAINT_OT_face_select_linked";

  ot->exec = paint_select_linked_exec;
  ot->poll = facemask_paint_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static wmOperatorStatus paint_select_linked_pick_invoke(bContext *C,
                                                        wmOperator *op,
                                                        const wmEvent *event)
{
  const bool select = !RNA_boolean_get(op->ptr, "deselect");
  view3d_operator_needs_gpu(C);
  paintface_select_linked(C, CTX_data_active_object(C), event->mval, select);
  ED_region_tag_redraw(CTX_wm_region(C));
  return OPERATOR_FINISHED;
}

void PAINT_OT_face_select_linked_pick(wmOperatorType *ot)
{
  ot->name = "Select Linked Pick";
  ot->description = "Select linked faces under the cursor";
  ot->idname = "PAINT_OT_face_select_linked_pick";

  ot->invoke = paint_select_linked_pick_invoke;
  ot->poll = facemask_paint_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_boolean(ot->srna, "deselect", false, "Deselect", "Deselect rather than select items");
}

static wmOperatorStatus face_select_all_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  if (paintface_deselect_all_visible(C, ob, RNA_enum_get(op->ptr, "action"), true)) {
    ED_region_tag_redraw(CTX_wm_region(C));
    return OPERATOR_FINISHED;
  }
  return OPERATOR_CANCELLED;
}

void PAINT_OT_face_select_all(wmOperatorType *ot)
{
  ot->name = "(De)select All";
  ot->description = "Change selection for all faces";
  ot->idname = "PAINT_OT_face_select_all";

  ot->exec = face_select_all_exec;
  ot->poll = facemask_paint_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  WM_operator_properties_select_all(ot);
}

static wmOperatorStatus paint_select_more_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  Mesh *mesh = BKE_mesh_from_object(ob);
  if (mesh == nullptr || mesh->faces_num == 0) {
    return OPERATOR_CANCELLED;
  }

  const bool face_step = RNA_boolean_get(op->ptr, "face_step");
  paintface_select_more(mesh, face_step);
  paintface_flush_flags(C, ob, true, false);

  ED_region_tag_redraw(CTX_wm_region(C));
  return OPERATOR_FINISHED;
}

void PAINT_OT_face_select_more(wmOperatorType *ot)
{
  ot->name = "Select More";
  ot->description = "Select Faces connected to existing selection";
  ot->idname = "PAINT_OT_face_select_more";

  ot->exec = paint_select_more_exec;
  ot->poll = facemask_paint_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_boolean(
      ot->srna, "face_step", true, "Face Step", "Also select faces that only touch on a corner");
}

static wmOperatorStatus paint_select_less_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  Mesh *mesh = BKE_mesh_from_object(ob);
  if (mesh == nullptr || mesh->faces_num == 0) {
    return OPERATOR_CANCELLED;
  }

  const bool face_step = RNA_boolean_get(op->ptr, "face_step");
  paintface_select_less(mesh, face_step);
  paintface_flush_flags(C, ob, true, false);

  ED_region_tag_redraw(CTX_wm_region(C));
  return OPERATOR_FINISHED;
}

void PAINT_OT_face_select_less(wmOperatorType *ot)
{
  ot->name = "Select Less";
  ot->description = "Deselect Faces connected to existing selection";
  ot->idname = "PAINT_OT_face_select_less";

  ot->exec = paint_select_less_exec;
  ot->poll = facemask_paint_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_boolean(
      ot->srna, "face_step", true, "Face Step", "Also deselect faces that only touch on a corner");
}

static wmOperatorStatus paintface_select_loop_invoke(bContext *C,
                                                     wmOperator *op,
                                                     const wmEvent *event)
{
  const bool select = RNA_boolean_get(op->ptr, "select");
  const bool extend = RNA_boolean_get(op->ptr, "extend");
  if (!extend) {
    paintface_deselect_all_visible(C, CTX_data_active_object(C), SEL_DESELECT, false);
  }
  view3d_operator_needs_gpu(C);
  paintface_select_loop(C, CTX_data_active_object(C), event->mval, select);
  ED_region_tag_redraw(CTX_wm_region(C));
  return OPERATOR_FINISHED;
}

void PAINT_OT_face_select_loop(wmOperatorType *ot)
{
  ot->name = "Select Loop";
  ot->description = "Select face loop under the cursor";
  ot->idname = "PAINT_OT_face_select_loop";

  ot->invoke = paintface_select_loop_invoke;
  ot->poll = facemask_paint_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_boolean(ot->srna, "select", true, "Select", "If false, faces will be deselected");
  RNA_def_boolean(ot->srna, "extend", false, "Extend", "Extend the selection");
}

static wmOperatorStatus paintvert_select_loop_invoke(bContext *C,
                                                     wmOperator *op,
                                                     const wmEvent *event)
{
  const bool select = RNA_boolean_get(op->ptr, "select");
  const bool extend = RNA_boolean_get(op->ptr, "extend");
  if (!extend) {
    paintvert_deselect_all_visible(CTX_data_active_object(C), SEL_DESELECT, false);
  }
  view3d_operator_needs_gpu(C);
  paintvert_select_loop(C, CTX_data_active_object(C), event->mval, select);
  ED_region_tag_redraw(CTX_wm_region(C));
  return OPERATOR_FINISHED;
}

void PAINT_OT_vert_select_loop(wmOperatorType *ot)
{
  ot->name = "Select Loop";
  ot->description = "Select vertex loop under the cursor";
  ot->idname = "PAINT_OT_vert_select_loop";

  ot->invoke = paintvert_select_loop_invoke;
  ot->poll = vert_paint_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_boolean(ot->srna, "select", true, "Select", "If false, vertices will be deselected");
  RNA_def_boolean(ot->srna, "extend", false, "Extend", "Extend the selection");
}

static wmOperatorStatus vert_select_all_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  paintvert_deselect_all_visible(ob, RNA_enum_get(op->ptr, "action"), true);
  paintvert_tag_select_update(C, ob);
  ED_region_tag_redraw(CTX_wm_region(C));
  return OPERATOR_FINISHED;
}

void PAINT_OT_vert_select_all(wmOperatorType *ot)
{
  ot->name = "(De)select All";
  ot->description = "Change selection for all vertices";
  ot->idname = "PAINT_OT_vert_select_all";

  ot->exec = vert_select_all_exec;
  ot->poll = vert_paint_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  WM_operator_properties_select_all(ot);
}

static wmOperatorStatus vert_select_ungrouped_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  Mesh *mesh = id_cast<Mesh *>(ob->data);

  if (BLI_listbase_is_empty(&mesh->vertex_group_names) || mesh->deform_verts().is_empty()) {
    BKE_report(op->reports, RPT_ERROR, "No weights/vertex groups on object");
    return OPERATOR_CANCELLED;
  }

  paintvert_select_ungrouped(ob, RNA_boolean_get(op->ptr, "extend"), true);
  paintvert_tag_select_update(C, ob);
  ED_region_tag_redraw(CTX_wm_region(C));
  return OPERATOR_FINISHED;
}

void PAINT_OT_vert_select_ungrouped(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Select Ungrouped";
  ot->idname = "PAINT_OT_vert_select_ungrouped";
  ot->description = "Select vertices without a group";

  /* API callbacks. */
  ot->exec = vert_select_ungrouped_exec;
  ot->poll = vert_paint_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_boolean(ot->srna, "extend", false, "Extend", "Extend the selection");
}

static wmOperatorStatus paintvert_select_linked_exec(bContext *C, wmOperator * /*op*/)
{
  paintvert_select_linked(C, CTX_data_active_object(C));
  ED_region_tag_redraw(CTX_wm_region(C));
  return OPERATOR_FINISHED;
}

void PAINT_OT_vert_select_linked(wmOperatorType *ot)
{
  ot->name = "Select Linked Vertices";
  ot->description = "Select linked vertices";
  ot->idname = "PAINT_OT_vert_select_linked";

  ot->exec = paintvert_select_linked_exec;
  ot->poll = vert_paint_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static wmOperatorStatus paintvert_select_linked_pick_invoke(bContext *C,
                                                            wmOperator *op,
                                                            const wmEvent *event)
{
  const bool select = RNA_boolean_get(op->ptr, "select");
  view3d_operator_needs_gpu(C);

  paintvert_select_linked_pick(C, CTX_data_active_object(C), event->mval, select);
  ED_region_tag_redraw(CTX_wm_region(C));
  return OPERATOR_FINISHED;
}

void PAINT_OT_vert_select_linked_pick(wmOperatorType *ot)
{
  ot->name = "Select Linked Vertices Pick";
  ot->description = "Select linked vertices under the cursor";
  ot->idname = "PAINT_OT_vert_select_linked_pick";

  ot->invoke = paintvert_select_linked_pick_invoke;
  ot->poll = vert_paint_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_boolean(ot->srna,
                  "select",
                  true,
                  "Select",
                  "Whether to select or deselect linked vertices under the cursor");
}

static wmOperatorStatus paintvert_select_more_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  Mesh *mesh = BKE_mesh_from_object(ob);
  if (mesh == nullptr || mesh->faces_num == 0) {
    return OPERATOR_CANCELLED;
  }

  const bool face_step = RNA_boolean_get(op->ptr, "face_step");
  paintvert_select_more(mesh, face_step);

  paintvert_flush_flags(ob);
  paintvert_tag_select_update(C, ob);
  ED_region_tag_redraw(CTX_wm_region(C));

  return OPERATOR_FINISHED;
}

void PAINT_OT_vert_select_more(wmOperatorType *ot)
{
  ot->name = "Select More";
  ot->description = "Select Vertices connected to existing selection";
  ot->idname = "PAINT_OT_vert_select_more";

  ot->exec = paintvert_select_more_exec;
  ot->poll = vert_paint_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_boolean(
      ot->srna, "face_step", true, "Face Step", "Also select faces that only touch on a corner");
}

static wmOperatorStatus paintvert_select_less_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  Mesh *mesh = BKE_mesh_from_object(ob);
  if (mesh == nullptr || mesh->faces_num == 0) {
    return OPERATOR_CANCELLED;
  }

  const bool face_step = RNA_boolean_get(op->ptr, "face_step");
  paintvert_select_less(mesh, face_step);

  paintvert_flush_flags(ob);
  paintvert_tag_select_update(C, ob);
  ED_region_tag_redraw(CTX_wm_region(C));

  return OPERATOR_FINISHED;
}

void PAINT_OT_vert_select_less(wmOperatorType *ot)
{
  ot->name = "Select Less";
  ot->description = "Deselect Vertices connected to existing selection";
  ot->idname = "PAINT_OT_vert_select_less";

  ot->exec = paintvert_select_less_exec;
  ot->poll = vert_paint_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_boolean(
      ot->srna, "face_step", true, "Face Step", "Also deselect faces that only touch on a corner");
}

static wmOperatorStatus face_select_hide_exec(bContext *C, wmOperator *op)
{
  const bool unselected = RNA_boolean_get(op->ptr, "unselected");
  Object *ob = CTX_data_active_object(C);
  paintface_hide(C, ob, unselected);
  ED_region_tag_redraw(CTX_wm_region(C));
  return OPERATOR_FINISHED;
}

void PAINT_OT_face_select_hide(wmOperatorType *ot)
{
  ot->name = "Face Select Hide";
  ot->description = "Hide selected faces";
  ot->idname = "PAINT_OT_face_select_hide";

  ot->exec = face_select_hide_exec;
  ot->poll = facemask_paint_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_boolean(
      ot->srna, "unselected", false, "Unselected", "Hide unselected rather than selected objects");
}

static wmOperatorStatus vert_select_hide_exec(bContext *C, wmOperator *op)
{
  const bool unselected = RNA_boolean_get(op->ptr, "unselected");
  Object *ob = CTX_data_active_object(C);
  paintvert_hide(C, ob, unselected);
  ED_region_tag_redraw(CTX_wm_region(C));
  return OPERATOR_FINISHED;
}

void PAINT_OT_vert_select_hide(wmOperatorType *ot)
{
  ot->name = "Vertex Select Hide";
  ot->description = "Hide selected vertices";
  ot->idname = "PAINT_OT_vert_select_hide";

  ot->exec = vert_select_hide_exec;
  ot->poll = vert_paint_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_boolean(ot->srna,
                  "unselected",
                  false,
                  "Unselected",
                  "Hide unselected rather than selected vertices");
}

static wmOperatorStatus face_vert_reveal_exec(bContext *C, wmOperator *op)
{
  const bool select = RNA_boolean_get(op->ptr, "select");
  Object *ob = CTX_data_active_object(C);

  if (BKE_paint_select_vert_test(ob)) {
    paintvert_reveal(C, ob, select);
  }
  else {
    paintface_reveal(C, ob, select);
  }

  ED_region_tag_redraw(CTX_wm_region(C));
  return OPERATOR_FINISHED;
}

static bool face_vert_reveal_poll(bContext *C)
{
  Object *ob = CTX_data_active_object(C);

  /* Allow using this operator when no selection is enabled but hiding is applied. */
  return BKE_paint_select_elem_test(ob) || BKE_paint_always_hide_test(ob);
}

void PAINT_OT_face_vert_reveal(wmOperatorType *ot)
{
  ot->name = "Reveal Faces/Vertices";
  ot->description = "Reveal hidden faces and vertices";
  ot->idname = "PAINT_OT_face_vert_reveal";

  ot->exec = face_vert_reveal_exec;
  ot->poll = face_vert_reveal_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_boolean(ot->srna,
                  "select",
                  true,
                  "Select",
                  "Specifies whether the newly revealed geometry should be selected");
}

}  // namespace blender
