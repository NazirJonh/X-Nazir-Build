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
  
  printf("[DEBUG] Curves Weight Draw Cache: Populating buffers for %d points\n", points_num);
  
  if (points_num == 0) {
    printf("[DEBUG] Curves Weight Draw Cache: No points to process\n");
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
  
  printf("[DEBUG] Curves Weight Draw Cache: Active vertex group (1-based): %d\n",
         active_group_1based);
  
  /* Check if object has vertex groups */
  const ListBase *defbase = BKE_object_defgroup_list(object_for_defgroups);
  const int vgroup_count = BLI_listbase_count(defbase);
  printf("[DEBUG] Curves Weight Draw Cache: Defgroup source='%s' (orig_of='%s') count=%d "
         "active_1based=%d active_0based=%d\n",
         object_for_defgroups ? object_for_defgroups->id.name + 2 : "NULL",
         object ? object->id.name + 2 : "NULL",
         vgroup_count,
         active_group_1based,
         active_group);

  if (active_group >= 0) {
    const bDeformGroup *active_group_ref = static_cast<const bDeformGroup *>(
        BLI_findlink(defbase, active_group));
    printf("[DEBUG] Curves Weight Draw Cache: Active group lookup by index=%d -> %s\n",
           active_group,
           active_group_ref ? active_group_ref->name : "<null>");
  }

  if (!BLI_listbase_is_empty(defbase) && active_group_1based > 0) {
    printf("[DEBUG] Curves Weight Draw Cache: Using vertex group %d weights\n",
           active_group_1based);
    /* Get actual vertex group weights */
    const Span<MDeformVert> dverts = curves_geometry.deform_verts();
    if (!dverts.is_empty()) {
      int non_zero_weights = 0;
      for (const int i : dverts.index_range()) {
        const MDeformWeight *dw = BKE_defvert_find_index(&dverts[i], active_group);
        weights[i] = dw ? dw->weight : 0.0f;
        if (weights[i] > 0.0f) {
          non_zero_weights++;
        }
      }
      printf("[DEBUG] Curves Weight Draw Cache: Found %d non-zero weights out of %d points\n", 
             non_zero_weights, points_num);
    }
  }
  else {
    printf("[DEBUG] Curves Weight Draw Cache: No vertex groups or no active group, using zero weights\n");
  }
  /* If no vertex groups or no active group, weights remain 0.0f (blue color) */
  
  /* Calculate tangents for each point */
  Array<float3> tangents(points_num);
  const OffsetIndices<int> points_by_curve = curves_geometry.points_by_curve();
  
  for (const int curve_i : curves_geometry.curves_range()) {
    const IndexRange points = points_by_curve[curve_i];
    
    for (const int point_i : points) {
      float3 tangent = float3(0.0f);
      
      if (points.size() > 1) {
        if (point_i == points.first()) {
          /* First point: use direction to next point */
          tangent = math::normalize(positions[point_i + 1] - positions[point_i]);
        }
        else if (point_i == points.last()) {
          /* Last point: use direction from previous point */
          tangent = math::normalize(positions[point_i] - positions[point_i - 1]);
        }
        else {
          /* Middle point: use average of directions */
          const float3 prev_dir = math::normalize(positions[point_i] - positions[point_i - 1]);
          const float3 next_dir = math::normalize(positions[point_i + 1] - positions[point_i]);
          tangent = math::normalize(prev_dir + next_dir);
        }
      }
      else {
        /* Single point curve: default tangent */
        tangent = float3(1.0f, 0.0f, 0.0f);
      }
      
      tangents[point_i] = tangent;
    }
  }
  
  /* Fill vertex buffer data */
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
  
  /* Create index buffers for points */
  GPUIndexBufBuilder points_builder;
  GPU_indexbuf_init(&points_builder, GPU_PRIM_POINTS, points_num, points_num);
  
  for (int i = 0; i < points_num; i++) {
    GPU_indexbuf_add_point_vert(&points_builder, i);
  }
  
  cache.weight_points_indices = GPU_indexbuf_build(&points_builder);
  
  /* Create index buffers for lines */
  int total_line_segments = 0;
  for (const int curve_i : curves_geometry.curves_range()) {
    const IndexRange points = points_by_curve[curve_i];
    if (points.size() > 1) {
      total_line_segments += points.size() - 1;
    }
  }
  
  if (total_line_segments > 0) {
    GPUIndexBufBuilder lines_builder;
    GPU_indexbuf_init(&lines_builder, GPU_PRIM_LINES, total_line_segments * 2, points_num);
    
    for (const int curve_i : curves_geometry.curves_range()) {
      const IndexRange points = points_by_curve[curve_i];
      
      for (const int i : IndexRange(points.size() - 1)) {
        GPU_indexbuf_add_line_verts(&lines_builder, points[i], points[i + 1]);
      }
    }
    
    cache.weight_lines_indices = GPU_indexbuf_build(&lines_builder);
  }
  
  /* Create batches */
  if (cache.weight_points_indices) {
    cache.weight_points = GPU_batch_create(GPU_PRIM_POINTS, cache.weight_points_pos, cache.weight_points_indices);
    printf("[DEBUG] Curves Weight Draw Cache: Created points batch with %d points\n", points_num);
  }
  
  if (cache.weight_lines_indices) {
    cache.weight_lines = GPU_batch_create(GPU_PRIM_LINES, cache.weight_points_pos, cache.weight_lines_indices);
    printf("[DEBUG] Curves Weight Draw Cache: Created lines batch with %d line segments\n", total_line_segments);
  }
  
  /* Allow creation of buffer texture */
  GPU_vertbuf_use(cache.weight_points_pos);
  
  printf("[DEBUG] Curves Weight Draw Cache: Buffer population completed successfully\n");
}

