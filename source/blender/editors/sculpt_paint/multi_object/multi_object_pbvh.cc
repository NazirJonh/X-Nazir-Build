/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "multi_object_pbvh.hh"

#include "BKE_curves.hh"
#include "BKE_lib_id.hh"
#include "BKE_object.hh"

#include "BLI_index_range.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector.hh"

#include "DNA_curves_types.h"
#include "DNA_object_types.h"

namespace blender::ed::sculpt_paint::multi_object::pbvh {

bool Manager::initialize(const Span<Object *> objects)
{
  clear();

  for (Object *ob : objects) {
    if (!ob || ob->type != OB_CURVES) {
      continue;
    }

    Curves *curves_id = id_cast<Curves *>(ob->data);
    if (!curves_id) {
      continue;
    }

    ObjectData data;
    data.object = ob;

    const int index = objects_.size();
    objects_.append(data);
    object_to_index_.add(ob, index);
  }

  return !objects_.is_empty();
}

void Manager::find_affected_nodes(const float3 &brush_pos_world, float radius)
{
  for (ObjectData &data : objects_) {
    data.affected_curve_indices.clear();
  }

  const float radius_sq = radius * radius;

  for (ObjectData &data : objects_) {
    Curves *curves_id = id_cast<Curves *>(data.object->data);
    bke::CurvesGeometry &curves = curves_id->geometry.wrap();

    const float4x4 world_to_local = math::invert(float4x4(data.object->object_to_world().ptr()));
    const float3 local_brush_pos = math::transform_point(world_to_local, brush_pos_world);

    const Span<float3> positions = curves.positions();
    const OffsetIndices points_by_curve = curves.points_by_curve();

    for (const int curve_i : curves.curves_range()) {
      const IndexRange points = points_by_curve[curve_i];
      if (points.is_empty()) {
        continue;
      }

      const float3 &root_pos = positions[points.first()];
      const float dist_sq = math::distance_squared(root_pos, local_brush_pos);

      if (dist_sq <= radius_sq) {
        data.affected_curve_indices.append(curve_i);
      }
    }
  }
}

void Manager::update_all_bounds()
{
  for (ObjectData &data : objects_) {
    if (data.affected_curve_indices.is_empty()) {
      continue;
    }

    Curves *curves_id = id_cast<Curves *>(data.object->data);
    bke::CurvesGeometry &curves = curves_id->geometry.wrap();
    curves.tag_positions_changed();
  }
}

void Manager::store_all_bounds_orig()
{
  for (ObjectData &data : objects_) {
    Curves *curves_id = id_cast<Curves *>(data.object->data);
    bke::CurvesGeometry &curves = curves_id->geometry.wrap();
    curves.bounds_min_max();
  }
}

void Manager::clear()
{
  objects_.clear();
  object_to_index_.clear();
}

}  // namespace blender::ed::sculpt_paint::multi_object::pbvh
