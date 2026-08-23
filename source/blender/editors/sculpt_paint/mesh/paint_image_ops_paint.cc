/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 * \brief Painting operator to paint in 2D and 3D.
 */

#include "DNA_brush_types.h"
#include "DNA_scene_types.h"
#include "DNA_space_types.h"

#include "BLI_math_base.hh"
#include "BLI_math_color.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_rect.h"
#include "BLI_utildefines.h"

#include <cfloat>
#include <memory>

#include "BKE_brush.hh"
#include "BKE_colortools.hh"
#include "BKE_context.hh"
#include "BKE_layer.hh"
#include "BKE_paint.hh"
#include "BKE_paint_types.hh"
#include "BKE_report.hh"
#include "BKE_undo_system.hh"

#include "ED_paint.hh"
#include "ED_view3d.hh"

#include "GPU_immediate.hh"
#include "GPU_state.hh"

#include "MEM_guardedalloc.h"

#include "RNA_access.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_image.hh"

#include "UI_resources.hh"
#include "UI_view2d.hh"

#include "../paint_curve_intern.hh"

#include "../paint_image_curve_patch.hh"
#include "../paint_image_curve_patch_anchor.hh"
#include "../paint_image_stroke_hook.hh"
#include "../paint_intern.hh"

namespace blender {

bool paint_image_viewport_fill_at_mouse(const bContext *C,
                                        const Paint *paint,
                                        Brush *brush,
                                        Object *ob,
                                        bool stroke_inverted,
                                        const float mouse[2])
{
  if (brush == nullptr || ob == nullptr) {
    return false;
  }
  if (brush->flag & BRUSH_USE_GRADIENT) {
    /* v2 — solid fallback for v1 */
    return false;
  }

  float color[3];
  if (stroke_inverted) {
    copy_v3_v3(color, BKE_brush_secondary_color_get(paint, brush));
  }
  else {
    copy_v3_v3(color, BKE_brush_color_get(paint, brush));
  }

  if (ELEM(brush->fill_expand,
           IMAGE_PAINT_SELECT_EXPAND_FACE,
           IMAGE_PAINT_SELECT_EXPAND_ISLAND,
           IMAGE_PAINT_SELECT_EXPAND_MESH))
  {
    return paint_image_proj_geometry_fill(C, color, brush, ob, mouse);
  }

  /* Pixel flood — minimal projection stroke session */
  bContext *C_mut = const_cast<bContext *>(C);
  Scene *scene = CTX_data_scene(C);
  ToolSettings *ts = scene->toolsettings;
  /* Sculpt fill: projection paint reads imapaint.paint/mode/canvas, not the sculpt canvas
   * (ts->paint_mode). Sync both for the duration of this one-shot stroke. */
  Brush *prev_imapaint_brush = ts->imapaint.paint.brush;
  int prev_imapaint_mode = ts->imapaint.mode;
  Image *prev_imapaint_canvas = ts->imapaint.canvas;
  ts->imapaint.paint.brush = brush;
  ts->imapaint.mode = ts->paint_mode.canvas_source;
  ts->imapaint.canvas = ts->paint_mode.canvas_image;
  void *stroke_handle = paint_proj_new_stroke(
      C_mut, ob, mouse, BrushStrokeMode::Normal, BrushSwitchMode::None);
  ts->imapaint.paint.brush = prev_imapaint_brush;
  ts->imapaint.mode = prev_imapaint_mode;
  ts->imapaint.canvas = prev_imapaint_canvas;
  if (stroke_handle == nullptr) {
    return false;
  }

  const float pressure = 1.0f;
  const float size = BKE_brush_radius_get(paint, brush);
  paint_proj_stroke(C, stroke_handle, mouse, mouse, 0, pressure, 0.0f, size);
  paint_proj_redraw(C, stroke_handle, false);
  paint_proj_redraw(C, stroke_handle, true);
  paint_proj_stroke_done(stroke_handle);
  return true;
}

}  // namespace blender

