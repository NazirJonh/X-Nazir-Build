/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 * \brief Painting operator to paint in 2D and 3D.
 */

#include "DNA_brush_types.h"
#include "DNA_material_types.h"
#include "DNA_screen_types.h"
#include "DNA_scene_types.h"
#include "DNA_space_types.h"

#include "BLI_math_base.hh"
#include "BLI_math_color.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_rect.h"
#include "BLI_utildefines.h"

#include <cfloat>
#include <cstdlib>
#include <memory>

#include "BKE_brush.hh"
#include "BKE_colortools.hh"
#include "BKE_context.hh"
#include "BKE_layer.hh"
#include "BKE_material.hh"
#include "BKE_paint.hh"
#include "BKE_paint_types.hh"
#include "BKE_report.hh"
#include "BKE_undo_system.hh"

#include "ED_paint.hh"
#include "ED_screen.hh"
#include "ED_undo.hh"
#include "ED_view3d.hh"

#include "GPU_immediate.hh"
#include "GPU_state.hh"

#include "MEM_guardedalloc.h"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_image.hh"

#include "UI_view2d.hh"

#include "../paint_curve_intern.hh"

#include "../paint_clone.hh"
#include "../paint_clone_2d.hh"
#include "../paint_image_curve_patch.hh"
#include "../paint_image_curve_patch_anchor.hh"
#include "../paint_image_stroke_hook.hh"
#include "../paint_intern.hh"

namespace blender {

static bool texture_fill_context_get(
    bContext *C, bool require_geometry_mode, Paint **r_paint, Brush **r_brush);
static void texture_fill_color_get(
    const Paint *paint, const Brush *brush, bool invert, float r_color[3]);

/**
 * Gesture-local click/drag bookkeeping plus the optional highlight overlay.
 *
 * Held by the box gesture's user data; #WM_generic_user_data_free runs on every exit path
 * (finish, cancel, click passthrough), so the overlay is never left behind.
 */
struct TextureFillGesture {
  /** Mouse position of the gesture start, for click-vs-drag detection. */
  int2 start_xy = {0, 0};
  bool is_drag = false;
  /** Highlight overlay state; null when there is nothing to project (no canvas, clipped view). */
  ImagePaintGeometryFillGestureState *highlight = nullptr;

