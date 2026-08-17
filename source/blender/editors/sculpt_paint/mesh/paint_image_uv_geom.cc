/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "paint_image_uv_geom.hh"

#include "BLI_array.hh"
#include "BLI_assert.h"
#include "BLI_math_color.h"
#include "BLI_math_geom.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector_types.hh"
#include "BLI_polyfill_2d.h"
#include "BLI_rect.h"
#include "BLI_set.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "BKE_image_paint_selection.hh"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"

#include "bmesh.hh"

namespace blender {

bool image_paint_uv_poly_contains_point(Span<float2> uv_poly,
                                        const float2 &uv,
                                        const bool include_boundary)
{
  const int verts_num = int(uv_poly.size());
  if (verts_num < 3) {
    return false;
  }

  const auto point_in_tri = [&](const float2 &p0, const float2 &p1, const float2 &p2) {
    const auto edge_fn = [](const float2 &a, const float2 &b, const float fx, const float fy) {
      return (fx - a.x) * (b.y - a.y) - (fy - a.y) * (b.x - a.x);
    };
    const float w0 = edge_fn(p1, p2, uv.x, uv.y);
    const float w1 = edge_fn(p2, p0, uv.x, uv.y);
    const float w2 = edge_fn(p0, p1, uv.x, uv.y);
    if (include_boundary) {
      const bool has_neg = (w0 < 0.0f) || (w1 < 0.0f) || (w2 < 0.0f);
      const bool has_pos = (w0 > 0.0f) || (w1 > 0.0f) || (w2 > 0.0f);
      return has_neg != has_pos;
    }
    /* Strict interior: exclude the edge so a shared UV fold can still be filled. */
    return (w0 > 0.0f && w1 > 0.0f && w2 > 0.0f) || (w0 < 0.0f && w1 < 0.0f && w2 < 0.0f);
  };

  if (verts_num == 3) {
    return point_in_tri(uv_poly[0], uv_poly[1], uv_poly[2]);
  }

  Vector<uint3, 8> tris(verts_num - 2);
  BLI_polyfill_calc(reinterpret_cast<const float (*)[2]>(uv_poly.data()),
                    uint(verts_num),
                    0,
                    reinterpret_cast<uint(*)[3]>(tris.data()));

  for (const uint3 &tri : tris) {
    if (point_in_tri(uv_poly[tri[0]], uv_poly[tri[1]], uv_poly[tri[2]])) {
      return true;
    }
  }
  return false;
}

bool image_paint_face_contains_uv(const BMFace *efa, const BMUVOffsets &offsets, const float2 &uv)
{
  Vector<float2, 8> poly;
  BMIter liter;
  BMLoop *l;
  BM_ITER_ELEM (l, &liter, const_cast<BMFace *>(efa), BM_LOOPS_OF_FACE) {
    const float *luv = BM_ELEM_CD_GET_FLOAT_P(l, offsets.uv);
    poly.append(float2(luv[0], luv[1]));
  }
  return image_paint_uv_poly_contains_point(poly, uv);
}

bool image_paint_uv_to_object_position(const BMFace *efa,
                                       const BMUVOffsets &offsets,
                                       const float2 &uv,
                                       float3 &r_position)
{
  Vector<float2, 8> poly;
  Vector<float3, 8> positions;
  BMIter liter;
  BMLoop *l;
  BM_ITER_ELEM (l, &liter, const_cast<BMFace *>(efa), BM_LOOPS_OF_FACE) {
    const float *luv = BM_ELEM_CD_GET_FLOAT_P(l, offsets.uv);
    poly.append(float2(luv[0], luv[1]));
    positions.append(float3(l->v->co));
  }

  const int verts_num = int(poly.size());
  if (verts_num < 3) {
    return false;
  }

  const auto solve_tri = [&](const int i0, const int i1, const int i2) {
    float weights[3];
    barycentric_weights_v2(poly[i0], poly[i1], poly[i2], uv, weights);
    /* `barycentric_weights_v2` yields a valid partition of unity even outside the
     * triangle, so containment must be tested separately. */
    const float2 tri[3] = {poly[i0], poly[i1], poly[i2]};
    if (!image_paint_uv_poly_contains_point(Span<float2>(tri, 3), uv)) {
      return false;
    }
    r_position = positions[i0] * weights[0] + positions[i1] * weights[1] +
                 positions[i2] * weights[2];
    return true;
  };

  if (verts_num == 3) {
    return solve_tri(0, 1, 2);
  }

  Vector<uint3, 8> tris(verts_num - 2);
  BLI_polyfill_calc(reinterpret_cast<const float (*)[2]>(poly.data()),
                    uint(verts_num),
                    0,
                    reinterpret_cast<uint(*)[3]>(tris.data()));

  for (const uint3 &tri : tris) {
    if (solve_tri(int(tri[0]), int(tri[1]), int(tri[2]))) {
      return true;
    }
  }
  return false;
}

void image_paint_faces_at_uv(BMesh *bm,
                             const BMUVOffsets &offsets,
                             const float2 &uv,
                             Vector<int> &r_faces)
{
  r_faces.clear();
  BMIter fiter;
  BMFace *efa;
  int face_index;
  BM_ITER_MESH_INDEX (efa, &fiter, bm, BM_FACES_OF_MESH, face_index) {
    if (BM_elem_flag_test(efa, BM_ELEM_HIDDEN)) {
      continue;
    }
    if (image_paint_face_contains_uv(efa, offsets, uv)) {
      r_faces.append(face_index);
    }
  }
}

void image_paint_tag_mesh_connected_faces(BMesh *bm,
                                          const Span<int> seed_face_indices,
                                          MutableSpan<bool> r_face_tag)
{
  if (bm == nullptr || r_face_tag.is_empty()) {
    return;
  }
  BLI_assert(r_face_tag.size() == bm->totface);
  r_face_tag.fill(false);
  if (seed_face_indices.is_empty() || bm->totface == 0) {
    return;
  }

  /* The flood fill reads neighbor indices via #BM_elem_index_get, which
   * #BM_mesh_elem_table_ensure does not refresh — it only rebuilds `bm->ftable`.
   * An edit-mode BMesh routinely arrives here with dirty face indices. */
  BM_mesh_elem_index_ensure(bm, BM_FACE);
  BM_mesh_elem_table_ensure(bm, BM_FACE);

  Vector<int> stack;
  stack.reserve(bm->totface);

  for (const int face_i : seed_face_indices) {
    if (face_i < 0 || face_i >= bm->totface) {
      continue;
    }
    BMFace *efa = BM_face_at_index(bm, face_i);
    if (efa == nullptr || BM_elem_flag_test(efa, BM_ELEM_HIDDEN)) {
      continue;
    }
    if (r_face_tag[face_i]) {
      continue;
    }
    r_face_tag[face_i] = true;
    stack.append(face_i);
  }

  while (!stack.is_empty()) {
    const int face_i = stack.pop_last();
    BMFace *efa = BM_face_at_index(bm, face_i);
    if (efa == nullptr) {
      continue;
    }
    BMIter liter;
    BMLoop *l;
    BM_ITER_ELEM (l, &liter, efa, BM_LOOPS_OF_FACE) {
      BMIter fiter;
      BMFace *efa_other;
      BM_ITER_ELEM (efa_other, &fiter, l->v, BM_FACES_OF_VERT) {
        if (BM_elem_flag_test(efa_other, BM_ELEM_HIDDEN)) {
          continue;
        }
        const int other_i = BM_elem_index_get(efa_other);
        if (other_i < 0 || other_i >= bm->totface || r_face_tag[other_i]) {
          continue;
        }
        r_face_tag[other_i] = true;
        stack.append(other_i);
      }
    }
  }
}

static void face_uv_bounds_init(const BMFace *efa, const BMUVOffsets &offsets, rctf &r_bounds)
{
  BLI_rctf_init_minmax(&r_bounds);
  BMIter liter;
  BMLoop *l;
  BM_ITER_ELEM (l, &liter, const_cast<BMFace *>(efa), BM_LOOPS_OF_FACE) {
    const float *uv = BM_ELEM_CD_GET_FLOAT_P(l, offsets.uv);
    BLI_rctf_do_minmax_v(&r_bounds, uv);
  }
}

void image_paint_uv_claim_buffer_build(BMesh *bm,
                                       const BMUVOffsets &offsets,
                                       const Span<int> fill_faces,
                                       const float2 &uv_origin,
                                       const int width,
                                       const int height,
                                       MutableSpan<uint8_t> r_claim)
{
  BLI_assert(r_claim.size() == int64_t(width) * int64_t(height));
  r_claim.fill(0);

  if (bm == nullptr || offsets.uv < 0) {
    return;
  }

  BM_mesh_elem_index_ensure(bm, BM_FACE);
  BM_mesh_elem_table_ensure(bm, BM_FACE);

  Set<int> fill_set;
  fill_set.reserve(fill_faces.size());
  for (const int face_index : fill_faces) {
    fill_set.add(face_index);
  }

  for (const int face_index : fill_faces) {
    BMFace *efa = BM_face_at_index(bm, face_index);
    if (efa == nullptr) {
      continue;
    }
    foreach_face_pixel(efa,
                       offsets,
                       uv_origin,
                       width,
                       height,
                       [&](const int x, const int y, const bool strict) {
                         const int64_t index = int64_t(y) * int64_t(width) + int64_t(x);
                         r_claim[index] |= UV_CLAIM_FILL_INCLUSIVE;
                         if (strict) {
                           r_claim[index] |= UV_CLAIM_FILL_STRICT;
                         }
                         return true;
                       });
  }

  /* Bounds-reject before any per-pixel work: on a dense mesh most faces live on other
   * parts of the layout, and rasterizing them would dominate the cost. */
  rctf tile_bounds;
  BLI_rctf_init(&tile_bounds, uv_origin.x, uv_origin.x + 1.0f, uv_origin.y, uv_origin.y + 1.0f);

  BMIter fiter;
  BMFace *efa;
  int face_index;
  BM_ITER_MESH_INDEX (efa, &fiter, bm, BM_FACES_OF_MESH, face_index) {
    if (BM_elem_flag_test(efa, BM_ELEM_HIDDEN) || fill_set.contains(face_index)) {
      continue;
    }
    rctf face_bounds;
    face_uv_bounds_init(efa, offsets, face_bounds);
    if (!BLI_rctf_isect(&tile_bounds, &face_bounds, nullptr)) {
      continue;
    }
    foreach_face_pixel(efa,
                       offsets,
                       uv_origin,
                       width,
                       height,
                       [&](const int x, const int y, const bool /*strict*/) {
                         r_claim[int64_t(y) * int64_t(width) + int64_t(x)] |=
                             UV_CLAIM_FOREIGN_INCLUSIVE;
                         return true;
                       });
  }
}

void image_paint_rasterize_faces_to_ibuf(BMesh *bm,
                                         const BMUVOffsets &offsets,
                                         const Span<int> face_indices,
                                         const float2 &uv_origin,
                                         Image *image,
                                         int tile_number,
                                         ImBuf *ibuf,
                                         const float color[3],
                                         float strength,
                                         IMB_BlendMode blend)
{
  if (bm == nullptr || ibuf == nullptr || face_indices.is_empty()) {
    return;
  }

  BM_mesh_elem_table_ensure(bm, BM_FACE);

  const bool do_float = (ibuf->float_data() != nullptr);
  float color_f[4];
  uint color_b = 0;
  if (!do_float) {
    float3 ibuf_color = color;
    IMB_colormanagement_scene_linear_to_colorspace_v3(ibuf_color, ibuf->byte_buffer.colorspace);
    rgb_float_to_uchar(reinterpret_cast<uchar *>(&color_b), ibuf_color);
    *(reinterpret_cast<char *>(&color_b) + 3) = char(strength * 255);
  }
  else {
    copy_v3_v3(color_f, color);
    color_f[3] = strength;
  }

  const bool has_mask = BKE_image_paint_selection_mask_has_any(image);
  const int width = ibuf->x;
  const int height = ibuf->y;

  const auto paint_pixel = [&](const int x, const int y) {
    float weight = 1.0f;
    if (has_mask) {
      weight = BKE_image_paint_selection_blend_sample(image, tile_number, x, y);
    }
    if (weight <= 0.001f) {
      return;
    }

    const size_t coordinate = size_t(y) * width + x;
    if (do_float) {
      float color_f_masked[4];
      copy_v4_v4(color_f_masked, color_f);
      mul_v4_fl(color_f_masked, weight);
      IMB_blend_color_float(ibuf->float_data_for_write() + 4 * coordinate,
                            ibuf->float_data_for_write() + 4 * coordinate,
                            color_f_masked,
                            blend);
    }
    else {
      float color_f_masked[4];
      rgba_uchar_to_float(color_f_masked, reinterpret_cast<uchar *>(&color_b));
      mul_v4_fl(color_f_masked, weight);
      uchar color_b_masked[4];
      rgba_float_to_uchar(color_b_masked, color_f_masked);
      IMB_blend_color_byte(ibuf->byte_data_for_write() + 4 * coordinate,
                           ibuf->byte_data_for_write() + 4 * coordinate,
                           color_b_masked,
                           blend);
    }
  };

  Array<uint8_t> claim(int64_t(width) * int64_t(height));
  image_paint_uv_claim_buffer_build(
      bm, offsets, face_indices, uv_origin, width, height, claim);

  Vector<int2> filled_pixels;
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      const int64_t index = int64_t(y) * int64_t(width) + int64_t(x);
      const uint8_t bits = claim[index];
      if ((bits & UV_CLAIM_FILL_INCLUSIVE) == 0) {
        continue;
      }
      /* A texel shared with a neighbor chart is left to that neighbor unless this fill
       * owns it outright, otherwise the fill bleeds across the seam at a shared corner. */
      if ((bits & UV_CLAIM_FOREIGN_INCLUSIVE) && (bits & UV_CLAIM_FILL_STRICT) == 0) {
        continue;
      }
      claim[index] |= UV_CLAIM_PAINTED;
      filled_pixels.append(int2(x, y));
    }
  }

  /* One 4-connected ring into texels no other face claims (including its
   * boundary). 8-connected / extra rings walked diagonally through corners. */
  const int2 ortho_dirs[4] = {int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1)};
  const int filled_num = int(filled_pixels.size());
  for (int i = 0; i < filled_num; i++) {
    const int2 p = filled_pixels[i];
    for (const int2 &d : ortho_dirs) {
      const int nx = p.x + d.x;
      const int ny = p.y + d.y;
      if (nx < 0 || ny < 0 || nx >= width || ny >= height) {
        continue;
      }
      const int64_t nindex = int64_t(ny) * int64_t(width) + int64_t(nx);
      if (claim[nindex] & (UV_CLAIM_PAINTED | UV_CLAIM_FOREIGN_INCLUSIVE)) {
        continue;
      }
      claim[nindex] |= UV_CLAIM_PAINTED;
      filled_pixels.append(int2(nx, ny));
    }
  }

  for (const int2 &p : filled_pixels) {
    paint_pixel(p.x, p.y);
  }
}

}  // namespace blender
