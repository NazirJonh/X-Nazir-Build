/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "BKE_curves.hh"
#include "BKE_curves_hide.hh"
#include "BKE_geometry_set.hh"
#include "BKE_object.hh"

#include "BLI_array_utils.hh"
#include "BLI_index_range.hh"
#include "BLI_set.hh"
#include "BLI_span.hh"
#include "BLI_task.hh"
#include "BLI_vector.hh"

#include "DNA_object_types.h"

#include "DEG_depsgraph.hh"

namespace blender::bke::curves::hide {

SpanAttributeWriter<bool> hide_point_ensure(Curves &curves_id)
{
  CurvesGeometry &curves = curves_id.geometry.wrap();
  MutableAttributeAccessor attributes = curves.attributes_for_write();

  if (attributes.contains(".hide_point")) {
    return attributes.lookup_for_write_span<bool>(".hide_point");
  }

  const int64_t point_len = curves.points_num();
  attributes.add(".hide_point",
                 AttrDomain::Point,
                 AttrType::Bool,
                 AttributeInitVArray(VArray<bool>::from_single(false, point_len)));

  return attributes.lookup_for_write_span<bool>(".hide_point");
}

SpanAttributeWriter<bool> hide_curve_ensure(Curves &curves_id)
{
  CurvesGeometry &curves = curves_id.geometry.wrap();
  MutableAttributeAccessor attributes = curves.attributes_for_write();

  if (attributes.contains(".hide_curve")) {
    return attributes.lookup_for_write_span<bool>(".hide_curve");
  }

  const int64_t curve_len = curves.curves_num();
  attributes.add(".hide_curve",
                 AttrDomain::Curve,
                 AttrType::Bool,
                 AttributeInitVArray(VArray<bool>::from_single(false, curve_len)));

  return attributes.lookup_for_write_span<bool>(".hide_curve");
}

void hide_attributes_remove(Curves &curves_id)
{
  CurvesGeometry &curves = curves_id.geometry.wrap();
  MutableAttributeAccessor attributes = curves.attributes_for_write();

  attributes.remove(".hide_point");
  attributes.remove(".hide_curve");
}

bool point_is_hidden(const CurvesGeometry &curves, int point_index)
{
  const AttributeAccessor attributes = curves.attributes();
  const VArray hide_point = *attributes.lookup<bool>(".hide_point", AttrDomain::Point);

  if (hide_point.is_empty()) {
    return false;
  }

  return hide_point[point_index];
}

bool curve_is_hidden(const CurvesGeometry &curves, int curve_index)
{
  const AttributeAccessor attributes = curves.attributes();
  const VArray hide_curve = *attributes.lookup<bool>(".hide_curve", AttrDomain::Curve);

  if (hide_curve.is_empty()) {
    return false;
  }

  return hide_curve[curve_index];
}

bool has_hidden_elements(const CurvesGeometry &curves)
{
  const AttributeAccessor attributes = curves.attributes();

  const VArray hide_point = *attributes.lookup<bool>(".hide_point", AttrDomain::Point);
  if (!hide_point.is_empty()) {
    const VArraySpan<bool> span(hide_point);
    for (int i : span.index_range()) {
      if (span[i]) {
        return true;
      }
    }
  }

  const VArray hide_curve = *attributes.lookup<bool>(".hide_curve", AttrDomain::Curve);
  if (!hide_curve.is_empty()) {
    const VArraySpan<bool> span(hide_curve);
    for (int i : span.index_range()) {
      if (span[i]) {
        return true;
      }
    }
  }

  return false;
}

void hide_points(Object &object, const IndexMask &point_mask, VisAction action)
{
  Curves &curves_id = *id_cast<Curves *>(object.data);
  CurvesGeometry &curves = curves_id.geometry.wrap();
  MutableAttributeAccessor attributes = curves.attributes_for_write();

  SpanAttributeWriter<bool> hide_point = hide_point_ensure(curves_id);

  const bool hide_value = (action == VisAction::Hide);

  point_mask.foreach_index([&](const int64_t i) { hide_point.span[i] = hide_value; });

  hide_point.finish();

  if (action == VisAction::Hide) {
    MutableAttributeAccessor attributes = curves_id.geometry.wrap().attributes_for_write();

    if (AttrDomain(curves_id.selection_domain) == AttrDomain::Point) {
      SpanAttributeWriter<float> selection = attributes.lookup_or_add_for_write_span<float>(
          ".selection", AttrDomain::Point);

      if (selection) {
        point_mask.foreach_index([&](const int64_t i) { selection.span[i] = 0.0f; });
        selection.finish();
      }
    }
    else if (AttrDomain(curves_id.selection_domain) == AttrDomain::Curve) {
      SpanAttributeWriter<float> selection = attributes.lookup_or_add_for_write_span<float>(
          ".selection", AttrDomain::Curve);

      if (selection) {
        const OffsetIndices<int> points_by_curve = curves.points_by_curve();
        Set<int> affected_curves;

        point_mask.foreach_index([&](const int64_t point_i) {
          const int curve_i = curves.point_to_curve_map()[point_i];
          affected_curves.add(curve_i);
        });

        for (const int curve_i : affected_curves) {
          selection.span[curve_i] = 0.0f;
        }

        selection.finish();
      }
    }
  }

  sync_hide_from_points_to_curves(curves_id);

  DEG_id_tag_update(&curves_id.id, ID_RECALC_GEOMETRY);
}

void hide_curves(Object &object, const IndexMask &curve_mask, VisAction action)
{
  Curves &curves_id = *id_cast<Curves *>(object.data);
  CurvesGeometry &curves = curves_id.geometry.wrap();
  MutableAttributeAccessor attributes = curves.attributes_for_write();

  SpanAttributeWriter<bool> hide_curve = hide_curve_ensure(curves_id);

  const bool hide_value = (action == VisAction::Hide);

  curve_mask.foreach_index([&](const int64_t i) { hide_curve.span[i] = hide_value; });

  hide_curve.finish();

  if (action == VisAction::Hide) {
    MutableAttributeAccessor attributes = curves_id.geometry.wrap().attributes_for_write();

    if (AttrDomain(curves_id.selection_domain) == AttrDomain::Curve) {
      SpanAttributeWriter<float> selection = attributes.lookup_or_add_for_write_span<float>(
          ".selection", AttrDomain::Curve);

      if (selection) {
        curve_mask.foreach_index([&](const int64_t i) { selection.span[i] = 0.0f; });
        selection.finish();
      }
    }
    else if (AttrDomain(curves_id.selection_domain) == AttrDomain::Point) {
      SpanAttributeWriter<float> selection = attributes.lookup_or_add_for_write_span<float>(
          ".selection", AttrDomain::Point);

      if (selection) {
        const OffsetIndices<int> points_by_curve = curves.points_by_curve();
        curve_mask.foreach_index([&](const int64_t curve_i) {
          const IndexRange point_range = points_by_curve[curve_i];
          for (const int point_i : point_range) {
            selection.span[point_i] = 0.0f;
          }
        });

        selection.finish();
      }
    }
  }

  sync_hide_from_curves_to_points(curves_id);

  DEG_id_tag_update(&curves_id.id, ID_RECALC_GEOMETRY);
}

void show_all(Object &object, bool select)
{
  Curves &curves_id = *id_cast<Curves *>(object.data);
  CurvesGeometry &curves = curves_id.geometry.wrap();
  MutableAttributeAccessor attributes = curves.attributes_for_write();

  const VArray hide_point = *attributes.lookup<bool>(".hide_point", AttrDomain::Point);
  const VArray hide_curve = *attributes.lookup<bool>(".hide_curve", AttrDomain::Curve);

  if (!hide_point.is_empty() && select) {
    SpanAttributeWriter<float> selection = attributes.lookup_or_add_for_write_span<float>(
        ".selection", AttrDomain::Point);
    const VArraySpan<bool> hide_point_span(hide_point);
    const int selection_size = selection.span.size();
    for (const int i : IndexRange(selection_size)) {
      if (i < hide_point_span.size() && hide_point_span[i]) {
        selection.span[i] = 1.0f;
      }
    }
    selection.finish();
  }

  if (!hide_curve.is_empty() && select) {
    SpanAttributeWriter<float> selection = attributes.lookup_or_add_for_write_span<float>(
        ".selection", AttrDomain::Curve);
    const VArraySpan<bool> hide_curve_span(hide_curve);
    const int selection_size = selection.span.size();
    for (const int i : IndexRange(selection_size)) {
      if (i < hide_curve_span.size() && hide_curve_span[i]) {
        selection.span[i] = 1.0f;
      }
    }
    selection.finish();
  }

  hide_attributes_remove(curves_id);

  DEG_id_tag_update(&curves_id.id, ID_RECALC_GEOMETRY);
}

void invert_hide(Object &object, const IndexMask &mask, AttrDomain domain)
{
  Curves &curves_id = *id_cast<Curves *>(object.data);
  CurvesGeometry &curves = curves_id.geometry.wrap();
  MutableAttributeAccessor attributes = curves.attributes_for_write();

  if (domain == AttrDomain::Point) {
    SpanAttributeWriter<bool> hide_point = hide_point_ensure(curves_id);

    mask.foreach_index([&](const int64_t i) { hide_point.span[i] = !hide_point.span[i]; });

    hide_point.finish();
    sync_hide_from_points_to_curves(curves_id);
  }
  else if (domain == AttrDomain::Curve) {
    SpanAttributeWriter<bool> hide_curve = hide_curve_ensure(curves_id);

    mask.foreach_index([&](const int64_t i) { hide_curve.span[i] = !hide_curve.span[i]; });

    hide_curve.finish();
    sync_hide_from_curves_to_points(curves_id);
  }

  DEG_id_tag_update(&curves_id.id, ID_RECALC_GEOMETRY);
}

void sync_hide_from_points_to_curves(Curves &curves_id)
{
  CurvesGeometry &curves = curves_id.geometry.wrap();
  MutableAttributeAccessor attributes = curves.attributes_for_write();

  if (!attributes.contains(".hide_point")) {
    return;
  }

  SpanAttributeWriter<bool> hide_curve = hide_curve_ensure(curves_id);
  const AttributeAccessor attributes_read = curves.attributes();
  const VArraySpan<bool> hide_point = *attributes_read.lookup<bool>(".hide_point",
                                                                    AttrDomain::Point);

  const OffsetIndices<int> points_by_curve = curves.points_by_curve();

  threading::parallel_for(curves.curves_range(), 1024, [&](const IndexRange range) {
    for (const int curve_i : range) {
      const IndexRange points = points_by_curve[curve_i];

      bool all_points_hidden = true;
      for (const int point_i : points) {
        if (!hide_point[point_i]) {
          all_points_hidden = false;
          break;
        }
      }

      hide_curve.span[curve_i] = all_points_hidden;
    }
  });

  hide_curve.finish();
}

void sync_hide_from_curves_to_points(Curves &curves_id)
{
  CurvesGeometry &curves = curves_id.geometry.wrap();
  MutableAttributeAccessor attributes = curves.attributes_for_write();

  if (!attributes.contains(".hide_curve")) {
    return;
  }

  SpanAttributeWriter<bool> hide_point = hide_point_ensure(curves_id);
  const AttributeAccessor attributes_read = curves.attributes();
  const VArraySpan<bool> hide_curve = *attributes_read.lookup<bool>(".hide_curve",
                                                                    AttrDomain::Curve);

  const OffsetIndices<int> points_by_curve = curves.points_by_curve();

  threading::parallel_for(curves.curves_range(), 1024, [&](const IndexRange range) {
    for (const int curve_i : range) {
      if (hide_curve[curve_i]) {
        const IndexRange points = points_by_curve[curve_i];
        for (const int point_i : points) {
          hide_point.span[point_i] = true;
        }
      }
    }
  });

  hide_point.finish();
}

}  // namespace blender::bke::curves::hide
