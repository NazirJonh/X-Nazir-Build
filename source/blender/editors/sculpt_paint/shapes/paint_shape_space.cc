/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Implementation of the shape spaces; see #paint_shape_space.hh.
 */

#include "paint_shape_space.hh"

#include <cfloat>
#include <cmath>

#include "BLI_index_mask.hh"
#include "BLI_index_range.hh"
#include "BLI_kdopbvh.hh"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_string.h"
#include "BLI_string_ref.hh"
#include "BLI_virtual_array.hh"

#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"

#include "BKE_attribute.hh"
#include "BKE_bvhutils.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_sample.hh"
#include "BKE_paint_bvh.hh"

#include "GEO_reverse_uv_sampler.hh"

#include "UI_view2d.hh"

namespace blender::ed::sculpt_paint::shape {

namespace {

/** The 2D Image Editor / PBR channel map space: reference-tile pixels of a UDIM tile. */
class CanvasUVSpace : public ShapeSpace {
 public:
  explicit CanvasUVSpace(const ShapeSpaceDesc &desc)
      : ref_tile_(desc.ref_tile), ref_tile_size_(desc.ref_tile_size)
  {
  }

  ShapeSpaceType type() const override
  {
    return ShapeSpaceType::CanvasUV;
  }

  bool project_point(const float3 & /*co_object*/,
                     const float3 & /*no_object*/,
                     float2 & /*r_p*/) const override
  {
    /* The canvas path maps texel UVs (#project_uv), not object-space points. */
    return false;
  }

  bool project_uv(const float2 &uv, float2 &r_p) const override
  {
    r_p = (uv - BKE_image_get_tile_uv_origin(ref_tile_)) * float2(ref_tile_size_);
    return true;
  }

  float units_per_world(const float3 & /*co_object*/) const override
  {
    /* Reference-tile pixels per UV unit; the screen-space stroke width uses it once wired. */
    return float(ref_tile_size_.x);
  }

  bool to_region(const float2 &p,
                 const ARegion &region,
                 const float4x4 & /*object_to_world*/,
                 float2 &r_region) const override
  {
    const float2 uv = p / float2(ref_tile_size_) + BKE_image_get_tile_uv_origin(ref_tile_);
    ui::view2d_view_to_region_fl(&region.v2d, uv.x, uv.y, &r_region.x, &r_region.y);
    return true;
  }

  ShapeViewBasis view_basis(const Object & /*ob*/) const override
  {
    return ShapeViewBasis{};
  }

  bool view_camera(const Object & /*ob*/, ViewProjectorCamera & /*r_camera*/) const override
  {
    return false;
  }

  bool view_projection(float4x4 & /*r_persmat*/, int2 & /*r_win_size*/) const override
  {
    return false;
  }

 private:
  int ref_tile_;
  int2 ref_tile_size_;
};

/** The frozen 3D viewport space: region pixels, projected through saved matrices. */
class ViewProjectorSpace : public ShapeSpace {
 public:
  explicit ViewProjectorSpace(const ShapeSpaceDesc &desc)
      : persmat_(desc.persmat),
        viewinv_(desc.viewinv),
        is_persp_(desc.is_persp),
        win_size_(desc.win_size),
        clip_start_(desc.clip_start),
        clip_end_(desc.clip_end)
  {
  }

  ShapeSpaceType type() const override
  {
    return ShapeSpaceType::ViewProjector;
  }

  bool project_point(const float3 &co_object,
                     const float3 & /*no_object*/,
                     float2 &r_p) const override
  {
    /* Equivalent to #ED_view3d_project_float_object / #ED_view3d_project_float_ex with
     * #V3D_PROJ_TEST_CLIP_NEAR, but from the saved matrices instead of a live #RegionView3D. */
    float vec4[4] = {co_object.x, co_object.y, co_object.z, 1.0f};
    mul_m4_v4(persmat_.ptr(), vec4);
    const float w = std::fabs(vec4[3]);
    if (vec4[2] <= -w) {
      return false;
    }
    const float scalar = (w != 0.0f) ? (1.0f / w) : 0.0f;
    r_p = float2(float(win_size_.x) * 0.5f * (1.0f + vec4[0] * scalar),
                 float(win_size_.y) * 0.5f * (1.0f + vec4[1] * scalar));
    return true;
  }

  bool project_uv(const float2 & /*uv*/, float2 & /*r_p*/) const override
  {
    return false;
  }

  float units_per_world(const float3 & /*co_object*/) const override
  {
    /* Region pixels per world unit at the view center; exact only for an orthographic view. */
    return float(win_size_.x);
  }

  bool to_region(const float2 &p,
                 const ARegion & /*region*/,
                 const float4x4 & /*object_to_world*/,
                 float2 &r_region) const override
  {
    /* The 3D shape is already in region pixels; its frozen matrix stays authoritative. */
    r_region = p;
    return true;
  }

