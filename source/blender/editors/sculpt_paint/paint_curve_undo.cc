/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include <cstring>

#include "MEM_guardedalloc.h"

#include "DNA_brush_types.h"
#include "DNA_userdef_types.h"

#include "BLI_math_vector.h"

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

#ifndef NDEBUG
#  include "BLI_array_utils.h" /* #BLI_array_is_zeroed */
#endif

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Undo Conversion
 * \{ */

namespace {

struct UndoCurve {
  PaintCurvePoint *points; /* points of curve */
  int tot_points;
  int add_index;
  bke::CurvesGeometry geometry;
  char use_3d_space;
  char _pad0[7];
};

}  // namespace

/**
 * Return true when the embedded `CurvesGeometry` has been placement-new'd.
 * Zeroed DNA/stack memory leaves `runtime` as null — accessing it is undefined behaviour.
 */
template<typename T> static bool geometry_runtime_is_initialized(const T *obj)
{
  return obj->geometry.wrap().runtime != nullptr;
}

static void undocurve_from_paintcurve(UndoCurve *uc, const PaintCurve *pc)
{
  BLI_assert(BLI_array_is_zeroed(uc, 1));
  uc->points = static_cast<PaintCurvePoint *>(MEM_dupalloc(pc->points));
  uc->tot_points = pc->tot_points;
  uc->add_index = pc->add_index;
  if (geometry_runtime_is_initialized(pc)) {
    new (&uc->geometry) bke::CurvesGeometry(pc->geometry.wrap());
  }
  else {
    new (&uc->geometry) bke::CurvesGeometry();
  }
  uc->use_3d_space = pc->use_3d_space;
}

static void paintcurve_bez_copy_meta_only(BezTriple &dst, const BezTriple &src)
{
  dst.h1 = src.h1;
  dst.h2 = src.h2;
  dst.f1 = src.f1;
  dst.f2 = src.f2;
  dst.f3 = src.f3;
  dst.weight = src.weight;
  dst.radius = src.radius;
}

static void undocurve_to_paintcurve_full(const UndoCurve *uc, PaintCurve *pc)
{
  MEM_SAFE_DELETE(pc->points);
  pc->points = static_cast<PaintCurvePoint *>(MEM_dupalloc(uc->points));
  pc->tot_points = uc->tot_points;
  pc->add_index = uc->add_index;
  if (geometry_runtime_is_initialized(pc)) {
    pc->geometry.wrap() = uc->geometry;
  }
  else {
    new (&pc->geometry) bke::CurvesGeometry(uc->geometry);
  }
  pc->use_3d_space = uc->use_3d_space;
}

static void paintcurve_remove_point_preserve_positions(PaintCurve *pc, const int remove_index)
{
  const int old_tot = pc->tot_points;
  const int new_tot = old_tot - 1;

  if (remove_index < 0 || remove_index >= old_tot) {
    return;
  }

  PaintCurvePoint *points_new = nullptr;
  if (new_tot > 0) {
    points_new = MEM_new_array<PaintCurvePoint>(new_tot, "PaintCurvePoint");
    int dst = 0;
    for (int i = 0; i < old_tot; i++) {
      if (i == remove_index) {
        continue;
      }
      points_new[dst++] = pc->points[i];
    }
  }

  MEM_SAFE_DELETE(pc->points);
  pc->points = points_new;
  pc->tot_points = new_tot;

  if (pc->use_3d_space && geometry_runtime_is_initialized(pc)) {
    bke::CurvesGeometry &geom = pc->geometry.wrap();
    if (geom.points_num() == old_tot) {
      bke::CurvesGeometry new_geom;
      paintcurve_geometry_init_bezier(new_geom, new_tot);
      int dst = 0;
      for (int i = 0; i < old_tot; i++) {
        if (i == remove_index) {
          continue;
        }
        for (int h = 0; h < 3; h++) {
          copy_v3_v3(paintcurve_geom_co(new_geom, dst, h), paintcurve_geom_co(geom, i, h));
        }
        dst++;
      }
      new_geom.tag_positions_changed();
      geom = std::move(new_geom);
    }
  }
}

static void paintcurve_insert_point_preserve_positions(PaintCurve *pc,
                                                       UndoCurve *uc,
                                                       const int insert_index)
{
  const int old_tot = pc->tot_points;
  const int new_tot = old_tot + 1;

  if (insert_index < 0 || insert_index > new_tot || !uc->points) {
    return;
  }

  PaintCurvePoint *points_new = MEM_new_array<PaintCurvePoint>(new_tot, "PaintCurvePoint");
  for (int i = 0; i < insert_index; i++) {
    points_new[i] = pc->points[i];
    paintcurve_bez_copy_meta_only(points_new[i].bez, uc->points[i].bez);
  }
  points_new[insert_index] = uc->points[insert_index];
  for (int i = insert_index; i < old_tot; i++) {
    points_new[i + 1] = pc->points[i];
    paintcurve_bez_copy_meta_only(points_new[i + 1].bez, uc->points[i + 1].bez);
  }

  MEM_SAFE_DELETE(pc->points);
  pc->points = points_new;
  pc->tot_points = new_tot;

  if (pc->use_3d_space && geometry_runtime_is_initialized(pc) && uc->geometry.runtime != nullptr) {
    bke::CurvesGeometry &geom = pc->geometry.wrap();
    bke::CurvesGeometry new_geom;
    paintcurve_geometry_init_bezier(new_geom, new_tot);

    for (int i = 0; i < insert_index; i++) {
      for (int h = 0; h < 3; h++) {
        if (geom.points_num() > i) {
          copy_v3_v3(paintcurve_geom_co(new_geom, i, h), paintcurve_geom_co(geom, i, h));
        }
        else {
          copy_v3_v3(paintcurve_geom_co(new_geom, i, h), paintcurve_geom_co(uc->geometry, i, h));
        }
      }
    }
    for (int h = 0; h < 3; h++) {
      copy_v3_v3(paintcurve_geom_co(new_geom, insert_index, h),
                 paintcurve_geom_co(uc->geometry, insert_index, h));
    }
    for (int i = insert_index; i < old_tot; i++) {
      for (int h = 0; h < 3; h++) {
        if (geom.points_num() > i) {
          copy_v3_v3(paintcurve_geom_co(new_geom, i + 1, h), paintcurve_geom_co(geom, i, h));
        }
        else {
          copy_v3_v3(paintcurve_geom_co(new_geom, i + 1, h),
                     paintcurve_geom_co(uc->geometry, i + 1, h));
        }
      }
    }
    new_geom.tag_positions_changed();
    geom = std::move(new_geom);
  }
}

/**
 * Apply structural paint-curve undo data without reverting handle positions.
 * Coordinates are only restored from undo when multiple points change at once (e.g. undo delete).
 */
static void undocurve_to_paintcurve_preserve_positions(UndoCurve *uc,
                                                       PaintCurve *pc,
                                                       const eUndoStepDir dir)
{
  const int old_tot = pc->tot_points;
  const int new_tot = uc->tot_points;

  /* Multi-spline geometry cannot be partially rebuilt — always do a full restore so
   * `curves_num` and `points_by_curve` are preserved exactly as recorded. */
  const bool pc_is_multi = (geometry_runtime_is_initialized(pc) &&
                            pc->geometry.wrap().curves_num() > 1);
  const bool uc_is_multi = (geometry_runtime_is_initialized(uc) &&
                            uc->geometry.wrap().curves_num() > 1);
  if (pc_is_multi || uc_is_multi) {
    undocurve_to_paintcurve_full(uc, pc);
    return;
  }

  if (new_tot == old_tot) {
    if (new_tot > 0 && uc->points && pc->points) {
      for (int i = 0; i < new_tot; i++) {
        paintcurve_bez_copy_meta_only(pc->points[i].bez, uc->points[i].bez);
      }
    }
    pc->add_index = uc->add_index;
    pc->use_3d_space = uc->use_3d_space;
    return;
  }

  if (new_tot < old_tot && (old_tot - new_tot) == 1) {
    paintcurve_remove_point_preserve_positions(pc, uc->add_index);
    if (new_tot > 0 && uc->points && pc->points) {
      for (int i = 0; i < new_tot; i++) {
        paintcurve_bez_copy_meta_only(pc->points[i].bez, uc->points[i].bez);
      }
    }
    pc->add_index = uc->add_index;
    pc->use_3d_space = uc->use_3d_space;
    return;
  }

  if (new_tot > old_tot && (new_tot - old_tot) == 1 && dir == STEP_REDO) {
    paintcurve_insert_point_preserve_positions(pc, uc, uc->add_index);
    pc->add_index = uc->add_index;
    pc->use_3d_space = uc->use_3d_space;
    return;
  }

  undocurve_to_paintcurve_full(uc, pc);
}

static void undocurve_free_data(UndoCurve *uc)
{
  MEM_SAFE_DELETE(uc->points);
  uc->points = nullptr;
  uc->tot_points = 0;
  if (geometry_runtime_is_initialized(uc)) {
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
    bContext *C, Main * /*bmain*/, UndoStep *us_p, const eUndoStepDir dir, bool /*is_final*/)
{
  PaintCurveUndoStep *us = reinterpret_cast<PaintCurveUndoStep *>(us_p);
  PaintCurve *pc = us->pc_ref.ptr;
  if (pc == nullptr) {
    return;
  }

  const int tot_points_before = pc->tot_points;
  undocurve_to_paintcurve_preserve_positions(&us->data, pc, dir);

  if (C && pc->use_3d_space && pc->tot_points != tot_points_before) {
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
  BKE_undosys_stack_limit_steps_and_memory_defaults(ustack);
  WM_file_tag_modified();
}

/** \} */

}  // namespace blender
