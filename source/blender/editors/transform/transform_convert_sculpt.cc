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

  BLI_assert(!(t->options & CTX_PAINT_CURVE));
  for (const int i : objects.index_range()) {
    Object &ob = *objects[i];
    SculptSession &ss = *ob.runtime->sculpt_session;
    const bool is_active = (i == 0);

    if (!is_active) {
      /* Secondary objects derive a fresh local pivot position + identity rotation from the
       * active object's shared world pivot every session (rather than reusing whatever stale
       * value their own #SculptSession happens to hold) -- interactively driving each
       * container's own quat/scale channel then composes correctly through THAT object's own
       * (possibly non-uniformly scaled) matrix, without any hand-written cross-object math. This
       * is the same mechanism vanilla multi-object Edit Mesh/Pose transforms already rely on. */
      float world_to_object[4][4];
      invert_m4_m4(world_to_object, ob.object_to_world().ptr());
      float local_pivot_pos[3];
      copy_v3_v3(local_pivot_pos, world_pivot_pos);
      mul_m4_v3(world_to_object, local_pivot_pos);
      copy_v3_v3(ss.pivot_pos, local_pivot_pos);

      unit_qt(ss.pivot_rot);
    }

    TransDataContainer *tc = &t->data_container[i];
    tc->data_len = 1;
    tc->is_active = is_active;
    TransData *td = tc->data = MEM_new_zeroed<TransData>(__func__);
    TransDataExtension *td_ext = tc->data_ext = MEM_new_zeroed<TransDataExtension>(__func__);

    td->flag = TD_SELECTED;
    copy_v3_v3(td->center, ss.pivot_pos);
    mul_m4_v3(ob.object_to_world().ptr(), td->center);

    td->loc = ss.pivot_pos;
    copy_v3_v3(td->iloc, ss.pivot_pos);

    float obmat_inv[3][3];
    copy_m3_m4(obmat_inv, ob.object_to_world().ptr());
    invert_m3(obmat_inv);

    td_ext->rot = nullptr;
    td_ext->rotAxis = nullptr;
    td_ext->rotAngle = nullptr;
    td_ext->quat = ss.pivot_rot;
    copy_m4_m4(td_ext->obmat, ob.object_to_world().ptr());
    copy_m3_m3(td_ext->l_smtx, obmat_inv);

    /* #r_mtx/r_smtx convert a world-space rotation delta into the pivot's rotation channel (see
     * their use in `transform_mode.cc`'s `fmat = r_smtx * mat * r_mtx` -> `mat3_to_quat`). They
     * must be pure orientation (no scale), matching how `td->axismtx` below is normalized:
     * composing a rotation with the object's raw (possibly non-uniform) scale here would skew
     * the extracted quaternion into a shear instead of a rotation. */
    copy_m3_m4(td_ext->r_mtx, ob.object_to_world().ptr());
    normalize_m3(td_ext->r_mtx);
    transpose_m3_m3(td_ext->r_smtx, td_ext->r_mtx);

    copy_qt_qt(td_ext->iquat, ss.pivot_rot);
    td_ext->rotOrder = ROT_MODE_QUAT;

    ss.pivot_scale[0] = 1.0f;
    ss.pivot_scale[1] = 1.0f;
    ss.pivot_scale[2] = 1.0f;
    td_ext->scale = ss.pivot_scale;
    copy_v3_v3(ss.init_pivot_scale, ss.pivot_scale);
    copy_v3_v3(td_ext->iscale, ss.init_pivot_scale);

    copy_m3_m3(td->smtx, obmat_inv);
    copy_m3_m4(td->mtx, ob.object_to_world().ptr());
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
    if (t->state == TRANS_CANCEL) {
      sculpt_paint::cancel_modal_transform(t->context, *ob);
    }
    else {
      sculpt_paint::update_modal_transform(t->context, *ob);
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
  BLI_assert(!(t->options & CTX_PAINT_CURVE));

  const Vector<Object *> objects = sculpt_transform_objects(C, active_ob);
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
