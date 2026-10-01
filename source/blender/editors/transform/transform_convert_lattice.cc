/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edtransform
 */

#include "DNA_curve_types.h"
#include "DNA_lattice_types.h"

#include "MEM_guardedalloc.h"

#include "BLI_math_matrix.h"
#include "BLI_math_vector.h"

#include "BKE_lattice.hh"

#include "ED_object.hh"

#include "transform.hh"
#include "transform_snap.hh"

/* Own include. */
#include "transform_convert.hh"

namespace blender::ed::transform {

/* -------------------------------------------------------------------- */
/** \name Lattice Transform Creation
 * \{ */

/* Used for both the mirror epsilon and #TD_MIRROR_EDGE_ (matches the edit-mesh transform). */
#define TRANSFORM_LATTICE_MAXDIST_MIRROR 0.00002f

/**
 * Mirror-pair mapping for the lattice grid, built the same way as the edit-mesh mirror data
 * (see #transform_convert_mesh_mirrordata_calc) but using the grid topology instead of a
 * position search: the mirror of a point across an axis is its grid counterpart, accepted only
 * when the two positions are (near) exact mirrors.
 */
struct LatticeMirrorData {
  /** Per BPoint index: the index of the point it mirrors from, -1 when not a mirror. */
  int *vert_map = nullptr;
  /** Per BPoint index: #TD_MIRROR_X/Y/Z bits, the axes to flip the source location along. */
  int *vert_flags = nullptr;
  int mirror_elem_len = 0;
};

static bool is_in_quadrant_v3(const float co[3], const int quadrant[3], const float epsilon)
{
  if (quadrant[0] && ((co[0] * quadrant[0]) < -epsilon)) {
    return false;
  }
  if (quadrant[1] && ((co[1] * quadrant[1]) < -epsilon)) {
    return false;
  }
  if (quadrant[2] && ((co[2] * quadrant[2]) < -epsilon)) {
    return false;
  }
  return true;
}

/** Mirror index of \a i across \a axis, or -1 when the point is its own mirror. */
static int lattice_mirror_index(const Lattice *latt, const int i, const int axis)
{
  const int flipped = BKE_lattice_index_flip(latt, i, axis == 0, axis == 1, axis == 2);
  return (flipped == i) ? -1 : flipped;
}

static void lattice_mirrordata_calc(const Lattice *latt,
                                    const bool use_select,
                                    const bool mirror_axis[3],
                                    LatticeMirrorData &r_mirror_data)
{
  const int tot = latt->pntsu * latt->pntsv * latt->pntsw;
  const BPoint *bp = latt->def;

  int *vert_map = MEM_new_array_zeroed<int>(tot, __func__);
  int *vert_flags = MEM_new_array_zeroed<int>(tot, __func__);

  float select_sum[3] = {0};
  for (int i = 0; i < tot; i++) {
    vert_map[i] = -1;
    if (bp[i].hide == 0 && (bp[i].f1 & SELECT)) {
      add_v3_v3(select_sum, bp[i].vec);
    }
  }

  /* Tag only elements that will be transformed within the quadrant. */
  int quadrant[3];
  for (int a = 0; a < 3; a++) {
    quadrant[a] = mirror_axis[a] ? (select_sum[a] >= 0.0f ? 1 : -1) : 0;
  }

  const bool is_single_mirror_axis = (mirror_axis[0] + mirror_axis[1] + mirror_axis[2]) == 1;

  uint mirror_elem_len = 0;
  int *index[3] = {nullptr, nullptr, nullptr};

  for (int a = 0; a < 3; a++) {
    if (!mirror_axis[a]) {
      continue;
    }

    index[a] = MEM_new_array_uninitialized<int>(tot, __func__);
    for (int i = 0; i < tot; i++) {
      index[a][i] = lattice_mirror_index(latt, i, a);
    }

    const int flag = TD_MIRROR_X << a;
    constexpr float maxdist_sq = TRANSFORM_LATTICE_MAXDIST_MIRROR * TRANSFORM_LATTICE_MAXDIST_MIRROR;
    for (int i = 0; i < tot; i++) {
      const int i_mirr = index[a][i];
      if (i_mirr < 0) {
        continue;
      }
      if (bp[i].hide || bp[i_mirr].hide) {
        continue;
      }
      if (use_select && (bp[i].f1 & SELECT) == 0) {
        continue;
      }
      if (!is_in_quadrant_v3(bp[i].vec, quadrant, TRANSFORM_LATTICE_MAXDIST_MIRROR)) {
        continue;
      }
      if (vert_map[i_mirr] != -1) {
        /* One mirror per element: it can happen when points occupy the same position. */
        continue;
      }
      if (vert_flags[i] & flag) {
        /* It's already a mirror, avoid a mirror point dependency cycle. */
        continue;
      }
      /* Accept the grid pair only when the two points are actual mirrors of each other. */
      float co_test[3];
      copy_v3_v3(co_test, bp[i].vec);
      co_test[a] = -co_test[a];
      if (len_squared_v3v3(co_test, bp[i_mirr].vec) > maxdist_sq) {
        continue;
      }

      vert_map[i_mirr] = i;
      vert_flags[i_mirr] = flag;
      mirror_elem_len++;
    }
  }

  if (mirror_elem_len && !is_single_mirror_axis) {
    /* Adjustment for elements that are mirrors of mirrored elements. */
    for (int a = 0; a < 3; a++) {
      if (!mirror_axis[a]) {
        continue;
      }
      const int flag = TD_MIRROR_X << a;
      for (int i = 0; i < tot; i++) {
        const int i_mirr = index[a][i];
        if (i_mirr < 0) {
          continue;
        }
        /* The grid mirror of a visible point can be hidden itself (unlike the mesh mirror cache,
         * which never contains hidden elements): such an entry would never be filled in the
         * #TransDataMirror loop below, so it must not be counted here either. */
        if (bp[i_mirr].hide) {
          continue;
        }
        if (vert_map[i] != -1 && (vert_flags[i] & flag) == 0) {
          if (vert_map[i_mirr] == -1) {
            mirror_elem_len++;
          }
          vert_map[i_mirr] = vert_map[i];
          vert_flags[i_mirr] |= vert_flags[i] | flag;
        }
      }
    }
  }

  for (int a = 0; a < 3; a++) {
    MEM_SAFE_DELETE(index[a]);
  }

  if (!mirror_elem_len) {
    MEM_delete(vert_map);
    MEM_delete(vert_flags);
    vert_map = nullptr;
    vert_flags = nullptr;
  }

  r_mirror_data.vert_map = vert_map;
  r_mirror_data.vert_flags = vert_flags;
  r_mirror_data.mirror_elem_len = mirror_elem_len;
}

static void lattice_mirrordata_free(LatticeMirrorData &mirror_data)
{
  MEM_SAFE_DELETE(mirror_data.vert_map);
  MEM_SAFE_DELETE(mirror_data.vert_flags);
}

static void createTransLatticeVerts(bContext * /*C*/, TransInfo *t)
{
  FOREACH_TRANS_DATA_CONTAINER (t, tc) {

    Lattice *latt = (id_cast<Lattice *>(tc->obedit->data))->editlatt->latt;
    TransData *td = nullptr;
    TransDataMirror *td_mirror = nullptr;
    BPoint *bp;
    float mtx[3][3], smtx[3][3];
    int count = 0, countsel = 0;
    const bool is_prop_edit = (t->flag & T_PROP_EDIT) != 0;
    const bool is_prop_connected = (t->flag & T_PROP_CONNECTED) != 0;
    const bool use_select = !is_prop_edit;

    /* Avoid editing locked shapes. */
    if (t->mode != TFM_DUMMY && object::shape_key_report_if_locked(tc->obedit, t->reports)) {
      continue;
    }

    const int tot = latt->pntsu * latt->pntsv * latt->pntsw;
    for (int a = 0; a < tot; a++) {
      if (latt->def[a].hide == 0) {
        if (latt->def[a].f1 & SELECT) {
          countsel++;
        }
        if (is_prop_edit) {
          count++;
        }
      }
    }

    /* Support other objects using proportional editing to adjust these, unless connected is
     * enabled. */
    if (((is_prop_edit && !is_prop_connected) ? count : countsel) == 0) {
      tc->data_len = 0;
      continue;
    }

    /* Create mirror-point mapping. */
    LatticeMirrorData mirror_data;
    if (tc->use_mirror_axis_any) {
      const bool mirror_axis[3] = {
          bool(tc->use_mirror_axis_x), bool(tc->use_mirror_axis_y), bool(tc->use_mirror_axis_z)};
      lattice_mirrordata_calc(latt, use_select, mirror_axis, mirror_data);
    }

    if (is_prop_edit) {
      tc->data_len = count;
    }
    else {
      tc->data_len = countsel;
    }

    /* Create TransDataMirror. Points that mirror a transformed point follow it instead of being
     * transformed directly, whether they are selected or not. */
    if (mirror_data.vert_map) {
      tc->data_mirror_len = mirror_data.mirror_elem_len;
      tc->data_mirror = MEM_new_array_zeroed<TransDataMirror>(mirror_data.mirror_elem_len,
                                                              "TransObDataMirror(Lattice)");

      for (int a = 0; a < tot; a++) {
        if (mirror_data.vert_map[a] != -1) {
          if (is_prop_edit || (latt->def[a].f1 & SELECT)) {
            /* Mirror points are excluded from #tc->data, shrink it accordingly. */
            tc->data_len--;
          }
        }
      }
    }

    BLI_assert(tc->data_len >= 0);
    if (tc->data_len == 0 && tc->data_mirror_len == 0) {
      lattice_mirrordata_free(mirror_data);
      continue;
    }

    tc->data = MEM_new_array_zeroed<TransData>(tc->data_len, "TransObData(Lattice EditMode)");

    copy_m3_m4(mtx, tc->obedit->object_to_world().ptr());
    pseudoinverse_m3_m3(smtx, mtx, PSEUDOINVERSE_EPSILON);

    td = tc->data;
    td_mirror = tc->data_mirror;
    for (int a = 0; a < tot; a++) {
      bp = &latt->def[a];
      if (bp->hide != 0) {
        continue;
      }

      if (mirror_data.vert_map && mirror_data.vert_map[a] != -1) {
        BPoint *bp_src = &latt->def[mirror_data.vert_map[a]];

        if (bp->f1 & SELECT) {
          td_mirror->flag |= TD_SELECTED;
        }
        td_mirror->flag |= mirror_data.vert_flags[a];
        td_mirror->extra = nullptr;
        td_mirror->loc = bp->vec;
        copy_v3_v3(td_mirror->iloc, bp->vec);
        copy_v3_v3(td_mirror->center, bp->vec);
        td_mirror->loc_src = bp_src->vec;
        td_mirror++;
        continue;
      }

      if (!(is_prop_edit || (bp->f1 & SELECT))) {
        continue;
      }

      copy_v3_v3(td->iloc, bp->vec);
      td->loc = bp->vec;
      copy_v3_v3(td->center, td->loc);
      if (bp->f1 & SELECT) {
        td->flag = TD_SELECTED;
      }
      else {
        td->flag = 0;
      }
      copy_m3_m3(td->smtx, smtx);
      copy_m3_m3(td->mtx, mtx);

      td->val = nullptr;

      if (tc->use_mirror_axis_any) {
        if (tc->use_mirror_axis_x && fabsf(td->loc[0]) < TRANSFORM_LATTICE_MAXDIST_MIRROR) {
          td->flag |= TD_MIRROR_EDGE_X;
        }
        if (tc->use_mirror_axis_y && fabsf(td->loc[1]) < TRANSFORM_LATTICE_MAXDIST_MIRROR) {
          td->flag |= TD_MIRROR_EDGE_Y;
        }
        if (tc->use_mirror_axis_z && fabsf(td->loc[2]) < TRANSFORM_LATTICE_MAXDIST_MIRROR) {
          td->flag |= TD_MIRROR_EDGE_Z;
        }
      }

      td++;
    }

    lattice_mirrordata_free(mirror_data);
  }
}

static void recalcData_lattice(TransInfo *t)
{
  const bool is_canceling = (t->state == TRANS_CANCEL);
  if (!is_canceling) {
    transform_snap_project_individual_apply(t);

    const bool do_mirror = (t->flag & T_NO_MIRROR) == 0;
    if (do_mirror) {
      FOREACH_TRANS_DATA_CONTAINER (t, tc) {
        transform_convert_mirror_apply(tc);
      }
    }
  }

  FOREACH_TRANS_DATA_CONTAINER (t, tc) {
    Lattice *la = id_cast<Lattice *>(tc->obedit->data);
    DEG_id_tag_update(tc->obedit->data, ID_RECALC_GEOMETRY);
    if (la->editlatt->latt->flag & LT_OUTSIDE) {
      outside_lattice(la->editlatt->latt);
    }
  }
}

/** \} */

TransConvertTypeInfo TransConvertType_Lattice = {
    /*flags*/ (T_EDIT | T_POINTS),
    /*create_trans_data*/ createTransLatticeVerts,
    /*recalc_data*/ recalcData_lattice,
    /*special_aftertrans_update*/ nullptr,
};

}  // namespace blender::ed::transform
