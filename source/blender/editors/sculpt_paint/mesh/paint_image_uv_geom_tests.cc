/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "paint_image_uv_geom.hh"

#include "BKE_customdata.hh"
#include "BKE_gtest_base.hh"

#include "BLI_array.hh"
#include "BLI_math_vector_types.hh"

#include "bmesh.hh"

#include "testing/testing.h"

namespace blender::tests {

TEST(ImagePaintUVGeom, PointInsideUnitSquare)
{
  const float2 poly[] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
  EXPECT_TRUE(image_paint_uv_poly_contains_point(poly, float2(0.5f, 0.5f)));
}

TEST(ImagePaintUVGeom, PointOutsideUnitSquare)
{
  const float2 poly[] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
  EXPECT_FALSE(image_paint_uv_poly_contains_point(poly, float2(1.5f, 0.5f)));
}

TEST(ImagePaintUVGeom, NeighborQuadDoesNotContain)
{
  const float2 a[] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
  const float2 b[] = {{2, 0}, {3, 0}, {3, 1}, {2, 1}};
  const float2 p(0.5f, 0.5f);
  EXPECT_TRUE(image_paint_uv_poly_contains_point(a, p));
  EXPECT_FALSE(image_paint_uv_poly_contains_point(b, p));
}

TEST(ImagePaintUVGeom, ConcaveQuadInsideVsAabbOnly)
{
  /* C-shape: AABB is [0,2]x[0,2]; hole-ish notch around (1.5, 1.0). */
  const float2 poly[] = {
      {0, 0}, {2, 0}, {2, 0.4f}, {0.6f, 0.4f}, {0.6f, 1.6f}, {2, 1.6f}, {2, 2}, {0, 2}};
  EXPECT_TRUE(image_paint_uv_poly_contains_point(poly, float2(0.3f, 1.0f)));
  EXPECT_FALSE(image_paint_uv_poly_contains_point(poly, float2(1.5f, 1.0f)));
}

TEST(ImagePaintUVGeom, BoundaryInclusiveOnEdgeAndVertex)
{
  const float2 poly[] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
  EXPECT_TRUE(image_paint_uv_poly_contains_point(poly, float2(0.0f, 0.0f)));
  EXPECT_TRUE(image_paint_uv_poly_contains_point(poly, float2(0.5f, 0.0f)));
}

TEST(ImagePaintUVGeom, OverlappingQuadsBothContain)
{
  const float2 a[] = {{0, 0}, {2, 0}, {2, 2}, {0, 2}};
  const float2 b[] = {{1, 1}, {3, 1}, {3, 3}, {1, 3}};
  const float2 p(1.5f, 1.5f);
  EXPECT_TRUE(image_paint_uv_poly_contains_point(a, p));
  EXPECT_TRUE(image_paint_uv_poly_contains_point(b, p));
}

TEST(ImagePaintUVGeom, PolygonNotAabbAcrossUdimBoundary)
{
  const float2 poly[] = {{0.9f, 0.4f}, {1.1f, 0.4f}, {1.1f, 0.6f}, {0.9f, 0.6f}};
  EXPECT_TRUE(image_paint_uv_poly_contains_point(poly, float2(0.95f, 0.5f)));
  EXPECT_TRUE(image_paint_uv_poly_contains_point(poly, float2(1.05f, 0.5f)));
  EXPECT_FALSE(image_paint_uv_poly_contains_point(poly, float2(0.95f, 0.9f)));
}

TEST(ImagePaintUVGeom, SharedEdgeIsBoundaryNotStrictInterior)
{
  const float2 a[] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
  const float2 b[] = {{1, 0}, {2, 0}, {2, 1}, {1, 1}};
  const float2 on_edge(1.0f, 0.5f);
  EXPECT_TRUE(image_paint_uv_poly_contains_point(a, on_edge));
  EXPECT_TRUE(image_paint_uv_poly_contains_point(b, on_edge));
  EXPECT_FALSE(image_paint_uv_poly_contains_point(a, on_edge, false));
  EXPECT_FALSE(image_paint_uv_poly_contains_point(b, on_edge, false));
  EXPECT_TRUE(image_paint_uv_poly_contains_point(b, float2(1.5f, 0.5f), false));
}

TEST(ImagePaintUVGeom, SharedVertexFourQuadrantsIsBoundaryNotStrictInterior)
{
  /* Four UV faces meeting at (0.5, 0.5). A texel on that vertex is on every
   * face's boundary; painting it from one fill leaks into the other charts. */
  const float2 tl[] = {{0, 0.5f}, {0.5f, 0.5f}, {0.5f, 1}, {0, 1}};
  const float2 bl[] = {{0, 0}, {0.5f, 0}, {0.5f, 0.5f}, {0, 0.5f}};
  const float2 tr[] = {{0.5f, 0.5f}, {1, 0.5f}, {1, 1}, {0.5f, 1}};
  const float2 br[] = {{0.5f, 0}, {1, 0}, {1, 0.5f}, {0.5f, 0.5f}};
  const float2 corner(0.5f, 0.5f);
  EXPECT_TRUE(image_paint_uv_poly_contains_point(tl, corner, true));
  EXPECT_TRUE(image_paint_uv_poly_contains_point(bl, corner, true));
  EXPECT_TRUE(image_paint_uv_poly_contains_point(tr, corner, true));
  EXPECT_TRUE(image_paint_uv_poly_contains_point(br, corner, true));
  EXPECT_FALSE(image_paint_uv_poly_contains_point(tl, corner, false));
  EXPECT_FALSE(image_paint_uv_poly_contains_point(bl, corner, false));

  const float2 in_bl(0.5f - 1.0f / 64.0f, 0.5f - 1.0f / 64.0f);
  EXPECT_FALSE(image_paint_uv_poly_contains_point(tl, in_bl, true));
  EXPECT_TRUE(image_paint_uv_poly_contains_point(bl, in_bl, true));
}

static BMesh *test_bmesh()
{
  BMeshCreateParams params{};
  return BM_mesh_create(&bm_mesh_allocsize_default, &params);
}

static BMVert *test_vert(BMesh *bm, const float x, const float y)
{
  const float co[3] = {x, y, 0.0f};
  return BM_vert_create(bm, co, nullptr, BM_CREATE_NOP);
}

static BMFace *test_quad(BMesh *bm, BMVert *a, BMVert *b, BMVert *c, BMVert *d)
{
  BMVert *verts[4] = {a, b, c, d};
  return BM_face_create_verts(bm, verts, 4, nullptr, BM_CREATE_NOP, true);
}

static BMesh *test_bmesh_with_uv()
{
  BMeshCreateParams params{};
  BMesh *bm = BM_mesh_create(&bm_mesh_allocsize_default, &params);
  BM_data_layer_add_named(bm, &bm->ldata, CD_PROP_FLOAT2, "UVMap");
  return bm;
}

/**
 * Create a quad whose 3D positions are irrelevant and whose four loop UVs are set
 * counter-clockwise starting at (u0, v0), spanning `size` in both directions.
 */
static BMFace *test_quad_uv(
    BMesh *bm, const BMUVOffsets &offsets, const float u0, const float v0, const float size)
{
  BMVert *verts[4] = {
      test_vert(bm, u0, v0),
      test_vert(bm, u0 + size, v0),
      test_vert(bm, u0 + size, v0 + size),
      test_vert(bm, u0, v0 + size),
  };
  BMFace *efa = BM_face_create_verts(bm, verts, 4, nullptr, BM_CREATE_NOP, true);
  const float uvs[4][2] = {{u0, v0}, {u0 + size, v0}, {u0 + size, v0 + size}, {u0, v0 + size}};
  BMIter liter;
  BMLoop *l;
  int i = 0;
  BM_ITER_ELEM (l, &liter, efa, BM_LOOPS_OF_FACE) {
    float *uv = BM_ELEM_CD_GET_FLOAT_P(l, offsets.uv);
    uv[0] = uvs[i][0];
    uv[1] = uvs[i][1];
    i++;
  }
  return efa;
}

class ImagePaintUVGeomMeshTest : public bke::BlenderGTestBase {};

TEST_F(ImagePaintUVGeomMeshTest, MeshConnectedSkipsDisconnectedFaces)
{
  BMesh *bm = test_bmesh();
  BMFace *fa = test_quad(
      bm, test_vert(bm, 0, 0), test_vert(bm, 1, 0), test_vert(bm, 1, 1), test_vert(bm, 0, 1));
  test_quad(
      bm, test_vert(bm, 3, 0), test_vert(bm, 4, 0), test_vert(bm, 4, 1), test_vert(bm, 3, 1));
  BM_mesh_elem_table_ensure(bm, BM_FACE);
  const int seed[] = {BM_elem_index_get(fa)};
  Array<bool> tags(bm->totface, true);
  image_paint_tag_mesh_connected_faces(bm, seed, tags);
  EXPECT_TRUE(tags[0]);
  EXPECT_FALSE(tags[1]);
  BM_mesh_free(bm);
}

TEST_F(ImagePaintUVGeomMeshTest, MeshConnectedFollowsSharedVertex)
{
  BMesh *bm = test_bmesh();
  BMVert *shared = test_vert(bm, 1, 0);
  BMFace *fa = test_quad(
      bm, test_vert(bm, 0, 0), shared, test_vert(bm, 1, 1), test_vert(bm, 0, 1));
  test_quad(bm, shared, test_vert(bm, 2, 0), test_vert(bm, 2, 1), test_vert(bm, 1, 1));
  BM_mesh_elem_table_ensure(bm, BM_FACE);
  const int seed[] = {BM_elem_index_get(fa)};
  Array<bool> tags(bm->totface, false);
  image_paint_tag_mesh_connected_faces(bm, seed, tags);
  EXPECT_TRUE(tags[0]);
  EXPECT_TRUE(tags[1]);
  BM_mesh_free(bm);
}

TEST_F(ImagePaintUVGeomMeshTest, MeshConnectedSkipsHiddenNeighbor)
{
  BMesh *bm = test_bmesh();
  BMVert *shared = test_vert(bm, 1, 0);
  BMFace *fa = test_quad(
      bm, test_vert(bm, 0, 0), shared, test_vert(bm, 1, 1), test_vert(bm, 0, 1));
  BMFace *fb = test_quad(
      bm, shared, test_vert(bm, 2, 0), test_vert(bm, 2, 1), test_vert(bm, 1, 1));
  BM_elem_flag_enable(fb, BM_ELEM_HIDDEN);
  BM_mesh_elem_table_ensure(bm, BM_FACE);
  const int seed[] = {BM_elem_index_get(fa)};
  Array<bool> tags(bm->totface, false);
  image_paint_tag_mesh_connected_faces(bm, seed, tags);
  EXPECT_TRUE(tags[BM_elem_index_get(fa)]);
  EXPECT_FALSE(tags[BM_elem_index_get(fb)]);
  BM_mesh_free(bm);
}

TEST_F(ImagePaintUVGeomMeshTest, MeshConnectedIsTransitive)
{
  /* A—B—C: A and C share no vertex directly, connectivity must still reach C. */
  BMesh *bm = test_bmesh();
  BMVert *ab = test_vert(bm, 1, 0);
  BMVert *ab_top = test_vert(bm, 1, 1);
  BMVert *bc = test_vert(bm, 2, 0);
  BMVert *bc_top = test_vert(bm, 2, 1);
  BMFace *fa = test_quad(bm, test_vert(bm, 0, 0), ab, ab_top, test_vert(bm, 0, 1));
  BMFace *fb = test_quad(bm, ab, bc, bc_top, ab_top);
  BMFace *fc = test_quad(bm, bc, test_vert(bm, 3, 0), test_vert(bm, 3, 1), bc_top);
  BM_mesh_elem_index_ensure(bm, BM_FACE);
  BM_mesh_elem_table_ensure(bm, BM_FACE);
  const int seed[] = {BM_elem_index_get(fa)};
  Array<bool> tags(bm->totface, false);
  image_paint_tag_mesh_connected_faces(bm, seed, tags);
  EXPECT_TRUE(tags[BM_elem_index_get(fa)]);
  EXPECT_TRUE(tags[BM_elem_index_get(fb)]);
  EXPECT_TRUE(tags[BM_elem_index_get(fc)]);
  BM_mesh_free(bm);
}

TEST_F(ImagePaintUVGeomMeshTest, MeshConnectedWithDirtyFaceIndices)
{
  /* Regression: the flood fill reads BM_elem_index_get, which is only valid after
   * BM_mesh_elem_index_ensure. BM_mesh_elem_table_ensure alone does not refresh it. */
  BMesh *bm = test_bmesh();
  BMVert *shared = test_vert(bm, 1, 0);
  BMVert *shared_top = test_vert(bm, 1, 1);
  BMFace *fa = test_quad(bm, test_vert(bm, 0, 0), shared, shared_top, test_vert(bm, 0, 1));
  BMFace *fb = test_quad(bm, shared, test_vert(bm, 2, 0), test_vert(bm, 2, 1), shared_top);
  BM_mesh_elem_index_ensure(bm, BM_FACE);
  const int seed[] = {BM_elem_index_get(fa)};
  const int expect_fb = BM_elem_index_get(fb);

  /* Poison the indices and mark them dirty, exactly as edit-mode operators leave them. */
  BM_elem_index_set(fa, 12345);
  BM_elem_index_set(fb, 12345);
  bm->elem_index_dirty |= BM_FACE;

  Array<bool> tags(bm->totface, false);
  image_paint_tag_mesh_connected_faces(bm, seed, tags);
  EXPECT_TRUE(tags[0]);
  EXPECT_TRUE(tags[expect_fb]);
  BM_mesh_free(bm);
}

TEST_F(ImagePaintUVGeomMeshTest, MeshConnectedEmptySeedAndOutOfRange)
{
  BMesh *bm = test_bmesh();
  test_quad(
      bm, test_vert(bm, 0, 0), test_vert(bm, 1, 0), test_vert(bm, 1, 1), test_vert(bm, 0, 1));
  BM_mesh_elem_index_ensure(bm, BM_FACE);
  BM_mesh_elem_table_ensure(bm, BM_FACE);

  Array<bool> tags(bm->totface, true);
  image_paint_tag_mesh_connected_faces(bm, Span<int>(), tags);
  EXPECT_FALSE(tags[0]);

  const int bad_seed[] = {-1, 999};
  tags.fill(true);
  image_paint_tag_mesh_connected_faces(bm, bad_seed, tags);
  EXPECT_FALSE(tags[0]);
  BM_mesh_free(bm);
}

TEST(ImagePaintUVGeom, TrianglePixelUsesCenterCoverage)
{
  /* Thin strip in the bottom of pixel row 0. Centers sit at y=0.5, outside the strip.
   * Shared-edge watertightness depends on this: do not paint a pixel whose center
   * belongs to a neighboring face. Island borders are padded in the rasterizer. */
  const float2 p0(0.1f, 0.1f);
  const float2 p1(2.9f, 0.1f);
  const float2 p2(0.1f, 0.4f);
  bool saw_row0 = false;
  foreach_triangle_pixel(
      p0, p1, p2, 8, 8, [&](const int /*x*/, const int y, const bool /*strict*/) {
        if (y == 0) {
          saw_row0 = true;
        }
        return true;
      });
  EXPECT_FALSE(saw_row0);
}

TEST(ImagePaintUVGeom, TrianglePixelReportsBoundaryAsNonStrict)
{
  /* A right triangle whose hypotenuse passes exactly through pixel centers on the
   * diagonal. Those centers must be reported with strict == false; centers well
   * inside must be reported with strict == true. */
  const float2 p0(0.0f, 0.0f);
  const float2 p1(4.0f, 0.0f);
  const float2 p2(0.0f, 4.0f);
  bool saw_strict = false;
  bool saw_boundary = false;
  foreach_triangle_pixel(p0, p1, p2, 8, 8, [&](const int x, const int y, const bool strict) {
    if (x == 0 && y == 0) {
      /* Deep interior. */
      EXPECT_TRUE(strict);
      saw_strict = true;
    }
    if (!strict) {
      saw_boundary = true;
    }
    return true;
  });
  EXPECT_TRUE(saw_strict);
  EXPECT_TRUE(saw_boundary);
}

TEST_F(ImagePaintUVGeomMeshTest, ClaimBufferCornerNotStrictlyOwned)
{
  /* Four UV quads meeting at (0.5, 0.5) on a 2x2 texture: the texel centers land
   * exactly on the shared corner. Filling the bottom-left quad must not claim the
   * texels that a neighboring chart also covers, or the fill bleeds at the corner. */
  BMesh *bm = test_bmesh_with_uv();
  const BMUVOffsets offsets = BM_uv_map_offsets_get(bm);
  BMFace *bl = test_quad_uv(bm, offsets, 0.0f, 0.0f, 0.5f);
  test_quad_uv(bm, offsets, 0.5f, 0.0f, 0.5f);
  test_quad_uv(bm, offsets, 0.0f, 0.5f, 0.5f);
  test_quad_uv(bm, offsets, 0.5f, 0.5f, 0.5f);
  BM_mesh_elem_index_ensure(bm, BM_FACE);
  BM_mesh_elem_table_ensure(bm, BM_FACE);

  const int fill[] = {BM_elem_index_get(bl)};
  Array<uint8_t> claim(2 * 2);
  image_paint_uv_claim_buffer_build(bm, offsets, fill, float2(0.0f, 0.0f), 2, 2, claim);

  /* Texel (0,0) center is (0.25, 0.25) — strictly inside the bottom-left quad only. */
  EXPECT_TRUE(claim[0] & UV_CLAIM_FILL_INCLUSIVE);
  EXPECT_TRUE(claim[0] & UV_CLAIM_FILL_STRICT);
  EXPECT_FALSE(claim[0] & UV_CLAIM_FOREIGN_INCLUSIVE);

  /* Texel (1,0) center is (0.75, 0.25) — inside the bottom-right quad only. */
  EXPECT_FALSE(claim[1] & UV_CLAIM_FILL_INCLUSIVE);
  EXPECT_TRUE(claim[1] & UV_CLAIM_FOREIGN_INCLUSIVE);

  BM_mesh_free(bm);
}

TEST_F(ImagePaintUVGeomMeshTest, ClaimBufferSharedEdgeIsNonStrict)
{
  /* Two quads sharing the UV edge u = 0.5 on a 2x1 texture whose texel centers land
   * at u = 0.25 and u = 0.75 — never on the seam. Widen to 4 texels so a center
   * lands exactly on the shared edge at u = 0.5. */
  BMesh *bm = test_bmesh_with_uv();
  const BMUVOffsets offsets = BM_uv_map_offsets_get(bm);
  BMFace *left = test_quad_uv(bm, offsets, 0.0f, 0.0f, 0.5f);
  test_quad_uv(bm, offsets, 0.5f, 0.0f, 0.5f);
  BM_mesh_elem_index_ensure(bm, BM_FACE);
  BM_mesh_elem_table_ensure(bm, BM_FACE);

  const int fill[] = {BM_elem_index_get(left)};
  Array<uint8_t> claim(4 * 1);
  image_paint_uv_claim_buffer_build(bm, offsets, fill, float2(0.0f, 0.0f), 4, 1, claim);

  /* Texel 0 center u = 0.125: strictly inside the left quad. */
  EXPECT_TRUE(claim[0] & UV_CLAIM_FILL_STRICT);
  EXPECT_FALSE(claim[0] & UV_CLAIM_FOREIGN_INCLUSIVE);
  /* Texel 2 center u = 0.625: inside the right quad only. */
  EXPECT_FALSE(claim[2] & UV_CLAIM_FILL_INCLUSIVE);
  EXPECT_TRUE(claim[2] & UV_CLAIM_FOREIGN_INCLUSIVE);

  BM_mesh_free(bm);
}

TEST_F(ImagePaintUVGeomMeshTest, ClaimBufferSkipsFacesOutsideTile)
{
  /* A face living entirely on UDIM tile 1002 must contribute nothing to tile 1001. */
  BMesh *bm = test_bmesh_with_uv();
  const BMUVOffsets offsets = BM_uv_map_offsets_get(bm);
  BMFace *here = test_quad_uv(bm, offsets, 0.0f, 0.0f, 1.0f);
  test_quad_uv(bm, offsets, 1.0f, 0.0f, 1.0f);
  BM_mesh_elem_index_ensure(bm, BM_FACE);
  BM_mesh_elem_table_ensure(bm, BM_FACE);

  const int fill[] = {BM_elem_index_get(here)};
  Array<uint8_t> claim(4 * 4);
  image_paint_uv_claim_buffer_build(bm, offsets, fill, float2(0.0f, 0.0f), 4, 4, claim);

  for (const int i : IndexRange(16)) {
    EXPECT_FALSE(claim[i] & UV_CLAIM_FOREIGN_INCLUSIVE);
    EXPECT_TRUE(claim[i] & UV_CLAIM_FILL_INCLUSIVE);
  }
  BM_mesh_free(bm);
}

TEST_F(ImagePaintUVGeomMeshTest, DilationSingleRingFourConnected)
{
  /* One quad covering the centre of a 5x5 tile, no neighbors. The dilation must add
   * exactly one orthogonal ring: a diagonal texel beyond the corner stays untouched. */
  BMesh *bm = test_bmesh_with_uv();
  const BMUVOffsets offsets = BM_uv_map_offsets_get(bm);
  BMFace *only = test_quad_uv(bm, offsets, 0.4f, 0.4f, 0.2f);
  BM_mesh_elem_index_ensure(bm, BM_FACE);
  BM_mesh_elem_table_ensure(bm, BM_FACE);

  const int fill[] = {BM_elem_index_get(only)};
  Array<uint8_t> claim(5 * 5);
  image_paint_uv_claim_buffer_build(bm, offsets, fill, float2(0.0f, 0.0f), 5, 5, claim);

  /* Only texel (2,2) has its centre (0.5, 0.5) inside the quad. */
  EXPECT_TRUE(claim[2 * 5 + 2] & UV_CLAIM_FILL_INCLUSIVE);
  EXPECT_FALSE(claim[1 * 5 + 1] & UV_CLAIM_FILL_INCLUSIVE);
  EXPECT_FALSE(claim[0 * 5 + 0] & UV_CLAIM_FILL_INCLUSIVE);
  BM_mesh_free(bm);
}

TEST_F(ImagePaintUVGeomMeshTest, UvToObjectPositionOnQuad)
{
  /* Vertex positions deliberately differ from UVs so a wrong mapping is visible:
   * the quad spans 0..1 in UV and 0..10 in X/Y. */
  BMesh *bm = test_bmesh_with_uv();
  const BMUVOffsets offsets = BM_uv_map_offsets_get(bm);
  BMVert *verts[4] = {
      test_vert(bm, 0.0f, 0.0f),
      test_vert(bm, 10.0f, 0.0f),
      test_vert(bm, 10.0f, 10.0f),
      test_vert(bm, 0.0f, 10.0f),
  };
  BMFace *efa = BM_face_create_verts(bm, verts, 4, nullptr, BM_CREATE_NOP, true);
  const float uvs[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
  BMIter liter;
  BMLoop *l;
  int i = 0;
  BM_ITER_ELEM (l, &liter, efa, BM_LOOPS_OF_FACE) {
    float *uv = BM_ELEM_CD_GET_FLOAT_P(l, offsets.uv);
    uv[0] = uvs[i][0];
    uv[1] = uvs[i][1];
    i++;
  }

  float3 position;
  EXPECT_TRUE(image_paint_uv_to_object_position(efa, offsets, float2(0.5f, 0.5f), position));
  EXPECT_NEAR(position.x, 5.0f, 1e-4f);
  EXPECT_NEAR(position.y, 5.0f, 1e-4f);
  EXPECT_NEAR(position.z, 0.0f, 1e-4f);

  EXPECT_TRUE(image_paint_uv_to_object_position(efa, offsets, float2(0.1f, 0.2f), position));
  EXPECT_NEAR(position.x, 1.0f, 1e-4f);
  EXPECT_NEAR(position.y, 2.0f, 1e-4f);

  BM_mesh_free(bm);
}

TEST_F(ImagePaintUVGeomMeshTest, UvToObjectPositionRejectsOutsideUv)
{
  BMesh *bm = test_bmesh_with_uv();
  const BMUVOffsets offsets = BM_uv_map_offsets_get(bm);
  BMFace *efa = test_quad_uv(bm, offsets, 0.0f, 0.0f, 1.0f);
  float3 position;
  EXPECT_FALSE(image_paint_uv_to_object_position(efa, offsets, float2(5.0f, 5.0f), position));
  BM_mesh_free(bm);
}

}  // namespace blender::tests
