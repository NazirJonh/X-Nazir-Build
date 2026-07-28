/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "GEO_join_geometries.hh"
#include "GEO_mesh_boolean.hh"

#include "BLI_math_geom.h"
#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_polyfill_2d.h"

#include "BKE_brush.hh"
#include "BKE_context.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_mesh.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_report.hh"

#include "DEG_depsgraph.hh"

#include "ED_sculpt.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "bmesh.hh"

#include "../paint_intern.hh"
#include "sculpt_face_set.hh"
#include "sculpt_gesture.hh"
#include "sculpt_intern.hh"
#include "sculpt_islands.hh"
#include "sculpt_undo.hh"

namespace blender::ed::sculpt_paint::trim {

enum class OperationType {
  Intersect = 0,
  Difference = 1,
  Union = 2,
  Join = 3,
};

/* Intersect is not exposed in the UI because it does not work correctly with symmetry (it deletes
 * the symmetrical part of the mesh in the first symmetry pass). */
static EnumPropertyItem operation_types[] = {
    {int(OperationType::Difference),
     "DIFFERENCE",
     0,
     "Difference",
     "Use a difference boolean operation"},
    {int(OperationType::Union), "UNION", 0, "Union", "Use a union boolean operation"},
    {int(OperationType::Join),
     "JOIN",
     0,
     "Join",
     "Join the new mesh as separate geometry, without performing any boolean operation"},
    {0, nullptr, 0, nullptr, nullptr},
};

enum class OrientationType {
  View = 0,
  Surface = 1,
};
static EnumPropertyItem orientation_types[] = {
    {int(OrientationType::View),
     "VIEW",
     0,
     "View",
     "Use the view to orientate the trimming shape"},
    {int(OrientationType::Surface),
     "SURFACE",
     0,
     "Surface",
     "Use the surface normal to orientate the trimming shape"},
    {0, nullptr, 0, nullptr, nullptr},
};

enum class ExtrudeMode {
  Project = 0,
  Fixed = 1,
};

static EnumPropertyItem extrude_modes[] = {
    {int(ExtrudeMode::Project),
     "PROJECT",
     0,
     "Project",
     "Align trim geometry with the perspective of the current view for a tapered shape"},
    {int(ExtrudeMode::Fixed),
     "FIXED",
     0,
     "Fixed",
     "Align trim geometry orthogonally for a shape with 90 degree angles"},
    {0, nullptr, 0, nullptr, nullptr},
};

static const EnumPropertyItem solver_items[] = {
    {int(geometry::boolean::Solver::MeshArr),
     "EXACT",
     0,
     "Exact",
     "Slower solver with the best results for coplanar faces"},
    {int(geometry::boolean::Solver::Float),
     "FLOAT",
     0,
     "Float",
     "Simple solver with good performance, without support for overlapping geometry"},
    {int(geometry::boolean::Solver::Manifold),
     "MANIFOLD",
     0,
     "Manifold",
     "Fastest solver that works only on manifold meshes but gives better results"},
    {0, nullptr, 0, nullptr, nullptr},
};

struct TrimOperation {
  gesture::Operation op;
  ReportList *reports;

  /* Operation-generated geometry, expressed in the PRIMARY object's (gesture_data.objects[0])
   * local space. Reprojected into each object's own local space in
   * gesture_apply_for_symmetry_pass() for multi-object gestures. */
  Mesh *mesh;
  float (*true_mesh_co)[3];

  /* Operator properties. */
  bool use_cursor_depth;

  bool initial_hit;
  float3 initial_location;
  float3 initial_normal;
  /* Object the cursor was actually over when the gesture started -- may differ from the active
   * object in multi-object sculpt mode. #initial_location/#initial_normal are in ITS local space.
   * Null when #initial_hit is false. */
  Object *hit_object;

  OperationType mode;
  geometry::boolean::Solver solver_mode;
  OrientationType orientation;
  ExtrudeMode extrude_mode;

