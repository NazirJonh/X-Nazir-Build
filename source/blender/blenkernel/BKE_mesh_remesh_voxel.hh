/* SPDX-FileCopyrightText: 2019 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#pragma once

#include "BLI_math_vector_types.hh"
#include "BLI_offset_indices.hh"
#include "BLI_span.hh"

namespace blender {

struct Mesh;
struct ModifierData;
struct Object;
struct ReportList;

Mesh *BKE_mesh_remesh_voxel_fix_poles(const Mesh *mesh);
Mesh *BKE_mesh_remesh_voxel(const Mesh *mesh,
                            float voxel_size,
                            float adaptivity,
                            float isovalue,
                            const Object *object,
                            ModifierData *modifier_data);
Mesh *BKE_mesh_remesh_voxel(
    const Mesh *mesh, float voxel_size, float adaptivity, float isovalue, ReportList *reports);
/**
 * Remesh \a mesh into quads using QuadriFlow.
 *
 * \param guide_dirs: Optional per-vertex orientation guidance, `mesh->verts_num * 3` floats in
 * object space, biasing the output edge flow. May be null.
 * \param guide_weights: Optional per-vertex guide strength, `mesh->verts_num` floats in [0, 1];
 * 0 means "no constraint". May be null. Both guide arrays must be provided together to take effect.
 * \param guide_pin_weights: Optional per-vertex positional pins, `mesh->verts_num` floats in
 * [0, 1], for vertices on feature lines (e.g. face set boundaries) that the output geometry must
 * pass through; a pinned vertex still slides along its guide direction. May be null.
 * \param guide_scales: Optional per-vertex relative edge-length multipliers (`mesh->verts_num`
 * floats) driving adaptive quad density: < 1 gives smaller quads, > 1 larger ones, 1 is neutral.
 * May be null for uniform density.
 */
Mesh *BKE_mesh_remesh_quadriflow(const Mesh *mesh,
                                 int target_faces,
                                 int seed,
                                 bool preserve_sharp,
                                 bool preserve_boundary,
                                 bool adaptive_scale,
                                 void (*update_cb)(void *, float progress, int *cancel),
                                 void *update_cb_data,
                                 const float *guide_dirs = nullptr,
                                 const float *guide_weights = nullptr,
                                 const float *guide_pin_weights = nullptr,
                                 const float *guide_scales = nullptr);

namespace bke {
void mesh_remesh_reproject_attributes(const Mesh &src, Mesh &dst);

/**
 * Compute a per-vertex orientation guide from surface curvature, for biasing a
 * cross-field remesher (QuadriFlow) toward the natural edge flow.
 *
 * The dominant principal-curvature direction is estimated per vertex from the
 * variation of face normals in its one-ring, then the resulting line field is
 * smoothed across the surface in a period-4 (4-RoSy) representation. Flat and
 * spherical (umbilic) regions, where the direction is undefined, receive ≈0
 * weight.
 *
 * \param strength: Global multiplier for the confidence weight, in [0, 1].
 * \param r_dirs: Object-space guide direction per vertex (length `verts_num`).
 * \param r_weights: Guide weight per vertex in [0, 1] (length `verts_num`).
 */
void mesh_curvature_guide_field(const Mesh &mesh,
                                float strength,
                                MutableSpan<float3> r_dirs,
                                MutableSpan<float> r_weights);

/**
 * Rasterize guide strokes (polylines lying on or near the surface) into a
 * per-vertex orientation guide for the cross-field remesher. Each vertex within
 * \a radius of a stroke segment takes the segment tangent (projected onto its
 * tangent plane) as a soft constraint, weighted by \a strength and a distance
 * falloff.
 *
 * \param stroke_points: Stroke points in the mesh's object space.
 * \param stroke_offsets: Point range of each stroke into \a stroke_points.
 * \param radius: Radius of influence around each segment, in object space.
 * \param strength: Base guide weight in [0, 1].
 * \param r_dirs, r_weights: Per-vertex guide arrays, updated in place (length
 * `verts_num`). Pass the curvature output here to combine the two; the stronger
 * weight wins per vertex.
 */
void mesh_guide_strokes_field(const Mesh &mesh,
                              Span<float3> stroke_points,
                              OffsetIndices<int> stroke_offsets,
                              float radius,
                              float strength,
                              MutableSpan<float3> r_dirs,
                              MutableSpan<float> r_weights);

/**
 * Detect Face Set boundaries and output them as a per-vertex orientation and
 * positional guide. This is used to constrain QuadriFlow edge flow and geometry
 * to exactly match the boundaries between different Face Sets.
 *
 * \param r_pin_weights: Positional pin weights for #BKE_mesh_remesh_quadriflow
 * (length `verts_num`), set to 1 on boundary vertices. May be empty to skip pins.
 */
void mesh_face_set_boundaries_field(const Mesh &mesh,
                                    MutableSpan<float3> r_dirs,
                                    MutableSpan<float> r_weights,
                                    MutableSpan<float> r_pin_weights);

/**
 * Compute per-vertex relative edge-length multipliers from the absolute
 * surface curvature, for adaptive quad density in #BKE_mesh_remesh_quadriflow:
 * strongly curved regions get values < 1 (smaller quads), flat regions > 1
 * (larger quads). The median curvature maps to 1 so the overall face count
 * stays close to the target.
 *
 * \param adaptivity: Blend in [0, 1] between uniform density (0) and the full
 * curvature-adaptive range (1).
 * \param r_scales: Multiplier per vertex (length `verts_num`), in [0.5, 2].
 */
void mesh_curvature_density_field(const Mesh &mesh, float adaptivity, MutableSpan<float> r_scales);

/**
 * Relax vertex positions with uniform Laplacian smoothing while keeping them
 * on \a source: after each smoothing step every vertex is projected back onto
 * the closest point of the source surface. Vertices on open boundaries,
 * non-manifold edges and sharp creases are kept fixed. Improves quad shape
 * uniformity of remesher output without shrinking the model or rounding its
 * sharp features.
 *
 * \param factor: Smoothing step in [0, 1] per iteration.
 * \param sharp_angle: Edges whose faces meet at more than this angle (in
 * radians) are treated as sharp features and their vertices stay fixed.
 */
void mesh_relax_reproject(
    Mesh &mesh, const Mesh &source, int iterations, float factor, float sharp_angle);
}

}  // namespace blender
