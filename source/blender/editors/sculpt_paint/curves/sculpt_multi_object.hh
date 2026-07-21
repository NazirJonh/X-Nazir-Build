/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup edsculpt
 * Multi-object ("global") state of a curves sculpt stroke.
 */

#include "BLI_map.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "DNA_scene_types.h"

namespace blender {

struct Curves;
struct Main;
struct Object;
struct Scene;
struct View3D;
struct ViewContext;
struct ViewLayer;

namespace ed::sculpt_paint {

/**
 * One Curves object a sculpt stroke acts on.
 *
 * \note #object and #curves_id are original (not evaluated) data; brushes edit the original
 * geometry and tag it for re-evaluation.
 */
struct CurvesSculptTarget {
  Object *object;
  Curves *curves_id;
};

/**
 * The Curves objects a sculpt stroke acts on.
 *
 * Membership is stable for the duration of a stroke -- objects cannot enter or leave sculpt mode
 * while one is modal -- so it is resolved once in #SculptCurvesBrushStroke::test_start rather than
 * re-queried from the context on every stroke step.
 */
struct CurvesMultiObjectStrokeContext {
  /**
   * Every Curves object in sculpt mode, active object first, with linked duplicates collapsed to
   * one entry so the same #Curves data is never sculpted twice in one step.
   */
  Vector<CurvesSculptTarget> mode_targets;
  /**
   * Subset of #mode_targets that deforming brushes act on, per
   * #CurvesSculpt.multi_object_edit_scope.
   */
  Vector<CurvesSculptTarget> deform_targets;
  /**
   * Objects the Add and Density brushes create new curves in, per #CurvesSculpt.add_curves_target.
   * Usually a subset of #deform_targets, except for #CURVES_SCULPT_ADD_TARGET_OBJECT, where the
   * explicitly named object is used even when the edit scope would exclude it.
   */
  Vector<CurvesSculptTarget> add_targets;

  /**
   * Fill the three target lists from the objects currently in curves sculpt mode.
   *
   * \param curves_sculpt: May be null, in which case the defaults (edit every object, add to every
   * object) are used.
   */
  void resolve(const ViewContext &vc, const CurvesSculpt *curves_sculpt);

  /**
   * True when \a object is the active object of the stroke.
   *
   * Brushes use this to decide which target may write scene-global stroke state -- the view pivot
   * in #remember_stroke_position -- since every target of a step would otherwise overwrite the
   * previous one and the last target in the list would win by accident.
   */
  bool is_active(const Object &object) const;
};

/** True when \a object is a Curves object currently in curves sculpt mode. */
bool is_curves_sculpt_target(const Object &object);

/**
 * Every Curves object currently in sculpt mode, active object first, with linked duplicates
 * collapsed to one entry.
 *
 * Empty when the active object is not itself a visible target, since the first entry is what
 * #CURVES_SCULPT_MULTI_OBJECT_EDIT_ACTIVE and the brush pivot treat as the active object.
 */
Vector<CurvesSculptTarget> curves_sculpt_mode_targets(const Main &bmain,
                                                      const Scene *scene,
                                                      ViewLayer *view_layer,
                                                      const View3D *v3d);

/**
 * The subset of \a mode_targets that deforming brushes act on. \a mode_targets must have the
 * active object first, since that is what #CURVES_SCULPT_MULTI_OBJECT_EDIT_ACTIVE selects.
 */
Vector<CurvesSculptTarget> curves_sculpt_deform_targets(
    Span<CurvesSculptTarget> mode_targets, eCurvesSculptMultiObjectEditScope edit_scope);

/**
 * The objects the Add and Density brushes create new curves in.
 *
 * \param add_object: The object named by #CURVES_SCULPT_ADD_TARGET_OBJECT. It is used even when
 * the edit scope excludes it -- naming an object is a more specific request than the scope -- but
 * it must still be a Curves object in sculpt mode.
 */
Vector<CurvesSculptTarget> curves_sculpt_add_targets(Span<CurvesSculptTarget> deform_targets,
                                                     eCurvesSculptAddTarget add_target,
                                                     Object *add_object);

/**
 * State a brush keeps for one target across the steps of a single stroke, for example a cached 3D
 * brush position or a constraint solver.
 *
 * Keyed by #Curves rather than #Object because that is what #CurvesMultiObjectStrokeContext
 * deduplicates on: two objects sharing one #Curves data-block are one target.
 */
template<typename StateT> class CurvesSculptTargetStates {
 private:
  Map<Curves *, StateT> map_;

 public:
  StateT &ensure(Curves &curves_id)
  {
    return map_.lookup_or_add_default(&curves_id);
  }

  /** The states created so far, for example to free resources they do not own. */
  typename Map<Curves *, StateT>::MutableValueIterator values()
  {
    return map_.values();
  }
};

}  // namespace ed::sculpt_paint

}  // namespace blender
