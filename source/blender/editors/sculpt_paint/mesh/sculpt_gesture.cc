/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 * Common helper methods and structures for gesture operations.
 */
#include "sculpt_gesture.hh"

#include "MEM_guardedalloc.h"

#include "DNA_scene_types.h"
#include "DNA_vec_types.h"

#include "BLI_bitmap_draw_2d.h"
#include "BLI_lasso_2d.hh"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_rect.h"

#include "BKE_context.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"

#include "ED_view3d.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "../paint_intern.hh"
#include "sculpt_intern.hh"
#include "sculpt_multi_object.hh"

namespace blender::ed::sculpt_paint::gesture {

void operator_properties(wmOperatorType *ot, ShapeType shapeType)
{
  RNA_def_boolean(ot->srna,
                  "use_front_faces_only",
                  false,
                  "Front Faces Only",
                  "Affect only faces facing towards the view");

  if (shapeType == ShapeType::Line) {
    RNA_def_boolean(ot->srna,
                    "use_limit_to_segment",
                    false,
                    "Limit to Segment",
                    "Apply the gesture action only to the area that is contained within the "
                    "segment without extending its effect to the entire line");
  }
}

static void init_common(bContext *C, const wmOperator *op, GestureData &gesture_data)
{
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  gesture_data.vc = ED_view3d_viewcontext_init(C, depsgraph);
  const Object &object = *gesture_data.vc.obact;

  /* Operator properties. */
  gesture_data.front_faces_only = RNA_boolean_get(op->ptr, "use_front_faces_only");
  gesture_data.selection_type = SelectionType::Inside;

  gesture_data.paint = BKE_paint_get_active_from_context(C);
  gesture_data.brush = BKE_paint_brush_for_read(gesture_data.paint);

  /* SculptSession */
  gesture_data.ss = object.runtime->sculpt_session;

  /* Symmetry. The authoritative value is set once in #apply(), from the reference object
   * (objects[0]) -- not recomputed here, see that function's comment. */

  /* View Normal. */
  const float3x3 view_inv(float4x4(gesture_data.vc.rv3d->viewinv));
  const float3 view_dir = math::transform_direction(view_inv, {0.0f, 0.0f, 1.0f});

  gesture_data.world_space_view_normal = math::normalize(view_dir);
  gesture_data.true_view_normal = math::normalize(
      math::transform_direction(object.world_to_object(), view_dir));

  /* View Origin. */
  gesture_data.world_space_view_origin = gesture_data.vc.rv3d->viewinv[3];
  gesture_data.true_view_origin = gesture_data.vc.rv3d->viewinv[3];

  /* Single-object default. Multi-object gesture operators overwrite this with
   * sculpt_mode_objects(vc) after init_from_*() returns; apply() then recomputes all per-object
   * state via init_object_space() for each entry. The object-dependent values computed above are
   * for the active object and get recomputed per object in init_object_space() — they are still
   * set here because init_from_line()'s line_calculate_plane_points() reads true_view_normal /
   * true_view_origin before the per-object loop begins. */
  gesture_data.objects = {gesture_data.vc.obact};
}

static void line_plane_from_tri(float *r_plane,
                                const Object &object,
                                const bool flip,
                                const float3 &p1,
                                const float3 &p2,
                                const float3 &p3)
{
  float3 normal;
  normal_tri_v3(normal, p1, p2, p3);
  /* A world-space plane normal maps to local space via the inverse-transpose rule (the transpose
   * of object_to_world), not the naive world_to_object() transform used here previously -- the two
   * only agree (up to a scalar) when the object has no non-uniform scale, so the previous formula
   * silently tilted the local-space gesture plane on any non-uniformly scaled object. */
  normal = math::normalize(math::transpose(float3x3(object.object_to_world())) * normal);
  if (flip) {
    normal *= -1.0f;
  }
  const float3 plane_point_object_space = math::transform_point(object.world_to_object(), p1);
  plane_from_point_normal_v3(r_plane, plane_point_object_space, normal);
}

/* Recomputes every per-object field of `gesture_data` (SculptSession, symmetry, view normal,
 * clip/line planes) for `object`, from the object-independent anchors captured by init_common()
 * and init_from_box()/init_from_lasso()/init_from_line(). Called once per object by apply(). */
static void init_object_space(GestureData &gesture_data, Object &object)
{
  gesture_data.ss = object.runtime->sculpt_session;
  gesture_data.true_view_normal = math::normalize(
      math::transform_direction(object.world_to_object(), gesture_data.world_space_view_normal));

  switch (gesture_data.shape_type) {
    case ShapeType::Box: {
      BoundBox bb;
      ED_view3d_clipping_calc(&bb,
                              gesture_data.true_clip_planes,
                              gesture_data.vc.region,
                              &object,
                              &gesture_data.box_rect);
      break;
    }
    case ShapeType::Lasso: {
      gesture_data.lasso.projviewobjmat = ED_view3d_ob_project_mat_get(gesture_data.vc.rv3d,
                                                                       &object);
      BoundBox bb;
      ED_view3d_clipping_calc(&bb,
                              gesture_data.true_clip_planes,
                              gesture_data.vc.region,
                              &object,
                              &gesture_data.lasso.boundbox);
      break;
    }
    case ShapeType::Line: {
      const bool flip = gesture_data.line.flip ^ (!gesture_data.vc.rv3d->is_persp);
      line_plane_from_tri(gesture_data.line.true_plane,
                          object,
                          flip,
                          gesture_data.line_plane_points[0],
                          gesture_data.line_plane_points[1],
                          gesture_data.line_plane_points[2]);
      line_plane_from_tri(gesture_data.line.true_side_plane[0],
                          object,
                          false,
                          gesture_data.line_plane_points[1],
                          gesture_data.line_plane_points[0],
                          gesture_data.line_offset_plane_points[0]);
      line_plane_from_tri(gesture_data.line.true_side_plane[1],
                          object,
                          false,
                          gesture_data.line_plane_points[3],
                          gesture_data.line_plane_points[2],
                          gesture_data.line_offset_plane_points[1]);
      break;
    }
  }
}

static void lasso_px_cb(const int x, const int x_end, const int y, void *user_data)
{
  GestureData *gesture_data = static_cast<GestureData *>(user_data);
  LassoData *lasso = &gesture_data->lasso;
  int index = (y * lasso->width) + x;
  const int index_end = (y * lasso->width) + x_end;
  do {
    lasso->mask_px[index].set();
  } while (++index != index_end);
}

std::unique_ptr<GestureData> init_from_polyline(bContext *C, wmOperator *op)
{
  return init_from_lasso(C, op);
}

std::unique_ptr<GestureData> init_from_lasso(bContext *C, wmOperator *op)
{
  const Array<int2> mcoords = WM_gesture_lasso_path_to_array(C, op);
  if (mcoords.size() <= 1) {
    return nullptr;
  }

  std::unique_ptr<GestureData> gesture_data = std::make_unique<GestureData>();
  gesture_data->shape_type = ShapeType::Lasso;

  init_common(C, op, *gesture_data);

  BLI_lasso_boundbox(&gesture_data->lasso.boundbox, mcoords);
  const int lasso_width = 1 + gesture_data->lasso.boundbox.xmax -
                          gesture_data->lasso.boundbox.xmin;
  const int lasso_height = 1 + gesture_data->lasso.boundbox.ymax -
                           gesture_data->lasso.boundbox.ymin;
  gesture_data->lasso.width = lasso_width;
  gesture_data->lasso.mask_px.resize(lasso_width * lasso_height);

  BLI_bitmap_draw_2d_poly_v2i_n(gesture_data->lasso.boundbox.xmin,
                                gesture_data->lasso.boundbox.ymin,
                                gesture_data->lasso.boundbox.xmax,
                                gesture_data->lasso.boundbox.ymax,
                                mcoords,
                                lasso_px_cb,
                                gesture_data.get());

  gesture_data->gesture_points.reinitialize(mcoords.size());
  for (const int i : mcoords.index_range()) {
    gesture_data->gesture_points[i][0] = mcoords[i][0];
    gesture_data->gesture_points[i][1] = mcoords[i][1];
  }

  init_object_space(*gesture_data, *gesture_data->vc.obact);

  return gesture_data;
}

std::unique_ptr<GestureData> init_from_box(bContext *C, wmOperator *op)
{
  std::unique_ptr<GestureData> gesture_data = std::make_unique<GestureData>();
  gesture_data->shape_type = ShapeType::Box;

  init_common(C, op, *gesture_data);

  WM_operator_properties_border_to_rcti(op, &gesture_data->box_rect);

  gesture_data->gesture_points.reinitialize(4);

  gesture_data->gesture_points[0][0] = gesture_data->box_rect.xmax;
  gesture_data->gesture_points[0][1] = gesture_data->box_rect.ymax;

  gesture_data->gesture_points[1][0] = gesture_data->box_rect.xmax;
  gesture_data->gesture_points[1][1] = gesture_data->box_rect.ymin;

  gesture_data->gesture_points[2][0] = gesture_data->box_rect.xmin;
  gesture_data->gesture_points[2][1] = gesture_data->box_rect.ymin;

  gesture_data->gesture_points[3][0] = gesture_data->box_rect.xmin;
  gesture_data->gesture_points[3][1] = gesture_data->box_rect.ymax;

  init_object_space(*gesture_data, *gesture_data->vc.obact);

  return gesture_data;
}

/* Creates 4 points in the plane defined by the line and 2 extra points with an offset relative to
 * this plane. */
static void line_calculate_plane_points(const GestureData &gesture_data,
                                        const Span<float2> line_points,
                                        std::array<float3, 4> &r_plane_points,
                                        std::array<float3, 2> &r_offset_plane_points)
{
  const float3 depth_point = gesture_data.true_view_origin + gesture_data.true_view_normal;
  ED_view3d_win_to_3d(
      gesture_data.vc.v3d, gesture_data.vc.region, depth_point, line_points[0], r_plane_points[0]);
  ED_view3d_win_to_3d(
      gesture_data.vc.v3d, gesture_data.vc.region, depth_point, line_points[1], r_plane_points[3]);

  const float3 offset_depth_point = gesture_data.true_view_origin +
                                    gesture_data.true_view_normal * 10.0f;
  ED_view3d_win_to_3d(gesture_data.vc.v3d,
                      gesture_data.vc.region,
                      offset_depth_point,
                      line_points[0],
                      r_plane_points[1]);
  ED_view3d_win_to_3d(gesture_data.vc.v3d,
                      gesture_data.vc.region,
                      offset_depth_point,
                      line_points[1],
                      r_plane_points[2]);

  float3 normal;
  normal_tri_v3(normal, r_plane_points[0], r_plane_points[1], r_plane_points[2]);
  r_offset_plane_points[0] = r_plane_points[0] + normal;
  r_offset_plane_points[1] = r_plane_points[3] + normal;
}

std::unique_ptr<GestureData> init_from_line(bContext *C, const wmOperator *op)
{
  std::unique_ptr<GestureData> gesture_data = std::make_unique<GestureData>();
  gesture_data->shape_type = ShapeType::Line;
  gesture_data->line.use_side_planes = RNA_boolean_get(op->ptr, "use_limit_to_segment");

  init_common(C, op, *gesture_data);

  gesture_data->gesture_points.reinitialize(2);
  gesture_data->gesture_points[0] = {float(RNA_int_get(op->ptr, "xstart")),
                                     float(RNA_int_get(op->ptr, "ystart"))};
  gesture_data->gesture_points[1] = {float(RNA_int_get(op->ptr, "xend")),
                                     float(RNA_int_get(op->ptr, "yend"))};

  gesture_data->line.flip = RNA_boolean_get(op->ptr, "flip");

  line_calculate_plane_points(*gesture_data,
                              gesture_data->gesture_points,
                              gesture_data->line_plane_points,
                              gesture_data->line_offset_plane_points);

  init_object_space(*gesture_data, *gesture_data->vc.obact);

  return gesture_data;
}

GestureData::~GestureData()
{
  MEM_SAFE_DELETE(this->operation);
}

static void flip_plane(float out[4], const float in[4], const char symm)
{
  if (symm & PAINT_SYMM_X) {
    out[0] = -in[0];
  }
  else {
    out[0] = in[0];
  }
  if (symm & PAINT_SYMM_Y) {
    out[1] = -in[1];
  }
  else {
    out[1] = in[1];
  }
  if (symm & PAINT_SYMM_Z) {
    out[2] = -in[2];
  }
  else {
    out[2] = in[2];
  }

  out[3] = in[3];
}

float3 mirror_world_point(const GestureData &gesture_data, const float3 &world_point)
{
  BLI_assert(gesture_data.use_shared_symmetry_frame);
  return math::transform_point(
      gesture_data.symm_space_to_world,
      symmetry_flip(math::transform_point(gesture_data.world_to_symm_space, world_point),
                    gesture_data.symmpass));
}

/* Unprojects `rect` into world space (ob=nullptr skips the object-transform step
 * #ED_view3d_clipping_calc would otherwise apply), mirrors each corner through the shared
 * symmetry frame, and rebuilds clip planes from the mirrored+localized boundbox for
 * `current_object`. Shared by the Box and Lasso cases below: Box uses these as its precise
 * "is affected" test, Lasso uses them only for broad-phase node culling (the precise test mirrors
 * the query point directly, see #is_affected_lasso).
 *
 * \note Negates the result to match #flip_plane's convention for `gesture_data.clip_planes` (used
 * as-is by #isect_point_planes_v3 in #is_affected, and un-negated again locally by
 * #update_affected_nodes_by_clip_planes for #node_frustum_contain_aabb) -- #true_clip_planes /
 * #ED_view3d_clipping_calc_from_boundbox use the opposite (non-negated) sign convention. */
static void build_shared_frame_clip_planes(const GestureData &gesture_data,
                                           const Object &current_object,
                                           const rcti &rect,
                                           float r_clip_planes[4][4])
{
  BoundBox bb_world;
  float unused_planes[4][4];
  ED_view3d_clipping_calc(&bb_world, unused_planes, gesture_data.vc.region, nullptr, &rect);

  BoundBox bb_local;
  for (int k = 0; k < 8; k++) {
    const float3 mirrored_world = mirror_world_point(gesture_data, float3(bb_world.vec[k]));
    const float3 mirrored_local = math::transform_point(current_object.world_to_object(),
                                                        mirrored_world);
    copy_v3_v3(bb_local.vec[k], mirrored_local);
  }
  const bool flip_sign = is_negative_m4(current_object.object_to_world().ptr());
  ED_view3d_clipping_calc_from_boundbox(r_clip_planes, &bb_local, flip_sign);
  negate_m4(r_clip_planes);
}

static void flip_for_symmetry_pass(GestureData &gesture_data, const ePaintSymmetryFlags symmpass)
{
  gesture_data.symmpass = symmpass;

  if (!gesture_data.use_shared_symmetry_frame) {
    for (int j = 0; j < 4; j++) {
      flip_plane(gesture_data.clip_planes[j], gesture_data.true_clip_planes[j], symmpass);
    }

    negate_m4(gesture_data.clip_planes);

    gesture_data.view_normal = symmetry_flip(gesture_data.true_view_normal, symmpass);
    gesture_data.view_origin = symmetry_flip(gesture_data.true_view_origin, symmpass);
    flip_plane(gesture_data.line.plane, gesture_data.line.true_plane, symmpass);
    flip_plane(gesture_data.line.side_plane[0], gesture_data.line.true_side_plane[0], symmpass);
    flip_plane(gesture_data.line.side_plane[1], gesture_data.line.true_side_plane[1], symmpass);
    return;
  }

  /* Shared World/Cursor symmetry: mirror the WORLD-SPACE anchors around the shared frame, then
   * carry the result into the current object's own local space -- instead of mirroring each
   * object's already-local planes around its own origin. Matches the approach used for brush
   * strokes (#shared_symmetry_world_daubs). `view_origin` is intentionally left unset here: it
   * has no reader anywhere in the gesture code (verified), so it is not worth mirroring. */
  Object &current_object = *gesture_data.vc.obact;

  const float3 mirrored_world_view_normal = symmetry_flip(gesture_data.world_space_view_normal,
                                                          symmpass);
  gesture_data.view_normal = math::normalize(
      math::transform_direction(current_object.world_to_object(), mirrored_world_view_normal));

  switch (gesture_data.shape_type) {
    case ShapeType::Box:
      build_shared_frame_clip_planes(
          gesture_data, current_object, gesture_data.box_rect, gesture_data.clip_planes);
      break;
    case ShapeType::Lasso:
      /* Broad-phase node culling only -- the precise "is affected" test mirrors the query point
       * directly in #is_affected_lasso using the same shared frame. */
      build_shared_frame_clip_planes(
          gesture_data, current_object, gesture_data.lasso.boundbox, gesture_data.clip_planes);
      break;
    case ShapeType::Line: {
      const bool flip = gesture_data.line.flip ^ (!gesture_data.vc.rv3d->is_persp);
      const float3 p0 = mirror_world_point(gesture_data, gesture_data.line_plane_points[0]);
      const float3 p1 = mirror_world_point(gesture_data, gesture_data.line_plane_points[1]);
      const float3 p2 = mirror_world_point(gesture_data, gesture_data.line_plane_points[2]);
      const float3 p3 = mirror_world_point(gesture_data, gesture_data.line_plane_points[3]);
      const float3 op0 = mirror_world_point(gesture_data,
                                            gesture_data.line_offset_plane_points[0]);
      const float3 op1 = mirror_world_point(gesture_data,
                                            gesture_data.line_offset_plane_points[1]);
      line_plane_from_tri(gesture_data.line.plane, current_object, flip, p0, p1, p2);
      line_plane_from_tri(gesture_data.line.side_plane[0], current_object, false, p1, p0, op0);
      line_plane_from_tri(gesture_data.line.side_plane[1], current_object, false, p3, p2, op1);
      break;
    }
  }
}

static void update_affected_nodes_by_line_plane(GestureData &gesture_data)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(*gesture_data.vc.obact);
  std::array<float4, 3> clip_planes;
  copy_v4_v4(clip_planes[0], gesture_data.line.plane);
  copy_v4_v4(clip_planes[1], gesture_data.line.side_plane[0]);
  copy_v4_v4(clip_planes[2], gesture_data.line.side_plane[1]);

