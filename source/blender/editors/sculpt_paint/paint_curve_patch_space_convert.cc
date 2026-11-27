/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "paint_curve_patch_space_convert.hh"

#include <array>
#include <cfloat>
#include <optional>

#include "BLI_index_mask.hh"
#include "BLI_kdopbvh.hh"
#include "BLI_math_geom.h"
#include "BLI_math_vector.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"
#include "BLI_virtual_array.hh"

#include "DNA_mesh_types.h"

#include "BKE_attribute.hh"
#include "BKE_bvhutils.hh"
#include "BKE_curves.hh"
#include "BKE_mesh.hh"
#include "BKE_paint_layers_gradient_surface.hh"

#include "GEO_reverse_uv_sampler.hh"

namespace blender::ed::sculpt_paint {

namespace {

/** The new coordinates of one curve, kept aside until every curve converted successfully. */
struct ConvertedCurve {
  Vector<float3> positions;
  Vector<float3> handles_left;
  Vector<float3> handles_right;
};

}  // namespace

bool curve_patch_space_convert(const Mesh &mesh,
                               Span<bke::CurvesGeometry *> curves,
                               const bool to_uv,
                               int &r_failed_points_num)
{
  r_failed_points_num = 0;

  const bke::AttributeAccessor attributes = mesh.attributes();
  /* Bind through the UV the layered material itself reads, so a 2D curve lands on the surface the
   * canvas composite shows even when the editable active UV map is another one. This is the same
   * `default_uv_map_name()` the shader's Texture Coordinate.UV node reads (see
   * #BKE_paint_layers_gradient_uv_map_name and `paint_material_composite_projection.cc`), so the
   * 3D -> 2D trip and the projected composite share one map by construction. */
  const VArraySpan<float2> uv_map = *attributes.lookup<float2>(
      BKE_paint_layers_gradient_uv_map_name(mesh), bke::AttrDomain::Corner);
  const Span<float3> positions = mesh.vert_positions();
  const Span<int> corner_verts = mesh.corner_verts();
  const Span<int3> corner_tris = mesh.corner_tris();
  if (uv_map.is_empty() || corner_tris.is_empty()) {
    return false;
  }

  const auto tri_positions = [&](const int3 &tri) {
    return std::array<float3, 3>{positions[corner_verts[tri[0]]],
                                 positions[corner_verts[tri[1]]],
                                 positions[corner_verts[tri[2]]]};
  };

  /* One tree for the whole call: a per-point scan of every triangle froze the UI on dense meshes. */
  std::optional<bke::BVHTreeFromMesh> bvh;
  std::optional<geometry::ReverseUVSampler> reverse_uv_sampler;
  if (to_uv) {
    bvh = bke::bvhtree_from_mesh_tris_init(mesh, IndexMask(mesh.faces_num));
    if (bvh->tree == nullptr) {
      return false;
    }
  }
  else {
    reverse_uv_sampler.emplace(uv_map, corner_tris);
  }

  /* Returns false on a miss; \a r_co is only written on a hit. */
  const auto convert_point = [&](const float3 &co, float3 &r_co) -> bool {
    if (to_uv) {
      BVHTreeNearest nearest;
      nearest.index = -1;
      nearest.dist_sq = FLT_MAX;
      BLI_bvhtree_find_nearest(bvh->tree, co, &nearest, bvh->nearest_callback, &*bvh);
      if (nearest.index < 0) {
        return false;
      }
      const int3 &tri = corner_tris[nearest.index];
      const std::array<float3, 3> v = tri_positions(tri);
      float3 weights;
      interp_weights_tri_v3(weights, v[0], v[1], v[2], float3(nearest.co));
      const float2 uv = uv_map[tri[0]] * weights.x + uv_map[tri[1]] * weights.y +
                        uv_map[tri[2]] * weights.z;
      r_co = float3(uv.x, uv.y, 0.0f);
      return true;
    }
    const geometry::ReverseUVSampler::Result hit = reverse_uv_sampler->sample(float2(co.x, co.y));
    if (hit.type != geometry::ReverseUVSampler::ResultType::Ok) {
      return false;
    }
    const std::array<float3, 3> v = tri_positions(corner_tris[hit.tri_index]);
    r_co = v[0] * hit.bary_weights.x + v[1] * hit.bary_weights.y + v[2] * hit.bary_weights.z;
    return true;
  };

  /* Control points must all find a counterpart. A handle may legitimately stick out of a UV island
   * (or off the surface) while its point is inside: it then collapses onto its point, and the
   * auto-handle pass below rebuilds it. */
  const auto convert_handles = [&](const Span<float3> src,
                                   const Span<float3> converted_points,
                                   Vector<float3> &r_dst) {
    r_dst.reserve(src.size());
    for (const int i : src.index_range()) {
      float3 converted;
      r_dst.append(convert_point(src[i], converted) ? converted : converted_points[i]);
    }
  };

  Vector<ConvertedCurve> converted(curves.size());
  for (const int curve_i : curves.index_range()) {
    const bke::CurvesGeometry &geom = *curves[curve_i];
    ConvertedCurve &out = converted[curve_i];
    const Span<float3> src_positions = geom.positions();
    out.positions.reserve(src_positions.size());
    for (const float3 &co : src_positions) {
      float3 converted_co;
      if (convert_point(co, converted_co)) {
        out.positions.append(converted_co);
      }
      else {
        r_failed_points_num++;
        out.positions.append(co);
      }
    }
    const std::optional<Span<float3>> left = geom.handle_positions_left();
    const std::optional<Span<float3>> right = geom.handle_positions_right();
    if (left.has_value() && right.has_value()) {
      convert_handles(*left, out.positions, out.handles_left);
      convert_handles(*right, out.positions, out.handles_right);
    }
  }
  if (r_failed_points_num > 0) {
    return false;
  }

  for (const int curve_i : curves.index_range()) {
    bke::CurvesGeometry &geom = *curves[curve_i];
    const ConvertedCurve &in = converted[curve_i];
    geom.positions_for_write().copy_from(in.positions);
    if (!in.handles_left.is_empty()) {
      geom.handle_positions_left_for_write().copy_from(in.handles_left);
      geom.handle_positions_right_for_write().copy_from(in.handles_right);
    }
    geom.calculate_bezier_auto_handles();
    geom.tag_positions_changed();
  }
  return true;
}

}  // namespace blender::ed::sculpt_paint