namespace blender {

namespace ed::sculpt_paint::image::ops::paint {

/**
 * Interface to use the same painting operator for 3D and 2D painting. Interface removes the
 * differences between the actual calls that are being performed.
 */
class AbstractPaintMode {
 public:
  virtual ~AbstractPaintMode() = default;
  virtual void *paint_new_stroke(bContext *C,
                                 wmOperator *op,
                                 Object *ob,
                                 const float mouse[2],
                                 BrushStrokeMode mode,
                                 BrushSwitchMode brush_switch_mode) = 0;
  virtual void paint_stroke(bContext *C,
                            void *stroke_handle,
                            float prev_mouse[2],
                            float mouse[2],
                            int eraser,
                            float pressure,
                            float distance,
                            float size) = 0;

  /**
   * Same as #paint_stroke with the per-dab 2D Roll mapping contract (#ImagePaintRollDab).
   * Only the Image Editor mode consumes the contract; all other modes must not receive a
   * non-null `roll_dab` and fall back to the plain #paint_stroke behavior.
   */
  virtual void paint_stroke_roll(bContext *C,
                                 void *stroke_handle,
                                 float prev_mouse[2],
                                 float mouse[2],
                                 int eraser,
                                 float pressure,
                                 float distance,
                                 float size,
                                 const ImagePaintRollDab *roll_dab)
  {
    BLI_assert(roll_dab == nullptr);
    UNUSED_VARS_NDEBUG(roll_dab);
    paint_stroke(C, stroke_handle, prev_mouse, mouse, eraser, pressure, distance, size);
  }

  virtual void paint_stroke_redraw(const bContext *C, void *stroke_handle, bool final) = 0;
  virtual void paint_stroke_done(void *stroke_handle) = 0;
  virtual void paint_gradient_fill(const bContext *C,
                                   const Paint *paint,
                                   Brush *brush,
                                   PaintStroke *stroke,
                                   void *stroke_handle,
                                   float mouse_start[2],
                                   float mouse_end[2]) = 0;
  virtual void paint_bucket_fill(const bContext *C,
                                 const Paint *paint,
                                 Brush *brush,
                                 PaintStroke *stroke,
                                 void *stroke_handle,
                                 float mouse_start[2],
                                 float mouse_end[2]) = 0;
};

class ImagePaintMode : public AbstractPaintMode {
 public:
  void *paint_new_stroke(bContext *C,
                         wmOperator *op,
                         Object * /*ob*/,
                         const float /*mouse*/[2],
                         const BrushStrokeMode mode,
                         const BrushSwitchMode /*brush_switch_mode*/) override
  {
    return paint_2d_new_stroke(C, op, mode);
  }

  void paint_stroke(bContext * /*C*/,
                    void *stroke_handle,
                    float prev_mouse[2],
                    float mouse[2],
                    int eraser,
                    float pressure,
                    float distance,
                    float size) override
  {
    paint_2d_stroke(stroke_handle, prev_mouse, mouse, eraser, pressure, distance, size);
  }

  void paint_stroke_roll(bContext * /*C*/,
                         void *stroke_handle,
                         float prev_mouse[2],
                         float mouse[2],
                         int eraser,
                         float pressure,
                         float distance,
                         float size,
                         const ImagePaintRollDab *roll_dab) override
  {
    paint_2d_stroke(stroke_handle, prev_mouse, mouse, eraser, pressure, distance, size, roll_dab);
  }

  void paint_stroke_redraw(const bContext *C, void *stroke_handle, bool final) override
  {
    paint_2d_redraw(C, stroke_handle, final);
  }

  void paint_stroke_done(void *stroke_handle) override
  {
    paint_2d_stroke_done(stroke_handle);
  }

  void paint_gradient_fill(const bContext *C,
                           const Paint * /*paint*/,
                           Brush *brush,
                           PaintStroke * /*stroke*/,
                           void *stroke_handle,
                           float mouse_start[2],
                           float mouse_end[2]) override
  {
    paint_2d_gradient_fill(C, brush, mouse_start, mouse_end, stroke_handle);
  }

