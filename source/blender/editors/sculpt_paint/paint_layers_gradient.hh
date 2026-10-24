/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup edsculpt
 *
 * The Gradient Layer tools: a straight-line gesture that writes the line of the active Gradient
 * row, and the sampling helpers its persistent gizmo shares with it. The row stays procedural;
 * the tools only edit its parameters.
 */

#include "BLI_math_vector_types.hh"

namespace blender {

struct bContext;
struct Material;
struct MaterialPaintLayer;
struct MaterialPaintLayerGradient;
struct Mesh;
struct Object;
struct wmOperatorType;

/**
 * One point sampled from the view. The position and normal are in object space; the UV is the
 * active UV map coordinate at the same point. The Image Editor only fills the UV fields, because a
 * 2D view has no surface under the cursor.
 */
struct PaintLayerGradientHit {
  /** Object-space position on the surface (Image Editor: `(u, v, 0)`). */
  float3 position = float3(0.0f);
  /** Object-space surface normal; only meaningful with #has_normal. */
  float3 normal = float3(0.0f);
  /** Active UV map coordinate at the hit; only meaningful with #has_uv. */
  float2 uv = float2(0.0f);
  /** The mesh triangle of a 3D View hit, or -1. */
  int tri = -1;
  /** Whether #position is usable. */
  bool valid = false;
  /** Whether #normal is usable (3D View raycast hits only). */
  bool has_normal = false;
  /** Whether #uv is usable (a 3D hit without a UV map is still a valid surface point). */
  bool has_uv = false;
};

/** The active layered material and its active Gradient row, or null. */
Material *paint_layer_gradient_target(const bContext *C, MaterialPaintLayer **r_layer);

/** Whether the active row is a Gradient generator the layer tools can edit. */
bool paint_layer_gradient_active(const bContext *C);

/**
 * The sampled point under \a mval. The Image Editor is UV-only; the 3D View raycasts the active
 * mesh and reports object-space position, normal and the active UV map coordinate.
 */
bool paint_layer_gradient_hit_at_mouse(bContext &C,
                                       const MaterialPaintLayerGradient &gradient,
                                       const float2 &mval,
                                       PaintLayerGradientHit &r_hit);

/**
 * The evaluated mesh of \a ob: the geometry the material is shaded on, which every point the tools
 * and the gizmo handle has to be measured against. Null when it is not available.
 */
const Mesh *paint_layer_gradient_mesh_eval(const bContext &C, const Object &ob);

/**
 * Convert a sampled hit into the row's own space (UV, object or generated). \a mesh is the
 * evaluated mesh (see #paint_layer_gradient_mesh_eval); \a object is only used for the bounds
 * fallback of a Generated point and may be null.
 */
bool paint_layer_gradient_point_from_hit(const MaterialPaintLayerGradient &gradient,
                                         const Object *object,
                                         const Mesh *mesh,
                                         const PaintLayerGradientHit &hit,
                                         float3 &r_point);

/**
 * The row-space point under \a mval: the surface hit when the cursor is on the mesh, otherwise the
 * point on the tangent plane of \a plane_source (null disables the fallback; UV rows have none).
 */
bool paint_layer_gradient_point_at_mouse(bContext &C,
                                         const MaterialPaintLayerGradient &gradient,
                                         Object *ob,
                                         const Mesh *mesh,
                                         const float2 &mval,
                                         const PaintLayerGradientHit *plane_source,
                                         float3 &r_point);

/**
 * Fallback for a drag whose end misses the mesh: intersect the end view ray with the tangent plane
 * of the first hit. The result is an object-space surface point.
 */
bool paint_layer_gradient_plane_hit(const bContext &C,
                                    Object &ob,
                                    const PaintLayerGradientHit &start_hit,
                                    const float2 &mval,
                                    float3 &r_object_point);

/**
 * The UV the view ray under \a mval reaches on the plane of triangle \a tri_index of \a mesh,
 * continued past the triangle: the free placement of a UV row, which may leave the island and the
 * mesh. False for a degenerate triangle, a mesh without a UV map or a ray parallel to the plane.
 */
bool paint_layer_gradient_uv_on_tri_plane(const bContext &C,
                                          Object &ob,
                                          const Mesh &mesh,
                                          int tri_index,
                                          const float2 &mval,
                                          float2 &r_uv);

/**
 * A row-space point in object space. A UV point outside every island lands on the nearest island
 * point, so the overlay can always place it; see #paint_layer_gradient_world_point for
 * \a r_tri_cache.
 */
bool paint_layer_gradient_object_point(const Object &ob,
                                       const Mesh &mesh,
                                       const MaterialPaintLayerGradient &gradient,
                                       const float3 &point,
                                       int &r_tri_cache,
                                       float3 &r_object);

/**
 * A row-space point in world space, for the overlay. \a r_tri_cache keeps the triangle a UV point
 * was last found in, so a repeated call is constant time; pass -1 to start a search. Only a mesh
 * without a UV map cannot place a UV point and returns false.
 */
bool paint_layer_gradient_world_point(const Object &ob,
                                      const Mesh &mesh,
                                      const MaterialPaintLayerGradient &gradient,
                                      const float3 &point,
                                      int &r_tri_cache,
                                      float3 &r_world);

void PAINT_OT_paint_layer_gradient(wmOperatorType *ot);

}  // namespace blender
