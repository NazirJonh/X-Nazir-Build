/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "sculpt_multi_object.hh"

#include "BLI_assert.h"

#include "BKE_layer.hh"

#include "DNA_curves_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "ED_view3d.hh"

namespace blender::ed::sculpt_paint {

bool is_curves_sculpt_target(const Object &object)
{
  return object.type == OB_CURVES && (object.mode & OB_MODE_SCULPT_CURVES) != 0 &&
         object.data != nullptr;
}

Vector<CurvesSculptTarget> curves_sculpt_mode_targets(const Main &bmain,
                                                      const Scene *scene,
                                                      ViewLayer *view_layer,
                                                      const View3D *v3d)
{
  /* The `unique_data` variant collapses linked duplicates, so a #Curves data-block shared by
   * several objects is sculpted once rather than once per object. */
  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_mode_unique_data(
      bmain, scene, view_layer, v3d, OB_MODE_SCULPT_CURVES);

  /* #BKE_view_layer_bases_in_mode_iterator_next restarts from the head of the base list when the
   * active base is not in the mode or is hidden, so the first object it yields is an arbitrary one
   * in that case. Everything downstream reads the first entry as the active object, so the only
   * safe answer then is no targets at all. */
  BKE_view_layer_synced_ensure(bmain, scene, view_layer);
  const Object *active_object = BKE_view_layer_active_object_get(view_layer);
  if (objects.is_empty() || objects.first() != active_object) {
    return {};
  }

  Vector<CurvesSculptTarget> targets;
  for (Object *object : objects) {
    if (!is_curves_sculpt_target(*object)) {
      continue;
    }
    targets.append({object, id_cast<Curves *>(object->data)});
  }
  return targets;
}

Vector<CurvesSculptTarget> curves_sculpt_deform_targets(
    const Span<CurvesSculptTarget> mode_targets,
    const eCurvesSculptMultiObjectEditScope edit_scope)
{
  if (edit_scope == CURVES_SCULPT_MULTI_OBJECT_EDIT_ACTIVE && !mode_targets.is_empty()) {
    return {mode_targets.first()};
  }
  return mode_targets;
}

Vector<CurvesSculptTarget> curves_sculpt_add_targets(
    const Span<CurvesSculptTarget> deform_targets,
    const eCurvesSculptAddTarget add_target,
    Object *add_object)
{
  switch (add_target) {
    case CURVES_SCULPT_ADD_TARGET_ALL:
      return deform_targets;
    case CURVES_SCULPT_ADD_TARGET_ACTIVE:
      if (deform_targets.is_empty()) {
        return {};
      }
      return {deform_targets.first()};
    case CURVES_SCULPT_ADD_TARGET_OBJECT:
      if (add_object == nullptr || !is_curves_sculpt_target(*add_object)) {
        return {};
      }
      return {CurvesSculptTarget{add_object, id_cast<Curves *>(add_object->data)}};
  }
  BLI_assert_unreachable();
  return {};
}

void CurvesMultiObjectStrokeContext::resolve(const ViewContext &vc,
                                             const CurvesSculpt *curves_sculpt)
{
  this->mode_targets = curves_sculpt_mode_targets(*vc.bmain, vc.scene, vc.view_layer, vc.v3d);

  const eCurvesSculptMultiObjectEditScope edit_scope =
      curves_sculpt ? eCurvesSculptMultiObjectEditScope(curves_sculpt->multi_object_edit_scope) :
                      CURVES_SCULPT_MULTI_OBJECT_EDIT_ALL;
  this->deform_targets = curves_sculpt_deform_targets(this->mode_targets, edit_scope);

  const eCurvesSculptAddTarget add_target = curves_sculpt ?
                                                eCurvesSculptAddTarget(
                                                    curves_sculpt->add_curves_target) :
                                                CURVES_SCULPT_ADD_TARGET_ALL;
  this->add_targets = curves_sculpt_add_targets(
      this->deform_targets, add_target, curves_sculpt ? curves_sculpt->add_curves_object : nullptr);
}

bool CurvesMultiObjectStrokeContext::is_active(const Object &object) const
{
  return !this->mode_targets.is_empty() && this->mode_targets.first().object == &object;
}

}  // namespace blender::ed::sculpt_paint
