/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * The space a shape lives in: how a mesh element (vertex, texel) or a UV coordinate is mapped
 * into the 2D space the shape's #PaintShape geometry is authored in, and back into the region for
 * the overlay. This is the abstraction that lets the same shape and the same backends serve the
 * Image Editor (reference-tile pixels), the frozen 3D viewport (region pixels) and, later,
 * surface-anchored shapes.
 *
 * A #ShapeSpaceDesc is the serializable description (for the operator properties, F9 and the ID);
 * a #ShapeSpace is the runtime object built from it and the active #Object / #Depsgraph.
 */

#pragma once

#include <memory>

#include "BLI_math_matrix.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"

#include "paint_shape.hh"

namespace blender {

struct ARegion;
struct Depsgraph;
struct Object;

}  // namespace blender

namespace blender::ed::sculpt_paint::shape {

enum class ShapeSpaceType : int8_t {
  /** Reference-tile pixels of a UDIM tile: the 2D Image Editor and PBR channel maps. */
  CanvasUV,
  /** Region pixels of a frozen viewport: the 3D Sculpt tools. */
  ViewProjector,
  /** A tangent plane anchored to the mesh surface: surface-anchored shapes. */
  SurfaceAnchored,
};

/**
 * Back-side rejection threshold of the SurfaceAnchored space: a vertex is dropped when its normal
 * faces away from the anchor normal (`dot(vertex_normal, anchor_normal) < threshold`). Exposed as a
 * named constant (not a magic 0) so the behaviour is one place to tune.
 */
constexpr float SURFACE_ANCHOR_BACKFACE_DOT = 0.0f;

/** Rejection depth along the anchor normal, as a fraction of the shape's world-space size (larger
 * half-axis). Surfaces further behind the plane than this are not painted; a small floor keeps a
 * flat shape usable. */
constexpr float SURFACE_ANCHOR_MAX_DEPTH_FACTOR = 0.5f;
constexpr float SURFACE_ANCHOR_MIN_DEPTH = 1e-4f;

/** A triangle whose cross product is shorter than this (object units, squared) is degenerate and
 * cannot supply an anchor frame; the caller falls back to the cached anchor. */
constexpr float SURFACE_ANCHOR_DEGENERATE_EPS = 1e-12f;

/**
 * The surface-hit acceptance rules of the SurfaceAnchored space, shared by the space's vertex
 * projection (#SurfaceAnchoredSpace::project_point) and the Image-Editor contour trace, so the
 * contour in UV cannot drift from what the bake actually writes: the hit must face the anchor
 * normal (the #SURFACE_ANCHOR_BACKFACE_DOT test) and sit within the anchor's rejection depth
 * along the plane normal (0 = unlimited). Pure.
 */
inline bool surface_anchor_hit_accepts(const float3 &anchor_co,
                                       const float3 &anchor_normal,
                                       const float3 &hit_co,
                                       const float3 &hit_normal,
                                       const float max_depth)
{
  if (math::dot(hit_normal, anchor_normal) < SURFACE_ANCHOR_BACKFACE_DOT) {
    return false;
  }
  if (max_depth > 0.0f && math::abs(math::dot(hit_co - anchor_co, anchor_normal)) > max_depth) {
    return false;
  }
  return true;
}

/**
 * A SurfaceAnchored anchor: a point on the mesh surface and the local tangent plane. The frame is
 * cached so a bake needs no mesh; `surface_uv` + `tri` are the restoration cache used after a
 * remesh / reload (`geometry::ReverseUVSampler`).
 */
struct SurfaceAnchor {
  float3 co = float3(0.0f);
  float3 normal = float3(0.0f, 0.0f, 1.0f);
  float3 tangent = float3(1.0f, 0.0f, 0.0f);
  /** Object-space UV of the anchor and the hit triangle (restoration cache; -1 = unset). */
  float2 surface_uv = float2(0.0f);
  int tri = -1;
  /** Corner indices of #tri at creation, to detect a topology change before trusting it. */
  int3 tri_corners = int3(-1, -1, -1);
  /** Barycentric weights of #co on #tri (evaluated positions), the unambiguous restore path. */
  float3 bary = float3(0.0f);
  /** True when #surface_uv / #tri are valid (a UV map was available at creation); without it a
   * restore would sample (0,0) and move the anchor to a UV island corner. */
  bool has_surface_uv = false;
  /** Region pixels per world unit at the anchor: the shape is authored in "plane pixels", so an
   * 8 px stroke stays 8 px on the surface regardless of zoom. */
  float px_per_unit = 1.0f;
  /** Rejection depth along #normal, world units (0 = unlimited); surfaces behind the plane are
   * skipped so a shape does not paint unrelated geometry within its outline. */
  float max_depth = 0.0f;
  /** UV map the anchor was sampled through; empty = the object's active UV map. */
  char surface_uv_map[64] = "";
};

/**
 * The orthonormal plane frame of a #SurfaceAnchor: the normal normalized, the tangent
 * orthonormalized against it (a stale or view-tilted stored tangent cannot skew the plane) and
 * the bitangent as `normal x tangent`. This is the single definition of the anchor frame: the
 * creation gesture, the session gestures, the 3D cage and the #SurfaceAnchoredSpace projection
 * all build their basis through it, so the drawn contour, the cage and the baked shape always
 * agree, on any surface inclination.
 */
struct SurfaceAnchorFrame {
  float3 normal = float3(0.0f, 0.0f, 1.0f);
  float3 tangent = float3(1.0f, 0.0f, 0.0f);
  float3 bitangent = float3(0.0f, 1.0f, 0.0f);
};

inline SurfaceAnchorFrame surface_anchor_frame(const float3 &normal, const float3 &tangent)
{
  SurfaceAnchorFrame frame;
  if (math::length_squared(normal) > 0.0f) {
    frame.normal = math::normalize(normal);
  }
  const float3 tangent_perp = tangent - frame.normal * math::dot(tangent, frame.normal);
  if (math::length_squared(tangent_perp) > 0.0f) {
    frame.tangent = math::normalize(tangent_perp);
  }
  else {
    /* The tangent is parallel to the normal (or unset): pick any perpendicular. */
    frame.tangent = math::normalize(math::cross(frame.normal, float3(0.0f, 0.0f, 1.0f)));
    if (!(math::length_squared(frame.tangent) > 0.0f)) {
      frame.tangent = float3(1.0f, 0.0f, 0.0f);
    }
  }
  frame.bitangent = math::cross(frame.normal, frame.tangent);
  return frame;
}

/**
 * Serializable description of a shape's space. POD: copied into operator properties and, later,
 * the vector ID. The unused fields for a given #type are ignored.
 */
struct ShapeSpaceDesc {
  ShapeSpaceType type = ShapeSpaceType::CanvasUV;