  void paint_bucket_fill(const bContext *C,
                         const Paint *paint,
                         Brush *brush,
                         PaintStroke *stroke,
                         void *stroke_handle,
                         float mouse_start[2],
                         float mouse_end[2]) override
  {
    float color[3];
    if (stroke->stroke_inverted()) {
      copy_v3_v3(color, BKE_brush_secondary_color_get(paint, brush));
    }
    else {
      copy_v3_v3(color, BKE_brush_color_get(paint, brush));
    }
    paint_2d_bucket_fill(C, color, brush, mouse_start, mouse_end, stroke_handle);
  }
};

class ProjectionPaintMode : public AbstractPaintMode {
 public:
  void *paint_new_stroke(bContext *C,
                         wmOperator * /*op*/,
                         Object *ob,
                         const float mouse[2],
                         BrushStrokeMode mode,
                         BrushSwitchMode brush_switch_mode) override
  {
    return paint_proj_new_stroke(C, ob, mouse, mode, brush_switch_mode);
  }

  void paint_stroke(bContext *C,
                    void *stroke_handle,
                    float prev_mouse[2],
                    float mouse[2],
                    int eraser,
                    float pressure,
                    float distance,
                    float size) override
  {
    paint_proj_stroke(C, stroke_handle, prev_mouse, mouse, eraser, pressure, distance, size);
  };

  void paint_stroke_redraw(const bContext *C, void *stroke_handle, bool final) override
  {
    paint_proj_redraw(C, stroke_handle, final);
  }

  void paint_stroke_done(void *stroke_handle) override
  {
    paint_proj_stroke_done(stroke_handle);
  }

  void paint_gradient_fill(const bContext *C,
                           const Paint *paint,
                           Brush *brush,
                           PaintStroke *stroke,
                           void *stroke_handle,
                           float mouse_start[2],
                           float mouse_end[2]) override
  {
    paint_fill(C, paint, brush, stroke, stroke_handle, mouse_start, mouse_end);
  }

  void paint_bucket_fill(const bContext *C,
                         const Paint *paint,
                         Brush *brush,
                         PaintStroke *stroke,
                         void *stroke_handle,
                         float mouse_start[2],
                         float mouse_end[2]) override
  {
    paint_fill(C, paint, brush, stroke, stroke_handle, mouse_start, mouse_end);
  }

 private:
  void paint_fill(const bContext *C,
                  const Paint *paint,
                  Brush *brush,
                  PaintStroke *stroke,
                  void * /*stroke_handle*/,
                  float mouse_start[2],
                  float /*mouse_end*/[2])
  {
    paint_image_viewport_fill_at_mouse(
        C, paint, brush, CTX_data_active_object(C), stroke->stroke_inverted(), mouse_start);
  }
};

struct PaintOperation : public PaintModeData {
  AbstractPaintMode *mode = nullptr;

  void *stroke_handle = nullptr;

  float prevmouse[2] = {0.0f, 0.0f};
  float startmouse[2] = {0.0f, 0.0f};
  double starttime = 0.0;

  wmPaintCursor *cursor = nullptr;
  ViewContext vc = {nullptr};

  /* 2D Roll trajectory. Populated and read by `ImagePaintStroke::update_step` so the Roll
   * mapping contract reaches `paint_2d_stroke()` per dab without going through the 3D Roll
   * record path. `#texture_paint_init()` sets `enabled` based on the brush's stroke method and
   * texture mapping; the buffers stay empty for non-Image Paint modes. */
  ImagePaintRollTraj2D roll_traj;

  /* This stroke method's own state, or null for a method that needs none. Records the gesture
   * dab by dab and acts when the stroke ends -- see #ImageStrokeMethodHook. */
  std::unique_ptr<ImageStrokeMethodHook> method_hook;