  ShapeViewBasis view_basis(const Object &ob) const override
  {
    ShapeViewBasis basis;
    this->compute_view(ob, &basis, nullptr);
    return basis;
  }

  bool view_camera(const Object &ob, ViewProjectorCamera &r_camera) const override
  {
    this->compute_view(ob, nullptr, &r_camera);
    return true;
  }

  bool view_projection(float4x4 &r_persmat, int2 &r_win_size) const override
  {
    r_persmat = persmat_;
    r_win_size = win_size_;
    return true;
  }

 private:
  /** The single place the saved matrices become object-space camera data; both #view_basis and
   * #view_camera derive from it so the front-facing test and the occlusion ray cannot drift. */
  void compute_view(const Object &ob,
                    ShapeViewBasis *r_basis,
                    ViewProjectorCamera *r_camera) const
  {
    const float4x4 world_to_object = math::invert(ob.object_to_world());
    const float4x4 view_to_object = world_to_object * viewinv_;
    const float3 view_right = math::normalize(
        math::transform_direction(view_to_object, float3(1.0f, 0.0f, 0.0f)));
    const float3 position_object = math::transform_point(
        world_to_object, viewinv_.location());
    const float3 view_dir_object = math::normalize(
        math::transform_direction(view_to_object, float3(0.0f, 0.0f, 1.0f)));
    if (r_basis != nullptr) {
      r_basis->is_persp = is_persp_;
      r_basis->view_right = view_right;
      if (is_persp_) {
        r_basis->camera = position_object;
      }
      else {
        r_basis->view_normal = view_dir_object;
      }
    }
    if (r_camera != nullptr) {
      r_camera->is_persp = is_persp_;
      r_camera->position_object = position_object;
      r_camera->view_dir_object = view_dir_object;
      r_camera->clip_start = clip_start_;
      r_camera->clip_end = clip_end_;
    }
  }

  float4x4 persmat_;
  float4x4 viewinv_;
  bool is_persp_ = false;
  int2 win_size_ = int2(0);
  float clip_start_ = 0.0f;
  float clip_end_ = 1000.0f;
};

/** Surface-anchored space: the shape is authored in the tangent plane of #SurfaceAnchor; object
 * points are projected onto that plane (world units), and vertices whose normal faces away from
 * the anchor normal are rejected. No view: the overlay path is not wired for 3D. */
class SurfaceAnchoredSpace : public ShapeSpace {
 public:
  explicit SurfaceAnchoredSpace(const ShapeSpaceDesc &desc)
      : co_(desc.anchor.co),
        normal_(desc.anchor.normal),
        tangent_(desc.anchor.tangent),
        bitangent_(0.0f),
        px_per_unit_(desc.anchor.px_per_unit > 0.0f ? desc.anchor.px_per_unit : 1.0f),
        max_depth_(desc.anchor.max_depth),
        persmat_(desc.persmat),
        viewinv_(desc.viewinv),
        win_size_(desc.win_size)
  {
    /* The one shared frame (see #surface_anchor_frame): the space, the gestures and the cage
     * cannot drift apart. */
    const SurfaceAnchorFrame frame = surface_anchor_frame(desc.anchor.normal, desc.anchor.tangent);
    normal_ = frame.normal;
    tangent_ = frame.tangent;
    bitangent_ = frame.bitangent;
  }

  ShapeSpaceType type() const override
  {
    return ShapeSpaceType::SurfaceAnchored;
  }

  bool project_point(const float3 &co_object,
                     const float3 &no_object,
                     float2 &r_p) const override
  {
    /* The shared acceptance rules (see #surface_anchor_hit_accepts): the trace in the Image
     * Editor applies them to the same hits. */
    if (!surface_anchor_hit_accepts(co_, normal_, co_object, no_object, max_depth_)) {
      return false;
    }
    const float3 d = co_object - co_;
    r_p = float2(math::dot(d, tangent_), math::dot(d, bitangent_)) * px_per_unit_;
    return true;
  }

  bool project_uv(const float2 & /*uv*/, float2 & /*r_p*/) const override
  {
    return false;
  }

  float units_per_world(const float3 & /*co_object*/) const override
  {
    return px_per_unit_;
  }