  ~TextureFillGesture()
  {
    if (highlight != nullptr) {
      MEM_delete(highlight);
      highlight = nullptr;
    }
  }
};

static TextureFillGesture *texture_fill_gesture_get(const wmOperator *op)
{
  const wmGesture *gesture = static_cast<const wmGesture *>(op->customdata);
  return gesture ? static_cast<TextureFillGesture *>(gesture->user_data.data) : nullptr;
}

/**
 * Resolve the active paint context for the Texture Fill operators.
 * \a require_geometry_mode excludes Pixels mode, which is handled by the regular paint operator.
 */
static bool texture_fill_context_get(bContext *C,
                                     const bool require_geometry_mode,
                                     Paint **r_paint,
                                     Brush **r_brush)
{
  Paint *paint = BKE_paint_get_active_from_context(C);
  Brush *brush = paint ? BKE_paint_brush(paint) : nullptr;
  if (paint == nullptr || brush == nullptr) {
    return false;
  }
  if (require_geometry_mode &&
      !ELEM(brush->fill_expand,
            IMAGE_PAINT_SELECT_EXPAND_FACE,
            IMAGE_PAINT_SELECT_EXPAND_ISLAND,
            IMAGE_PAINT_SELECT_EXPAND_MESH))
  {
    return false;
  }

  if (CTX_wm_space_image(C) != nullptr) {
    if (image_select_canvas_paint_blocked(C) || !ED_image_tools_paint_poll(C) ||
        brush->image_brush_type != IMAGE_PAINT_BRUSH_TYPE_FILL)
    {
      return false;
    }
  }
  else {
    const Object *ob = CTX_data_active_object(C);
    if (ob == nullptr || !CTX_wm_region_view3d(C) ||
        !ELEM(ob->mode, OB_MODE_TEXTURE_PAINT, OB_MODE_SCULPT))
    {
      return false;
    }
    if ((ob->mode & OB_MODE_TEXTURE_PAINT) &&
        brush->image_brush_type != IMAGE_PAINT_BRUSH_TYPE_FILL)
    {
      return false;
    }
    if ((ob->mode & OB_MODE_SCULPT) &&
        brush->sculpt_brush_type != SCULPT_BRUSH_TYPE_TEXTURE_FILL)
    {
      return false;
    }
  }

  if (r_paint != nullptr) {
    *r_paint = paint;
  }
  if (r_brush != nullptr) {
    *r_brush = brush;
  }
  return true;
}

/**
 * Check whether the viewport (3D sculpt/texture paint) canvas has a texture assigned to fill
 * into. The tool itself stays visible and enabled without one; #texture_fill_click and
 * #texture_fill_exec use this to report why nothing was filled instead of failing silently.
 */
static bool texture_fill_canvas_has_texture(const bContext *C, const Object *ob)
{
  if (CTX_wm_space_image(C) != nullptr || ob == nullptr) {
    return true;
  }

  const Scene *scene = CTX_data_scene(C);
  const PaintModeSettings &paint_mode = scene->toolsettings->paint_mode;
  switch (ePaintCanvasSource(paint_mode.canvas_source)) {
    case PAINT_CANVAS_SOURCE_IMAGE:
      return paint_mode.canvas_image != nullptr;
    case PAINT_CANVAS_SOURCE_MATERIAL: {
      const Material *mat = BKE_object_material_get(const_cast<Object *>(ob), ob->actcol);
      if (mat == nullptr || mat->texpaintslot == nullptr ||
          mat->paint_active_slot >= mat->tot_slots)
      {
        return false;
      }
      return mat->texpaintslot[mat->paint_active_slot].ima != nullptr;
    }
    default:
      return false;
  }
}

static bool texture_fill_poll(bContext *C)
{
  Paint *paint;
  Brush *brush;
  return texture_fill_context_get(C, true, &paint, &brush);
}

static bool texture_fill_mode_set_poll(bContext *C)
{
  Brush *brush;
  return texture_fill_context_get(C, false, nullptr, &brush);
}

static PaintMode texture_fill_paint_mode_get(const bContext *C)
{
  if (CTX_wm_space_image(C) != nullptr) {
    return PaintMode::Texture2D;
  }
  const Object *ob = CTX_data_active_object(C);
  return (ob != nullptr && (ob->mode & OB_MODE_SCULPT)) ? PaintMode::Sculpt : PaintMode::Texture3D;
}

static void texture_fill_color_get(const Paint *paint,
                                   const Brush *brush,
                                   const bool invert,
                                   float r_color[3])
{
  if (invert) {
    copy_v3_v3(r_color, BKE_brush_secondary_color_get(paint, brush));
  }
  else {
    copy_v3_v3(r_color, BKE_brush_color_get(paint, brush));
  }
}

static bool texture_fill_click(bContext *C, wmOperator *op, const float mouse[2])
{
  Paint *paint;
  Brush *brush;
  if (!texture_fill_context_get(C, true, &paint, &brush)) {
    return false;
  }
  if (!texture_fill_canvas_has_texture(C, CTX_data_active_object(C))) {
    BKE_report(op->reports, RPT_WARNING, "Texture Fill requires a PBR Paint texture to fill into");
    return false;
  }

  const bool invert = RNA_boolean_get(op->ptr, "invert");
  float color[3];
  texture_fill_color_get(paint, brush, invert, color);

  ED_image_undo_push_begin(op->type->name, texture_fill_paint_mode_get(C));
  bool did_fill;
  if (CTX_wm_space_image(C) != nullptr) {
    paint_2d_bucket_fill(C, color, brush, mouse, mouse, nullptr);
    did_fill = true;
  }
  else {
    did_fill = paint_image_viewport_fill_at_mouse(
        C, paint, brush, CTX_data_active_object(C), invert, mouse);
  }
  ED_image_undo_push_end();

  return did_fill;
}

static wmOperatorStatus texture_fill_exec(bContext *C, wmOperator *op)
{
  Paint *paint;
  Brush *brush;
  if (!texture_fill_context_get(C, true, &paint, &brush)) {
    return OPERATOR_CANCELLED;
  }
  if (!texture_fill_canvas_has_texture(C, CTX_data_active_object(C))) {
    BKE_report(op->reports, RPT_WARNING, "Texture Fill requires a PBR Paint texture to fill into");
    return OPERATOR_CANCELLED;
  }

  rcti rect;
  WM_operator_properties_border_to_rcti(op, &rect);
  if (BLI_rcti_size_x(&rect) == 0 || BLI_rcti_size_y(&rect) == 0) {
    return OPERATOR_CANCELLED;
  }

  float color[3];
  texture_fill_color_get(paint, brush, RNA_boolean_get(op->ptr, "invert"), color);

  ED_image_undo_push_begin(op->type->name, texture_fill_paint_mode_get(C));
  bool did_fill;
  if (CTX_wm_space_image(C) != nullptr) {
    did_fill = paint_image_2d_geometry_fill_rect(C, color, brush, rect);
  }
  else {
    did_fill = paint_image_proj_geometry_fill_rect(C, color, brush, CTX_data_active_object(C), rect);
  }
  ED_image_undo_push_end();

  return did_fill ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
}

static wmOperatorStatus texture_fill_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  Paint *paint;
  Brush *brush;
  if (!texture_fill_context_get(C, true, &paint, &brush)) {
    return OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH;
  }

