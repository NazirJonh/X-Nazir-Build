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

void face_selection_mask_build(const Mesh &mesh, FaceSelectionMask &mask)
{
  mask = FaceSelectionMask{};
  if (!(mesh.editflag & ME_EDIT_PAINT_FACE_SEL)) {
    mask.state = FaceSelectionMaskState::Disabled;
    return;
  }
  /* Plain lookup: a missing attribute means "nothing is selected" (strict, like the projection
   * texture paint) and must not materialize a default varray. */
  const VArray<bool> select_poly = *mesh.attributes().lookup<bool>(".select_poly",
                                                                   bke::AttrDomain::Face);
  if (!select_poly) {
    mask.state = FaceSelectionMaskState::Empty;
    return;
  }
  /* Parallel "any" over the whole array: this runs once per stroke instead of on every dab, and
   * doubles as the block-all decision. */
  const array_utils::BooleanMix mix = array_utils::booleans_mix_calc(select_poly);
  if (mix == array_utils::BooleanMix::AllFalse) {
    mask.state = FaceSelectionMaskState::Empty;
    return;
  }
  mask.state = FaceSelectionMaskState::Active;
  mask.select_poly_varray = select_poly;
  mask.select_poly = VArraySpan<bool>(select_poly);

  const VArraySpan<bool> select_poly_span(mask.select_poly_varray);
  const GroupedSpan<int> vert_to_face_map = mesh.vert_to_face_map();
  mask.vert_paintable.reinitialize(mesh.verts_num);
  threading::parallel_for(mask.vert_paintable.index_range(), 2048, [&](const IndexRange range) {
    for (const int vert : range) {
      bool has_selected_face = false;
      for (const int face : vert_to_face_map[vert]) {
        if (select_poly_span[face]) {
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
  if (mask.state == FaceSelectionMaskState::Unset) {
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

  if (face_selection_mask.state != FaceSelectionMaskState::Active) {
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

bool face_selection_mask_blocks_paint(const Mesh &mesh)
{
  if (!(mesh.editflag & ME_EDIT_PAINT_FACE_SEL)) {
    return false;
  }
  const VArray<bool> select_poly = *mesh.attributes().lookup<bool>(".select_poly",
                                                                   bke::AttrDomain::Face);
  if (!select_poly) {
    /* Masking enabled but the mesh has no face selection attribute: nothing was ever selected. */
    return true;
  }
  /* Stored attributes are span-backed, so this is a zero-copy scan with early exit. */
  const VArraySpan<bool> select_poly_span(select_poly);
  return std::find(select_poly_span.begin(), select_poly_span.end(), true) ==
         select_poly_span.end();
}

}  // namespace blender::ed::sculpt_paint