  gesture_data.node_mask = bke::pbvh::search_nodes(
      pbvh, gesture_data.node_mask_memory, [&](const bke::pbvh::Node &node) {
        return bke::pbvh::node_frustum_contain_aabb(
            node, Span(clip_planes).take_front(gesture_data.line.use_side_planes ? 3 : 1));
      });
}

static void update_affected_nodes_by_clip_planes(GestureData &gesture_data)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(*gesture_data.vc.obact);
  float clip_planes[4][4];
  copy_m4_m4(clip_planes, gesture_data.clip_planes);
  negate_m4(clip_planes);

  Span planes(reinterpret_cast<float4 *>(clip_planes), 4);

  gesture_data.node_mask = bke::pbvh::search_nodes(
      pbvh, gesture_data.node_mask_memory, [&](const bke::pbvh::Node &node) {
        switch (gesture_data.selection_type) {
          case SelectionType::Inside:
            return bke::pbvh::node_frustum_contain_aabb(node, planes);
          case SelectionType::Outside:
            /* Certain degenerate cases of a lasso shape can cause the resulting
             * frustum planes to enclose a node's AABB, therefore we must submit it
             * to be more thoroughly evaluated. */
            if (gesture_data.shape_type == ShapeType::Lasso) {
              return true;
            }
            return bke::pbvh::node_frustum_exclude_aabb(node, planes);
        }
        BLI_assert_unreachable();
        return false;
      });
}