  RNA_boolean_set(op->ptr, "invert", event->modifier & KM_CTRL);
  const wmOperatorStatus status = WM_gesture_box_invoke(C, op, event);
  if (status & OPERATOR_RUNNING_MODAL) {
    wmGesture *gesture = static_cast<wmGesture *>(op->customdata);
    if (gesture) {
      TextureFillGesture *gesture_state = MEM_new<TextureFillGesture>(__func__);
      gesture_state->start_xy = int2(event->mval[0], event->mval[1]);
      gesture->user_data.data = gesture_state;
      gesture->user_data.free_fn = [](void *data) {
        MEM_delete(static_cast<TextureFillGesture *>(data));
      };
      gesture->user_data.use_free = true;

      float color[3];
      texture_fill_color_get(paint, brush, RNA_boolean_get(op->ptr, "invert"), color);
      paint_image_geometry_fill_gesture_begin(C, color, &gesture_state->highlight);
    }
  }
  return status;
}

static wmOperatorStatus texture_fill_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  TextureFillGesture *gesture_state = texture_fill_gesture_get(op);
  if (event->type == MOUSEMOVE && gesture_state != nullptr && !gesture_state->is_drag) {
    /* #WM_event_drag_threshold is DPI-aware, unlike a hardcoded pixel count. */
    const int drag_threshold = WM_event_drag_threshold(event);
    if (std::abs(event->mval[0] - gesture_state->start_xy.x) >= drag_threshold ||
        std::abs(event->mval[1] - gesture_state->start_xy.y) >= drag_threshold)
    {
      gesture_state->is_drag = true;
    }
  }

  if (event->type == EVT_MODAL_MAP &&
      ELEM(event->val, GESTURE_MODAL_SELECT, GESTURE_MODAL_DESELECT, GESTURE_MODAL_IN,
           GESTURE_MODAL_OUT) &&
      (gesture_state == nullptr || !gesture_state->is_drag))
  {
    const float2 mouse = gesture_state ? float2(gesture_state->start_xy) : float2(0.0f);
    /* The overlay state dies with the gesture, through its own destructor. */
    WM_gesture_box_cancel(C, op);
    return texture_fill_click(C, op, mouse) ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
  }

  const wmOperatorStatus status = WM_gesture_box_modal(C, op, event);
  if ((status & OPERATOR_RUNNING_MODAL) && event->type == MOUSEMOVE &&
      gesture_state != nullptr && gesture_state->is_drag)
  {
    if (gesture_state->highlight != nullptr) {
      Paint *paint;
      Brush *brush;
      if (texture_fill_context_get(C, true, &paint, &brush)) {
        /* #WM_gesture_box_modal has already written the sorted rect into the operator
         * properties for this move. The gesture's own rect must not be used: it stores the
         * drag start and end verbatim, so dragging left or up leaves `xmin > xmax` and every
         * intersection test fails -- the highlight would vanish for half the drag directions
         * while the fill, which reads the same sorted properties, still covers those faces. */
        rcti rect;
        WM_operator_properties_border_to_rcti(op, &rect);
        if (BLI_rcti_size_x(&rect) != 0 && BLI_rcti_size_y(&rect) != 0) {
          paint_image_geometry_fill_gesture_resolve(C, brush, gesture_state->highlight, rect);
        }
      }
    }
  }
  return status;
}

static void texture_fill_cancel(bContext *C, wmOperator *op)
{
  /* The gesture frees the gesture state through its own destructor. */
  WM_gesture_box_cancel(C, op);
}