gpu::Batch *DRW_cache_curves_weight_points_get(Object *object)
{
  printf("[DEBUG] Curves Weight Draw Cache: Getting points batch for object '%s'\n", 
         object ? object->id.name + 2 : "NULL");
  
  if (!object || object->type != OB_CURVES) {
    printf("[DEBUG] Curves Weight Draw Cache: Invalid object or not curves type\n");
    return nullptr;
  }
  
  const Curves *curves = id_cast<const Curves *>(object->data);
  
  /* Get the batch cache - this will ensure it's valid */
  CurvesBatchCache &cache = get_batch_cache(*const_cast<Curves *>(curves));
  
  /* Check if weight paint batches need to be created */
  if (cache.weight_points == nullptr) {
    printf("[DEBUG] Curves Weight Draw Cache: Creating weight paint batches\n");
    curves_weight_batch_populate(object, curves, cache);
  }
  
  if (!cache.weight_points) {
    printf("[DEBUG] Curves Weight Draw Cache: No points batch available\n");
    return nullptr;
  }
  
  printf("[DEBUG] Curves Weight Draw Cache: Returning points batch\n");
  return cache.weight_points;
}

gpu::Batch *DRW_cache_curves_weight_lines_get(Object *object)
{
  printf("[DEBUG] Curves Weight Draw Cache: Getting lines batch for object '%s'\n", 
         object ? object->id.name + 2 : "NULL");
  
  if (!object || object->type != OB_CURVES) {
    printf("[DEBUG] Curves Weight Draw Cache: Invalid object or not curves type\n");
    return nullptr;
  }
  
  const Curves *curves = id_cast<const Curves *>(object->data);
  
  /* Get the batch cache - this will ensure it's valid */
  CurvesBatchCache &cache = get_batch_cache(*const_cast<Curves *>(curves));
  
  /* Check if weight paint batches need to be created */
  if (cache.weight_lines == nullptr) {
    printf("[DEBUG] Curves Weight Draw Cache: Creating weight paint batches\n");
    curves_weight_batch_populate(object, curves, cache);
  }
  
  if (!cache.weight_lines) {
    printf("[DEBUG] Curves Weight Draw Cache: No lines batch available\n");
    return nullptr;
  }
  
  printf("[DEBUG] Curves Weight Draw Cache: Returning lines batch\n");
  return cache.weight_lines;
}

}  // namespace blender::draw
