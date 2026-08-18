/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#pragma once

#include <algorithm>
#include <cmath>

#include "BLI_math_base.h"
#include "BLI_math_vector_types.hh"
#include "BLI_polyfill_2d.h"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "BKE_customdata.hh"

#include "IMB_imbuf.hh"

#include "bmesh.hh"

namespace blender {

struct Image;

/**
 * True if \a uv lies inside \a uv_poly.
 * \a include_boundary: a point on an edge or vertex counts as inside (default).
 */
bool image_paint_uv_poly_contains_point(Span<float2> uv_poly,
                                        const float2 &uv,
                                        bool include_boundary = true);

/** True if absolute UV \a uv (including UDIM offset) lies inside \a efa's UV polygon. */
bool image_paint_face_contains_uv(const BMFace *efa, const BMUVOffsets &offsets, const float2 &uv);

/**
 * Recover a position in object space from an absolute UV coordinate inside \a efa.
 *
 * The face is triangulated with #BLI_polyfill_calc, the triangle containing \a uv is
 * located, and its barycentric weights in UV space are applied to the vertex positions.
 * Returns false when \a uv falls outside every triangle of the face.
 */
bool image_paint_uv_to_object_position(const BMFace *efa,
                                       const BMUVOffsets &offsets,
                                       const float2 &uv,
                                       float3 &r_position);

/**
 * Append indices of non-hidden faces whose UV polygon contains \a uv.
 * Boundary-inclusive. Overlapping UVs all match.
 * Order is mesh face index. Deduplicate (a face is appended at most once).
 */
void image_paint_faces_at_uv(BMesh *bm,
                             const BMUVOffsets &offsets,
                             const float2 &uv,
                             Vector<int> &r_faces);

/**
 * Tag every non-hidden face in the same vertex-connected component as any seed face.
 * Faces that share no vertices with the seed component stay false.
 * \a r_face_tag must be sized to \a bm->totface.
 */
void image_paint_tag_mesh_connected_faces(BMesh *bm,
                                          Span<int> seed_face_indices,
                                          MutableSpan<bool> r_face_tag);

/**
 * Per-texel ownership bits for one UDIM tile, produced by
 * #image_paint_uv_claim_buffer_build and consumed by
 * #image_paint_rasterize_faces_to_ibuf.
 */
enum UVPixelClaim : uint8_t {
  /** A face being filled covers this texel center (boundary counts). */
  UV_CLAIM_FILL_INCLUSIVE = 1 << 0,
  /** A face being filled covers this texel center strictly, not merely on its edge. */
  UV_CLAIM_FILL_STRICT = 1 << 1,
  /** Some face outside the fill set covers this texel center (boundary counts). */
  UV_CLAIM_FOREIGN_INCLUSIVE = 1 << 2,
  /** Scratch bit owned by the rasterizer to avoid painting a texel twice. */
  UV_CLAIM_PAINTED = 1 << 3,
};

/**
 * Build the per-texel ownership map for one UDIM tile.
 *
 * Two passes over faces, never over (pixels x faces): the fill set writes
 * #UV_CLAIM_FILL_INCLUSIVE (plus #UV_CLAIM_FILL_STRICT on strict hits), then every other
 * visible face whose UV bounds meet the tile writes #UV_CLAIM_FOREIGN_INCLUSIVE.
 *
 * \a r_claim must be sized `width * height`; it is cleared by this function.
 */
void image_paint_uv_claim_buffer_build(BMesh *bm,
                                       const BMUVOffsets &offsets,
                                       Span<int> fill_faces,
                                       const float2 &uv_origin,
                                       int width,
                                       int height,
                                       MutableSpan<uint8_t> r_claim);

/**
 * Rasterize \a face_indices (indices into \a bm only) into \a ibuf.
 * Pixel coords: (uv - uv_origin) * ibuf size, same as selection.
 * Mask clip uses whole-image semantics (skip weight <= 0.001f).
 *
 * \a color is the same sRGB-space RGB that `paint_2d_bucket_fill` receives.
 * The rasterizer converts per destination \a ibuf (byte vs float, colorspace).
 */
void image_paint_rasterize_faces_to_ibuf(BMesh *bm,
                                         const BMUVOffsets &offsets,
                                         const Span<int> face_indices,
                                         const float2 &uv_origin,
                                         Image *image,
                                         int tile_number,
                                         ImBuf *ibuf,
                                         const float color[3],
                                         float strength,
                                         IMB_BlendMode blend);

/**
 * Visit every pixel center covered by a triangle given in tile-local pixel space.
 *
 * Shared UV edges stay watertight: a pixel belongs to the face that contains its
 * center, so filling one face cannot paint into a neighbor across a fold.
 * Island-border texels whose centers fall just outside are added later by
 * occupancy-aware dilation in #image_paint_rasterize_faces_to_ibuf.
 *
 * \a fn is invoked as `fn(x, y, strict)` and returns `true` to continue or `false` to stop
 * early. \a strict is `true` only when the pixel center lies strictly inside the triangle;
 * a center exactly on an edge or vertex reports `false`. The rasterizer needs the
 * distinction because a texel shared with a neighboring chart must not be claimed.
 * Returns `false` if \a fn requested an early stop, otherwise `true`.
 */
template<typename Fn>
inline bool foreach_triangle_pixel(const float2 &p0,
                                   const float2 &p1,
                                   const float2 &p2,
                                   const int width,
                                   const int height,
                                   Fn &&fn)
{
  int min_x = int(floorf(std::min({p0.x, p1.x, p2.x})));
  int max_x = int(ceilf(std::max({p0.x, p1.x, p2.x})));
  int min_y = int(floorf(std::min({p0.y, p1.y, p2.y})));
  int max_y = int(ceilf(std::max({p0.y, p1.y, p2.y})));

  min_x = max_ii(min_x, 0);
  min_y = max_ii(min_y, 0);
  max_x = min_ii(max_x, width - 1);
  max_y = min_ii(max_y, height - 1);

  const auto edge_fn = [](const float2 &a, const float2 &b, const float fx, const float fy) {
    return (fx - a.x) * (b.y - a.y) - (fy - a.y) * (b.x - a.x);
  };

  for (int y = min_y; y <= max_y; y++) {
    for (int x = min_x; x <= max_x; x++) {
      const float fx = float(x) + 0.5f;
      const float fy = float(y) + 0.5f;
      const float w0 = edge_fn(p1, p2, fx, fy);
      const float w1 = edge_fn(p2, p0, fx, fy);
      const float w2 = edge_fn(p0, p1, fx, fy);
      const bool has_neg = (w0 < 0.0f) || (w1 < 0.0f) || (w2 < 0.0f);
      const bool has_pos = (w0 > 0.0f) || (w1 > 0.0f) || (w2 > 0.0f);
      if (has_neg == has_pos) {
        continue;
      }
      const bool strict = (w0 > 0.0f && w1 > 0.0f && w2 > 0.0f) ||
                          (w0 < 0.0f && w1 < 0.0f && w2 < 0.0f);
      if (!fn(x, y, strict)) {
        return false;
      }
    }
  }
  return true;
}

/**
 * Tessellate a face's UV polygon into tile-local pixel space and visit every covered pixel.
 * Coordinates are mapped to pixels via `(uv - uv_origin) * size`; the point-in-triangle test is
 * orientation-preserving under this map, so the same primitive serves both reads and writes.
 * See #foreach_triangle_pixel for the \a fn contract, including the \a strict argument;
 * an early stop propagates across triangles.
 *
 * The polygon is triangulated with #BLI_polyfill_calc rather than fanned from vertex 0: a face
 * that is convex in 3D can still be concave in UV space, and a fan over a concave polygon emits
 * triangles that cover area outside the face, flooding the mask beyond the UV island.
 */
template<typename Fn>
inline void foreach_face_pixel(const BMFace *efa,
                               const BMUVOffsets &offsets,
                               const float2 &uv_origin,
                               const int width,
                               const int height,
                               Fn &&fn)
{
  Vector<float2, 8> px_verts;
  BMIter liter;
  BMLoop *l;
  BM_ITER_ELEM (l, &liter, const_cast<BMFace *>(efa), BM_LOOPS_OF_FACE) {
    const float *uv = BM_ELEM_CD_GET_FLOAT_P(l, offsets.uv);
    px_verts.append(float2((uv[0] - uv_origin.x) * width, (uv[1] - uv_origin.y) * height));
  }

  const int verts_num = px_verts.size();
  if (verts_num < 3) {
    return;
  }

  if (verts_num == 3) {
    foreach_triangle_pixel(px_verts[0], px_verts[1], px_verts[2], width, height, fn);
    return;
  }

  Vector<uint3, 8> tris(verts_num - 2);
  BLI_polyfill_calc(reinterpret_cast<const float (*)[2]>(px_verts.data()),
                    uint(verts_num),
                    0,
                    reinterpret_cast<uint(*)[3]>(tris.data()));

  for (const uint3 &tri : tris) {
    if (!foreach_triangle_pixel(
            px_verts[tri[0]], px_verts[tri[1]], px_verts[tri[2]], width, height, fn))
    {
      return;
    }
  }
}

}  // namespace blender
