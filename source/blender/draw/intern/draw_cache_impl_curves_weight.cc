/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 *
 * \brief Curves Weight Paint API for render engines
 */

#include "draw_cache_impl_curves_weight.hh"
#include "draw_cache_impl_curves_private.hh"

#include "BKE_attribute.hh"
#include "BKE_curves.hh"
#include "BKE_lib_id.hh"
#include "BKE_paint.hh"
#include "BKE_deform.hh"

#include "BLI_array_utils.hh"
#include "BLI_listbase.h"
#include "BLI_math_vector.hh"
#include "BLI_offset_indices.hh"
#include "BLI_task.hh"

#include "DEG_depsgraph_query.hh"

#include "DNA_curves_types.h"
#include "DNA_object_types.h"

#include "DRW_render.hh"

#include "GPU_batch.hh"
#include "GPU_index_buffer.hh"
#include "GPU_vertex_buffer.hh"

namespace blender::draw {

/* Vertex format for curves weight paint points */
static const GPUVertFormat *curves_weight_point_format()
{
  static const GPUVertFormat format = []() {
    GPUVertFormat format{};
    GPU_vertformat_attr_add(&format, "weight", gpu::VertAttrType::SFLOAT_32);
    GPU_vertformat_attr_add(&format, "pos", gpu::VertAttrType::SFLOAT_32_32_32);
    GPU_vertformat_attr_add(&format, "tangent", gpu::VertAttrType::SFLOAT_32_32_32);
    return format;
  }();
  return &format;
}

static void curves_weight_batch_populate(Object *object,
                                         const Curves *curves,
                                         CurvesBatchCache &cache)
{
  const bke::CurvesGeometry &curves_geometry = curves->geometry.wrap();
  const int points_num = curves_geometry.points_num();

  if (points_num == 0) {
    return;
  }

  /* Create vertex buffer */
  cache.weight_points_pos = GPU_vertbuf_create_with_format(*curves_weight_point_format());
  GPU_vertbuf_data_alloc(*cache.weight_points_pos, points_num);

  /* Get curve positions */
  const Span<float3> positions = curves_geometry.positions();

  /* Vertex groups are stored on original object data, evaluated object can have empty defbase. */
  const Object *object_for_defgroups = DEG_get_original(object);
  if (object_for_defgroups == nullptr) {
    object_for_defgroups = object;
  }

  /* Active vertex group index is stored as 1-based (0 means "no active group"). */
  const int active_group_1based = BKE_object_defgroup_active_index_get(object_for_defgroups);
  const int active_group = active_group_1based - 1;
  Array<float> weights(points_num, 0.0f);

  /* Check if object has vertex groups */
  const ListBase *defbase = BKE_object_defgroup_list(object_for_defgroups);

  if (!BLI_listbase_is_empty(defbase) && active_group_1based > 0) {
    /* Get actual vertex group weights */
    const Span<MDeformVert> dverts = curves_geometry.deform_verts();
    if (!dverts.is_empty()) {
      for (const int i : dverts.index_range()) {
        const MDeformWeight *dw = BKE_defvert_find_index(&dverts[i], active_group);
        weights[i] = dw ? dw->weight : 0.0f;
      }
    }
  }
  /* If no vertex groups or no active group, weights remain 0.0f (blue color). */

  /* Calculate tangents for each point. */
  Array<float3> tangents(points_num);
  curves_paint_compute_tangents(curves_geometry, tangents);

  /* Fill vertex buffer data. */
  struct WeightPointVert {
    float weight;
    float3 pos;
    float3 tangent;
  };

  MutableSpan<WeightPointVert> verts = cache.weight_points_pos->data<WeightPointVert>();

  for (const int i : positions.index_range()) {
    verts[i].pos = positions[i];
    verts[i].weight = weights[i];
    verts[i].tangent = tangents[i];
  }

  curves_paint_build_point_and_line_batches(curves_geometry,
                                            cache.weight_points_pos,
                                            &cache.weight_points_indices,
                                            &cache.weight_lines_indices,
                                            &cache.weight_points,
                                            &cache.weight_lines);
}

static gpu::Batch *curves_weight_batch_get(Object *object, const bool lines)
{
  if (!object || object->type != OB_CURVES) {
    return nullptr;
  }

  const Curves *curves = id_cast<const Curves *>(object->data);
  CurvesBatchCache &cache = get_batch_cache(*const_cast<Curves *>(curves));

  /* Populate only when neither batch exists yet. A single populate builds both the points and
   * lines batches, so guarding on both avoids re-running it (and leaking the previously created
   * GPU buffers) when the requested batch stays null for single-point curves. */
  gpu::Batch **batch = lines ? &cache.weight_lines : &cache.weight_points;
  gpu::Batch **other_batch = lines ? &cache.weight_points : &cache.weight_lines;

  if (*batch == nullptr && *other_batch == nullptr) {
    curves_weight_batch_populate(object, curves, cache);
  }

  return *batch;
}

gpu::Batch *DRW_cache_curves_weight_points_get(Object *object)
{
  return curves_weight_batch_get(object, false);
}

gpu::Batch *DRW_cache_curves_weight_lines_get(Object *object)
{
  return curves_weight_batch_get(object, true);
}

}  // namespace blender::draw