  /* CanvasUV. */
  int ref_tile = 1001;
  int2 ref_tile_size = int2(1024);

  /* ViewProjector: the frozen view. `persmat` is the object-space projection matrix (what
   * `rv3d->persmatob` holds at creation); `viewinv` the camera-to-world matrix for the front-
   * facing side and the normal write basis. */
  float4x4 persmat = float4x4::identity();
  float4x4 viewinv = float4x4::identity();
  bool is_persp = false;
  int2 win_size = int2(0);
  /** Frozen view clip range, used by the occlusion ray of a non-perspective projector. */
  float clip_start = 0.0f;
  float clip_end = 1000.0f;

  /* SurfaceAnchored: the tangent-plane frame and its surface restoration cache. */
  SurfaceAnchor anchor = {};
};

/**
 * Object-space view basis for the front-facing test. Orthographic views share one direction; a
 * perspective view needs the camera position, since the direction to it changes per vertex.
 */
struct ShapeViewBasis {
  bool is_persp = false;
  /** Orthographic: object-space direction toward the viewer. */
  float3 view_normal = float3(0.0f, 0.0f, 1.0f);
  /** Perspective: object-space camera position. */
  float3 camera = float3(0.0f);
  /** Object-space camera right; the fallback screen axis of #build_normal_write_basis. */
  float3 view_right = float3(1.0f, 0.0f, 0.0f);
  /** SurfaceAnchored: the view does not gate visibility; the space's own back-face test does. */
  bool always_front_facing = false;

  bool front_facing(const float3 &position, const float3 &normal) const
  {
    if (always_front_facing) {
      return true;
    }
    const float3 to_viewer = is_persp ? math::normalize(camera - position) : view_normal;
    return math::dot(normal, to_viewer) > 0.0f;
  }
};

/**
 * Object-space camera of a frozen projector, for the occlusion ray: perspective stores the camera
 * position, orthographic the view direction. The clip range bounds a non-perspective ray.
 */
struct ViewProjectorCamera {
  bool is_persp = false;
  /** Perspective: object-space camera position. */
  float3 position_object = float3(0.0f);
  /** Orthographic: object-space direction toward the viewer. */
  float3 view_dir_object = float3(0.0f, 0.0f, 1.0f);
  float clip_start = 0.0f;
  float clip_end = 1000.0f;
};

/** Runtime space: maps object-space points (and UVs) into the shape's 2D space and back. */
class ShapeSpace {
 public:
  virtual ~ShapeSpace() = default;

  virtual ShapeSpaceType type() const = 0;

  /**
   * Map an object-space point into the shape's 2D space. \a no_object is the object-space normal,
   * used by surface spaces for the tangent-plane / back-face test; the others ignore it.
   * \return false when the point has no image in this space (behind the camera, wrong side).
   */
  virtual bool project_point(const float3 &co_object,
                             const float3 &no_object,
                             float2 &r_p) const = 0;

