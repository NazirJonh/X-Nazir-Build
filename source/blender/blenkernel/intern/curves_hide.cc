/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "BLI_array_utils.hh"
#include "BLI_index_mask.hh"
#include "BLI_task.hh"

#include "BKE_curves.hh"
#include "BKE_curves_hide.hh"

namespace blender::bke::curves {

/* -------------------------------------------------------------------- */
/** \name Queries
 * \{ */

bool has_hidden_elements(const CurvesGeometry &curves)
{
  const AttributeAccessor attributes = curves.attributes();
  const VArray<bool> hide_point = *attributes.lookup<bool>(".hide_point", AttrDomain::Point);
  if (!hide_point.is_empty() && array_utils::contains(hide_point, hide_point.index_range(), true))
  {
    return true;
  }
  const VArray<bool> hide_curve = *attributes.lookup<bool>(".hide_curve", AttrDomain::Curve);
  return !hide_curve.is_empty() &&
         array_utils::contains(hide_curve, hide_curve.index_range(), true);
}

IndexMask visible_mask(const CurvesGeometry &curves,
                       const AttrDomain domain,
                       IndexMaskMemory &memory)
{
  const AttributeAccessor attributes = curves.attributes();
  const IndexRange domain_range(attributes.domain_size(domain));

  StringRef attribute_name;
  switch (domain) {
    case AttrDomain::Point:
      attribute_name = ".hide_point";
      break;
    case AttrDomain::Curve:
      attribute_name = ".hide_curve";
      break;
    default:
      return domain_range;
  }

  const VArray<bool> hide = *attributes.lookup<bool>(attribute_name, domain);
  if (hide.is_empty()) {
    return domain_range;
  }
  return IndexMask::from_bools_inverse(hide, memory);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Domain Flushing
 * \{ */

/**
 * Following the mesh convention, a missing attribute means "nothing hidden". Drop both attributes
 * so that later lookups can take their early-out paths.
 *
 * \return True when everything is visible and both attributes were removed. The caller must not
 * hold a #VArray of either attribute across this call.
 */
static bool remove_hide_attributes_if_visible(MutableAttributeAccessor &attributes,
                                              const StringRef name,
                                              const AttrDomain domain)
{
  {
    const VArray<bool> hide = *attributes.lookup<bool>(name, domain);
    if (!hide.is_empty() && array_utils::contains(hide, hide.index_range(), true)) {
      return false;
    }
  }
  attributes.remove(".hide_point");
  attributes.remove(".hide_curve");
  return true;
}

void hide_point_flush(CurvesGeometry &curves)
{
  MutableAttributeAccessor attributes = curves.attributes_for_write();
  if (remove_hide_attributes_if_visible(attributes, ".hide_point", AttrDomain::Point)) {
    return;
  }

  const VArraySpan<bool> hide_point_span(
      *attributes.lookup<bool>(".hide_point", AttrDomain::Point));
  const OffsetIndices points_by_curve = curves.points_by_curve();
  SpanAttributeWriter<bool> hide_curve = attributes.lookup_or_add_for_write_only_span<bool>(
      ".hide_curve", AttrDomain::Curve);

  threading::parallel_for(curves.curves_range(), 4096, [&](const IndexRange range) {
    for (const int curve : range) {
      const IndexRange points = points_by_curve[curve];
      hide_curve.span[curve] = !points.is_empty() &&
                               !hide_point_span.slice(points).contains(false);
    }
  });

  hide_curve.finish();
}

void hide_curve_flush(CurvesGeometry &curves)
{
  MutableAttributeAccessor attributes = curves.attributes_for_write();
  if (remove_hide_attributes_if_visible(attributes, ".hide_curve", AttrDomain::Curve)) {
    return;
  }

  const VArraySpan<bool> hide_curve_span(
      *attributes.lookup<bool>(".hide_curve", AttrDomain::Curve));
  const OffsetIndices points_by_curve = curves.points_by_curve();
  SpanAttributeWriter<bool> hide_point = attributes.lookup_or_add_for_write_only_span<bool>(
      ".hide_point", AttrDomain::Point);

  threading::parallel_for(curves.curves_range(), 4096, [&](const IndexRange range) {
    for (const int curve : range) {
      hide_point.span.slice(points_by_curve[curve]).fill(hide_curve_span[curve]);
    }
  });

  hide_point.finish();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Hide and Show
 * \{ */

void hide_points(CurvesGeometry &curves, const IndexMask &mask, const bool hide)
{
  MutableAttributeAccessor attributes = curves.attributes_for_write();
  SpanAttributeWriter<bool> hide_point = attributes.lookup_or_add_for_write_span<bool>(
      ".hide_point", AttrDomain::Point);
  index_mask::masked_fill(hide_point.span, hide, mask);
  hide_point.finish();

  hide_point_flush(curves);
}

void hide_curves(CurvesGeometry &curves, const IndexMask &mask, const bool hide)
{
  MutableAttributeAccessor attributes = curves.attributes_for_write();
  SpanAttributeWriter<bool> hide_curve = attributes.lookup_or_add_for_write_span<bool>(
      ".hide_curve", AttrDomain::Curve);
  index_mask::masked_fill(hide_curve.span, hide, mask);
  hide_curve.finish();

  hide_curve_flush(curves);
}

bool show_all(CurvesGeometry &curves)
{
  MutableAttributeAccessor attributes = curves.attributes_for_write();
  const bool had_hidden = attributes.contains(".hide_point") || attributes.contains(".hide_curve");
  attributes.remove(".hide_point");
  attributes.remove(".hide_curve");
  return had_hidden;
}

/** \} */

}  // namespace blender::bke::curves