  PaintOperation() = default;
  ~PaintOperation() override
  {
    MEM_delete(mode);
    mode = nullptr;

    if (cursor) {
      WM_paint_cursor_end(cursor);
      cursor = nullptr;
    }
  }
};

static void gradient_draw_line(bContext * /*C*/,
                               const int2 &xy,
                               const float2 & /*tilt*/,
                               void *customdata)
{
  PaintOperation *pop = static_cast<PaintOperation *>(customdata);

  if (pop) {
    GPU_line_smooth(true);
    GPU_blend(GPU_BLEND_ALPHA);

    GPUVertFormat *format = immVertexFormat();
    uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32);

    ARegion *region = pop->vc.region;

    immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

    GPU_line_width(4.0);
    immUniformColor4ub(0, 0, 0, 255);

    immBegin(GPU_PRIM_LINES, 2);
    immVertex2fv(pos, float2(xy));
    immVertex2f(
        pos, pop->startmouse[0] + region->winrct.xmin, pop->startmouse[1] + region->winrct.ymin);
    immEnd();

    GPU_line_width(2.0);
    immUniformColor4ub(255, 255, 255, 255);

    immBegin(GPU_PRIM_LINES, 2);
    immVertex2fv(pos, float2(xy));
    immVertex2f(
        pos, pop->startmouse[0] + region->winrct.xmin, pop->startmouse[1] + region->winrct.ymin);
    immEnd();

    immUnbindProgram();

    GPU_blend(GPU_BLEND_NONE);
    GPU_line_smooth(false);
  }
}

static std::unique_ptr<PaintOperation> texture_paint_init(bContext *C,
                                                          wmOperator *op,
                                                          const float mouse[2])
{
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  const Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ToolSettings *settings = scene->toolsettings;
  std::unique_ptr<PaintOperation> pop = std::make_unique<PaintOperation>();
  Brush *brush = BKE_paint_brush(&settings->imapaint.paint);
  auto mode = BrushStrokeMode(RNA_enum_get(op->ptr, "mode"));
  auto brush_switch_mode = BrushSwitchMode(RNA_enum_get(op->ptr, "brush_toggle"));
  pop->vc = ED_view3d_viewcontext_init(C, depsgraph);

  copy_v2_v2(pop->prevmouse, mouse);
  copy_v2_v2(pop->startmouse, mouse);

  ViewLayer *view_layer = CTX_data_view_layer(C);
  BKE_view_layer_synced_ensure(*bmain, scene, view_layer);
  Object *ob = BKE_view_layer_active_object_get(view_layer);

  /* initialize from context */
  if (CTX_wm_region_view3d(C)) {
    bool uvs, mat, tex, stencil;
    if (!ED_paint_proj_mesh_data_check(*scene, *ob, &uvs, &mat, &tex, &stencil)) {
      ED_paint_data_warning(op->reports, uvs, mat, tex, stencil);
      WM_event_add_notifier(C, NC_SCENE | ND_TOOLSETTINGS, nullptr);
      return nullptr;
    }
    pop->mode = MEM_new<ProjectionPaintMode>("ProjectionPaintMode");
  }
  else {
    pop->mode = MEM_new<ImagePaintMode>("ImagePaintMode");

    /* 2D Roll mapping must be served by the local UV trajectory, never by the 3D Roll record
     * path that lives on `PaintStroke::RollSpline`. Decide once here so each `update_step`
     * call only checks a single boolean instead of recomputing brush state every dab. Empty
     * trajectory is fine: a brush that does not use Roll never pushes samples. */
    const bool brush_uses_roll_texture = (brush->mtex.brush_map_mode == MTEX_MAP_MODE_ROLL) ||
                                         (brush->mask_mtex.brush_map_mode == MTEX_MAP_MODE_ROLL);
    pop->roll_traj.enabled =
        (brush->stroke_method == BRUSH_STROKE_ROLL) ||
        (ELEM(brush->stroke_method, BRUSH_STROKE_CURVE, BRUSH_STROKE_CURVE_PATCH) &&
         brush_uses_roll_texture);
    pop->roll_traj.clear();

    /* The only stroke method that currently needs per-stroke state of its own. Created here, in
     * the Image Editor branch alone, because that is where the Curve Patch anchor is supported;
     * every other stroke leaves the pointer null and pays nothing. */
    if (brush->stroke_method == BRUSH_STROKE_CURVE_PATCH) {
      pop->method_hook = image_curve_patch_anchor_hook_create();
    }
  }

  pop->stroke_handle = pop->mode->paint_new_stroke(C, op, ob, mouse, mode, brush_switch_mode);
  if (!pop->stroke_handle) {
    return nullptr;
  }

  if ((brush->image_brush_type == IMAGE_PAINT_BRUSH_TYPE_FILL) &&
      (brush->flag & BRUSH_USE_GRADIENT))
  {
    pop->cursor = WM_paint_cursor_activate(
        SPACE_TYPE_ANY, RGN_TYPE_ANY, ED_image_tools_paint_poll, gradient_draw_line, pop.get());
  }

  settings->imapaint.flag |= IMAGEPAINT_DRAWING;
  ED_image_undo_push_begin(op->type->name, PaintMode::Texture2D);

  BKE_curvemapping_init(brush->curve_rand_hue);
  BKE_curvemapping_init(brush->curve_rand_saturation);
  BKE_curvemapping_init(brush->curve_rand_value);

  return pop;
}

struct ImagePaintStroke final : public PaintStroke {
  ImagePaintStroke(bContext *C, wmOperator *op, const int event_type)
      : PaintStroke(C, op, event_type)
  {
  }

