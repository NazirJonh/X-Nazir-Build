/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edtransform
 */

#include "MEM_guardedalloc.h"

#include "BLI_math_matrix.h"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.h"
#include "BLI_vector.hh"

#include "BKE_context.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_report.hh"

#include "ED_sculpt.hh"

#include "transform.hh"
#include "transform_convert.hh"

namespace blender::ed::transform {

/* -------------------------------------------------------------------- */
/** \name Sculpt Transform Creation
 * \{ */

/**
 * Objects targeted by this Transform session, active object first. Recomputed (not cached) in
 * each of #createTransSculpt/#recalcData_sculpt/#special_aftertrans_update__sculpt -- all three
 * are pure functions of the same stable context state (the `transform_all_objects` toggle plus
 * which objects are currently in Sculpt Mode), which does not change mid-drag, so re-deriving is
 * simpler and safer than threading a container-to-object map through #TransDataContainer.
 */
static Vector<Object *> sculpt_transform_objects(bContext *C, Object &active_ob)
{
  Vector<Object *> objects = sculpt_paint::transform_target_objects(C);
  for (const int i : objects.index_range()) {
    if (objects[i] == &active_ob) {
      if (i != 0) {
        Object *tmp = objects[0];
        objects[0] = objects[i];
        objects[i] = tmp;
      }
      break;
    }
  }
  return objects;
}

static void createTransSculpt(bContext *C, TransInfo *t)
{
  Scene *scene = t->scene;
  if (!BKE_id_is_editable(t->bmain, &scene->id)) {
    BKE_report(t->reports, RPT_ERROR, "Cannot create transform on linked data");
    return;
  }

  BKE_view_layer_synced_ensure(*t->bmain, t->scene, t->view_layer);
  Object &active_ob = *BKE_view_layer_active_object_get(t->view_layer);

  const Vector<Object *> objects = sculpt_transform_objects(C, active_ob);
  BLI_assert(!objects.is_empty() && objects[0] == &active_ob);

  /* Avoid editing locked shapes. Checked for every target object up front, before opening any
   * undo step below, so a locked secondary object cancels the whole session cleanly (matching
   * the pre-existing single-object early-return) instead of leaving a partially-opened
   * multi-object undo step. */
  if (t->mode != TFM_DUMMY) {
    for (Object *ob : objects) {
      if (!sculpt_paint::shape_key_check(*ob, t->reports)) {
        return;
      }
      /* A weight-mask editing session has the user's own sculpt mask parked and the layer's
       * weights in its place, so a transform would be shaped by a mask the user cannot see and
       * did not paint — and unlike a refused brush, it has already moved the surface by the time
       * anything could notice. Checked here, before any undo step is opened, for the same reason
       * #shape_key_check is above -- and per object, so one locked/session-open secondary object
       * cancels the whole session cleanly rather than leaving a partially-opened multi-object undo
       * step. Mirrored in #special_aftertrans_update__sculpt, which must not end a transform this
       * never started. */
      if (sculpt_paint::layers::mask_edit_refuse_deform(*ob, t->reports)) {
        return;
      }
    }
  }

  if (t->data_container) {
    MEM_delete(t->data_container);
  }
  t->data_container = MEM_new_array_zeroed<TransDataContainer>(objects.size(), __func__);
  t->data_container_len = objects.size();

  SculptSession &active_ss = *active_ob.runtime->sculpt_session;
  float world_pivot_pos[3];
  copy_v3_v3(world_pivot_pos, active_ss.pivot_pos);
  mul_m4_v3(active_ob.object_to_world().ptr(), world_pivot_pos);

  float world_pivot_rot[4];
  sculpt_paint::local_pivot_rot_to_world(active_ob, active_ss.pivot_rot, world_pivot_rot);

  BLI_assert(!(t->options & CTX_PAINT_CURVE));
  for (const int i : objects.index_range()) {
    Object &ob = *objects[i];
    SculptSession &ss = *ob.runtime->sculpt_session;
    const bool is_active = (i == 0);

    /* Every object in the session shares ONE world-space pivot position + rotation -- seed it
     * here, then refresh this object's LOCAL #pivot_pos/#pivot_rot from it (a no-op round-trip
     * for the active object, since the shared value was derived from its own pivot a few lines
     * up; for secondary objects this replaces whatever stale local value their own
     * #SculptSession happened to hold). See #sync_local_pivot_from_world's doc comment for why
     * #td below operates in world space at all. */
    copy_v3_v3(ss.transform_pivot_pos_world, world_pivot_pos);
    copy_qt_qt(ss.transform_pivot_rot_world, world_pivot_rot);
    sculpt_paint::sync_local_pivot_from_world(ob);

    TransDataContainer *tc = &t->data_container[i];
    tc->data_len = 1;
    tc->is_active = is_active;
    TransData *td = tc->data = MEM_new_zeroed<TransData>(__func__);
    TransDataExtension *td_ext = tc->data_ext = MEM_new_zeroed<TransDataExtension>(__func__);

    td->flag = TD_SELECTED;

    /* #td->loc/#td->center and #td_ext->quat point at the SHARED world-space pivot fields above
     * (identity #td->mtx/#td->smtx below), NOT this object's own local space. Blender's generic
     * rotation math (#ElementRotation_ex) only produces a valid (non-sheared) rotation when its
     * conjugation matrix is a pure rotation; for an object with non-uniform #Object.scale, using
     * this object's own local-to-world matrix for that (as a prior version of this code did)
     * does not qualify -- it shears the interactively-dragged rotation instead of just turning
     * it. Working directly in world space sidesteps the conjugation: every object shares the
     * identical pivot values and the whole group moves as one rigid unit. Each object's own
     * LOCAL #pivot_pos/#pivot_rot -- consumed by #sculpt_transform_all_vertices for the actual
     * per-vertex displacement -- is re-derived from the shared world value every modal step in
     * #update_modal_transform, via #sync_local_pivot_from_world. */
    copy_v3_v3(td->center, world_pivot_pos);
    td->loc = ss.transform_pivot_pos_world;
    copy_v3_v3(td->iloc, world_pivot_pos);

    td_ext->rot = nullptr;
    td_ext->rotAxis = nullptr;
    td_ext->rotAngle = nullptr;
    td_ext->quat = ss.transform_pivot_rot_world;
    copy_m4_m4(td_ext->obmat, ob.object_to_world().ptr());
    /* #td_ext->l_smtx is left zero-initialized: it is only consumed under the pose-bone flag
     * #TD_PBONE_LOCAL_MTX_C, which this sculpt #td never sets, so it is dead on this path. */

    copy_qt_qt(td_ext->iquat, world_pivot_rot);
    td_ext->rotOrder = ROT_MODE_QUAT;

    ss.pivot_scale[0] = 1.0f;
    ss.pivot_scale[1] = 1.0f;
    ss.pivot_scale[2] = 1.0f;
    td_ext->scale = ss.pivot_scale;
    copy_v3_v3(ss.init_pivot_scale, ss.pivot_scale);
    copy_v3_v3(td_ext->iscale, ss.init_pivot_scale);

    unit_m3(td->smtx);
    unit_m3(td->mtx);
    copy_m3_m4(td->axismtx, ob.object_to_world().ptr());
    normalize_m3(td->axismtx);

    if (is_active) {
      sculpt_paint::init_transform(C, ob, t->mval, t->undo_name);
    }
    else {
      sculpt_paint::init_transform_add_object(C, ob, t->mval);
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Recalc Data object
 * \{ */

static void recalcData_sculpt(TransInfo *t)
{
  BKE_view_layer_synced_ensure(*t->bmain, t->scene, t->view_layer);
  Object &active_ob = *BKE_view_layer_active_object_get(t->view_layer);

  const Vector<Object *> objects = sculpt_transform_objects(t->context, active_ob);
  for (Object *ob : objects) {
    const bool is_active = (ob == &active_ob);
    if (t->state == TRANS_CANCEL) {
      sculpt_paint::cancel_modal_transform(t->context, *ob, is_active);
    }
    else {
      sculpt_paint::update_modal_transform(t->context, *ob, is_active);
    }
  }
}

static void special_aftertrans_update__sculpt(bContext *C, TransInfo *t)
{
  Scene *scene = t->scene;
  if (!BKE_id_is_editable(t->bmain, &scene->id)) {
    /* `sculpt_paint::init_transform` was not called in this case. */
    return;
  }

  BKE_view_layer_synced_ensure(*t->bmain, t->scene, t->view_layer);
  Object &active_ob = *BKE_view_layer_active_object_get(t->view_layer);

  const Vector<Object *> objects = sculpt_transform_objects(C, active_ob);

  /* Mirrors the refusal in #createTransSculpt, silently because that pass already reported it:
   * #sculpt_paint::init_transform was not called, so ending here would close an undo step that was
   * never opened. Kept for the same reason as the #BKE_id_is_editable mirror above, and it is
   * unreachable for the same reason too — a refusal returns before `tc->data_len` is set, and
   * #special_aftertrans_update bails on an empty `data_len_all` before dispatching here. Checked
   * for every object in the session, mirroring #createTransSculpt's per-object refusal loop, since
   * insurance that only covers the active object is not insurance for the rest of the group. Both
   * are insurance against that entry condition changing, not live paths. */
  for (Object *ob : objects) {
    if (sculpt_paint::layers::mask_edit_refuse_deform(*ob, nullptr)) {
      return;
    }
  }

  BLI_assert(!(t->options & CTX_PAINT_CURVE));

  if (objects.size() == 1) {
    sculpt_paint::end_transform(C, *objects[0]);
  }
  else {
    sculpt_paint::end_transform(C, objects.as_span());
  }
}

/** \} */

TransConvertTypeInfo TransConvertType_Sculpt = {
    /*flags*/ 0,
    /*create_trans_data*/ createTransSculpt,
    /*recalc_data*/ recalcData_sculpt,
    /*special_aftertrans_update*/ special_aftertrans_update__sculpt,
};

}  // namespace blender::ed::transform
