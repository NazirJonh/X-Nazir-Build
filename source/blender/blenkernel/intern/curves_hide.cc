/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_curves.hh"
#include "BKE_curves_hide.hh"
#include "BKE_geometry_set.hh"
#include "BKE_object.hh"

#include "BLI_array_utils.hh"
#include "BLI_index_range.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "DNA_object_types.h"

#include "DEG_depsgraph.hh"

namespace blender::ed::sculpt_paint::curves_hide {

bke::SpanAttributeWriter<bool> hide_point_ensure(Curves &curves_id)
{
  bke::CurvesGeometry &curves = curves_id.geometry.wrap();
  bke::MutableAttributeAccessor attributes = curves.attributes_for_write();

  if (attributes.contains(".hide_point")) {
    return attributes.lookup_for_write_span<bool>(".hide_point");
  }

  const int64_t point_len = curves.points_num();
  attributes.add(".hide_point",
                 bke::AttrDomain::Point,
                 bke::AttrType::Bool,
                 bke::AttributeInitVArray(VArray<bool>::from_single(false, point_len)));

  return attributes.lookup_for_write_span<bool>(".hide_point");
}

bke::SpanAttributeWriter<bool> hide_curve_ensure(Curves &curves_id)
{
  bke::CurvesGeometry &curves = curves_id.geometry.wrap();
  bke::MutableAttributeAccessor attributes = curves.attributes_for_write();

  if (attributes.contains(".hide_curve")) {
    return attributes.lookup_for_write_span<bool>(".hide_curve");
  }

  const int64_t curve_len = curves.curves_num();
  attributes.add(".hide_curve",
                 bke::AttrDomain::Curve,
                 bke::AttrType::Bool,
                 bke::AttributeInitVArray(VArray<bool>::from_single(false, curve_len)));

  return attributes.lookup_for_write_span<bool>(".hide_curve");
}

void hide_attributes_remove(Curves &curves_id)
{
  bke::CurvesGeometry &curves = curves_id.geometry.wrap();
  bke::MutableAttributeAccessor attributes = curves.attributes_for_write();

  attributes.remove(".hide_point");
  attributes.remove(".hide_curve");
}

bool point_is_hidden(const bke::CurvesGeometry &curves, int point_index)
{
  const bke::AttributeAccessor attributes = curves.attributes();
  const VArray hide_point = *attributes.lookup<bool>(".hide_point", bke::AttrDomain::Point);

  if (hide_point.is_empty()) {
    return false;
  }

  return hide_point[point_index];
}

bool curve_is_hidden(const bke::CurvesGeometry &curves, int curve_index)
{
  const bke::AttributeAccessor attributes = curves.attributes();
  const VArray hide_curve = *attributes.lookup<bool>(".hide_curve", bke::AttrDomain::Curve);

  if (hide_curve.is_empty()) {
    return false;
  }

  return hide_curve[curve_index];
}

bool has_hidden_elements(const bke::CurvesGeometry &curves)
{
  const bke::AttributeAccessor attributes = curves.attributes();

  const VArray hide_point = *attributes.lookup<bool>(".hide_point", bke::AttrDomain::Point);
  if (!hide_point.is_empty()) {
    const VArraySpan<bool> span(hide_point);
    for (int i : span.index_range()) {
      if (span[i]) {
        return true;
      }
    }
  }

  const VArray hide_curve = *attributes.lookup<bool>(".hide_curve", bke::AttrDomain::Curve);
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
  printf("DEBUG: hide_points called\n");
  printf("DEBUG: action = %s\n", action == VisAction::Hide ? "Hide" : "Reveal");
  printf("DEBUG: point_mask.size() = %d\n", int(point_mask.size()));

  Curves &curves_id = *reinterpret_cast<Curves *>(object.data);
  bke::CurvesGeometry &curves = curves_id.geometry.wrap();
  bke::MutableAttributeAccessor attributes = curves.attributes_for_write();

  bke::SpanAttributeWriter<bool> hide_point = hide_point_ensure(curves_id);

  const bool hide_value = (action == VisAction::Hide);
  printf("DEBUG: hide_value = %s\n", hide_value ? "true" : "false");
  printf("DEBUG: curves.points_num() = %d\n", curves.points_num());

  int hidden_count = 0;
  point_mask.foreach_index([&](const int i) { 
    hide_point.span[i] = hide_value;
    if (hide_value) {
      hidden_count++;
    }
  });

  printf("DEBUG: Total hidden points = %d\n", hidden_count);

  hide_point.finish();

  sync_hide_from_points_to_curves(curves_id);

  DEG_id_tag_update(&curves_id.id, ID_RECALC_GEOMETRY);
  printf("DEBUG: hide_points completed, geometry tagged for update\n");
}

void hide_curves(Object &object, const IndexMask &curve_mask, VisAction action)
{
  printf("DEBUG: hide_curves called\n");
  printf("DEBUG: action = %s\n", action == VisAction::Hide ? "Hide" : "Reveal");
  printf("DEBUG: curve_mask.size() = %d\n", int(curve_mask.size()));

  Curves &curves_id = *reinterpret_cast<Curves *>(object.data);
  bke::CurvesGeometry &curves = curves_id.geometry.wrap();
  bke::MutableAttributeAccessor attributes = curves.attributes_for_write();

  bke::SpanAttributeWriter<bool> hide_curve = hide_curve_ensure(curves_id);

  const bool hide_value = (action == VisAction::Hide);
  printf("DEBUG: hide_value = %s\n", hide_value ? "true" : "false");
  printf("DEBUG: curves.curves_num() = %d\n", curves.curves_num());

  int hidden_count = 0;
  curve_mask.foreach_index([&](const int i) { 
    hide_curve.span[i] = hide_value;
    if (hide_value) {
      hidden_count++;
    }
  });

  printf("DEBUG: Total hidden curves = %d\n", hidden_count);

  hide_curve.finish();

  sync_hide_from_curves_to_points(curves_id);

  DEG_id_tag_update(&curves_id.id, ID_RECALC_GEOMETRY);
  printf("DEBUG: hide_curves completed, geometry tagged for update\n");
}

void show_all(Object &object)
{
  Curves &curves_id = *reinterpret_cast<Curves *>(object.data);
  
  hide_attributes_remove(curves_id);

  DEG_id_tag_update(&curves_id.id, ID_RECALC_GEOMETRY);
}

void invert_hide(Object &object, const IndexMask &mask, bke::AttrDomain domain)
{
  Curves &curves_id = *reinterpret_cast<Curves *>(object.data);
  bke::CurvesGeometry &curves = curves_id.geometry.wrap();
  bke::MutableAttributeAccessor attributes = curves.attributes_for_write();

  if (domain == bke::AttrDomain::Point) {
    bke::SpanAttributeWriter<bool> hide_point = hide_point_ensure(curves_id);
    mask.foreach_index([&](const int i) { hide_point.span[i] = !hide_point.span[i]; });
    hide_point.finish();
    sync_hide_from_points_to_curves(curves_id);
  }
  else if (domain == bke::AttrDomain::Curve) {
    bke::SpanAttributeWriter<bool> hide_curve = hide_curve_ensure(curves_id);
    mask.foreach_index([&](const int i) { hide_curve.span[i] = !hide_curve.span[i]; });
    hide_curve.finish();
    sync_hide_from_curves_to_points(curves_id);
  }

  DEG_id_tag_update(&curves_id.id, ID_RECALC_GEOMETRY);
}

void sync_hide_from_points_to_curves(Curves &curves_id)
{
  bke::CurvesGeometry &curves = curves_id.geometry.wrap();
  bke::MutableAttributeAccessor attributes = curves.attributes_for_write();

  const VArray hide_point = *attributes.lookup<bool>(".hide_point", bke::AttrDomain::Point);
  if (hide_point.is_empty()) {
    return;
  }

  bke::SpanAttributeWriter<bool> hide_curve = hide_curve_ensure(curves_id);
  const VArraySpan<bool> hide_point_span(hide_point);

  const OffsetIndices<int> points_by_curve = curves.points_by_curve();
  
  for (const int curve_i : curves.curves_range()) {
    const IndexRange points = points_by_curve[curve_i];
    bool all_hidden = true;
    
    for (const int point_i : points) {
      if (!hide_point_span[point_i]) {
        all_hidden = false;
        break;
      }
    }
    
    hide_curve.span[curve_i] = all_hidden;
  }

  hide_curve.finish();
}

void sync_hide_from_curves_to_points(Curves &curves_id)
{
  bke::CurvesGeometry &curves = curves_id.geometry.wrap();
  bke::MutableAttributeAccessor attributes = curves.attributes_for_write();

  const VArray hide_curve = *attributes.lookup<bool>(".hide_curve", bke::AttrDomain::Curve);
  if (hide_curve.is_empty()) {
    return;
  }

  bke::SpanAttributeWriter<bool> hide_point = hide_point_ensure(curves_id);
  const VArraySpan<bool> hide_curve_span(hide_curve);

  const OffsetIndices<int> points_by_curve = curves.points_by_curve();
  
  for (const int curve_i : curves.curves_range()) {
    const IndexRange points = points_by_curve[curve_i];
    const bool curve_hidden = hide_curve_span[curve_i];
    
    for (const int point_i : points) {
      hide_point.span[point_i] = curve_hidden;
    }
  }

  hide_point.finish();
}

}  // namespace blender::ed::sculpt_paint::curves_hide 
