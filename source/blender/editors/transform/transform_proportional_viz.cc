/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edtransform
 *
 * Proportional editing falloff visualization.
 *
 * Publishes the per-element falloff factors (#TransData.factor) computed by
 * #calculatePropRatio so the draw module can highlight the edited elements with their actual
 * influence. The factors are stored per edited data-block and are only valid while a
 * proportional transform is running (cleared in #postTrans).
 *
 * The factors are never recomputed here: they are the exact values the transform applies, so
 * the visualization is correct for every falloff mode, including connected and projected
 * editing and the random falloff.
 *
 * The storages are keyed by whatever pointer the draw module can resolve to the edited data
 * without a depsgraph lookup:
 * - Edit meshes by #BMesh, factors indexed by #BMVert index.
 * - New curves by the original #Curves ID, factors laid out like the edit points vertex buffer
 *   (`[points] [left bezier handles] [right bezier handles]`).
 * - Legacy curves and surfaces by #EditNurb, factors keyed by the #TransData location pointers
 *   (which are the `BezTriple::vec` / `BPoint::vec` addresses the vertex buffers are filled
 *   from).
 */

#include "BLI_array.hh"
#include "BLI_index_mask.hh"
#include "BLI_index_range.hh"
#include "BLI_map.hh"
#include "BLI_offset_indices.hh"
#include "BLI_span.hh"

#include "BKE_curves.hh"
#include "BKE_curves_utils.hh"
#include "BKE_editmesh.hh"

#include "DEG_depsgraph.hh"

#include "DNA_curve_types.h"
#include "DNA_curves_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"

#include "ED_transform.hh"

#include "transform_convert.hh"
#include "transform.hh"

namespace blender::ed::transform {

namespace detail {

/** Above this element count the highlight is skipped to keep the transform responsive. */
constexpr int prop_falloff_viz_max_verts = 2'000'000;

struct PropFalloffVizData {
  /** Falloff factors indexed by #BMVert index. */
  Array<float> factors;
  /** The original mesh, used to tag the draw cache when the data is cleared. */
  Mesh *mesh_orig;
};

struct PropFalloffVizCurvesData {
  /**
   * Falloff factors in the edit points vertex buffer layout:
   * `[points 0..points_num) [left bezier handles] [right bezier handles]`.
   */
  Array<float> factors;
  /** The original curves ID, used to tag the draw cache when the data is cleared. */
  Curves *curves_orig;
};

struct PropFalloffVizCurveEditData {
  /** Falloff factors keyed by the `TransData::loc` pointers (`BezTriple::vec` / `BPoint::vec`). */
  Map<const float *, float> factor_by_loc;
  /** The original curve ID, used to tag the draw cache when the data is cleared. */
  Curve *curve_orig;
};

static Map<const BMesh *, PropFalloffVizData> &get_prop_falloff_map()
{
  static Map<const BMesh *, PropFalloffVizData> map;
  return map;
}

static Map<const Curves *, PropFalloffVizCurvesData> &get_prop_falloff_curves_map()
{
  static Map<const Curves *, PropFalloffVizCurvesData> map;
  return map;
}

static Map<const EditNurb *, PropFalloffVizCurveEditData> &get_prop_falloff_curve_edit_map()
{
  static Map<const EditNurb *, PropFalloffVizCurveEditData> map;
  return map;
}

/** Tag the draw caches of everything currently published so a rebuild picks the new state up. */
static void prop_falloff_viz_tag()
{
  for (const PropFalloffVizData &data : get_prop_falloff_map().values()) {
    DEG_id_tag_update(&data.mesh_orig->id, ID_RECALC_SELECT);
  }
  for (const PropFalloffVizCurvesData &data : get_prop_falloff_curves_map().values()) {
    /* #ID_RECALC_SELECT is not handled for #Curves data, so use the geometry tag (which the
     * transform triggers on every frame anyway) to invalidate the draw caches. */
    DEG_id_tag_update(&data.curves_orig->id, ID_RECALC_GEOMETRY);
  }
  for (const PropFalloffVizCurveEditData &data : get_prop_falloff_curve_edit_map().values()) {
    DEG_id_tag_update(&data.curve_orig->id, ID_RECALC_SELECT);
  }
}

/** Publish the factors of one proportional edit-mesh container. */
static void prop_falloff_viz_update_mesh(const TransDataContainer &tc)
{
  Object *obedit = tc.obedit;
  if ((obedit == nullptr) || (obedit->type != OB_MESH)) {
    return;
  }
  BMEditMesh *em = BKE_editmesh_from_object(obedit);
  if ((em == nullptr) || (em->bm == nullptr) || (em->bm->totvert == 0)) {
    return;
  }
  BMesh *bm = em->bm;
  if (bm->totvert > prop_falloff_viz_max_verts) {
    return;
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
  get_prop_falloff_map().add(bm, std::move(data));
}

/** Publish the factors of one proportional edit-curves container. */
static void prop_falloff_viz_update_curves(const TransDataContainer &tc)
{
  Object *obedit = tc.obedit;
  if ((obedit == nullptr) || (obedit->type != OB_CURVES)) {
    return;
  }
  Curves *curves_orig = id_cast<Curves *>(obedit->data);
  const bke::CurvesGeometry &curves = curves_orig->geometry.wrap();
  const int points_num = curves.points_num();
  if (points_num == 0) {
    return;
  }
  const CurvesTransformData *transform_data = static_cast<const CurvesTransformData *>(
      tc.custom.type.data);
  if ((transform_data == nullptr) || (tc.data_len == 0)) {
    return;
  }
  const Span<IndexMask> selection_by_layer = transform_data->selection_by_layer;
  const OffsetIndices<int> layer_offsets(transform_data->layer_offsets.as_span());
  BLI_assert(selection_by_layer.size() <= 3);

  const OffsetIndices<int> points_by_curve = curves.points_by_curve();
  IndexMaskMemory memory;
  const IndexMask bezier_curves = bke::curves::indices_for_type(curves.curve_types(),
                                                                curves.curve_type_counts(),
                                                                CURVE_TYPE_BEZIER,
                                                                curves.curves_range(),
                                                                memory);
  Array<int> bezier_offset_data(bezier_curves.size() + 1);
  const OffsetIndices<int> bezier_offsets = offset_indices::gather_selected_offsets(
      points_by_curve, bezier_curves, bezier_offset_data);
  const int handles_num = bezier_offsets.total_size();

  if (points_num + 2 * handles_num > prop_falloff_viz_max_verts) {
    return;
  }

  Array<float> factors(points_num + 2 * handles_num, 0.0f);

  /* Under proportional editing the first layer holds every control point. */
  MutableSpan<float> point_factors = factors.as_mutable_span().take_front(points_num);
  selection_by_layer[0].foreach_index([&](const int64_t point, const int64_t pos) {
    point_factors[point] = tc.data[layer_offsets[0].start() + pos].factor;
  });

  /* The handle layers only exist when the geometry has bezier curves. Their selection masks
   * address points, while the draw vertex buffers address the handles by slot within each
   * bezier curve. Transpose like the handle vertex buffers do (see
   * #create_edit_points_position in draw_cache_impl_curves.cc). */
  if (selection_by_layer.size() > 1 && handles_num > 0) {
    Array<int> point_to_layer_pos(points_num, -1);
    selection_by_layer[1].foreach_index([&](const int64_t point, const int64_t pos) {
      point_to_layer_pos[point] = int(pos);
    });
    MutableSpan<float> all_factors = factors.as_mutable_span();
    MutableSpan<float> left_handle_factors = all_factors.slice(points_num, handles_num);
    MutableSpan<float> right_handle_factors = all_factors.slice(points_num + handles_num,
                                                                handles_num);
    bezier_curves.foreach_index([&](const int64_t curve, const int64_t rank) {
      int slot = bezier_offsets[rank].start();
      for (const int point : points_by_curve[curve]) {
        const int layer_pos = point_to_layer_pos[point];
        if (layer_pos != -1) {
          left_handle_factors[slot] = tc.data[layer_offsets[1].start() + layer_pos].factor;
          right_handle_factors[slot] = tc.data[layer_offsets[2].start() + layer_pos].factor;
        }
        slot++;
      }
    });
  }

  /* Keyed by the original #Curves ID: the draw side resolves evaluated objects back to the
   * original data when it builds the edit points batch. The falloff VBO is re-extracted on
   * every rebuild of the batch cache, which the transform triggers each frame anyway
   * (#recalcData_curves tags ID_RECALC_GEOMETRY). */
  PropFalloffVizCurvesData data;
  data.factors = std::move(factors);
  data.curves_orig = curves_orig;
  get_prop_falloff_curves_map().add(curves_orig, std::move(data));
}

/** Publish the factors of one proportional legacy curve or surface container. */
static void prop_falloff_viz_update_curve_edit(const TransDataContainer &tc)
{
  Object *obedit = tc.obedit;
  if ((obedit == nullptr) || !ELEM(obedit->type, OB_CURVES_LEGACY, OB_SURF)) {
    return;
  }
  Curve *curve_orig = id_cast<Curve *>(obedit->data);
  if (curve_orig->editnurb == nullptr) {
    return;
  }
  if (tc.data_len == 0) {
    return;
  }
  if (tc.data_len > prop_falloff_viz_max_verts) {
    return;
  }

  PropFalloffVizCurveEditData data;
  data.curve_orig = curve_orig;
  data.factor_by_loc.reserve(tc.data_len);
  for (const int i : IndexRange(tc.data_len)) {
    const TransData &td = tc.data[i];
    if (td.loc == nullptr) {
      continue;
    }
    /* The `td.loc` pointers are the addresses the draw module writes into its vertex buffers,
     * so they are the natural key. Every visible element gets a distinct one. */
    data.factor_by_loc.add_new(td.loc, td.factor);
  }

  /* Keyed by #EditNurb: the original and evaluated curves share it, and the draw module reads
   * the same list of nurbs when it fills the edit vertices batch. */
  get_prop_falloff_curve_edit_map().add(curve_orig->editnurb, std::move(data));
}

}  // namespace detail

void prop_falloff_viz_update(const TransInfo &t)
{
  /* Only the edit mesh, edit curves and legacy curve/surface transforms are visualized. In
   * particular the Grease Pencil transform populates its data the same way as the curves one,
   * but has a different overlay that is out of scope here. */
  const bool supported_type = ELEM(t.data_type,
                                   &TransConvertType_Mesh,
                                   &curves::TransConvertType_Curves,
                                   &TransConvertType_Curve);
  if ((t.flag & T_PROP_EDIT) == 0 || !supported_type) {
    if (!detail::get_prop_falloff_map().is_empty() ||
        !detail::get_prop_falloff_curves_map().is_empty() ||
        !detail::get_prop_falloff_curve_edit_map().is_empty())
    {
      prop_falloff_viz_clear();
    }
    return;
  }

  detail::get_prop_falloff_map().clear();
  detail::get_prop_falloff_curves_map().clear();
  detail::get_prop_falloff_curve_edit_map().clear();

  for (const int tc_index : IndexRange(t.data_container_len)) {
    const TransDataContainer &tc = t.data_container[tc_index];
    if (tc.data_len == 0) {
      continue;
    }
    if (t.data_type == &TransConvertType_Mesh) {
      detail::prop_falloff_viz_update_mesh(tc);
    }
    else if (t.data_type == &curves::TransConvertType_Curves) {
      detail::prop_falloff_viz_update_curves(tc);
    }
    else {
      detail::prop_falloff_viz_update_curve_edit(tc);
    }
  }

  /* Tag the draw caches so the highlight appears right away, even before the first mouse
   * move would trigger a geometry rebuild through the recalc callbacks. */
  detail::prop_falloff_viz_tag();
}

void prop_falloff_viz_clear()
{
  /* Nothing was published: do not tag any data-block. This runs from every transform #postTrans
   * (see #transform_generics.cc), including the many that never touched proportional editing. */
  if (detail::get_prop_falloff_map().is_empty() &&
      detail::get_prop_falloff_curves_map().is_empty() &&
      detail::get_prop_falloff_curve_edit_map().is_empty())
  {
    return;
  }
  /* The last drawn batch caches were built while the factors were still published, so tag the
   * data-blocks to make the highlight disappear right away. #ID_RECALC_SELECT is the same path
   * selection changes use to invalidate the edit-mode draw data. */
  detail::prop_falloff_viz_tag();
  detail::get_prop_falloff_map().clear();
  detail::get_prop_falloff_curves_map().clear();
  detail::get_prop_falloff_curve_edit_map().clear();
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

Span<float> get_proportional_falloff_factors(const Curves &curves_orig)
{
  const detail::PropFalloffVizCurvesData *data = detail::get_prop_falloff_curves_map().lookup_ptr(
      &curves_orig);
  return data != nullptr ? data->factors.as_span() : Span<float>();
}

bool has_proportional_falloff_data(const Curves &curves_orig)
{
  return detail::get_prop_falloff_curves_map().contains(&curves_orig);
}

std::optional<float> get_proportional_falloff_factor(const EditNurb &editnurb, const float *loc)
{
  const detail::PropFalloffVizCurveEditData *data =
      detail::get_prop_falloff_curve_edit_map().lookup_ptr(&editnurb);
  if (data == nullptr) {
    return std::nullopt;
  }
  const float *factor = data->factor_by_loc.lookup_ptr(loc);
  if (factor == nullptr) {
    return std::nullopt;
  }
  return *factor;
}

bool has_proportional_falloff_data(const EditNurb &editnurb)
{
  return detail::get_prop_falloff_curve_edit_map().contains(&editnurb);
}

}  // namespace blender::ed::transform