  bool get_location(float location[3], const float mouse[2], bool force_original) override;
  bool test_start(wmOperator *op, const float mouse[2]) override;
  void update_step(wmOperator *op, PointerRNA *itemptr) override;
  void redraw(bool final) override;
  bool test_cancel() override;
  void done(bool is_cancel, bool stroke_started) override;

  void update_for_exec(bContext *C,
                       const Brush &brush,
                       PaintMode mode,
                       const float mouse_init[2],
                       float mouse[2],
                       float pressure,
                       float r_location[3],
                       bool *r_location_is_set);
};

void ImagePaintStroke::update_step(wmOperator *op, PointerRNA *itemptr)
{
  PaintOperation *pop = static_cast<PaintOperation *>(mode_data_.get());
  BLI_assert(pop != nullptr);
  if (pop == nullptr) {
    return;
  }

  Paint *paint = BKE_paint_get_active_from_context(this->evil_C);
  bke::PaintRuntime *paint_runtime = paint->runtime;
  Brush *brush = BKE_paint_brush(paint);

  float alphafac = (brush->flag & BRUSH_ACCUMULATE) ? paint_runtime->overlap_factor : 1.0f;

  /* initial brush values. Maybe it should be considered moving these to stroke system */
  float startalpha = BKE_brush_alpha_get(paint, brush);

  float mouse[2];
  float pressure;
  float size;
  float distance = this->stroke_distance();
  int eraser;

  RNA_float_get_array(itemptr, "mouse", mouse);
  pressure = RNA_float_get(itemptr, "pressure");
  eraser = RNA_boolean_get(op->ptr, "pen_flip");
  size = RNA_float_get(itemptr, "size");

  /* stroking with fill tool only acts on stroke end */
  if (brush->image_brush_type == IMAGE_PAINT_BRUSH_TYPE_FILL) {
    copy_v2_v2(pop->prevmouse, mouse);
    return;
  }

  if (BKE_brush_use_alpha_pressure(brush)) {
    pressure = BKE_curvemapping_evaluateF(brush->curve_strength, 0, pressure);
    BKE_brush_alpha_set(paint, brush, max_ff(0.0f, startalpha * pressure * alphafac));
  }
  else {
    BKE_brush_alpha_set(paint, brush, max_ff(0.0f, startalpha * alphafac));
  }

  if (ELEM(brush->stroke_method, BRUSH_STROKE_DRAG_DOT, BRUSH_STROKE_ANCHORED)) {
    UndoStack *ustack = CTX_wm_manager(this->evil_C)->runtime->undo_stack;
    ED_image_undo_restore(ustack->step_init);
  }

  /* Stack-allocated dab so the address is stable across the call into #paint_stroke_roll.
   * `nullptr` keeps the existing path identical for any brush without 2D Roll mapping. */
  ImagePaintRollDab roll_dab_local;
  const ImagePaintRollDab *roll_dab = nullptr;

  /* A stroke-method hook needs the per-dab UV whether or not the brush uses Roll texture mapping:
   * for the Curve Patch anchor the UVs ARE the gesture that becomes the editable curve. Gating
   * this on `roll_traj.enabled` (as the first implementation did) silently produced an empty
   * anchor for every non-Roll brush, so no session was ever opened and no curve appeared. */
  if (pop->roll_traj.enabled || pop->method_hook) {
    /* `mouse` here is in window-absolute region pixels, the same coordinate space that
     * `paint_2d_stroke` passes to `view2d_region_to_view`. In Image Editor, `region->v2d.mask`
     * spans `[0, winx] x [0, winy]`, so the conversion is direct without a winrct subtract
     * (the mask already covers the whole region). */
    ARegion *region = CTX_wm_region(this->evil_C);
    float dab_uv[2] = {0.0f, 0.0f};
    if (region && region->v2d.mask.xmax > region->v2d.mask.xmin &&
        region->v2d.mask.ymax > region->v2d.mask.ymin)
    {
      ui::view2d_region_to_view(&region->v2d, mouse[0], mouse[1], &dab_uv[0], &dab_uv[1]);

      if (pop->roll_traj.enabled) {
        pop->roll_traj.append(float2(dab_uv[0], dab_uv[1]));

        /* Stage 3 mapping (`brush_painter_2d_tex_mapping`) reads
         * `paint_runtime->start_pixel_radius` directly; `radius_uv` therefore stays
         * informational here and defaults to `0` -- the Tile-local canvas pixels would need a
         * per-dab UV conversion, which the opaque `ImagePaintState` does not yet expose. The
         * contract reserves the field for when a future consumer needs it; the current 2D Roll
         * path does not. */
        pop->roll_traj.compute_roll_dab(0.0f, roll_dab_local);
        roll_dab = &roll_dab_local;
      }

      if (pop->method_hook) {
        pop->method_hook->on_dab(float2(dab_uv[0], dab_uv[1]), pressure);
      }
    }
  }
  /* A recording hook (the Curve Patch anchor) must never paint real dabs onto the canvas: it
   * only captures the gesture. Rolling the dabs back afterwards is NOT equivalent -- that restore
   * leaves a visible remnant of the anchor's own stroke permanently baked under the patch.
   * Skipping the paint call is what actually prevents the remnant from ever reaching the canvas.
   */
  if (!pop->method_hook || !pop->method_hook->suppresses_dabs()) {
    pop->mode->paint_stroke_roll(this->evil_C,
                                 pop->stroke_handle,
                                 pop->prevmouse,
                                 mouse,
                                 eraser,
                                 pressure,
                                 distance,
                                 size,
                                 roll_dab);
  }

  copy_v2_v2(pop->prevmouse, mouse);

  /* restore brush values */
  BKE_brush_alpha_set(paint, brush, startalpha);
}

void ImagePaintStroke::redraw(bool final)
{
  PaintOperation *pop = static_cast<PaintOperation *>(mode_data_.get());
  BLI_assert(pop != nullptr);
  if (pop == nullptr) {
    return;
  }

  pop->mode->paint_stroke_redraw(this->evil_C, pop->stroke_handle, final);
}

void ImagePaintStroke::done(const bool is_cancel, const bool stroke_started)
{
  Scene *scene = CTX_data_scene(this->evil_C);
  ToolSettings *toolsettings = scene->toolsettings;
  PaintOperation *pop = static_cast<PaintOperation *>(mode_data_.get());

  if (!pop) {
    return;
  }

  const Paint *paint = BKE_paint_get_active_from_context(this->evil_C);
  Brush *brush = BKE_paint_brush(&toolsettings->imapaint.paint);

  toolsettings->imapaint.flag &= ~IMAGEPAINT_DRAWING;

  if (brush->image_brush_type == IMAGE_PAINT_BRUSH_TYPE_FILL) {
    if (brush->flag & BRUSH_USE_GRADIENT) {
      pop->mode->paint_gradient_fill(
          this->evil_C, paint, brush, this, pop->stroke_handle, pop->startmouse, pop->prevmouse);
    }
    else {
      pop->mode->paint_bucket_fill(
          this->evil_C, paint, brush, this, pop->stroke_handle, pop->startmouse, pop->prevmouse);
    }
  }
  pop->mode->paint_stroke_done(pop->stroke_handle);
  pop->stroke_handle = nullptr;

  /* Drop the dab trajectory so the next stroke starts empty regardless of brush changes. The
   * `PaintOperation` itself is destroyed right after this, but keeping `clear()` here matches
   * the contract documented on #ImagePaintRollTraj2D and would survive any future caller that
   * kept `mode_data_` alive past `done()`. */
  pop->roll_traj.clear();

  /* Let this stroke method act on the finished gesture. A hook that reports true has taken over
   * the in-flight image undo step -- it either aborted it or replaced it with one of its own --
   * so this stroke must not close it. */
  bool undo_step_taken_over = false;
  if (pop->method_hook) {
    undo_step_taken_over = pop->method_hook->on_stroke_end(
        *this->evil_C, is_cancel, stroke_started);
  }

  if (!undo_step_taken_over && !is_cancel) {
    ED_image_undo_push_end();
  }

/* duplicate warning, see texpaint_init */
#if 0
  if (pop->s.warnmultifile) {
    BKE_reportf(op->reports,
                RPT_WARNING,
                "Image requires 4 color channels to paint: %s",
                pop->s.warnmultifile);
  }
  if (pop->s.warnpackedfile) {
    BKE_reportf(op->reports,
                RPT_WARNING,
                "Packed MultiLayer files cannot be painted: %s",
                pop->s.warnpackedfile);
  }
#endif
}
bool ImagePaintStroke::get_location(float /*location*/[3],
                                    const float /*mouse*/[2],
                                    bool /*force_original*/)
{
  return true;
}

bool ImagePaintStroke::test_cancel()
{
  return true;
}

bool ImagePaintStroke::test_start(wmOperator *op, const float mouse[2])
{
  std::unique_ptr<PaintOperation> pop;

  /* TODO: Should avoid putting this here. Instead, last position should be requested
   * from stroke system. */

  if (!(pop = texture_paint_init(this->evil_C, op, mouse))) {
    return false;
  }

  mode_data_ = std::move(pop);

  return true;
}

/**
 * Hand a finished anchor stroke over to the live Curve Patch modal editor.
 *
 * `ImagePaintStroke::done()` opens the #ImageCurvePatchSession when the anchor completed with
 * `BRUSH_STROKE_CURVE_PATCH`, but the session is inert until the modal editor that owns its
 * lifetime is invoked. Both operator exits that can follow a finished stroke must run this: a
 * real drag ends inside #paint_modal, while a stroke that finishes on the very first event ends
 * inside #paint_invoke.
 *
 * `InvokeDefault` runs the operator's `poll()`. When the image context was lost during the
 * anchor the poll fails and nothing adopts the session, so cancel it here -- otherwise it leaks
 * together with its open image-undo transaction, leaving painted pixels no undo entry covers.
 */
static void curve_patch_session_takeover(bContext *C)
{
  if (!image_curve_patch_session_active()) {
    return;
  }
  WM_operator_name_call(
      C, "PAINT_OT_image_curve_patch_edit", wm::OpCallContext::InvokeDefault, nullptr, nullptr);

  ImageCurvePatchSession *session = image_curve_patch_session_active_get();
  if (session != nullptr && !session->modal_active) {
    image_curve_patch_session_cancel(C, session);
  }
}

static wmOperatorStatus paint_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  ImagePaintStroke *stroke = MEM_new<ImagePaintStroke>(__func__, C, op, event->type);
  op->customdata = stroke;