  /** Map a UV coordinate into the shape's 2D space (the Image Editor / channel maps path). */
  virtual bool project_uv(const float2 &uv, float2 &r_p) const = 0;

  /**
   * Scale from world units to the shape's 2D space at \a co_object, for a stroke width authored
   * in screen/region units. Returns 1.0 when the space has no such scale.
   */
  virtual float units_per_world(const float3 &co_object) const = 0;

  /**
   * Map a shape-space point back to region pixels, for the overlay / hit tests.
   *
   * \param object_to_world: current object matrix; SurfaceAnchored needs it (the plane is object
   * space), CanvasUV / ViewProjector ignore it.
   * \param region: the live region; SurfaceAnchored reads the *current* view from
   * `region.regiondata` (a RegionView3D) so the shape follows the model when the view rotates.
   */
  virtual bool to_region(const float2 &p,
                         const ARegion &region,
                         const float4x4 &object_to_world,
                         float2 &r_region) const = 0;

  /** The front-facing / normal-write basis for \a ob in this space. */
  virtual ShapeViewBasis view_basis(const Object &ob) const = 0;

  /** The occlusion camera for \a ob; false when the space has no view (CanvasUV). */
  virtual bool view_camera(const Object &ob, ViewProjectorCamera &r_camera) const = 0;

  /** The frozen projector's object-space projection matrix and region size, for the normal write
   * basis; false when the space is not a projector. */
  virtual bool view_projection(float4x4 &r_persmat, int2 &r_win_size) const = 0;
};

/**
 * Object-space pixels per object unit from a world pixels-per-unit, scaled by the object matrix
 * along the two plane axes. With a non-uniform object scale the mean of the axes is used (the same
 * rule as the stroke width, O2). Kept as a free function so the scale behaviour is unit-testable.
 */
inline float surface_anchor_px_per_unit(const float px_per_unit_world,
                                        const float4x4 &object_to_world,
                                        const float3 &tangent_object,
                                        const float3 &bitangent_object)
{
  const float world_per_obj_t = math::length(
      math::transform_direction(object_to_world, tangent_object));
  const float world_per_obj_b = math::length(
      math::transform_direction(object_to_world, bitangent_object));
  return px_per_unit_world * 0.5f * (world_per_obj_t + world_per_obj_b);
}

/** Build the runtime space described by \a desc for \a ob. Never null; unknown types fall back to
 * a CanvasUV space so a stale operator property cannot crash a draw. */
std::unique_ptr<ShapeSpace> shape_space_create(const ShapeSpaceDesc &desc);

/**
 * Refresh a SurfaceAnchored anchor from the mesh (restoration after reload): the saved triangle +
 * barycentric is the preferred path (unambiguous, follows a deformation), the saved UV through
 * `geometry::ReverseUVSampler` the fallback. The anchor's object position / normal are recomputed
 * from the **evaluated** vertex positions (the same ones the backends sample, so the anchor
 * follows a deformation); the tangent is left as stored and the space orthonormalizes it.
 *
 * \return false (and leaves \a r_anchor unchanged) when the surface cannot be restored: the
 * topology changed and the UV sample is missing or ambiguous. The caller decides what that means
 * (a refusal with its own message -- a bake must not silently land on a stale anchor).
 */
bool surface_anchor_restore(const Object &ob, const Depsgraph &depsgraph, SurfaceAnchor &r_anchor);

/**
 * Fill the restoration cache of \a r_anchor (`tri`, `tri_corners`, `bary`, `surface_uv`,
 * `surface_uv_map`, `has_surface_uv`) for the point `r_anchor.co`, which must lie on the evaluated
 * surface of \a ob. The nearest triangle is searched among the triangles of \a face_hint when it is
 * a valid face (the face under the cursor: cheap), otherwise over the whole mesh through a BVH.
 * Leaves the cache unset (tri = -1) and returns false when no triangle is found.
 */
bool surface_anchor_cache_fill(const Object &ob,
                               const Depsgraph &depsgraph,
                               int face_hint,
                               SurfaceAnchor &r_anchor);

/**
 * Pure core of #surface_anchor_restore, decoupled from the mesh API so it can be unit-tested:
 * sample `r_anchor.surface_uv` through `geometry::ReverseUVSampler` over \a uv_map (corner domain)
 * and \a corner_tris (corner indices), then recompute `co` / `normal` / `tri` from \a positions
 * indexed through \a corner_verts. Returns false (anchor unchanged) on a missing / ambiguous
 * sample or a degenerate triangle.
 */
bool surface_anchor_restore_from_mesh(Span<float2> uv_map,
                                      Span<int3> corner_tris,
                                      Span<int> corner_verts,
                                      Span<float3> positions,
                                      SurfaceAnchor &r_anchor);

}  // namespace blender::ed::sculpt_paint::shape