static void update_affected_nodes(GestureData &gesture_data)
{
  switch (gesture_data.shape_type) {
    case ShapeType::Box:
    case ShapeType::Lasso:
      update_affected_nodes_by_clip_planes(gesture_data);
      break;
    case ShapeType::Line:
      update_affected_nodes_by_line_plane(gesture_data);
      break;
  }
}

static bool is_affected_lasso(const GestureData &gesture_data, const float3 &position)
{
  /* First project point to 2d space. `position` is local to the current object.
   * `use_shared_symmetry_frame`: mirror through the shared World/Cursor frame in world space,
   * then project with the view's own world-to-screen matrix (#RegionView3D::persmat) -- the
   * per-object `projviewobjmat` already bakes in this object's matrix, which would double-apply
   * it if fed a world-space point. Otherwise (Active Object space or single-object gesture):
   * mirror the point around the object's own local origin, unchanged from before. */
  float2 scr_co_f;
  if (gesture_data.use_shared_symmetry_frame) {
    const float3 world_pos = math::transform_point(gesture_data.vc.obact->object_to_world(),
                                                   position);
    const float3 mirrored_world = mirror_world_point(gesture_data, world_pos);
    scr_co_f = ED_view3d_project_float_v2_m4(
        gesture_data.vc.region, mirrored_world, float4x4(gesture_data.vc.rv3d->persmat));
  }
  else {
    const float3 co_final = symmetry_flip(position, gesture_data.symmpass);
    scr_co_f = ED_view3d_project_float_v2_m4(
        gesture_data.vc.region, co_final, gesture_data.lasso.projviewobjmat);
  }

  int2 screen_coords = {int(scr_co_f[0]), int(scr_co_f[1])};

  /* Clip against lasso boundbox. */
  const LassoData &lasso = gesture_data.lasso;
  if (!BLI_rcti_isect_pt_v(&lasso.boundbox, screen_coords)) {
    return gesture_data.selection_type == SelectionType::Outside;
  }

  screen_coords[0] -= lasso.boundbox.xmin;
  screen_coords[1] -= lasso.boundbox.ymin;

  const bool bitmap_result =
      lasso.mask_px[screen_coords[1] * lasso.width + screen_coords[0]].test();
  switch (gesture_data.selection_type) {
    case SelectionType::Inside:
      return bitmap_result;
    case SelectionType::Outside:
      return !bitmap_result;
  }
  BLI_assert_unreachable();
  return false;
}