  const wmOperatorStatus retval = op->type->modal(C, op, event);
  OPERATOR_RETVAL_CHECK(retval);

  if (retval == OPERATOR_FINISHED) {
    ImagePaintStroke *stroke = static_cast<ImagePaintStroke *>(op->customdata);
    if (stroke) {
      stroke->finish(C);
      MEM_delete(stroke);
    }

    curve_patch_session_takeover(C);
    return OPERATOR_FINISHED;
  }
  /* add modal handler */
  WM_event_add_modal_handler(C, op);

  BLI_assert(retval == OPERATOR_RUNNING_MODAL);

  return OPERATOR_RUNNING_MODAL;
}

void ImagePaintStroke::update_for_exec(bContext *C,
                                       const Brush &brush,
                                       PaintMode mode,
                                       const float mouse_init[2],
                                       float mouse[2],
                                       float pressure,
                                       float r_location[3],
                                       bool *r_location_is_set)
{
  this->update(C, brush, mode, mouse_init, mouse, pressure, r_location, r_location_is_set);
}

static wmOperatorStatus paint_exec(bContext *C, wmOperator *op)
{
  PropertyRNA *strokeprop;
  PointerRNA firstpoint;
  float mouse[2];

  strokeprop = RNA_struct_find_property(op->ptr, "stroke");

  if (!RNA_property_collection_lookup_int(op->ptr, strokeprop, 0, &firstpoint)) {
    return OPERATOR_CANCELLED;
  }

  RNA_float_get_array(&firstpoint, "mouse", mouse);

  ImagePaintStroke *stroke = MEM_new<ImagePaintStroke>(__func__, C, op, 0);
  op->customdata = stroke;

  /* Make sure we have proper coordinates for sampling (mask) textures -- these get stored in
   * #UnifiedPaintSettings -- as well as support randomness and jitter. */
  PaintMode mode = BKE_paintmode_get_active_from_context(C);
  Paint &paint = *BKE_paint_get_active_from_context(C);
  const Brush &brush = *BKE_paint_brush_for_read(&paint);
  float pressure;
  pressure = RNA_float_get(&firstpoint, "pressure");
  bool dummy;
  float dummy_location[3];

  BrushStrokeMode stroke_mode = BrushStrokeMode(RNA_enum_get(op->ptr, "mode"));
  float zoomx;
  float zoomy;
  get_imapaint_zoom(C, &zoomx, &zoomy);
  float zoom_2d = std::max(zoomx, zoomy);
  float2 mouse_out = paint_stroke_jitter_pos(
      &paint, mode, brush, pressure, stroke_mode, zoom_2d, mouse);

  stroke->update_for_exec(C, brush, mode, mouse, mouse_out, pressure, dummy_location, &dummy);
  wmOperatorStatus ret_val = stroke->exec(C, op);

  MEM_delete(stroke);

  return ret_val;
}

