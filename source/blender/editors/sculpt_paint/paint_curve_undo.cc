/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "MEM_guardedalloc.h"

#include "DNA_brush_types.h"
#include "DNA_userdef_types.h"

#include "BKE_context.hh"
#include "BKE_curves.hh"
#include "BKE_paint.hh"
#include "BKE_paint_types.hh"
#include "BKE_undo_system.hh"

#include "ED_paint.hh"
#include "ED_undo.hh"

#include "WM_api.hh"

#include "paint_curve_intern.hh"
#include "paint_intern.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Undo Conversion
 * \{ */

namespace {

struct UndoCurve {
  bke::CurvesGeometry geometry;
  int add_index;
  int active_curve;
  char use_3d_space;
  char _pad0[3];
};

}  // namespace

static void undocurve_from_paintcurve(UndoCurve *uc, const PaintCurve *pc)
{
  uc->add_index = pc->add_index;
  uc->active_curve = pc->active_curve;
  uc->use_3d_space = pc->use_3d_space;
  if (paintcurve_geometry_runtime_is_initialized(pc->geometry.wrap())) {
    new (&uc->geometry) bke::CurvesGeometry(pc->geometry.wrap());
  }
  else {
    new (&uc->geometry) bke::CurvesGeometry();
  }
}

static void undocurve_to_paintcurve(const UndoCurve *uc, PaintCurve *pc)
{
  pc->add_index = uc->add_index;
  pc->active_curve = uc->active_curve;
  pc->use_3d_space = uc->use_3d_space;
  if (paintcurve_geometry_runtime_is_initialized(uc->geometry.wrap())) {
    if (paintcurve_geometry_runtime_is_initialized(pc->geometry.wrap())) {
      pc->geometry.wrap() = uc->geometry;
    }
    else {
      new (&pc->geometry) bke::CurvesGeometry(uc->geometry);
    }
  }
  else if (paintcurve_geometry_runtime_is_initialized(pc->geometry.wrap())) {
    pc->geometry.wrap().~CurvesGeometry();
    new (&pc->geometry) bke::CurvesGeometry();
  }
}

static void undocurve_free_data(UndoCurve *uc)
{
  if (paintcurve_geometry_runtime_is_initialized(uc->geometry.wrap())) {
    uc->geometry.wrap().~CurvesGeometry();
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Implements ED Undo System
 * \{ */

struct PaintCurveUndoStep {
  UndoStep step;

  UndoRefID_PaintCurve pc_ref;

  UndoCurve data;
};

static bool paintcurve_undosys_poll(bContext *C)
{
  if (C == nullptr || !paint_curve_poll(C)) {
    return false;
  }
  /* Sculpt uses BKE_UNDOSYS_TYPE_SCULPT for strokes; avoid hijacking generic
   * #OPTYPE_UNDO pushes (and the "Original Mode" init step) via paint-curve poll. */
  if (BKE_paintmode_get_active_from_context(C) == PaintMode::Sculpt) {
    return false;
  }
  Paint *paint = BKE_paint_get_active_from_context(C);
  Brush *brush = BKE_paint_brush(paint);
  return (brush && brush->paint_curve);
}

static void paintcurve_undosys_step_encode_init(bContext *C, UndoStep *us_p)
{
  /* XXX, use to set the undo type only. */
  UNUSED_VARS(C, us_p);
}

static bool paintcurve_undosys_step_encode(bContext *C, Main * /*bmain*/, UndoStep *us_p)
{
  if (C == nullptr || !paint_curve_poll(C)) {
    return false;
  }

  Paint *paint = BKE_paint_get_active_from_context(C);
  Brush *brush = BKE_paint_brush(paint);
  PaintCurve *pc = paint ? (brush ? brush->paint_curve : nullptr) : nullptr;
  if (pc == nullptr) {
    return false;
  }

  PaintCurveUndoStep *us = reinterpret_cast<PaintCurveUndoStep *>(us_p);
  BLI_assert(us->step.data_size == 0);

  us->pc_ref.ptr = pc;
  undocurve_from_paintcurve(&us->data, pc);

  return true;
}

static void paintcurve_undosys_step_decode(
    bContext *C, Main * /*bmain*/, UndoStep *us_p, const eUndoStepDir /*dir*/, bool /*is_final*/)
{
  PaintCurveUndoStep *us = reinterpret_cast<PaintCurveUndoStep *>(us_p);
  PaintCurve *pc = us->pc_ref.ptr;
  if (pc == nullptr) {
    return;
  }

  undocurve_to_paintcurve(&us->data, pc);

  if (C && pc->use_3d_space) {
    paintcurve_sync_to_source_object(C, pc);
  }
}

static void paintcurve_undosys_step_free(UndoStep *us_p)
{
  PaintCurveUndoStep *us = reinterpret_cast<PaintCurveUndoStep *>(us_p);
  undocurve_free_data(&us->data);
}

static void paintcurve_undosys_foreach_ID_ref(UndoStep *us_p,
                                              UndoTypeForEachIDRefFn foreach_ID_ref_fn,
                                              void *user_data)
{
  PaintCurveUndoStep *us = reinterpret_cast<PaintCurveUndoStep *>(us_p);
  foreach_ID_ref_fn(user_data, (reinterpret_cast<UndoRefID *>(&us->pc_ref)));
}

void ED_paintcurve_undosys_type(UndoType *ut)
{
  ut->name = "Paint Curve";
  ut->poll = paintcurve_undosys_poll;
  ut->step_encode_init = paintcurve_undosys_step_encode_init;
  ut->step_encode = paintcurve_undosys_step_encode;
  ut->step_decode = paintcurve_undosys_step_decode;
  ut->step_free = paintcurve_undosys_step_free;

  ut->step_foreach_ID_ref = paintcurve_undosys_foreach_ID_ref;

  ut->flags = 0;

  ut->step_size = sizeof(PaintCurveUndoStep);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Utilities
 * \{ */

void ED_paintcurve_undo_push_begin(bContext *C, const char *name)
{
  UndoStack *ustack = ED_undo_stack_get();
  BKE_undosys_step_push_init_with_type(ustack, C, name, BKE_UNDOSYS_TYPE_PAINTCURVE);
}

void ED_paintcurve_undo_push_end(bContext *C)
{
  UndoStack *ustack = ED_undo_stack_get();
  BKE_undosys_step_push(ustack, C, nullptr);
  WM_file_tag_modified();
}

/** \} */

}  // namespace blender
