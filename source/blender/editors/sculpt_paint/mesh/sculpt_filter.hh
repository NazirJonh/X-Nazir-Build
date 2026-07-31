/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#pragma once

#include <array>

#include "BLI_array.hh"
#include "BLI_index_mask.hh"

#include "ED_view3d.hh"

namespace blender {

struct wmOperatorType;

namespace bke::pbvh {
class Node;
}
namespace ed::sculpt_paint {
enum class TransformDisplacementMode;
namespace auto_mask {
struct Cache;
}
namespace cloth {
struct SimulationData;
}
namespace undo {
enum class Type : int8_t;
enum class NodeDataFlag : uint8_t;
}  // namespace undo
}  // namespace ed::sculpt_paint

namespace ed::sculpt_paint::filter {

enum class FilterOrientation {
  Local = 0,
  World = 1,
  View = 2,
};

struct Cache {
  std::array<bool, 3> enabled_axis;
  int random_seed;

  /* Used for alternating between filter operations in filters that need to apply different ones to
   * achieve certain effects. */
  int iteration_count;

  /* Stores the displacement produced by the laplacian step of HC smooth. */
  Array<float3> surface_smooth_laplacian_disp;
  float surface_smooth_shape_preservation;
  float surface_smooth_current_vertex;

  /* Sharpen mesh filter. */
  float sharpen_smooth_ratio;
  float sharpen_intensify_detail_strength;
  int sharpen_curvature_smooth_iterations;
  Array<float> sharpen_factor;
  Array<float3> detail_directions;

  /* Filter orientation. */
  FilterOrientation orientation;
  float4x4 obmat;
  float4x4 obmat_inv;
  float4x4 viewmat;
  float4x4 viewmat_inv;

  /* Maps this object's local space into the reference (active) object's local space, used by
   * #FilterOrientation::Local. This is what makes a multi-object filter deform along ONE set of
   * axes, as if every mesh were joined into the active object, rather than along each mesh's own
   * axes. Identity for the active object and for single-object filters, so #World and #View --
   * which already go through world space and are therefore consistent across objects -- are
   * unaffected. */
  float3x3 local_orientation_mat = float3x3::identity();
  float3x3 local_orientation_mat_inv = float3x3::identity();

  /* Displacement eraser. */
  Array<float3> limit_surface_co;

  /* unmasked nodes */
  IndexMaskMemory node_mask_memory;
  IndexMask node_mask;

  /* Cloth filter. */
  std::unique_ptr<cloth::SimulationData> cloth_sim;
  float3 cloth_sim_pinch_point;

  /* mask expand iteration caches */
  int mask_update_current_it;
  int mask_update_last_it;
  Array<int> mask_update_it;
  Array<float> normal_factor;
  Array<float> edge_factor;
  Array<float> prev_mask;
  float3 mask_expand_initial_co;

  int new_face_set;
  Array<int> prev_face_set;

  int active_face_set;

  TransformDisplacementMode transform_displacement_mode;

  std::unique_ptr<auto_mask::Cache> automasking;
  float3 initial_normal;
  float3 view_normal;

  /* Pre-smoothed colors used by sharpening. Colors are HSL. */
  Array<float4> pre_smoothed_color;

  ViewContext vc;
  float start_filter_strength;
  bool has_dragged;

  ~Cache();
};

void cache_init(bContext *C,
                Object &ob,
                Sculpt &sd,
                undo::NodeDataFlag undo_flags,
                const float mval_fl[2],
                float area_normal_radius,
                float start_strength);
void register_operator_props(wmOperatorType *ot);

/* Filter orientation utils. */
float3x3 to_orientation_space(const filter::Cache &filter_cache);
float3x3 to_object_space(const filter::Cache &filter_cache);
void zero_disabled_axis_components(const filter::Cache &filter_cache, MutableSpan<float3> vectors);
}  // namespace ed::sculpt_paint::filter

}  // namespace blender