void PAINT_OT_texture_fill(wmOperatorType *ot)
{
  ot->name = "Texture Fill";
  ot->idname = "PAINT_OT_texture_fill";
  ot->description = "Fill faces with a click or rectangular gesture";

  ot->invoke = texture_fill_invoke;
  ot->modal = texture_fill_modal;
  ot->exec = texture_fill_exec;
  ot->cancel = texture_fill_cancel;
  ot->poll = texture_fill_poll;

  ot->flag = OPTYPE_REGISTER;

  WM_operator_properties_border(ot);
  PropertyRNA *prop = RNA_def_boolean(
      ot->srna, "invert", false, "Invert", "Use the secondary brush color");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
}

static wmOperatorStatus texture_fill_mode_set_exec(bContext *C, wmOperator *op)
{
  Brush *brush;
  if (!texture_fill_context_get(C, false, nullptr, &brush)) {
    /* Defensive: #texture_fill_mode_set_poll should have caught this. */
    return OPERATOR_CANCELLED;
  }

  brush->fill_expand = RNA_enum_get(op->ptr, "mode");
  BKE_brush_tag_unsaved_changes(brush);
  WM_main_add_notifier(NC_BRUSH | NA_EDITED, brush);
  return OPERATOR_FINISHED;
}

void PAINT_OT_texture_fill_mode_set(wmOperatorType *ot)
{
  static const EnumPropertyItem mode_items[] = {
      {IMAGE_PAINT_SELECT_EXPAND_FACE, "FACE", 0, "Face", "Fill the clicked faces"},
      {IMAGE_PAINT_SELECT_EXPAND_ISLAND,
       "ISLAND",
       0,
       "Island",
       "Fill connected UV islands"},
      {IMAGE_PAINT_SELECT_EXPAND_MESH, "MESH", 0, "Mesh", "Fill all visible faces"},
      {IMAGE_PAINT_SELECT_EXPAND_PIXELS,
       "PIXELS",
       0,
       "Pixels",
       "Flood fill connected pixels"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  ot->name = "Set Texture Fill Mode";
  ot->idname = "PAINT_OT_texture_fill_mode_set";
  ot->description = "Set the expansion mode for the active Texture Fill brush";

  ot->exec = texture_fill_mode_set_exec;
  ot->poll = texture_fill_mode_set_poll;
  /* Brush data changes must be undoable like other brush edits. */
  ot->flag = OPTYPE_UNDO;

  ot->prop = RNA_def_enum(
      ot->srna, "mode", mode_items, IMAGE_PAINT_SELECT_EXPAND_FACE, "Mode", "Fill expansion mode");
  RNA_def_property_flag(ot->prop, PROP_HIDDEN | PROP_SKIP_SAVE);
}

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
  UndoStack *ustack = ED_undo_stack_get();
  if (ustack == nullptr || ustack->step_init == nullptr ||
      ustack->step_init->type != BKE_UNDOSYS_TYPE_IMAGE)
  {
    /* The flood piggy-backs on the caller's open image undo step; reaching this means the
     * caller forgot to open one and the fill would silently do nothing. */
    BLI_assert_msg(0, "Pixel flood fill expects an open image undo step");
    return false;
  }

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
  /* See the matching guard in `sculpt_brush_stroke_invoke()`: one live Curve Patch at a time,
   * refused before the stroke rather than after it. This is the path the exclusivity actually
   * matters on -- the 2D modal only registers in its own area, so without this a live Image
   * Editor patch left 3D painting wide open, and vice versa. */
  if (const char *blocked = curve_patch_active_session_message(*C)) {
    BKE_report(op->reports, RPT_WARNING, blocked);
    return OPERATOR_CANCELLED;
  }

  /* Shift+LMB with the Clone brush sets the PBR clone source instead of starting a stroke
   * (same invoke-time redirect as the sculpt-path guard in #sculpt_brush_stroke_invoke).
   * In the Image Editor only the Clone Stamp tool picks a source: the legacy Clone tool shares
   * the brush and keeps its stroke (and Shift+Click must not silently switch it to stamping). */
  const Paint *paint = BKE_paint_get_active_from_context(C);
  const Brush *brush = (paint != nullptr) ? BKE_paint_brush_for_read(paint) : nullptr;
  if (brush != nullptr && brush->image_brush_type == IMAGE_PAINT_BRUSH_TYPE_CLONE &&
      (event->modifier & KM_SHIFT) != 0 &&
      (CTX_wm_region_view3d(C) != nullptr || ed::sculpt_paint::clone::clone_2d_tool_active(C)))
  {
    WM_operator_name_call(
        C, "PAINT_OT_clone_source_set", wm::OpCallContext::InvokeDefault, nullptr, event);
    return OPERATOR_FINISHED;
  }

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