  /* Face set ID assigned to the new geometry, shared across every object in a multi-object
   * gesture (as if the meshes were joined) -- computed once in gesture_begin(). */
  int new_face_set_id;
};

/* Recalculate the mesh normals for the generated trim mesh. */
static void update_normals(gesture::GestureData &gesture_data)
{
  TrimOperation *trim_operation = reinterpret_cast<TrimOperation *>(gesture_data.operation);
  Mesh *trim_mesh = trim_operation->mesh;

  const BMAllocTemplate allocsize = BMALLOC_TEMPLATE_FROM_ME(trim_mesh);

  BMeshCreateParams bm_create_params{};
  bm_create_params.use_toolflags = true;
  BMesh *bm = BM_mesh_create(&allocsize, &bm_create_params);

  BMeshFromMeshParams bm_from_me_params{};
  bm_from_me_params.calc_face_normal = true;
  bm_from_me_params.calc_vert_normal = true;
  BM_mesh_bm_from_me(bm, trim_mesh, &bm_from_me_params);

  BM_mesh_elem_hflag_enable_all(bm, BM_FACE, BM_ELEM_TAG, false);
  BMO_op_callf(bm,
               (BMO_FLAG_DEFAULTS & ~BMO_FLAG_RESPECT_HIDE),
               "recalc_face_normals faces=%hf",
               BM_ELEM_TAG);
  BM_mesh_elem_hflag_disable_all(bm, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_TAG, false);

  BMeshToMeshParams convert_params{};
  convert_params.calc_object_remap = false;
  Mesh *result = BKE_mesh_from_bmesh_nomain(bm, &convert_params, trim_mesh);

  BM_mesh_free(bm);
  BKE_id_free(nullptr, trim_mesh);
  trim_operation->mesh = result;
}

/* Get the origin and normal that are going to be used for calculating the depth and position of
 * the trimming geometry. */
static void get_origin_and_normal(gesture::GestureData &gesture_data,
                                  float *r_origin,
                                  float *r_normal)
{
  TrimOperation *trim_operation = reinterpret_cast<TrimOperation *>(gesture_data.operation);
  /* initial_location/initial_normal are in the space of whichever object the cursor was actually
   * over when the gesture started (hit_object) -- may differ from the active object in
   * multi-object sculpt mode. Fall back to the active object if nothing was hit. */
  Object &hit_object = trim_operation->hit_object ? *trim_operation->hit_object :
                                                    *gesture_data.vc.obact;
  /* Use the view origin and normal in world space. The trimming mesh coordinates are
   * calculated in world space, aligned to the view, and then converted to object space to
   * store them in the final trimming mesh which is going to be used in the boolean operation.
   */
  switch (trim_operation->orientation) {
    case OrientationType::View:
      mul_v3_m4v3(r_origin, hit_object.object_to_world().ptr(), trim_operation->initial_location);
      copy_v3_v3(r_normal, gesture_data.world_space_view_normal);
      negate_v3(r_normal);
      break;
    case OrientationType::Surface:
      mul_v3_m4v3(r_origin, hit_object.object_to_world().ptr(), trim_operation->initial_location);
      /* Transforming the normal does not take non uniform scaling into account. Sculpt mode is not
       * expected to work on object with non uniform scaling. */
      copy_v3_v3(r_normal, trim_operation->initial_normal);
      mul_mat3_m4_v3(hit_object.object_to_world().ptr(), r_normal);
      break;
  }
}

/* Calculates the depth of the drawn shape inside the scene. */
static void calculate_depth(gesture::GestureData &gesture_data,
                            float &r_depth_front,
                            float &r_depth_back)
{
  TrimOperation *trim_operation = reinterpret_cast<TrimOperation *>(gesture_data.operation);

  SculptSession &ss = *gesture_data.ss;
  ViewContext &vc = gesture_data.vc;

  float shape_plane[4];
  float shape_origin[3];
  float shape_normal[3];
  get_origin_and_normal(gesture_data, shape_origin, shape_normal);
  plane_from_point_normal_v3(shape_plane, shape_origin, shape_normal);

  float depth_front = FLT_MAX;
  float depth_back = -FLT_MAX;

  for (Object *object : gesture_data.objects) {
    const Span<float3> positions = bke::pbvh::vert_positions_eval(*vc.depsgraph, *object);
    const float4x4 &object_to_world = object->object_to_world();

    for (const int i : positions.index_range()) {
      /* Convert the coordinates to world space to calculate the depth. When generating the
       * trimming mesh, coordinates are first calculated in world space, then converted to each
       * object's local space to store them. */
      const float3 world_space_vco = math::transform_point(object_to_world, positions[i]);
      const float dist = dist_signed_to_plane_v3(world_space_vco, shape_plane);
      depth_front = std::min(dist, depth_front);
      depth_back = std::max(dist, depth_back);
    }
  }

  if (trim_operation->use_cursor_depth) {
    Object &hit_object = trim_operation->hit_object ? *trim_operation->hit_object : *vc.obact;
    float world_space_gesture_initial_location[3];
    mul_v3_m4v3(world_space_gesture_initial_location,
                hit_object.object_to_world().ptr(),
                trim_operation->initial_location);

    float mid_point_depth;
    if (trim_operation->orientation == OrientationType::View) {
      mid_point_depth = trim_operation->initial_hit ?
                            dist_signed_to_plane_v3(world_space_gesture_initial_location,
                                                    shape_plane) :
                            (depth_back + depth_front) * 0.5f;
    }
    else {
      /* When using normal orientation, if the stroke started over the mesh, position the mid point
       * at 0 distance from the shape plane. This positions the trimming shape half inside of the
       * surface. */
      mid_point_depth = trim_operation->initial_hit ? 0.0f : (depth_back + depth_front) * 0.5f;
    }

    float depth_radius;

    if (trim_operation->initial_hit) {
      depth_radius = ss.cursor_radius;
    }
    else {
      /* ss.cursor_radius is only valid if the stroke started
       * over the sculpt mesh.  If it's not we must
       * compute the radius ourselves.  See #81452.
       */

      depth_radius = object_space_radius_get(
          vc, *gesture_data.paint, *gesture_data.brush, trim_operation->initial_location);
    }

    depth_front = mid_point_depth - depth_radius;
    depth_back = mid_point_depth + depth_radius;
  }

  r_depth_front = depth_front;
  r_depth_back = depth_back;
}

/* Calculates a scalar factor to use to ensure a drawn line gesture
 * encompasses the entire object to be acted on. */
static float calc_expand_factor(const gesture::GestureData &gesture_data)
{
  float2 min_corner(FLT_MAX, FLT_MAX);
  float2 max_corner(-FLT_MAX, -FLT_MAX);

  for (Object *object : gesture_data.objects) {
    rcti rect;
    const Bounds<float3> bounds = *BKE_object_boundbox_get(object);
    paint_convert_bb_to_rect(
        &rect, bounds.min, bounds.max, *gesture_data.vc.region, *gesture_data.vc.rv3d, *object);

    min_corner.x = std::min(min_corner.x, float(rect.xmin));
    min_corner.y = std::min(min_corner.y, float(rect.ymin));
    max_corner.x = std::max(max_corner.x, float(rect.xmax));
    max_corner.y = std::max(max_corner.y, float(rect.ymax));
  }

  /* Multiply the screen space bounds by an arbitrary factor to ensure the created points are
   * sufficiently far and enclose all the meshes to be operated on. */
  return math::distance(min_corner, max_corner) * 2.0f;
}

/* Converts a line gesture's points into usable screen points. */
static Array<float2> gesture_to_screen_points(gesture::GestureData &gesture_data)
{
  if (gesture_data.shape_type != gesture::ShapeType::Line) {
    return gesture_data.gesture_points;
  }

  const float expand_factor = calc_expand_factor(gesture_data);

  float2 start(gesture_data.gesture_points[0]);
  float2 end(gesture_data.gesture_points[1]);

  const float2 dir = math::normalize(end - start);

  if (!gesture_data.line.use_side_planes) {
    end = end + dir * expand_factor;
    start = start - dir * expand_factor;
  }

  float2 perp(dir.y, -dir.x);

  if (gesture_data.line.flip) {
    perp *= -1;
  }

  const float2 parallel_start = start + perp * expand_factor;
  const float2 parallel_end = end + perp * expand_factor;

  return {start, end, parallel_end, parallel_start};
}

static void generate_geometry(gesture::GestureData &gesture_data)
{
  TrimOperation *trim_operation = reinterpret_cast<TrimOperation *>(gesture_data.operation);
  ViewContext &vc = gesture_data.vc;
  ARegion *region = vc.region;

  const Array<float2> screen_points = gesture_to_screen_points(gesture_data);
  BLI_assert(screen_points.size() > 1);

  const int trim_totverts = screen_points.size() * 2;
  const int trim_faces_nums = (2 * (screen_points.size() - 2)) + (2 * screen_points.size());
  trim_operation->mesh = BKE_mesh_new_nomain(
      trim_totverts, 0, trim_faces_nums, trim_faces_nums * 3);
  trim_operation->true_mesh_co = MEM_new_array_uninitialized<float[3]>(trim_totverts, "mesh orco");

  float shape_origin[3];
  float shape_normal[3];
  float shape_plane[4];
  get_origin_and_normal(gesture_data, shape_origin, shape_normal);
  plane_from_point_normal_v3(shape_plane, shape_origin, shape_normal);

  /* true_mesh_co (and the mesh's own positions, immediately overwritten by the first call to
   * gesture_apply_for_symmetry_pass()) are stored in the PRIMARY object's
   * (gesture_data.objects[0]) local space, NOT necessarily vc.obact's -- the active object may
   * have been filtered out of gesture_data.objects (e.g. Multires/Dyntopo), in which case the
   * first remaining object becomes the reference space. */
  const float (*ob_imat)[4] = gesture_data.objects.first()->world_to_object().ptr();

  /* Write vertices coordinates OperationType::Difference for the front face. */
  MutableSpan<float3> positions = trim_operation->mesh->vert_positions_for_write();

  float depth_front;
  float depth_back;
  calculate_depth(gesture_data, depth_front, depth_back);

  if (!trim_operation->use_cursor_depth) {
    float pad_factor = (depth_back - depth_front) * 0.01f + 0.001f;

    /* When using cursor depth, don't modify the depth set by the cursor radius. If full depth is
     * used, adding a little padding to the trimming shape can help avoiding booleans with coplanar
     * faces. */
    depth_front -= pad_factor;
    depth_back += pad_factor;
  }

  float depth_point[3];

  /* Get origin point for OrientationType::View.
   * NOTE: for projection extrusion we add depth_front here
   * instead of in the loop.
   */
  if (trim_operation->extrude_mode == ExtrudeMode::Fixed) {
    copy_v3_v3(depth_point, shape_origin);
  }
  else {
    madd_v3_v3v3fl(depth_point, shape_origin, shape_normal, depth_front);
  }

  for (const int i : screen_points.index_range()) {
    float new_point[3];
    if (trim_operation->orientation == OrientationType::View) {
      ED_view3d_win_to_3d(vc.v3d, region, depth_point, screen_points[i], new_point);

      /* For fixed mode we add the shape normal here to avoid projection errors. */
      if (trim_operation->extrude_mode == ExtrudeMode::Fixed) {
        madd_v3_v3fl(new_point, shape_normal, depth_front);
      }
    }
    else {
      ED_view3d_win_to_3d_on_plane(region, shape_plane, screen_points[i], false, new_point);
      madd_v3_v3fl(new_point, shape_normal, depth_front);
    }

    copy_v3_v3(positions[i], new_point);
  }

  /* Write vertices coordinates for the back face. */
  madd_v3_v3v3fl(depth_point, shape_origin, shape_normal, depth_back);
  for (const int i : screen_points.index_range()) {
    float new_point[3];

    if (trim_operation->extrude_mode == ExtrudeMode::Project) {
      if (trim_operation->orientation == OrientationType::View) {
        ED_view3d_win_to_3d(vc.v3d, region, depth_point, screen_points[i], new_point);
      }
      else {
        ED_view3d_win_to_3d_on_plane(region, shape_plane, screen_points[i], false, new_point);
        madd_v3_v3fl(new_point, shape_normal, depth_back);
      }
    }
    else {
      copy_v3_v3(new_point, positions[i]);
      float dist = dist_signed_to_plane_v3(new_point, shape_plane);

      madd_v3_v3fl(new_point, shape_normal, depth_back - dist);
    }

    copy_v3_v3(positions[i + screen_points.size()], new_point);
  }

  /* Project to object space. */
  for (int i = 0; i < screen_points.size() * 2; i++) {
    float new_point[3];

    copy_v3_v3(new_point, positions[i]);
    mul_v3_m4v3(positions[i], ob_imat, new_point);
    mul_v3_m4v3(trim_operation->true_mesh_co[i], ob_imat, new_point);
  }

  /* Get the triangulation for the front/back poly. */
  const int face_tris_num = bke::mesh::face_triangles_num(screen_points.size());
  Array<uint3> tris(face_tris_num);
  BLI_polyfill_calc(reinterpret_cast<const float (*)[2]>(screen_points.data()),
                    screen_points.size(),
                    0,
                    reinterpret_cast<uint(*)[3]>(tris.data()));

  /* Write the front face triangle indices. */
  MutableSpan<int> face_offsets = trim_operation->mesh->face_offsets_for_write();
  MutableSpan<int> corner_verts = trim_operation->mesh->corner_verts_for_write();
  int face_index = 0;
  int corner = 0;
  for (const int i : tris.index_range()) {
    face_offsets[face_index] = corner;
    corner_verts[corner + 0] = tris[i][0];
    corner_verts[corner + 1] = tris[i][1];
    corner_verts[corner + 2] = tris[i][2];
    face_index++;
    corner += 3;
  }

  /* Write the back face triangle indices. */
  for (const int i : tris.index_range()) {
    face_offsets[face_index] = corner;
    corner_verts[corner + 0] = tris[i][0] + screen_points.size();
    corner_verts[corner + 1] = tris[i][1] + screen_points.size();
    corner_verts[corner + 2] = tris[i][2] + screen_points.size();
    face_index++;
    corner += 3;
  }

  /* Write the indices for the lateral triangles. */
  for (const int i : screen_points.index_range()) {
    face_offsets[face_index] = corner;
    int current_index = i;
    int next_index = current_index + 1;
    if (next_index >= screen_points.size()) {
      next_index = 0;
    }
    corner_verts[corner + 0] = next_index + screen_points.size();
    corner_verts[corner + 1] = next_index;
    corner_verts[corner + 2] = current_index;
    face_index++;
    corner += 3;
  }

  for (const int i : screen_points.index_range()) {
    face_offsets[face_index] = corner;
    int current_index = i;
    int next_index = current_index + 1;
    if (next_index >= screen_points.size()) {
      next_index = 0;
    }
    corner_verts[corner + 0] = current_index;
    corner_verts[corner + 1] = current_index + screen_points.size();
    corner_verts[corner + 2] = next_index + screen_points.size();
    face_index++;
    corner += 3;
  }

  bke::mesh_smooth_set(*trim_operation->mesh, false);
  bke::mesh_calc_edges(*trim_operation->mesh, false, false);
  update_normals(gesture_data);
}

static void gesture_begin(bContext &C, wmOperator &op, gesture::GestureData &gesture_data)
{
  const Scene &scene = *CTX_data_scene(&C);
  TrimOperation *trim_operation = reinterpret_cast<TrimOperation *>(gesture_data.operation);

  for (Object *object : gesture_data.objects) {
    const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(*object);
    switch (pbvh.type()) {
      case bke::pbvh::Type::Mesh:
        face_set::create_face_sets_mesh(*object);
        break;
      default:
        BLI_assert_unreachable();
    }
  }

  /* One shared ID for the new geometry across every object, as if the meshes were joined --
   * computed after every object's face-set layer above so it accounts for all existing IDs. */
  trim_operation->new_face_set_id = face_set::find_shared_next_available_id(gesture_data.objects);

  generate_geometry(gesture_data);

  for (Object *object : gesture_data.objects) {
    islands::invalidate(*object->runtime->sculpt_session);
  }

  undo::geometry_begin(scene, *gesture_data.objects.first(), &op);
  for (Object *object : gesture_data.objects.as_span().drop_front(1)) {
    undo::geometry_begin_add_object(*object);
  }
}

static void apply_join_operation(Object &object, Mesh &sculpt_mesh, Mesh &trim_mesh)
{
  bke::GeometrySet joined = geometry::join_geometries(
      {bke::GeometrySet::from_mesh(&sculpt_mesh, bke::GeometryOwnershipType::ReadOnly),
       bke::GeometrySet::from_mesh(&trim_mesh, bke::GeometryOwnershipType::ReadOnly)},
      {});
  Mesh *result = joined.get_component_for_write<bke::MeshComponent>().release();
  BKE_mesh_nomain_to_mesh(result, &sculpt_mesh, &object);
}

static void apply_trim(gesture::GestureData &gesture_data)
{
  TrimOperation *trim_operation = reinterpret_cast<TrimOperation *>(gesture_data.operation);
  Object *object = gesture_data.vc.obact;
  Mesh &sculpt_mesh = *id_cast<Mesh *>(object->data);
  Mesh &trim_mesh = *trim_operation->mesh;

  geometry::boolean::Operation boolean_op;
  switch (trim_operation->mode) {
    case OperationType::Intersect:
      boolean_op = geometry::boolean::Operation::Intersect;
      break;
    case OperationType::Difference:
      boolean_op = geometry::boolean::Operation::Difference;
      break;
    case OperationType::Union:
      boolean_op = geometry::boolean::Operation::Union;
      break;
    case OperationType::Join:
      apply_join_operation(*object, sculpt_mesh, trim_mesh);
      return;
  }

  geometry::boolean::BooleanOpParameters op_params;
  op_params.boolean_mode = boolean_op;
  op_params.no_self_intersections = true;
  op_params.watertight = false;
  op_params.no_nested_components = true;
  geometry::boolean::BooleanError error;
  Mesh *result = geometry::boolean::mesh_boolean({&sculpt_mesh, &trim_mesh},
                                                 {float4x4::identity(), float4x4::identity()},
                                                 {Array<short>(), Array<short>()},
                                                 op_params,
                                                 trim_operation->solver_mode,
                                                 nullptr,
                                                 &error);
  if (error.type == geometry::boolean::BooleanErrorType::NonManifold) {
    BKE_report(trim_operation->reports, RPT_ERROR, "Solver requires a manifold mesh");
    return;
  }
  if (error.type == geometry::boolean::BooleanErrorType::ResultTooBig) {
    BKE_report(
        trim_operation->reports, RPT_ERROR, "Boolean result is too big for solver to handle");
    return;
  }
  if (error.type == geometry::boolean::BooleanErrorType::SolverNotAvailable) {
    BKE_report(
        trim_operation->reports, RPT_ERROR, "Boolean solver not available (compiled without it)");
    return;
  }
  if (error.type == geometry::boolean::BooleanErrorType::UnknownError) {
    BKE_report(trim_operation->reports, RPT_ERROR, "Unknown boolean error");
    return;
  }

  BKE_mesh_nomain_to_mesh(result, &sculpt_mesh, object);
}

static void gesture_apply_for_symmetry_pass(bContext & /*C*/, gesture::GestureData &gesture_data)
{
  TrimOperation *trim_operation = reinterpret_cast<TrimOperation *>(gesture_data.operation);
  Object &primary_object = *gesture_data.objects.first();
  Object &current_object = *gesture_data.vc.obact;

  Mesh *trim_mesh = trim_operation->mesh;
  MutableSpan<float3> positions = trim_mesh->vert_positions_for_write();
  for (int i = 0; i < trim_mesh->verts_num; i++) {
    if (gesture_data.use_shared_symmetry_frame) {
      /* Shared World/Cursor symmetry: mirror through the shared frame instead of the primary
       * object's own local axes -- matches the brush-stroke precedent in sculpt_multi_object.cc,
       * where even the reference object loses its "no round trip" shortcut once shared symmetry
       * is active. */
      const float3 world = math::transform_point(primary_object.object_to_world(),
                                                 float3(trim_operation->true_mesh_co[i]));
      const float3 mirrored_world = gesture::mirror_world_point(gesture_data, world);
      positions[i] = math::transform_point(current_object.world_to_object(), mirrored_world);
      continue;
    }

    const float3 primary_local = symmetry_flip(trim_operation->true_mesh_co[i],
                                               gesture_data.symmpass);
    if (&current_object == &primary_object) {
      /* Bit-exact with the pre-multi-object code: no extra world-space round trip. */
      positions[i] = primary_local;
    }
    else {
      /* Reproject the shared trim shape (built once in the primary object's local space) through
       * world space into the current object's own local space. */
      const float3 world = math::transform_point(primary_object.object_to_world(), primary_local);
      positions[i] = math::transform_point(current_object.world_to_object(), world);
    }
  }
  update_normals(gesture_data);
  apply_trim(gesture_data);
}

static void free_geometry(gesture::GestureData &gesture_data)
{
  TrimOperation *trim_operation = reinterpret_cast<TrimOperation *>(gesture_data.operation);
  BKE_id_free(nullptr, trim_operation->mesh);
  MEM_delete(trim_operation->true_mesh_co);
}

static void gesture_end(bContext & /*C*/, gesture::GestureData &gesture_data)
{
  TrimOperation *trim_operation = reinterpret_cast<TrimOperation *>(gesture_data.operation);

  for (Object *object : gesture_data.objects) {
    Mesh *mesh = id_cast<Mesh *>(object->data);

    /* Assign the shared face set ID (computed once across all objects in gesture_begin) to the
     * new faces created by the trim operation, so every object's added geometry gets the same
     * ID, as if the meshes were joined. */
    face_set::initialize_none_to_id(mesh, trim_operation->new_face_set_id);

    /* Per-object snapshot only -- do NOT use #undo::geometry_end here, it also finalizes/pushes
     * the whole step, which would leave every object after the first with no pending step to
     * write into. */
    undo::geometry_end_add_object(*object);
    BKE_sculptsession_free_pbvh(*object);
    BKE_mesh_batch_cache_dirty_tag(mesh, BKE_MESH_BATCH_DIRTY_ALL);
    DEG_id_tag_update(&object->id, ID_RECALC_GEOMETRY);
  }

  /* Finalize and push the multi-object step once, now that every object's snapshot has been
   * captured above (mirrors #finish_multi_object's use of the same generic helper). */
  undo::push_end_all_ex(false, true);

  free_geometry(gesture_data);
}

static void init_operation(gesture::GestureData &gesture_data, wmOperator &op)
{
  TrimOperation *trim_operation = reinterpret_cast<TrimOperation *>(gesture_data.operation);
  trim_operation->reports = op.reports;
  trim_operation->op.begin = gesture_begin;
  trim_operation->op.apply_for_symmetry_pass = gesture_apply_for_symmetry_pass;
  trim_operation->op.end = gesture_end;

  trim_operation->mode = OperationType(RNA_enum_get(op.ptr, "trim_mode"));
  trim_operation->use_cursor_depth = RNA_boolean_get(op.ptr, "use_cursor_depth");
  trim_operation->orientation = OrientationType(RNA_enum_get(op.ptr, "trim_orientation"));
  trim_operation->extrude_mode = ExtrudeMode(RNA_enum_get(op.ptr, "trim_extrude_mode"));
  trim_operation->solver_mode = geometry::boolean::Solver(RNA_enum_get(op.ptr, "trim_solver"));

  /* If the cursor was not over the mesh, force the orientation to view. */
  if (!trim_operation->initial_hit) {
    trim_operation->orientation = OrientationType::View;
  }

  if (gesture_data.shape_type == gesture::ShapeType::Line) {
    /* Line gestures only support Difference, no extrusion. */
    trim_operation->mode = OperationType::Difference;
  }
}

static void operator_properties(wmOperatorType *ot)
{
  PropertyRNA *prop;

  prop = RNA_def_int_vector(ot->srna,
                            "location",
                            2,
                            nullptr,
                            INT_MIN,
                            INT_MAX,
                            "Location",
                            "Mouse location",
                            INT_MIN,
                            INT_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);

  RNA_def_enum(ot->srna,
               "trim_mode",
               operation_types,
               int(OperationType::Difference),
               "Trim Mode",
               nullptr);
  RNA_def_boolean(
      ot->srna,
      "use_cursor_depth",
      false,
      "Use Cursor for Depth",
      "Use cursor location and radius for the dimensions and position of the trimming shape");
  RNA_def_enum(ot->srna,
               "trim_orientation",
               orientation_types,
               int(OrientationType::View),
               "Shape Orientation",
               nullptr);
  RNA_def_enum(ot->srna,
               "trim_extrude_mode",
               extrude_modes,
               int(ExtrudeMode::Fixed),
               "Extrude Mode",
               nullptr);

  RNA_def_enum(ot->srna,
               "trim_solver",
               solver_items,
               int(geometry::boolean::Solver::Manifold),
               "Solver",
               nullptr);
}

static bool can_invoke(const bContext &C)
{
  const View3D &v3d = *CTX_wm_view3d(&C);
  const Base &base = *CTX_data_active_base(&C);
  if (!BKE_base_is_visible(&v3d, &base)) {
    return false;
  }

  return true;
}

/* Filter the sculpt-mode objects down to the ones Trim can actually operate on (regular Mesh pbvh
 * with at least one face -- Multires and Dyntopo are not supported), reporting a warning for each
 * skipped object. An empty result means the whole gesture should be cancelled. */
static Vector<Object *> trimmable_objects(const ViewContext &vc, ReportList &reports)
{
  Vector<Object *> result;
  for (Object *object : sculpt_mode_objects(vc)) {
    /* The PBVH is only guaranteed to already exist for the active object (built lazily on cursor
     * hover, see #paint_cursor.cc). A secondary object that was never hovered/stroked before this
     * gesture would otherwise still have a null PBVH here. */
    const bke::pbvh::Tree &pbvh = bke::object::pbvh_ensure(*vc.depsgraph, *object);
    if (pbvh.type() == bke::pbvh::Type::BMesh) {
      BKE_reportf(&reports,
                  RPT_WARNING,
                  "Trim: skipping \"%s\" (not supported in dynamic topology mode)",
                  object->id.name + 2);
      continue;
    }
    if (pbvh.type() == bke::pbvh::Type::Grids) {
      BKE_reportf(&reports,
                  RPT_WARNING,
                  "Trim: skipping \"%s\" (not supported in multi-resolution mode)",
                  object->id.name + 2);
      continue;
    }
    const Mesh &mesh = *id_cast<const Mesh *>(object->data);
    if (mesh.faces_num == 0) {
      /* No geometry to trim or to detect a valid position for the trimming shape. */
      continue;
    }

    /* Checked per object, alongside the pbvh-type gates above, rather than aborting the whole
     * gesture -- mirrors the warn-and-skip treatment already given to BMesh/Grids objects here.
     *
     * The session check is needed in addition to the bake check, not instead of it:
     * #destructive_edit_check counts *layers* (see #bke::sculpt_layers::layers, which never
     * collects folders), while a session can be anchored on a folder — including an empty one. On
     * a mesh carrying no layers at all, an empty folder holding a mask therefore passes the bake
     * check while a session is open, and the changed element count then sends #mask_edit_end down
     * its destructive branch, which removes the user's own `.sculpt_mask` for good. */
    if (layers::mask_edit_refuse_deform(*object, &reports)) {
      /* #mask_edit_refuse_deform's own message doesn't name the object, so a per-object skip
       * would otherwise be indistinguishable from the whole gesture having failed. Matches the
       * BMesh/Grids warnings above in shape and wording. */
      BKE_reportf(&reports,
                  RPT_WARNING,
                  "Trim: skipping \"%s\" (weight-mask editing session is open)",
                  object->id.name + 2);
      continue;
    }

    if (!layers::destructive_edit_check(mesh, &reports)) {
      /* Same naming problem as the mask-session skip just above: #destructive_edit_check reports
       * for the mesh in general, not for which selected object it skipped. */
      BKE_reportf(&reports,
                  RPT_WARNING,
                  "Trim: skipping \"%s\" (sculpt layers not baked)",
                  object->id.name + 2);
      continue;
    }

    result.append(object);
  }
  return result;
}

static void initialize_cursor_info(bContext &C,
                                   const wmOperator &op,
                                   gesture::GestureData &gesture_data)
{
  Object &ob = *CTX_data_active_object(&C);

  vert_random_access_ensure(ob);

  int mval[2];
  RNA_int_get_array(op.ptr, "location", mval);

  const float mval_fl[2] = {float(mval[0]), float(mval[1])};

  TrimOperation *trim_operation = reinterpret_cast<TrimOperation *>(gesture_data.operation);
  Object *hit_ob = nullptr;
  const std::optional<CursorGeometryInfo> cgi = cursor_geometry_info_update(
      &C, mval_fl, false, &hit_ob);

  trim_operation->initial_hit = cgi.has_value();
  if (trim_operation->initial_hit) {
    copy_v3_v3(trim_operation->initial_location, cgi->location);
    copy_v3_v3(trim_operation->initial_normal, cgi->normal);
    trim_operation->hit_object = hit_ob;
  }
}

static wmOperatorStatus gesture_box_exec(bContext *C, wmOperator *op)
{
  std::unique_ptr<gesture::GestureData> gesture_data = gesture::init_from_box(C, op);
  if (!gesture_data) {
    return OPERATOR_CANCELLED;
  }

  gesture_data->objects = trimmable_objects(gesture_data->vc, *op->reports);
  if (gesture_data->objects.is_empty()) {
    return OPERATOR_CANCELLED;
  }

  gesture_data->operation = reinterpret_cast<gesture::Operation *>(
      MEM_new_zeroed<TrimOperation>(__func__));
  initialize_cursor_info(*C, *op, *gesture_data);
  init_operation(*gesture_data, *op);

  gesture::apply(*C, *gesture_data, *op);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus gesture_box_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  if (!can_invoke(*C)) {
    return OPERATOR_CANCELLED;
  }

  RNA_int_set_array(op->ptr, "location", event->mval);

  return WM_gesture_box_invoke(C, op, event);
}

static wmOperatorStatus gesture_lasso_exec(bContext *C, wmOperator *op)
{
  std::unique_ptr<gesture::GestureData> gesture_data = gesture::init_from_lasso(C, op);
  if (!gesture_data) {
    return OPERATOR_CANCELLED;
  }

  gesture_data->objects = trimmable_objects(gesture_data->vc, *op->reports);
  if (gesture_data->objects.is_empty()) {
    return OPERATOR_CANCELLED;
  }

  gesture_data->operation = reinterpret_cast<gesture::Operation *>(
      MEM_new_zeroed<TrimOperation>(__func__));
  initialize_cursor_info(*C, *op, *gesture_data);
  init_operation(*gesture_data, *op);

  gesture::apply(*C, *gesture_data, *op);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus gesture_lasso_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  if (!can_invoke(*C)) {
    return OPERATOR_CANCELLED;
  }

  RNA_int_set_array(op->ptr, "location", event->mval);

  return WM_gesture_lasso_invoke(C, op, event);
}

static wmOperatorStatus gesture_line_exec(bContext *C, wmOperator *op)
{
  std::unique_ptr<gesture::GestureData> gesture_data = gesture::init_from_line(C, op);
  if (!gesture_data) {
    return OPERATOR_CANCELLED;
  }

  gesture_data->objects = trimmable_objects(gesture_data->vc, *op->reports);
  if (gesture_data->objects.is_empty()) {
    return OPERATOR_CANCELLED;
  }

  gesture_data->operation = reinterpret_cast<gesture::Operation *>(
      MEM_new_zeroed<TrimOperation>(__func__));

  initialize_cursor_info(*C, *op, *gesture_data);
  init_operation(*gesture_data, *op);
  gesture::apply(*C, *gesture_data, *op);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus gesture_line_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  if (!can_invoke(*C)) {
    return OPERATOR_CANCELLED;
  }

  RNA_int_set_array(op->ptr, "location", event->mval);

  return WM_gesture_straightline_active_side_invoke(C, op, event);
}

static wmOperatorStatus gesture_polyline_exec(bContext *C, wmOperator *op)
{
  std::unique_ptr<gesture::GestureData> gesture_data = gesture::init_from_polyline(C, op);
  if (!gesture_data) {
    return OPERATOR_CANCELLED;
  }

  gesture_data->objects = trimmable_objects(gesture_data->vc, *op->reports);
  if (gesture_data->objects.is_empty()) {
    return OPERATOR_CANCELLED;
  }

  gesture_data->operation = reinterpret_cast<gesture::Operation *>(
      MEM_new_zeroed<TrimOperation>(__func__));
  initialize_cursor_info(*C, *op, *gesture_data);
  init_operation(*gesture_data, *op);

  gesture::apply(*C, *gesture_data, *op);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus gesture_polyline_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  if (!can_invoke(*C)) {
    return OPERATOR_CANCELLED;
  }

  RNA_int_set_array(op->ptr, "location", event->mval);

  return WM_gesture_polyline_invoke(C, op, event);
}

void SCULPT_OT_trim_lasso_gesture(wmOperatorType *ot)
{
  ot->name = "Trim Lasso Gesture";
  ot->idname = "SCULPT_OT_trim_lasso_gesture";
  ot->description = "Execute a boolean operation on the mesh and a shape defined by the cursor";

  ot->invoke = gesture_lasso_invoke;
  ot->modal = WM_gesture_lasso_modal;
  ot->exec = gesture_lasso_exec;

  ot->poll = sculpt_mode_poll_view3d;

  ot->flag = OPTYPE_REGISTER | OPTYPE_DEPENDS_ON_CURSOR;

  /* Properties. */
  WM_operator_properties_gesture_lasso(ot);
  gesture::operator_properties(ot, gesture::ShapeType::Lasso);

  operator_properties(ot);
}

void SCULPT_OT_trim_box_gesture(wmOperatorType *ot)
{
  ot->name = "Trim Box Gesture";
  ot->idname = "SCULPT_OT_trim_box_gesture";
  ot->description =
      "Execute a boolean operation on the mesh and a rectangle defined by the cursor";

  ot->invoke = gesture_box_invoke;
  ot->modal = WM_gesture_box_modal;
  ot->exec = gesture_box_exec;

  ot->poll = sculpt_mode_poll_view3d;

  ot->flag = OPTYPE_REGISTER;

  /* Properties. */
  WM_operator_properties_border(ot);
  gesture::operator_properties(ot, gesture::ShapeType::Box);

  operator_properties(ot);
}

void SCULPT_OT_trim_line_gesture(wmOperatorType *ot)
{
  ot->name = "Trim Line Gesture";
  ot->idname = "SCULPT_OT_trim_line_gesture";
  ot->description = "Remove a portion of the mesh on one side of a line";

  ot->invoke = gesture_line_invoke;
  ot->modal = WM_gesture_straightline_oneshot_modal;
  ot->exec = gesture_line_exec;

  ot->poll = sculpt_mode_poll_view3d;

  ot->flag = OPTYPE_REGISTER;

  /* Properties. */
  WM_operator_properties_gesture_straightline(ot, WM_CURSOR_EDIT);
  gesture::operator_properties(ot, gesture::ShapeType::Line);

  operator_properties(ot);
}

void SCULPT_OT_trim_polyline_gesture(wmOperatorType *ot)
{
  ot->name = "Trim Polyline Gesture";
  ot->idname = "SCULPT_OT_trim_polyline_gesture";
  ot->description =
      "Execute a boolean operation on the mesh and a polygonal shape defined by the cursor";

  ot->invoke = gesture_polyline_invoke;
  ot->modal = WM_gesture_polyline_modal;
  ot->exec = gesture_polyline_exec;

  ot->poll = sculpt_mode_poll_view3d;

  ot->flag = OPTYPE_REGISTER;

  /* Properties. */
  WM_operator_properties_gesture_polyline(ot);
  gesture::operator_properties(ot, gesture::ShapeType::Lasso);

  operator_properties(ot);
}
}  // namespace blender::ed::sculpt_paint::trim
