/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_curves.hh"
#include "BKE_brush.hh"

#include "sculpt_intern.hh"

/* Debug throttling counter */
static int debug_print_counter = 0;
#define DEBUG_PRINT_INTERVAL 60
#define DEBUG_PRINTF(...) \
  do { \
    debug_print_counter++; \
    if (debug_print_counter % DEBUG_PRINT_INTERVAL == 1) { \
      printf(__VA_ARGS__); \
    } \
  } while (0)

namespace blender::ed::sculpt_paint {

using bke::AttrDomain;
using bke::AttrType;

bke::SpanAttributeWriter<float> float_selection_ensure(Curves &curves_id)
{
  /* TODO: Use a generic attribute conversion utility instead of this function. */
  bke::CurvesGeometry &curves = curves_id.geometry.wrap();
  bke::MutableAttributeAccessor attributes = curves.attributes_for_write();

  if (const auto meta_data = attributes.lookup_meta_data(".selection")) {
    if (meta_data->data_type == AttrType::Bool) {
      const VArray<float> selection = *attributes.lookup<float>(".selection");
      float *dst = MEM_new_array_uninitialized<float>(selection.size(), __func__);
      selection.materialize({dst, selection.size()});

      attributes.remove(".selection");
      attributes.add(
          ".selection", meta_data->domain, AttrType::Float, bke::AttributeInitMoveArray(dst));
    }
  }
  else {
    attributes.add(".selection",
                   AttrDomain(curves_id.selection_domain),
                   AttrType::Float,
                   bke::AttributeInitValue(1.0f));
  }

  return curves.attributes_for_write().lookup_for_write_span<float>(".selection");
}

bke::SpanAttributeWriter<float> brush_highlight_ensure(Curves &curves_id)
{
  bke::CurvesGeometry &curves = curves_id.geometry.wrap();
  DEBUG_PRINTF("[BrushHighlight] brush_highlight_ensure called, points=%d\n", curves.points_num());
  return curves.attributes_for_write().lookup_or_add_for_write_span<float>(
      ".brush_highlight", AttrDomain::Point);
}

void clear_brush_highlight(Curves &curves_id)
{
  bke::CurvesGeometry &curves = curves_id.geometry.wrap();
  if (curves.is_empty()) {
    return;
  }

  bke::SpanAttributeWriter<float> brush_highlight = brush_highlight_ensure(curves_id);
  brush_highlight.span.fill(0.0f);
  brush_highlight.finish();

  DEBUG_PRINTF("[BrushHighlight] clear_brush_highlight: cleared %d points\n", curves.points_num());
}

void update_brush_highlight(const CurvesGeometry &curves,
                            const Curves &curves_id,
                            const Span<float3> positions,
                            const IndexMask &point_mask,
                            const float3 &brush_pos_cu,
                            const float brush_radius_cu,
                            const Brush *brush,
                            const float strength,
                            MutableSpan<float> highlight)
{
  printf("[BrushHighlight] update_brush_highlight:\n");
  printf("  brush_pos_cu: (%.3f, %.3f, %.3f)\n", brush_pos_cu.x, brush_pos_cu.y, brush_pos_cu.z);
  printf("  brush_radius_cu: %.3f, strength: %.3f\n", brush_radius_cu, strength);
  printf("  point_mask size: %zu, positions size: %zu\n", point_mask.size(), positions.size());

  if (!positions.is_empty()) {
    printf("  positions[0]: (%.3f, %.3f, %.3f)\n", positions[0].x, positions[0].y, positions[0].z);
  }

  const AttrDomain domain = AttrDomain(curves_id.selection_domain);
  const float brush_radius_sq_cu = math::square(brush_radius_cu);

  int points_inside = 0;
  float max_highlight = 0.0f;

  if (domain == AttrDomain::Point) {
    point_mask.foreach_index([&](const int point_i) {
      const float distance_to_brush_sq_cu = math::distance_squared(positions[point_i], brush_pos_cu);
      if (distance_to_brush_sq_cu > brush_radius_sq_cu) {
        highlight[point_i] = 0.0f;
        return;
      }

      const float distance_to_brush_cu = math::sqrt(distance_to_brush_sq_cu);
      const float radius_falloff = BKE_brush_curve_strength(
          brush, distance_to_brush_cu, brush_radius_cu);
      highlight[point_i] = radius_falloff * strength;
      points_inside++;
      max_highlight = std::max(max_highlight, highlight[point_i]);
    });
  }
  else if (domain == AttrDomain::Curve) {
    const OffsetIndices points_by_curve = curves.points_by_curve();

    for (const int curve_i : curves.curves_range()) {
      const IndexRange points = points_by_curve[curve_i];
      bool any_point_in_brush = false;
      float max_falloff = 0.0f;

      for (const int point_i : points) {
        const float distance_to_brush_sq_cu = math::distance_squared(positions[point_i], brush_pos_cu);
        if (distance_to_brush_sq_cu <= brush_radius_sq_cu) {
          const float distance_to_brush_cu = math::sqrt(distance_to_brush_sq_cu);
          const float radius_falloff = BKE_brush_curve_strength(
              brush, distance_to_brush_cu, brush_radius_cu);
          any_point_in_brush = true;
          max_falloff = std::max(max_falloff, radius_falloff);
        }
      }

      if (any_point_in_brush) {
        for (const int point_i : points) {
          highlight[point_i] = max_falloff * strength;
          points_inside++;
          max_highlight = std::max(max_highlight, highlight[point_i]);
        }
      }
      else {
        for (const int point_i : points) {
          highlight[point_i] = 0.0f;
        }
      }
    }
  }

  printf("  points_inside_brush: %d, max_highlight: %.3f\n", points_inside, max_highlight);
}

}  // namespace blender::ed::sculpt_paint
