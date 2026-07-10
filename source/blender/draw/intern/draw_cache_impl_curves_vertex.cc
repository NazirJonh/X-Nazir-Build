/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 *
 * \brief Curves Vertex Paint API for render engines
 */

#include "draw_cache_impl_curves_vertex.hh"
#include "draw_cache_impl_curves_private.hh"

#include "BKE_attribute.hh"
#include "BKE_curves.hh"
#include "BKE_lib_id.hh"

#include "BLI_array.hh"
#include "BLI_color_types.hh"
#include "BLI_math_vector.hh"
#include "BLI_offset_indices.hh"

#include "DNA_curves_types.h"
#include "DNA_object_types.h"

#include "GPU_batch.hh"
#include "GPU_index_buffer.hh"
#include "GPU_vertex_buffer.hh"

namespace blender::draw {

static constexpr const char *vertex_color_attr_name = "vertex_color";

static const GPUVertFormat *curves_vertex_paint_point_format()
{
  static const GPUVertFormat format = []() {
    GPUVertFormat format{};
    GPU_vertformat_attr_add(&format, "vert_color", gpu::VertAttrType::SFLOAT_32_32_32_32);
    GPU_vertformat_attr_add(&format, "pos", gpu::VertAttrType::SFLOAT_32_32_32);
    GPU_vertformat_attr_add(&format, "tangent", gpu::VertAttrType::SFLOAT_32_32_32);
    return format;
  }();
  return &format;
}

static void curves_vertex_paint_batch_populate(const Curves *curves, CurvesBatchCache &cache)
{
  const bke::CurvesGeometry &curves_geometry = curves->geometry.wrap();
  const int points_num = curves_geometry.points_num();

  if (points_num == 0) {
    return;
  }

  cache.vertex_paint_points_pos = GPU_vertbuf_create_with_format(*curves_vertex_paint_point_format());
  GPU_vertbuf_data_alloc(*cache.vertex_paint_points_pos, points_num);

  const Span<float3> positions = curves_geometry.positions();

  const VArray<ColorGeometry4f> colors = *curves_geometry.attributes().lookup_or_default<ColorGeometry4f>(
      vertex_color_attr_name, bke::AttrDomain::Point, ColorGeometry4f(1.0f, 1.0f, 1.0f, 1.0f));

  Array<float3> tangents(points_num);
  curves_paint_compute_tangents(curves_geometry, tangents);

  struct VertexPaintPointVert {
    ColorGeometry4f color;
    float3 pos;
    float3 tangent;
  };

  MutableSpan<VertexPaintPointVert> verts =
      cache.vertex_paint_points_pos->data<VertexPaintPointVert>();

  for (const int i : positions.index_range()) {
    verts[i].pos = positions[i];
    verts[i].color = colors[i];
    verts[i].tangent = tangents[i];
  }

  curves_paint_build_point_and_line_batches(curves_geometry,
                                            cache.vertex_paint_points_pos,
                                            &cache.vertex_paint_points_indices,
                                            &cache.vertex_paint_lines_indices,
                                            &cache.vertex_paint_points,
                                            &cache.vertex_paint_lines);
}

static gpu::Batch *curves_vertex_paint_batch_get(Object *object, const bool lines)
{
  if (!object || object->type != OB_CURVES) {
    return nullptr;
  }

  const Curves *curves = id_cast<const Curves *>(object->data);
  CurvesBatchCache &cache = get_batch_cache(*const_cast<Curves *>(curves));

  gpu::Batch **batch = lines ? &cache.vertex_paint_lines : &cache.vertex_paint_points;
  gpu::Batch **other_batch = lines ? &cache.vertex_paint_points : &cache.vertex_paint_lines;

  if (*batch == nullptr && *other_batch == nullptr) {
    curves_vertex_paint_batch_populate(curves, cache);
  }

  return *batch;
}

gpu::Batch *DRW_cache_curves_vertex_paint_points_get(Object *object)
{
  return curves_vertex_paint_batch_get(object, false);
}

gpu::Batch *DRW_cache_curves_vertex_paint_lines_get(Object *object)
{
  return curves_vertex_paint_batch_get(object, true);
}

}  // namespace blender::draw
