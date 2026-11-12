/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * The planar mode of the projection frame gizmo, for the rows that read a flat frame: a thin box
 * cage in the frame plane, one roll ring, a depth arrow, a direction arrow (flip), and a move ring
 * that drags the frame along the painted surface.
 */

#include <algorithm>
#include <cmath>
#include <cstring>

#include "BLI_array.hh"
#include "BLI_index_range.hh"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector_types.hh"

#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_view3d_types.h"

#include "BKE_context.hh"
#include "BKE_object.hh"
#include "BKE_paint_layers.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "ED_gizmo_library.hh"

#include "RNA_access.hh"

#include "UI_resources.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "projection_gizmo_intern.hh"

namespace blender::ed::sculpt_paint::projection {

/* -------------------------------------------------------------------- */
/** \name Surface height (planar display)
 * \{ */

/**
 * The most protruding point of the painted surface along the frame's +Z, within its XY footprint.
 * Vertices inside the footprint count directly; the surface under a few footprint samples is added
 * so a coarse mesh (a large flat face with no vertex inside the frame) still gives its height.
 * Zero when the object has no evaluated mesh or nothing lies under the frame.
 */
static float projection_surface_height(const bContext *C,
                                       ProjectionGizmoGroup &ggd,
                                       const Object &space_ob,
                                       const ProjectionFrame &frame,
                                       const float rot[3][3])
{
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  Object *painted = ggd.target.object();
  if (depsgraph == nullptr || painted == nullptr) {
    return 0.0f;
  }
  const Object *painted_eval = DEG_get_evaluated(depsgraph, painted);
  const Object *space_eval = DEG_get_evaluated(depsgraph, &space_ob);
  if (painted_eval == nullptr || space_eval == nullptr || painted_eval->type != OB_MESH) {
    return 0.0f;
  }
  const Mesh *mesh = BKE_object_get_evaluated_mesh(painted_eval);
  if (mesh == nullptr) {
    return 0.0f;
  }

  float frame_basis[4][4];
  projection_frame_basis(rot, frame.center, frame_basis);
  const float4x4 frame_to_space(frame_basis);
  const float4x4 frame_to_world = space_eval->object_to_world() * frame_to_space;
  const float4x4 painted_to_frame = math::invert(frame_to_world) * painted_eval->object_to_world();
  const float half_x = fabsf(frame.size[0]) * 0.5f;
  const float half_y = fabsf(frame.size[1]) * 0.5f;

  float key[PROJECTION_SURFACE_KEY_SIZE];
  memcpy(key, painted_to_frame.ptr(), sizeof(float) * 16);
  key[16] = half_x;
  key[17] = half_y;
  key[18] = float(mesh->verts_num);
  /* The evaluated mesh's identity: a sculpt edit moves vertices while nothing in the transform
   * key changes, and a copy-on-write update gives the edited mesh its own block. */
  memcpy(&key[19], &mesh, sizeof(mesh));
  if (ggd.surface_key_valid && memcmp(key, ggd.surface_key, sizeof(key)) == 0) {
    return ggd.surface_height;
  }
  memcpy(ggd.surface_key, key, sizeof(key));
  ggd.surface_key_valid = true;
  ggd.surface_height = 0.0f;

  const blender::Span<float3> positions = mesh->vert_positions();
  blender::Array<float3> in_frame(positions.size());
  float best = 0.0f;
  bool found = false;
  const auto consider = [&](const float z) {
    if (!std::isfinite(z)) {
      return;
    }
    if (!found || z > best) {
      best = z;
      found = true;
    }
  };
  for (const int64_t i : positions.index_range()) {
    in_frame[i] = math::transform_point(painted_to_frame, positions[i]);
    if (fabsf(in_frame[i].x) <= half_x && fabsf(in_frame[i].y) <= half_y) {
      consider(in_frame[i].z);
    }
  }

  const float samples[5][2] = {
      {0.0f, 0.0f},
      {half_x * 0.5f, 0.0f},
      {-half_x * 0.5f, 0.0f},
      {0.0f, half_y * 0.5f},
      {0.0f, -half_y * 0.5f},
  };
  const blender::Span<int> corner_verts = mesh->corner_verts();
  for (const int3 &tri : mesh->corner_tris()) {
    const float3 &a = in_frame[corner_verts[tri[0]]];
    const float3 &b = in_frame[corner_verts[tri[1]]];
    const float3 &c = in_frame[corner_verts[tri[2]]];
    const float min_x = std::min({a.x, b.x, c.x});
    const float max_x = std::max({a.x, b.x, c.x});
    const float min_y = std::min({a.y, b.y, c.y});
    const float max_y = std::max({a.y, b.y, c.y});
    for (const float *sample : samples) {
      if (sample[0] < min_x || sample[0] > max_x || sample[1] < min_y || sample[1] > max_y) {
        continue;
      }
      const float a2[2] = {a.x, a.y};
      const float b2[2] = {b.x, b.y};
      const float c2[2] = {c.x, c.y};
      if (!isect_point_tri_v2(sample, a2, b2, c2)) {
        continue;
      }
      float w[3];
      barycentric_weights_v2(a2, b2, c2, sample, w);
      consider(a.z * w[0] + b.z * w[1] + c.z * w[2]);
    }
  }

  ggd.surface_height = found ? best : 0.0f;
  return ggd.surface_height;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Cage (frame move + scale XY)
 * \{ */

static void projection_cage_matrix_get(const wmGizmo * /*gz*/,
                                       wmGizmoProperty *gz_prop,
                                       void *value_p)
{
  float(*matrix)[4] = static_cast<float(*)[4]>(value_p);
  auto *ggd = static_cast<ProjectionGizmoGroup *>(gz_prop->custom_func.user_data);
  ProjectionFrame frame;
  if (ggd != nullptr && ggd->target.read(frame)) {
    projection_frame_cage_matrix_get(frame, matrix);
    /* The handle is drawn on the surface, so the drag starts from there. `matrix[2]` is the same
     * direction #projection_cage_matrix_set shifts back out of the stored center. */
    madd_v3_v3v3fl(matrix[3], matrix[3], matrix[2], ggd->surface_height);
  }
  else {
    unit_m4(matrix);
  }
}

static void projection_cage_matrix_set(const wmGizmo * /*gz*/,
                                       wmGizmoProperty *gz_prop,
                                       const void *value_p)
{
  const float(*matrix)[4] = static_cast<const float(*)[4]>(value_p);
  auto *ggd = static_cast<ProjectionGizmoGroup *>(gz_prop->custom_func.user_data);
  if (ggd == nullptr) {
    return;
  }
  float loc[3];
  float rot[3][3];
  float scale[3];
  mat4_to_loc_rot_size(loc, rot, scale, matrix);

  /* Read the current frame to preserve normal/roll; write only center and XY size. */
  ProjectionFrame frame;
  if (!ggd->target.read(frame)) {
    return;
  }
  /* Undo the display shift to the surface: only the drag's own movement reaches the stored center. */
  float rot_frame[3][3];
  BKE_paint_layers_projection_frame_matrix(frame.normal, frame.roll, rot_frame);
  madd_v3_v3v3fl(frame.center, loc, rot_frame[2], -ggd->cage_surface_init);
  /* The cage scales uniformly, so a square frame stays square. */
  for (int i = 0; i < 2; i++) {
    frame.size[i] = ggd->cage_size_init[i] * fabsf(scale[i]) * ggd->cage_sign_init[i];
  }
  /* Z (depth) is not touched by the cage. */
  if (ggd->target.write(frame)) {
    projection_target_notify(ggd->target.material());
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Roll dial
 * \{ */

static void projection_roll_get(const wmGizmo * /*gz*/, wmGizmoProperty *gz_prop, void *value_p)
{
  auto *ggd = static_cast<ProjectionGizmoGroup *>(gz_prop->custom_func.user_data);
  ProjectionFrame frame;
  *static_cast<float *>(value_p) =
      (ggd != nullptr && ggd->target.read(frame)) ? frame.roll : 0.0f;
}

static void projection_roll_set(const wmGizmo * /*gz*/,
                                wmGizmoProperty *gz_prop,
                                const void *value_p)
{
  auto *ggd = static_cast<ProjectionGizmoGroup *>(gz_prop->custom_func.user_data);
  if (ggd == nullptr) {
    return;
  }
  /* Positive dial angle corresponds to positive roll (right-hand rule around frame +Z). */
  if (ggd->target.write_roll(*static_cast<const float *>(value_p))) {
    projection_target_notify(ggd->target.material());
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Depth arrow
 * \{ */

static void projection_depth_get(const wmGizmo * /*gz*/, wmGizmoProperty *gz_prop, void *value_p)
{
  auto *ggd = static_cast<ProjectionGizmoGroup *>(gz_prop->custom_func.user_data);
  ProjectionFrame frame;
  *static_cast<float *>(value_p) =
      (ggd != nullptr && ggd->target.read(frame)) ? frame.size[2] * 0.5f : 0.5f;
}

static void projection_depth_set(const wmGizmo * /*gz*/,
                                 wmGizmoProperty *gz_prop,
                                 const void *value_p)
{
  auto *ggd = static_cast<ProjectionGizmoGroup *>(gz_prop->custom_func.user_data);
  if (ggd == nullptr) {
    return;
  }
  ProjectionFrame frame;
  if (!ggd->target.read(frame)) {
    return;
  }
  const float half = *static_cast<const float *>(value_p);
  frame.size[2] = std::max(half * 2.0f, PAINT_LAYER_PROJECTION_SIZE_MIN);
  if (ggd->target.write(frame)) {
    projection_target_notify(ggd->target.material());
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Setup and place
 * \{ */

void projection_gizmo_planar_setup(wmGizmoGroup *gzgroup, ProjectionGizmoGroup &ggd)
{
  /* --- Cage. --- */
  const wmGizmoType *gzt_cage = WM_gizmotype_find("GIZMO_GT_cage_3d", true);
  if (gzt_cage != nullptr) {
    wmGizmo *gz = WM_gizmo_new_ptr(gzt_cage, gzgroup, nullptr);
    ggd.gz_cage = gz;
    if (gz != nullptr) {
      /* No translate: moving in the cage plane lifts the frame off the surface and knows nothing
       * of other faces; the Move handle does that along the mesh. */
      RNA_enum_set(gz->ptr,
                   "transform",
                   ED_GIZMO_CAGE_XFORM_FLAG_SCALE | ED_GIZMO_CAGE_XFORM_FLAG_SCALE_UNIFORM);
      RNA_enum_set(gz->ptr, "draw_style", ED_GIZMO_CAGE3D_STYLE_BOX);
      const float dims[3] = {1.0f, 1.0f, 1.0f};
      RNA_float_set_array(gz->ptr, "dimensions", dims);
      ui::theme::get_color_3fv(TH_GIZMO_PRIMARY, gz->color);
      ui::theme::get_color_3fv(TH_GIZMO_HI, gz->color_hi);
      gz->color[3] = PROJECTION_GIZMO_ALPHA;
      gz->color_hi[3] = PROJECTION_GIZMO_ALPHA;
      WM_gizmo_set_line_width(gz, 1.5f);
      WM_gizmo_set_flag(gz, WM_GIZMO_NEEDS_UNDO, true);
      wmGizmoPropertyFnParams params{};
      params.value_get_fn = projection_cage_matrix_get;
      params.value_set_fn = projection_cage_matrix_set;
      params.user_data = &ggd;
      WM_gizmo_target_property_def_func(gz, "matrix", &params);
    }
  }

  /* --- Roll dial. --- */
  const wmGizmoType *gzt_dial = WM_gizmotype_find("GIZMO_GT_dial_3d", true);
  if (gzt_dial != nullptr) {
    wmGizmo *ring = WM_gizmo_new_ptr(gzt_dial, gzgroup, nullptr);
    ggd.gz_roll = ring;
    if (ring != nullptr) {
      RNA_enum_set(ring->ptr, "draw_options", ED_GIZMO_DIAL_DRAW_FLAG_ANGLE_VALUE);
      RNA_boolean_set(ring->ptr, "wrap_angle", true);
      ring->flag |= WM_GIZMO_DRAW_NO_SCALE;
      ui::theme::get_color_3fv(TH_GIZMO_PRIMARY, ring->color);
      ui::theme::get_color_3fv(TH_GIZMO_HI, ring->color_hi);
      ring->color[3] = PROJECTION_GIZMO_ALPHA;
      ring->color_hi[3] = PROJECTION_GIZMO_ALPHA;
      WM_gizmo_set_flag(ring, WM_GIZMO_NEEDS_UNDO, true);
      wmGizmoPropertyFnParams rp{};
      rp.value_get_fn = projection_roll_get;
      rp.value_set_fn = projection_roll_set;
      rp.user_data = &ggd;
      WM_gizmo_target_property_def_func(ring, "offset", &rp);
    }
  }

  const wmGizmoType *gzt_arrow = WM_gizmotype_find("GIZMO_GT_arrow_3d", true);

  /* --- Depth arrow. --- */
  if (gzt_arrow != nullptr) {
    wmGizmo *depth = WM_gizmo_new_ptr(gzt_arrow, gzgroup, nullptr);
    ggd.gz_depth = depth;
    if (depth != nullptr) {
      /* Not CONSTRAINED: that mode maps the value range onto the arrow's travel and requires a
       * range for every property update (#gizmo_property_data_update), which a custom get/set
       * pair without an RNA property cannot give. Unconstrained, the offset tracks the value
       * directly and #projection_depth_set clamps it. */
      ui::theme::get_color_3fv(TH_GIZMO_SECONDARY, depth->color);
      ui::theme::get_color_3fv(TH_GIZMO_HI, depth->color_hi);
      depth->color[3] = PROJECTION_GIZMO_ALPHA;
      depth->color_hi[3] = PROJECTION_GIZMO_ALPHA;
      WM_gizmo_set_flag(depth, WM_GIZMO_NEEDS_UNDO, true);
      wmGizmoPropertyFnParams dp{};
      dp.value_get_fn = projection_depth_get;
      dp.value_set_fn = projection_depth_set;
      dp.user_data = &ggd;
      WM_gizmo_target_property_def_func(depth, "offset", &dp);
    }
  }

  /* --- Direction arrow (flip). --- */
  if (gzt_arrow != nullptr) {
    wmGizmo *dir = WM_gizmo_new_ptr(gzt_arrow, gzgroup, nullptr);
    ggd.gz_direction = dir;
    if (dir != nullptr) {
      RNA_enum_set(dir->ptr, "draw_style", ED_GIZMO_ARROW_STYLE_CONE);
      RNA_enum_set(dir->ptr, "draw_options", ED_GIZMO_ARROW_DRAW_FLAG_ORIGIN);
      ui::theme::get_color_3fv(TH_GIZMO_SECONDARY, dir->color);
      ui::theme::get_color_3fv(TH_GIZMO_HI, dir->color_hi);
      dir->color[3] = PROJECTION_GIZMO_ALPHA;
      dir->color_hi[3] = PROJECTION_GIZMO_ALPHA;
      /* Bind to the flip operator: clicking the cone triggers it. */
      wmOperatorType *ot_flip = WM_operatortype_find("PAINT_OT_layer_projection_flip", true);
      if (ot_flip != nullptr) {
        WM_gizmo_operator_set(dir, 0, ot_flip, nullptr);
      }
    }
  }

  /* --- Move handle: drags the frame along the painted surface. --- */
  const wmGizmoType *gzt_move = WM_gizmotype_find("GIZMO_GT_move_3d", true);
  if (gzt_move != nullptr) {
    wmGizmo *move = WM_gizmo_new_ptr(gzt_move, gzgroup, nullptr);
    ggd.gz_move = move;
    if (move != nullptr) {
      RNA_enum_set(move->ptr, "draw_style", ED_GIZMO_MOVE_STYLE_RING_2D);
      RNA_enum_set(move->ptr,
                   "draw_options",
                   ED_GIZMO_MOVE_DRAW_FLAG_ALIGN_VIEW | ED_GIZMO_MOVE_DRAW_FLAG_FILL);
      ui::theme::get_color_3fv(TH_GIZMO_PRIMARY, move->color);
      ui::theme::get_color_3fv(TH_GIZMO_HI, move->color_hi);
      move->color[3] = PROJECTION_GIZMO_ALPHA;
      move->color_hi[3] = PROJECTION_GIZMO_ALPHA;
      wmOperatorType *ot_move = WM_operatortype_find("PAINT_OT_layer_projection_move", true);
      if (ot_move != nullptr) {
        WM_gizmo_operator_set(move, 0, ot_move, nullptr);
      }
    }
  }
}

bool projection_gizmo_planar_place(const bContext *C,
                                   ProjectionGizmoGroup &ggd,
                                   const Object &space_ob,
                                   const ProjectionFrame &frame,
                                   const float rot[3][3])
{
  if (ggd.gz_cage == nullptr) {
    return false;
  }
  ggd.gz_cage->flag &= ~WM_GIZMO_HIDDEN;
  if (ggd.gz_roll) {
    ggd.gz_roll->flag &= ~WM_GIZMO_HIDDEN;
  }
  if (ggd.gz_direction) {
    ggd.gz_direction->flag &= ~WM_GIZMO_HIDDEN;
  }
  if (ggd.gz_move) {
    ggd.gz_move->flag &= ~WM_GIZMO_HIDDEN;
  }

  /* The handles sit on the most protruding point of the surface under the frame. */
  const float surface_height = projection_surface_height(C, ggd, space_ob, frame, rot);
  float anchor[3];
  madd_v3_v3v3fl(anchor, frame.center, rot[2], surface_height);

  /* The dragged cage and roll dial draw from their own drag state (the cage modal keeps its scaled
   * matrix_offset, the dial its angle); re-placing them would reset that every redraw. */
  /* Cage: dimensions = size XY, thin Z. */
  if (ggd.drag_gz != ggd.gz_cage) {
    const float cage_dims[3] = {ggd.base_size[0], ggd.base_size[1], 0.001f};
    RNA_float_set_array(ggd.gz_cage->ptr, "dimensions", cage_dims);
    copy_m4_m4(ggd.gz_cage->matrix_basis, space_ob.object_to_world().ptr());
    /* matrix_offset = frame rotation + anchor at unit scale. */
    projection_frame_basis(rot, anchor, ggd.gz_cage->matrix_offset);
  }

  /* Roll dial: basis with Z = normal; radius via matrix_offset diagonal. */
  if (ggd.gz_roll != nullptr && ggd.drag_gz != ggd.gz_roll) {
    float basis[4][4];
    projection_frame_basis(rot, anchor, basis);
    mul_m4_m4m4(ggd.gz_roll->matrix_basis, space_ob.object_to_world().ptr(), basis);
    const float radius = std::max(0.1f, std::max(ggd.base_size[0], ggd.base_size[1]));
    unit_m4(ggd.gz_roll->matrix_offset);
    for (const int i : IndexRange(3)) {
      ggd.gz_roll->matrix_offset[i][i] = radius;
    }
  }

  /* Depth arrow: on the frame axis at the slab's +Z face. The unconstrained arrow shifts its own
   * origin by its value (the half depth) along local Z, which is exactly that face, so the basis
   * stays on the stored center and nothing here reads Size X/Y: resizing the footprint never moves
   * it. Its length is fixed rather than the depth, otherwise a deep slab would give a huge arrow. */
  if (ggd.gz_depth != nullptr) {
    const MaterialPaintLayer *layer = BKE_paint_layers_find(*ggd.target.material(),
                                                           ggd.target.marker());
    const bool depth_on = (layer != nullptr) && BKE_paint_layers_projection_depth_get(*layer);
    if (depth_on) {
      ggd.gz_depth->flag &= ~WM_GIZMO_HIDDEN;
      if (ggd.drag_gz != ggd.gz_depth) {
        float dep_basis[4][4];
        projection_frame_basis(rot, frame.center, dep_basis);
        mul_m4_m4m4(ggd.gz_depth->matrix_basis, space_ob.object_to_world().ptr(), dep_basis);
        unit_m4(ggd.gz_depth->matrix_offset);
        const float planar_max = std::max(ggd.base_size[0], ggd.base_size[1]);
        RNA_float_set(ggd.gz_depth->ptr, "length", std::max(0.05f, planar_max * 0.3f));
      }
    }
    else {
      ggd.gz_depth->flag |= WM_GIZMO_HIDDEN;
    }
  }

  /* Direction cone: at the surface anchor, independent of depth. */
  if (ggd.gz_direction != nullptr) {
    float dir_basis[4][4];
    projection_frame_basis(rot, anchor, dir_basis);
    mul_m4_m4m4(ggd.gz_direction->matrix_basis, space_ob.object_to_world().ptr(), dir_basis);
    unit_m4(ggd.gz_direction->matrix_offset);
    const float planar_max = std::max(ggd.base_size[0], ggd.base_size[1]);
    RNA_float_set(ggd.gz_direction->ptr, "length", std::max(0.05f, planar_max * 0.15f));
  }

  /* Move handle: on the surface anchor, screen-aligned, sized to the frame so it is easy to grab. */
  if (ggd.gz_move != nullptr) {
    float move_basis[4][4];
    projection_frame_basis(rot, anchor, move_basis);
    mul_m4_m4m4(ggd.gz_move->matrix_basis, space_ob.object_to_world().ptr(), move_basis);
    unit_m4(ggd.gz_move->matrix_offset);
    const float planar_max = std::max(ggd.base_size[0], ggd.base_size[1]);
    ggd.gz_move->scale_basis = std::max(0.02f, planar_max * 0.06f);
  }
  return true;
}

/** \} */

}  // namespace blender::ed::sculpt_paint::projection