  bool to_region(const float2 &p,
                 const ARegion &region,
                 const float4x4 &object_to_world,
                 float2 &r_region) const override
  {
    /* Plane pixels -> object-space point on the plane -> world -> region through the *current*
     * view (the stored persmat is only for px_per_unit / tangent at creation). */
    const float3 co_object =
        co_ + tangent_ * (p.x / px_per_unit_) + bitangent_ * (p.y / px_per_unit_);
    const float3 co_world = math::transform_point(object_to_world, co_object);
    const RegionView3D *rv3d = static_cast<const RegionView3D *>(region.regiondata);
    if (rv3d == nullptr) {
      return false;
    }
    float vec4[4] = {co_world.x, co_world.y, co_world.z, 1.0f};
    mul_m4_v4(rv3d->persmat, vec4);
    const float w = std::fabs(vec4[3]);
    if (vec4[2] <= -w) {
      return false;
    }
    const float scalar = (w != 0.0f) ? (1.0f / w) : 0.0f;
    r_region = float2(float(region.winx) * 0.5f * (1.0f + vec4[0] * scalar),
                      float(region.winy) * 0.5f * (1.0f + vec4[1] * scalar));
    return true;
  }

  ShapeViewBasis view_basis(const Object & /*ob*/) const override
  {
    ShapeViewBasis basis;
    basis.always_front_facing = true;
    return basis;
  }

  bool view_camera(const Object & /*ob*/, ViewProjectorCamera & /*r_camera*/) const override
  {
    return false;
  }

  bool view_projection(float4x4 &r_persmat, int2 &r_win_size) const override
  {
    r_persmat = persmat_;
    r_win_size = win_size_;
    return true;
  }