bool is_affected(const GestureData &gesture_data, const float3 &position, const float3 &normal)
{
  const float dot = math::dot(gesture_data.view_normal, normal);
  const bool is_affected_front_face = !(gesture_data.front_faces_only && dot < 0.0f);

  if (!is_affected_front_face) {
    return false;
  }

  switch (gesture_data.shape_type) {
    case ShapeType::Box: {
      const bool is_contained = isect_point_planes_v3(gesture_data.clip_planes, 4, position);
      return ((is_contained && gesture_data.selection_type == SelectionType::Inside) ||
              (!is_contained && gesture_data.selection_type == SelectionType::Outside));
    }
    case ShapeType::Lasso:
      return is_affected_lasso(gesture_data, position);
    case ShapeType::Line:
      if (gesture_data.line.use_side_planes) {
        return plane_point_side_v3(gesture_data.line.plane, position) > 0.0f &&
               plane_point_side_v3(gesture_data.line.side_plane[0], position) > 0.0f &&
               plane_point_side_v3(gesture_data.line.side_plane[1], position) > 0.0f;
      }
      return plane_point_side_v3(gesture_data.line.plane, position) > 0.0f;
  }
  return false;
}

void filter_factors(const GestureData &gesture_data,
                    const Span<float3> positions,
                    const Span<float3> normals,
                    const MutableSpan<float> factors)
{
  for (const int i : positions.index_range()) {
    if (!is_affected(gesture_data, positions[i], normals[i])) {
      factors[i] = 0.0f;
    }
  }
}

