/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edtransform
 *
 * Transform integration for the Sculpt Lattice tool's cage.
 *
 * Modeled on #TransConvertType_Sculpt: a single #TransData standing for one loc/quat/scale triple
 * that belongs to no scene object. The cage is a parentless no-main object (ADR-15), so its
 * loc/rot/scale *are* its world transform and every conversion matrix below is the identity.
 */

#include "MEM_guardedalloc.h"

#include "BLI_math_matrix.h"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.h"

#include "BKE_context.hh"
#include "BKE_layer.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"

#include "DNA_object_types.h"

#include "ED_screen.hh"
#include "ED_sculpt_lattice.hh"

#include "transform.hh"
#include "transform_convert.hh"

namespace blender::ed::transform {

/* -------------------------------------------------------------------- */
/** \name Cage Lookup
 * \{ */

static Object *sculpt_lattice_cage_get(TransInfo *t, Object **r_ob_mesh)
{
  BKE_view_layer_synced_ensure(*t->bmain, t->scene, t->view_layer);
  Object *ob_mesh = BKE_view_layer_active_object_get(t->view_layer);
  if (ob_mesh == nullptr) {
    return nullptr;
  }
  if (r_ob_mesh != nullptr) {
    *r_ob_mesh = ob_mesh;
  }
  return sculpt_paint::lattice::cage_object(*ob_mesh);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Sculpt Lattice Transform Creation
 * \{ */

static void createTransSculptLattice(bContext * /*C*/, TransInfo *t)
{
  Object *ob_mesh = nullptr;
  Object *lat_ob = sculpt_lattice_cage_get(t, &ob_mesh);
  if (lat_ob == nullptr) {
    return;
  }

  TransData *td;
  TransDataExtension *td_ext;
  {
    BLI_assert(t->data_container_len == 1);
    TransDataContainer *tc = t->data_container;
    tc->data_len = 1;
    tc->is_active = true;
    td = tc->data = MEM_new_zeroed<TransData>(__func__);
    td_ext = tc->data_ext = MEM_new_zeroed<TransDataExtension>(__func__);
  }

  td->flag = TD_SELECTED;

  td->loc = lat_ob->loc;
  copy_v3_v3(td->iloc, lat_ob->loc);
  copy_v3_v3(td->center, lat_ob->loc);

  td_ext->rot = nullptr;
  td_ext->rotAxis = nullptr;
  td_ext->rotAngle = nullptr;
  td_ext->quat = lat_ob->quat;
  copy_qt_qt(td_ext->iquat, lat_ob->quat);
  td_ext->rotOrder = ROT_MODE_QUAT;

  td_ext->scale = lat_ob->scale;
  copy_v3_v3(td_ext->iscale, lat_ob->scale);

  /* The cage has no parent and lives outside any scene, so local space is world space. */
  unit_m4(td_ext->obmat);
  unit_m3(td_ext->l_smtx);
  unit_m3(td_ext->r_mtx);
  unit_m3(td_ext->r_smtx);
  unit_m3(td->mtx);
  unit_m3(td->smtx);

  /* Local-axis constraints (pressing X twice and friends) follow the cage's own orientation. */
  copy_m3_m4(td->axismtx, lat_ob->object_to_world().ptr());
  normalize_m3(td->axismtx);

  /* Open a sculpt-typed undo step. Without it the OPTYPE_UNDO push at the end of the modal falls
   * through to the memfile undo type, because the sculpt undo type is registered with no context
   * poll and can never be selected automatically. A memfile step would reload Main, free the
   * sculpt session and silently destroy this tool session. */
  sculpt_paint::lattice::undo_push_begin(*t->scene, *ob_mesh, t->undo_name);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Recalc Data
 * \{ */

static void recalcData_sculpt_lattice(TransInfo *t)
{
  Object *lat_ob = sculpt_lattice_cage_get(t, nullptr);
  if (lat_ob == nullptr) {
    return;
  }

  /* The cage is in no depsgraph, so nothing else refreshes its runtime matrix from the loc/rot/
   * scale the transform system just wrote. Every reader (overlay, pick, deform) goes through
   * #Object::object_to_world(). On cancel the transform system has already restored the source
   * values, so the same refresh is all that is needed there too. */
  BKE_object_to_mat4(lat_ob, lat_ob->runtime->object_to_world.ptr());

  /* The cage is neutral throughout the placement phase, which makes the lattice deformation the
   * identity. Moving it therefore cannot change a single vertex: no deform, no PBVH work. */
  ED_region_tag_redraw(t->region);
}

static void special_aftertrans_update__sculpt_lattice(bContext * /*C*/, TransInfo *t)
{
  Object *ob_mesh = nullptr;
  /* Deliberately ignores whether the cage still exists. Reaching this callback at all means
   * #createTransSculptLattice found one and opened an undo step, and the session can die in
   * between — a tool switch frees it, and so does anything that tears down the sculpt session.
   * Leaving that step open would strand the undo stack, so only the mesh object is required here.
   * The push begin/end pair must stay balanced on every path out of the transform. */
  sculpt_lattice_cage_get(t, &ob_mesh);
  if (ob_mesh == nullptr) {
    return;
  }
  sculpt_paint::lattice::undo_push_end(*ob_mesh);
  ED_region_tag_redraw(t->region);
}

/** \} */

TransConvertTypeInfo TransConvertType_SculptLattice = {
    /*flags*/ 0,
    /*create_trans_data*/ createTransSculptLattice,
    /*recalc_data*/ recalcData_sculpt_lattice,
    /*special_aftertrans_update*/ special_aftertrans_update__sculpt_lattice,
};

}  // namespace blender::ed::transform
