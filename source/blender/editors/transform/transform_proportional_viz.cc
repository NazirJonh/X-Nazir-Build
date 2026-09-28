/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edtransform
 *
 * Proportional editing falloff visualization.
 *
 * Publishes the per-element falloff factors (#TransData.factor) computed by
 * #calculatePropRatio so the draw module can highlight the edit-mesh vertices with their
 * actual influence. The factors are stored per edited BMesh, indexed by #BMVert index, and
 * are only valid while a proportional transform is running (cleared in #postTrans).
 *
 * The factors are never recomputed here: they are the exact values the transform applies, so
 * the visualization is correct for every falloff mode, including connected and projected
 * editing and the random falloff.
 */

#include "BLI_array.hh"
#include "BLI_index_range.hh"
#include "BLI_map.hh"
#include "BLI_span.hh"

#include "BKE_editmesh.hh"

#include "DEG_depsgraph.hh"

#include "DNA_mesh_types.h"
#include "DNA_object_types.h"

#include "ED_transform.hh"

#include "transform_convert.hh"
#include "transform.hh"

namespace blender::ed::transform {

namespace detail {

/** Above this vertex count the highlight is skipped to keep the transform responsive. */
constexpr int prop_falloff_viz_max_verts = 2'000'000;

struct PropFalloffVizData {
  /** Falloff factors indexed by #BMVert index. */
  Array<float> factors;
  /** The original mesh, used to tag the draw cache when the data is cleared. */
  Mesh *mesh_orig;
};

Map<const BMesh *, PropFalloffVizData> &get_prop_falloff_map()
{
  static Map<const BMesh *, PropFalloffVizData> map;
  return map;
}

}  // namespace detail

void prop_falloff_viz_update(const TransInfo &t)
{
  using detail::PropFalloffVizData;
  Map<const BMesh *, PropFalloffVizData> &map = detail::get_prop_falloff_map();

  /* Only the vertex-based mesh transform stores #BMVert pointers in #TransData::extra (the UV
   * transform stores #BMLoop instead), so any other transform type gets no visualization. */
  if ((t.flag & T_PROP_EDIT) == 0 || t.data_type != &TransConvertType_Mesh) {
    if (!map.is_empty()) {
      prop_falloff_viz_clear();
    }
    return;
  }

  map.clear();

  for (const int tc_index : IndexRange(t.data_container_len)) {
    const TransDataContainer &tc = t.data_container[tc_index];
    Object *obedit = tc.obedit;
    if ((obedit == nullptr) || (obedit->type != OB_MESH)) {
      continue;
    }
    BMEditMesh *em = BKE_editmesh_from_object(obedit);
    if ((em == nullptr) || (em->bm == nullptr) || (em->bm->totvert == 0)) {
      continue;
    }
    BMesh *bm = em->bm;
    if (bm->totvert > detail::prop_falloff_viz_max_verts) {
      continue;
    }

    Array<float> factors(bm->totvert, 0.0f);

    for (const int i : IndexRange(tc.data_len)) {
      const TransData &td = tc.data[i];
      const BMVert *vert = static_cast<const BMVert *>(td.extra);
      if (vert == nullptr) {
        continue;
      }
      const int vert_index = BM_elem_index_get(vert);
      if (uint(vert_index) >= uint(bm->totvert)) {
        continue;
      }
      factors[vert_index] = td.factor;
    }

    /* Mirror vertices (#TransDataMirror) carry no factor of their own: inherit the factor of
     * the vertex they are mirrored from, matched through the source `BMVert.co` pointer. */
    if (tc.data_mirror_len > 0) {
      Map<const float *, float> factor_by_co;
      factor_by_co.reserve(tc.data_len);
      for (const int i : IndexRange(tc.data_len)) {
        factor_by_co.add_new(tc.data[i].loc, tc.data[i].factor);
      }
      for (const int i : IndexRange(tc.data_mirror_len)) {
        const TransDataMirror &td_mirror = tc.data_mirror[i];
        const BMVert *vert = static_cast<const BMVert *>(td_mirror.extra);
        const float *factor = factor_by_co.lookup_ptr(td_mirror.loc_src);
        if ((vert == nullptr) || (factor == nullptr)) {
          continue;
        }
        const int vert_index = BM_elem_index_get(vert);
        if (uint(vert_index) >= uint(bm->totvert)) {
          continue;
        }
        factors[vert_index] = *factor;
      }
    }

    /* Keyed by #BMesh: the draw side reads the factors through the mesh render data, which
     * deals with evaluated objects but always refers to this same edit-mesh. The falloff VBO
     * is re-extracted on every rebuild of the mesh batch cache, which the transform triggers
     * each frame anyway (#recalcData_mesh tags ID_RECALC_GEOMETRY). */
    PropFalloffVizData data;
    data.factors = std::move(factors);
    data.mesh_orig = id_cast<Mesh *>(obedit->data);
    map.add(bm, std::move(data));
  }

  /* Tag the draw caches so the highlight appears right away, even before the first mouse
   * move would trigger a geometry rebuild through #recalcData_mesh. */
  for (const detail::PropFalloffVizData &data : map.values()) {
    DEG_id_tag_update(&data.mesh_orig->id, ID_RECALC_SELECT);
  }
}

void prop_falloff_viz_clear()
{
  Map<const BMesh *, detail::PropFalloffVizData> &map = detail::get_prop_falloff_map();
  /* The last drawn batch cache was built while the factors were still published, so tag the
   * meshes to make the highlight disappear right away. #ID_RECALC_SELECT is the same path
   * selection changes use to invalidate the edit-mode draw data. */
  for (const detail::PropFalloffVizData &data : map.values()) {
    DEG_id_tag_update(&data.mesh_orig->id, ID_RECALC_SELECT);
  }
  map.clear();
}

Span<float> get_proportional_falloff_factors(const BMesh &bm)
{
  const detail::PropFalloffVizData *data = detail::get_prop_falloff_map().lookup_ptr(&bm);
  return data != nullptr ? data->factors.as_span() : Span<float>();
}

bool has_proportional_falloff_data(const BMesh &bm)
{
  return detail::get_prop_falloff_map().contains(&bm);
}

}  // namespace blender::ed::transform
