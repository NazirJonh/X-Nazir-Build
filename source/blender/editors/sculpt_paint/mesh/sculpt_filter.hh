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

#include "paint_face_selection_mask.hh"

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

/**
 * Proportional-edit falloff parameters for the Transform tool's cursor deform, copied from the
 * generic Transform system (#TransInfo::prop_size/#prop_mode) every modal step. Kept here so the
 * sculpt vertex math does not have to include the transform headers.
 */
struct TransformProportional {
  bool enabled = false;
  /** Falloff radius, in world units. */
  float radius = 1.0f;
  /** #eProportionalFalloff. */
  int falloff = 0;
  /** Measure distance in the view plane instead of in world space. */
  bool projected = false;
  /** World-space view direction, used when #projected is set. */
  float3 view_normal = float3(0.0f, 0.0f, 1.0f);

  /**
   * Per-vertex falloff distances, computed once from the original positions on the first
   * proportional step and reused for every later one: they only depend on the mask, the pivot and
   * (for #projected) the view, none of which change during a drag, while the radius and falloff
   * type are applied per step. Rebuilt when #projected or #view_normal change.
   */
  bool distances_valid = false;
  bool distances_projected = false;
  float3 distances_view_normal = float3(0.0f);
  /**
   * World-space distance from the pivot per vertex (#vertex_count_get indexing), `FLT_MAX` for
   * fully masked or hidden vertices.
   */
  Array<float> vert_dist;
  /**
   * World-space distance to the nearest fully masked or hidden vertex, used to fade the mask weight
   * out before the mask border. Empty when nothing is masked or hidden.
   */
  Array<float> vert_mask_dist;
  /** Per PBVH node: the smallest distance at which any vertex of the node can still move. */
  Array<float> node_min_dist;
  /**
   * Nodes deformed on the previous step. They have to be processed once more after leaving the
   * radius, since the undo restore moves them back without updating their bounds or draw data.
   * Empty means every node was deformed (proportional editing was off on the previous step).
   */
  Array<bool> prev_nodes;
};

struct Cache {
  std::array<bool, 3> enabled_axis;
  int random_seed;

  /** Face selection masking state, built lazily during the filter (see
   * #face_selection_mask_ensure). */
  FaceSelectionMask face_selection_mask;

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
  TransformProportional proportional;

  std::unique_ptr<auto_mask::Cache> automasking;
  float3 initial_normal;
  float3 view_normal;

  /* Pre-smoothed colors used by sharpening. Colors are HSL. */
  Array<float4> pre_smoothed_color;

  ViewContext vc;
  float start_filter_strength;
  bool has_dragged;

  /* Declared (not defaulted inline) because #cloth_sim and #automasking hold pointers to types
   * that are only forward-declared here: an implicit constructor/destructor would need those
   * types complete at every call site (see #MEM_new / #MEM_delete users of this type). Defined
   * out-of-line in `sculpt_filter_mesh.cc`, which includes both. */
  Cache();
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
