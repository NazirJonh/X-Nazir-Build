/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * View basis, occlusion setup and the shared bake preparation of the 3D shape backends: the
 * affected PBVH nodes and their per-vertex samples, gathered once and reused by every write
 * backend. Split out of `sculpt_paint_shape.cc`; see `sculpt_paint_shape_intern.hh`.
 */

#include "sculpt_paint_shape_intern.hh"

namespace blender::ed::sculpt_paint::shape {

float3 bounds_corner(const Bounds<float3> &bounds, const int corner)
{
  return float3((corner & 1) ? bounds.max.x : bounds.min.x,
                (corner & 2) ? bounds.max.y : bounds.min.y,
                (corner & 4) ? bounds.max.z : bounds.min.z);
}

bool shape_bake_prepare(Object &ob,
                        const ShapeSpace &space,
                        const Span<PaintShape> shapes,
                        const ShapeStyle &style,
                        const Depsgraph &depsgraph,
                        ShapeBakeData &r_bake,
                        ReportList *reports)
{
  bke::pbvh::Tree *pbvh = bke::object::pbvh_get(ob);
  if (pbvh == nullptr || pbvh->type() != bke::pbvh::Type::Mesh) {
    if (reports != nullptr) { BKE_report(reports, RPT_WARNING, "Paint Shape: the active object has no sculpt mesh"); }
    return false;
  }
  ViewProjectorCamera camera;
  /* A SurfaceAnchored space has no view camera (occlusion is its #SurfaceAnchor::max_depth). */
  const bool has_camera = space.view_camera(ob, camera);

  /* Shape-space domain of the evaluator: the union of the shapes' bounds, which are already
   * expanded by stroke width, feather and profile extent (#shape_bounds_calc). */
  rctf domain;
  BLI_rctf_init(&domain, FLT_MAX, -FLT_MAX, FLT_MAX, -FLT_MAX);
  for (const PaintShape &shape : shapes) {
    const rctf bounds = shape_bounds_calc(shape, style);
    if (bounds.xmin > bounds.xmax) {
      continue;
    }
    domain.xmin = std::min(domain.xmin, bounds.xmin);
    domain.xmax = std::max(domain.xmax, bounds.xmax);
    domain.ymin = std::min(domain.ymin, bounds.ymin);
    domain.ymax = std::max(domain.ymax, bounds.ymax);
  }
  if (domain.xmin > domain.xmax) {
    if (reports != nullptr) { BKE_report(reports, RPT_WARNING, "Paint Shape: the shape has no area"); }
    return false;
  }

  /* Dir/distance outputs are only needed for the profile-driven shading that vertex channels
   * use (fill profile coverage reads fill_d, the stroke profile reads stroke_t); Normal and
   * Height are map-only and never reach this backend. */
  const ShapeRasterOutputs outputs = ShapeRasterOutputs::Fill | ShapeRasterOutputs::Stroke |
                                     ShapeRasterOutputs::StrokeT | ShapeRasterOutputs::FillD;
  const ShapeEvaluator evaluator(shapes, style, domain, outputs);

  Vector<ePaintSymmetryFlags> passes;
  const int symmetry_flags = int(mesh_symmetry_xyz_get(ob));
  for (int i = 0; i <= symmetry_flags; i++) {
    if (is_symmetry_iteration_valid(i, symmetry_flags)) {
      passes.append(ePaintSymmetryFlags(i));
    }
  }

  const ShapeViewBasis view_basis = space.view_basis(ob);
  /* Built once here: #vertex_is_occluded lazily creates it, and the per-vertex parallel loop
   * below must not race on that. */
  vert_random_access_ensure(ob);

  const Span<float3> vert_positions = bke::pbvh::vert_positions_eval(depsgraph, ob);
  const Span<float3> vert_normals = bke::pbvh::vert_normals_eval(depsgraph, ob);
  const Mesh &bake_mesh = *id_cast<const Mesh *>(ob.data);
  MeshAttributeData attribute_data(bake_mesh);
  FaceSelectionMask face_selection_mask;
  face_selection_mask_build(bake_mesh, face_selection_mask);
  const bool use_face_selection = face_selection_mask.state == FaceSelectionState::Active;
  BLI_assert(!use_face_selection ||
             face_selection_mask.vert_paintable.size() == bake_mesh.verts_num);
  BLI_assert(!use_face_selection ||
             face_selection_mask.select_poly.size() == bake_mesh.faces_num);

  MutableSpan<bke::pbvh::MeshNode> nodes = pbvh->nodes<bke::pbvh::MeshNode>();

  /* Candidate nodes: their projected bounds (over every symmetry pass) must reach the shape's
   * region-pixel domain. Everything else cannot contribute a covered vertex. */
  for (const int i : nodes.index_range()) {
    const Bounds<float3> &bounds = nodes[i].bounds();
    rctf node_rct;
    BLI_rctf_init(&node_rct, FLT_MAX, -FLT_MAX, FLT_MAX, -FLT_MAX);
    bool any_corner = false;
    for (int corner = 0; corner < 8; corner++) {
      const float3 p = bounds_corner(bounds, corner);
      for (const ePaintSymmetryFlags pass : passes) {
      const float3 flipped = symmetry_flip(p, pass);
      float2 co;
        if (!space.project_point(flipped, float3(0.0f), co)) {
          continue;
        }
        node_rct.xmin = std::min(node_rct.xmin, co.x);
        node_rct.xmax = std::max(node_rct.xmax, co.x);
        node_rct.ymin = std::min(node_rct.ymin, co.y);
        node_rct.ymax = std::max(node_rct.ymax, co.y);
        any_corner = true;
      }
    }
    if (!any_corner) {
      continue;
    }
    rctf isect;
    if (BLI_rctf_isect(&node_rct, &domain, &isect)) {
      r_bake.candidates.append(i);
    }
  }
  if (r_bake.candidates.is_empty()) {
    if (reports != nullptr) { BKE_report(reports, RPT_INFO, "Paint Shape: the shape does not reach the mesh"); }
    return false;
  }

  r_bake.node_offset = Array<int>(r_bake.candidates.size() + 1, 0);
  int total_verts = 0;
  for (const int ci : r_bake.candidates.index_range()) {
    r_bake.node_offset[ci] = total_verts;
    total_verts += nodes[r_bake.candidates[ci]].verts().size();
  }
  r_bake.node_offset[r_bake.candidates.size()] = total_verts;
  r_bake.samples.reinitialize(total_verts);

  std::atomic<int> painted{0};
  threading::parallel_for(r_bake.candidates.index_range(), 1, [&](const IndexRange node_range) {
    for (const int ci : node_range) {
      const Span<int> verts = nodes[r_bake.candidates[ci]].verts();
      MutableSpan<ShapeVertexSample> node_samples = r_bake.samples.as_mutable_span().slice(
          r_bake.node_offset[ci], verts.size());
      for (const int k : verts.index_range()) {
        const int vert = verts[k];
        ShapeVertexSample &result = node_samples[k];
        if (!attribute_data.hide_vert.is_empty() && attribute_data.hide_vert[vert]) {
          continue;
        }
        const float mask_value = attribute_data.mask.is_empty() ? 0.0f :
                                                                 attribute_data.mask[vert];
        float factor = 1.0f - mask_value;
        if (factor <= 0.0f) {
          continue;
        }
        if (use_face_selection && !face_selection_mask.vert_paintable[vert]) {
          continue;
        }

        const float3 position = vert_positions[vert];
        const float3 normal = vert_normals[vert];
        float best_score = 0.0f;
        float3 best_position = float3(0.0f);
        ShapeSample best;
        for (const ePaintSymmetryFlags pass : passes) {
          const float3 mirrored = symmetry_flip(position, pass);
          /* The mirrored normal is the one the shape actually reaches once symmetry turned the
           * vertex into the shape's side. */
          const float3 mirrored_normal = symmetry_flip(normal, pass);
          if (!view_basis.front_facing(mirrored, mirrored_normal)) {
            continue;
          }
          float2 co;
          if (!space.project_point(mirrored, mirrored_normal, co)) {
            continue;
          }
          const ShapeSample candidate = evaluator.sample(co);
          const float score = std::max(candidate.fill, candidate.stroke);
          if (score > best_score) {
            best_score = score;
            best = candidate;
            best_position = mirrored;
          }
        }
        if (best_score <= 0.0f) {
          continue;
        }
        /* Depth test the covered, front-facing vertex against the rest of the mesh so a shape
         * behind another surface is not painted through it. */
        if (has_camera && vertex_is_occluded(depsgraph, ob, camera, best_position, false)) {
          continue;
        }
        result.sample = best;
        result.factor = factor;
        painted.fetch_add(1, std::memory_order_relaxed);
      }
    }
  });
  r_bake.painted = painted.load();

  if (r_bake.painted == 0) {
    if (reports != nullptr) { BKE_report(reports, RPT_INFO, "Paint Shape: no visible, unmasked vertex in the shape"); }
    return false;
  }
  return true;
}

}  // namespace blender::ed::sculpt_paint::shape
