/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * The paint curve as something a USER drives: the `PAINTCURVE_OT_*` registrations, the slide
 * modal's keymap and its cross-operator state, the sync hooks that keep a curve and its source
 * object in step, and conversion to and from curve objects.
 *
 * Implemented by `paint_curve.cc`, `paint_curve_slide.cc`, `paint_curve_topology.cc` and
 * `paint_curve_convert.cc`. The data itself lives in `paint_curve_geometry.hh`, screen-space work
 * in `paint_curve_screen.hh`.
 */

#pragma once

#include <cstdint>

#include "BLI_function_ref.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "ED_paint_curve_draw.hh"

#include "DNA_brush_types.h"

namespace blender {

namespace bke {
class CurvesGeometry;
}

namespace ed::curves {
/* Forward-declared rather than pulling in `ED_curves.hh`; the underlying type must match the
 * definition there. */
enum class SetHandleType : uint8_t;
}  // namespace ed::curves

struct Brush;
struct Curve;
struct Main;
struct Paint;
struct ViewContext;
struct bContext;
struct PaintCurveSnapContext;
struct wmKeyConfig;
struct wmKeyMap;
struct wmOperator;
struct wmOperatorType;

/* -------------------------------------------------------------------- */
/** \name Operators
 * \{ */

void PAINTCURVE_OT_new(wmOperatorType *ot);
void PAINTCURVE_OT_add_point(wmOperatorType *ot);
void PAINTCURVE_OT_insert_or_add_point(wmOperatorType *ot);
void PAINTCURVE_OT_new_spline(wmOperatorType *ot);
void PAINTCURVE_OT_delete_point(wmOperatorType *ot);
void PAINTCURVE_OT_clear(wmOperatorType *ot);
void PAINTCURVE_OT_duplicate(wmOperatorType *ot);
void PAINTCURVE_OT_select(wmOperatorType *ot);
void PAINTCURVE_OT_slide(wmOperatorType *ot);
void PAINTCURVE_OT_slide_radius(wmOperatorType *ot);
void PAINTCURVE_OT_draw(wmOperatorType *ot);
void PAINTCURVE_OT_cursor(wmOperatorType *ot);
void PAINTCURVE_OT_from_curve_object(wmOperatorType *ot);
void PAINTCURVE_OT_to_curve_object(wmOperatorType *ot);
void PAINTCURVE_OT_separate_to_curve_object(wmOperatorType *ot);
void PAINTCURVE_OT_sculpt_pick(wmOperatorType *ot);
void PAINTCURVE_OT_handle_type_set(wmOperatorType *ot);
void PAINTCURVE_OT_split(wmOperatorType *ot);
void PAINTCURVE_OT_make_segment(wmOperatorType *ot);
void PAINTCURVE_OT_select_linked(wmOperatorType *ot);
void PAINTCURVE_OT_toggle_cyclic(wmOperatorType *ot);
void PAINTCURVE_OT_context_menu(wmOperatorType *ot);
wmKeyMap *paintcurve_slide_modal_keymap(wmKeyConfig *keyconf);
bool paintcurve_slide_is_active();
bool paintcurve_slide_segment_active(int *r_point_a, int *r_point_b);

/**
 * Tell the next #PAINTCURVE_OT_slide invoke to no-op: a handle-type cycle or a segment insert
 * already consumed this click. Defined in `paint_curve_slide.cc`.
 */
void paintcurve_skip_next_slide();
void paintcurve_sync_to_source_if_3d(bContext *C, PaintCurve *pc);
void paintcurve_sync_after_handle_type_change(bContext *C, PaintCurve *pc);
bool paintcurve_update_add_index_from_selection(PaintCurve *pc, const bke::CurvesGeometry &geom);
/**
 * Paint Curve ID insert under the cursor. Hit-test is
 * #paintcurve_find_insert_segment_from_geometry; mutation and paint-curve undo stay here.
 * Curve Patch does not call this.
 */
bool paintcurve_try_insert_point_at_mouse(bContext *C,
                                          wmOperator *op,
                                          PaintCurve *pc,
                                          const float loc_fl[2]);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Object Conversion
 * \{ */

/** Create a new paint-curve data-block owned alongside `brush` and return it. */
PaintCurve *paintcurve_for_brush_add(Main *bmain, const char *name, const Brush *brush);
/**
 * Active sculpt brush paint curve, creating the data-block when missing.
 * Returns null when sculpt paint or brush is unavailable.
 */
PaintCurve *paintcurve_active_from_context(bContext *C, Brush **r_brush = nullptr);
void paintcurve_geometry_from_curves(PaintCurve *pc,
                                     const bke::CurvesGeometry &src,
                                     const float4x4 &transform);
void paintcurve_geometry_from_legacy_curve(PaintCurve *pc,
                                           const Curve *curve,
                                           const float4x4 &transform);
bool paintcurve_sync_to_source_object(bContext *C, PaintCurve *pc);
/**
 * Reproject paint-curve geometry when #PaintCurve::use_3d_space is toggled.
 * \a to_3d_space must match the new flag value: screen→object when true, object→screen when false.
 */
bool paintcurve_convert_geometry_space(bContext *C,
                                       PaintCurve *pc,
                                       const ViewContext *vc,
                                       const bool to_3d_space);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Constants
 * \{ */

/** `bToolRef::idname` of the standalone Curve Edit tool, which edits the brush's paint curve
 * whatever the brush's stroke method is. Spelled out here alone: three modules compare against it,
 * and a rename on the Python side must break one build, not three behaviors silently. */
constexpr const char *PAINT_CURVE_EDIT_TOOL_IDNAME = "builtin.curves_edit";

/** \} */

}  // namespace blender