void apply(bContext &C, GestureData &gesture_data, wmOperator &op)
{
  const Operation *operation = gesture_data.operation;

  operation->begin(C, op, gesture_data);

  /* Symmetry AXES/radial-count source: the reference (active, objects[0]) object's own
   * Mesh.symmetry -- secondary objects mirror using the reference's enabled axes even when their
   * own mesh has no mirror toggled on, matching #StrokeCache::symm_reference_object for brush
   * strokes (a multi-object gesture then matches the same geometry after Join). For a
   * single-object gesture objects[0] is the object itself, so this is bit-exact with the old
   * per-object computation. */
  gesture_data.symm = ePaintSymmetryFlags(mesh_symmetry_xyz_get(*gesture_data.objects.first()));

  /* Shared multi-object symmetry frame for Global World Origin / Global 3D Cursor
   * `symmetry_space` -- mirrors around world axes (optionally through the 3D cursor) instead of
   * each object's own local origin. Gated on more than one object so single-object gestures stay
   * bit-exact regardless of the symmetry_space setting, matching the brush-stroke
   * `multi_object_stroke` discipline. The frame itself is constant across objects/passes; only
   * `symmpass` (read separately per pass) varies the actual mirror. */
  const ePaintSymmetrySpace symmetry_space = ePaintSymmetrySpace(
      gesture_data.paint->symmetry_space);
  gesture_data.use_shared_symmetry_frame = gesture_data.objects.size() > 1 &&
                                           symmetry_space != PAINT_SYMM_SPACE_ACTIVE_OBJECT;
  if (gesture_data.use_shared_symmetry_frame) {
    const float4x4 cursor_to_world = gesture_data.vc.scene->cursor.matrix<float4x4>();
    gesture_data.world_to_symm_space = symmetry_space_frame(
        symmetry_space, gesture_data.objects.first()->world_to_object(), cursor_to_world);
    gesture_data.symm_space_to_world = math::invert(gesture_data.world_to_symm_space);
  }

  for (Object *object : gesture_data.objects) {
    gesture_data.vc.obact = object;
    init_object_space(gesture_data, *object);

    for (int symmpass = 0; symmpass <= gesture_data.symm; symmpass++) {
      if (is_symmetry_iteration_valid(symmpass, gesture_data.symm)) {
        flip_for_symmetry_pass(gesture_data, ePaintSymmetryFlags(symmpass));
        update_affected_nodes(gesture_data);

        operation->apply_for_symmetry_pass(C, gesture_data);
      }
    }
  }

  operation->end(C, gesture_data);

  tag_update_overlays(&C);
}
}  // namespace blender::ed::sculpt_paint::gesture