static wmOperatorStatus paint_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  ImagePaintStroke *stroke = static_cast<ImagePaintStroke *>(op->customdata);
  const wmOperatorStatus retval = stroke->modal(C, op, event);

  if (ELEM(retval, OPERATOR_FINISHED, OPERATOR_CANCELLED)) {
    MEM_delete(stroke);
    op->customdata = nullptr;

    /* A dragged stroke always ends here rather than in #paint_invoke, so this is the takeover
     * point that matters in practice for `BRUSH_STROKE_CURVE_PATCH`. */
    if (retval == OPERATOR_FINISHED) {
      curve_patch_session_takeover(C);
    }
  }

  return retval;
}

static void paint_cancel(bContext *C, wmOperator *op)
{
  ImagePaintStroke *stroke = static_cast<ImagePaintStroke *>(op->customdata);
  UndoStack *ustack = CTX_wm_manager(C)->runtime->undo_stack;
  if (ustack->step_init) {
    /* If the user cancels a stroke when none actually started, there is nothing to undo from. */
    ED_image_undo_restore(ustack->step_init);
  }

  stroke->cancel(C);
}
}  // namespace ed::sculpt_paint::image::ops::paint

void PAINT_OT_image_paint(wmOperatorType *ot)
{
  using namespace blender::ed::sculpt_paint::image::ops::paint;

  /* identifiers */
  ot->name = "Image Paint";
  ot->idname = "PAINT_OT_image_paint";
  ot->description = "Paint a stroke into the image";

  /* API callbacks. */
  ot->invoke = paint_invoke;
  ot->modal = paint_modal;
  ot->exec = paint_exec;
  ot->poll = ED_image_tools_paint_poll;
  ot->cancel = paint_cancel;

  /* flags */
  ot->flag = OPTYPE_BLOCKING;

  paint_stroke_operator_properties(ot);
}

}  // namespace blender
