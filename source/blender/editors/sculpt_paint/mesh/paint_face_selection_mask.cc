/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include <algorithm>

#include "BLI_array_utils.hh"
#include "BLI_task.hh"
#include "BLI_utildefines.h"

#include "DNA_mesh_types.h"

#include "BKE_attribute.hh"
#include "BKE_mesh.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"

#include "PRF_profile.hh"

#include "paint_face_selection_mask.hh"
#include "sculpt_filter.hh"
#include "sculpt_intern.hh"

namespace blender::ed::sculpt_paint {

FaceSelectionState face_selection_state(const Mesh &mesh)
{
  if (!(mesh.editflag & ME_EDIT_PAINT_FACE_SEL)) {
    return FaceSelectionState::Disabled;
  }
  /* Plain lookup: a missing attribute means "nothing is selected" (strict, like the projection
   * texture paint) and must not materialize a default varray. */
  const VArray<bool> select_poly = *mesh.attributes().lookup<bool>(".select_poly",
                                                                   bke::AttrDomain::Face);
  if (!select_poly) {
    return FaceSelectionState::Empty;
  }
  if (array_utils::booleans_mix_calc(select_poly) == array_utils::BooleanMix::AllFalse) {
    return FaceSelectionState::Empty;
  }
  return FaceSelectionState::Active;
}

void face_selection_mask_build(const Mesh &mesh, FaceSelectionMask &mask)
{
  mask = FaceSelectionMask{};
  mask.state = face_selection_state(mesh);
  if (mask.state != FaceSelectionState::Active) {
    return;
  }
  mask.select_poly = VArraySpan<bool>(
      *mesh.attributes().lookup<bool>(".select_poly", bke::AttrDomain::Face));

  const GroupedSpan<int> vert_to_face_map = mesh.vert_to_face_map();
  mask.vert_paintable.reinitialize(mesh.verts_num);
  threading::parallel_for(mask.vert_paintable.index_range(), 2048, [&](const IndexRange range) {
    for (const int vert : range) {
      bool has_selected_face = false;
      for (const int face : vert_to_face_map[vert]) {
        if (mask.select_poly[face]) {
          has_selected_face = true;
          break;
        }
      }
      mask.vert_paintable[vert] = has_selected_face;
    }
  });
}

const FaceSelectionMask &face_selection_mask_ensure(Object &ob)
{
  SculptSession &ss = *ob.runtime->sculpt_session;
  BLI_assert(ss.cache != nullptr || ss.filter_cache != nullptr);
  FaceSelectionMask &mask = ss.cache ? ss.cache->face_selection_mask :
                                       ss.filter_cache->face_selection_mask;
  if (!mask.state) {
    face_selection_mask_build(*id_cast<const Mesh *>(ob.data), mask);
  }
  return mask;
}

void filter_factors_with_face_selection(const FaceSelectionMask &face_selection_mask,
                                        const Span<int> verts,
                                        const MutableSpan<float> r_factors)
{
  PRF_scope(ProfileCategory::Editor);
  BLI_assert(verts.size() == r_factors.size());

  if (face_selection_mask.state != FaceSelectionState::Active) {
    return;
  }
  /* O(verts): the per-vertex table was built once for the stroke. */
  const Span<bool> vert_paintable = face_selection_mask.vert_paintable;
  for (const int i : verts.index_range()) {
    if (!vert_paintable[verts[i]]) {
      r_factors[i] = 0.0f;
    }
  }
}

}  // namespace blender::ed::sculpt_paint