 private:
  float3 co_;
  float3 normal_;
  float3 tangent_;
  float3 bitangent_;
  float px_per_unit_ = 1.0f;
  float max_depth_ = 0.0f;
  float4x4 persmat_;
  float4x4 viewinv_;
  int2 win_size_ = int2(0);
};

}  // namespace

std::unique_ptr<ShapeSpace> shape_space_create(const ShapeSpaceDesc &desc)
{
  switch (desc.type) {
    case ShapeSpaceType::ViewProjector:
      return std::make_unique<ViewProjectorSpace>(desc);
    case ShapeSpaceType::SurfaceAnchored:
      return std::make_unique<SurfaceAnchoredSpace>(desc);
    case ShapeSpaceType::CanvasUV:
      return std::make_unique<CanvasUVSpace>(desc);
  }
  return std::make_unique<CanvasUVSpace>(desc);
}

bool surface_anchor_restore(const Object &ob,
                            const Depsgraph &depsgraph,
                            SurfaceAnchor &r_anchor)
{
  if (ob.type != OB_MESH || ob.data == nullptr) {
    return false;
  }
  const Mesh &mesh = *id_cast<const Mesh *>(ob.data);
  const StringRefNull uv_name = (r_anchor.surface_uv_map[0] != '\0') ?
                                    StringRefNull(r_anchor.surface_uv_map) :
                                    mesh.active_or_default_uv_map_name();
  const bke::AttributeAccessor attributes = mesh.attributes();
  const bke::AttributeReader<float2> uv_attribute = attributes.lookup<float2>(
      uv_name, bke::AttrDomain::Corner);
  VArraySpan<float2> uv_map;
  if (uv_attribute) {
    uv_map = VArraySpan<float2>(*uv_attribute);
  }
  const Span<float3> positions = bke::pbvh::vert_positions_eval(depsgraph, ob);
  return surface_anchor_restore_from_mesh(
      uv_map, mesh.corner_tris(), mesh.corner_verts(), positions, r_anchor);
}

bool surface_anchor_cache_fill(const Object &ob,
                               const Depsgraph &depsgraph,
                               const int face_hint,
                               SurfaceAnchor &r_anchor)
{
  if (ob.type != OB_MESH || ob.data == nullptr) {
    return false;
  }
  const Mesh &mesh = *id_cast<const Mesh *>(ob.data);
  /* The evaluated positions: the same source as the raycast that produced the anchor and as the
   * backends, so the triangle and the barycentric weights stay consistent (an original-mesh
   * search mixed undeformed triangles with evaluated positions). */
  const Span<float3> positions = bke::pbvh::vert_positions_eval(depsgraph, ob);
  const Span<int3> corner_tris = mesh.corner_tris();
  const Span<int> corner_verts = mesh.corner_verts();

  int tri_index = -1;
  if (face_hint >= 0 && face_hint < mesh.faces_num) {
    float best_dist_sq = FLT_MAX;
    for (const int i : bke::mesh::face_triangles_range(mesh.faces(), face_hint)) {
      const int3 tri = corner_tris[i];
      float3 closest;
      closest_on_tri_to_point_v3(closest,
                                 r_anchor.co,
                                 positions[corner_verts[tri[0]]],
                                 positions[corner_verts[tri[1]]],
                                 positions[corner_verts[tri[2]]]);
      const float dist_sq = math::distance_squared(closest, r_anchor.co);
      if (dist_sq < best_dist_sq) {
        best_dist_sq = dist_sq;
        tri_index = i;
      }
    }
  }
  if (tri_index < 0) {
    const IndexMask all_faces(IndexRange(mesh.faces_num));
    bke::BVHTreeFromMesh bvh = bke::bvhtree_from_mesh_corner_tris_ex(
        positions, mesh.faces(), corner_verts, corner_tris, all_faces);
    if (bvh.tree != nullptr) {
      BVHTreeNearest nearest{};
      nearest.index = -1;
      nearest.dist_sq = FLT_MAX;
      BLI_bvhtree_find_nearest(bvh.tree, r_anchor.co, &nearest, bvh.nearest_callback, &bvh);
      tri_index = nearest.index;
    }
  }
  if (tri_index < 0) {
    return false;
  }

  const int3 tri = corner_tris[tri_index];
  /* The anchor point is on the evaluated surface, so its barycentric weights on the triangle are
   * exact (the closest point on the triangle would drift). `bary[i]` is the weight of `tri[i]`,
   * which is how #surface_anchor_restore and #sample_corner_attribute_with_bary_coords read it. */
  float3 bary;
  interp_weights_tri_v3(bary,
                        positions[corner_verts[tri[0]]],
                        positions[corner_verts[tri[1]]],
                        positions[corner_verts[tri[2]]],
                        r_anchor.co);
  r_anchor.bary = bary;
  r_anchor.tri = tri_index;
  r_anchor.tri_corners = tri;

  const StringRefNull uv_name = mesh.active_or_default_uv_map_name();
  const bke::AttributeReader<float2> uv_attribute = mesh.attributes().lookup<float2>(
      uv_name, bke::AttrDomain::Corner);
  if (uv_attribute) {
    const VArraySpan<float2> uv_map(*uv_attribute);
    r_anchor.surface_uv = bke::mesh_surface_sample::sample_corner_attribute_with_bary_coords(
        r_anchor.bary, tri, uv_map);
    BLI_strncpy(r_anchor.surface_uv_map, uv_name.c_str(), sizeof(r_anchor.surface_uv_map));
    r_anchor.has_surface_uv = true;
  }
  return true;
}

bool surface_anchor_restore_from_mesh(const Span<float2> uv_map,
                                      const Span<int3> corner_tris,
                                      const Span<int> corner_verts,
                                      const Span<float3> positions,
                                      SurfaceAnchor &r_anchor)
{
  /* Preferred path: the saved triangle + barycentric. Unambiguous (one triangle, not one UV that
   * may map to several places) and it follows mesh deformation. Trusted only while the topology
   * is unchanged, checked by the triangle's corner indices. */
  if (r_anchor.tri >= 0 && r_anchor.tri < corner_tris.size()) {
    const int3 tri = corner_tris[r_anchor.tri];
    if (tri.x == r_anchor.tri_corners.x && tri.y == r_anchor.tri_corners.y &&
        tri.z == r_anchor.tri_corners.z)
    {
      const float3 v0 = positions[corner_verts[tri[0]]];
      const float3 v1 = positions[corner_verts[tri[1]]];
      const float3 v2 = positions[corner_verts[tri[2]]];
      const float3 cross = math::cross(v1 - v0, v2 - v0);
      if (math::length_squared(cross) >= SURFACE_ANCHOR_DEGENERATE_EPS) {
        r_anchor.co = v0 * r_anchor.bary.x + v1 * r_anchor.bary.y + v2 * r_anchor.bary.z;
        r_anchor.normal = math::normalize(cross);
        return true;
      }
    }
  }

  /* Fallback (no triangle cache, or the topology changed): reverse-sample the saved UV. Ambiguous
   * when UVs overlap or are mirrored, so this is a last resort. */
  if (!r_anchor.has_surface_uv || uv_map.is_empty()) {
    return false;
  }
  const geometry::ReverseUVSampler sampler{uv_map, corner_tris};
  const geometry::ReverseUVSampler::Result result = sampler.sample(r_anchor.surface_uv);
  if (result.type != geometry::ReverseUVSampler::ResultType::Ok) {
    return false;
  }
  /* `corner_tris` holds *corner* indices; map them to vertices for the positions. */
  const int3 tri = corner_tris[result.tri_index];
  const float3 v0 = positions[corner_verts[tri[0]]];
  const float3 v1 = positions[corner_verts[tri[1]]];
  const float3 v2 = positions[corner_verts[tri[2]]];
  const float3 cross = math::cross(v1 - v0, v2 - v0);
  if (math::length_squared(cross) < SURFACE_ANCHOR_DEGENERATE_EPS) {
    return false;
  }
  const float3 bary = result.bary_weights;
  r_anchor.co = v0 * bary.x + v1 * bary.y + v2 * bary.z;
  r_anchor.normal = math::normalize(cross);
  r_anchor.tri = result.tri_index;
  r_anchor.tri_corners = tri;
  r_anchor.bary = bary;
  return true;
}

}  // namespace blender::ed::sculpt_paint::shape
