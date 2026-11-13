/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <cmath>

#include "BKE_paint_material_decal_map.hh"

#include "BLI_math_rotation.h"
#include "BLI_math_vector.hh"
#include "BLI_vector.hh"

#include "testing/testing.h"

#include "paint_material_composite_projection_internal.hh"

namespace blender::bke::tests {

/* Corner triangle data of a few quads; corner indices differ from vertex indices. */
struct DecalTestMesh {
  Vector<int3> corner_tris;
  Vector<int> corner_verts;
  Vector<float3> positions;
  Vector<float2> uvs;

  void add_quad(const int4 verts, const float2 uv[4])
  {
    const int corner = int(this->corner_verts.size());
    for (const int i : IndexRange(4)) {
      this->corner_verts.append(verts[i]);
      this->uvs.append(uv[i]);
    }
    this->corner_tris.append(int3(corner, corner + 1, corner + 2));
    this->corner_tris.append(int3(corner, corner + 2, corner + 3));
  }

  bool build(const PaintMaterialDecalParams &params,
             const int size,
             PaintMaterialDecalMap &r_map) const
  {
    return BKE_paint_material_decal_map_build(params,
                                              this->corner_tris,
                                              this->corner_verts,
                                              this->positions,
                                              this->uvs,
                                              {},
                                              size,
                                              size,
                                              r_map);
  }
};

/* On a flat surface facing the frame the map coordinate is Planar's `q.xy`, also for a frame
 * turned around its depth axis. */
TEST(paint_material_decal_map, plane_matches_planar)
{
  DecalTestMesh mesh;
  mesh.positions = {float3(-1, -1, 0), float3(1, -1, 0), float3(1, 1, 0), float3(-1, 1, 0)};
  const float2 uv[4] = {float2(0, 0), float2(1, 0), float2(1, 1), float2(0, 1)};
  mesh.add_quad(int4(0, 1, 2, 3), uv);

  const float angles[2] = {0.0f, 0.5f};
  for (const float angle : angles) {
    SCOPED_TRACE(::testing::Message() << "angle=" << angle);
    PaintMaterialDecalParams params;
    params.center = float3(0.1f, -0.2f, 0.0f);
    params.frame_axes[0] = float3(std::cos(angle), std::sin(angle), 0.0f);
    params.frame_axes[1] = float3(-std::sin(angle), std::cos(angle), 0.0f);
    params.frame_axes[2] = float3(0.0f, 0.0f, 1.0f);
    params.axis = 2;
    params.half_size = float2(2.0f, 1.5f);
    params.radius = 4.0f;

    constexpr int size = 32;
    PaintMaterialDecalMap map;
    ASSERT_TRUE(mesh.build(params, size, map));
    ASSERT_EQ(map.q.size(), int64_t(size) * size);
    for (const int y : IndexRange(size)) {
      for (const int x : IndexRange(size)) {
        const int64_t index = int64_t(y) * size + x;
        ASSERT_GE(map.patch_tri[index], 0);
        const float3 p(2.0f * (float(x) + 0.5f) / size - 1.0f,
                       2.0f * (float(y) + 0.5f) / size - 1.0f,
                       0.0f);
        const float3 offset = p - params.center;
        const float2 expected(math::dot(offset, params.frame_axes[0]) / params.half_size.x,
                              math::dot(offset, params.frame_axes[1]) / params.half_size.y);
        EXPECT_NEAR(map.q[index].x, expected.x, 1e-4f);
        EXPECT_NEAR(map.q[index].y, expected.y, 1e-4f);
      }
    }
  }
}

/* Two faces of a cube whose shared edge is a UV seam (the faces sit in separate islands): the map
 * coordinate runs on across the edge as if the front face were folded flat under the top one. */
TEST(paint_material_decal_map, cube_seam_is_continuous)
{
  DecalTestMesh mesh;
  mesh.positions = {float3(-1, -1, 1),
                    float3(1, -1, 1),
                    float3(1, 1, 1),
                    float3(-1, 1, 1),
                    float3(-1, -1, -1),
                    float3(1, -1, -1)};
  /* The top (+Z) face: u = 0.25 + 0.15 x, v = 0.75 + 0.15 y. */
  const float2 top_uv[4] = {
      float2(0.1f, 0.6f), float2(0.4f, 0.6f), float2(0.4f, 0.9f), float2(0.1f, 0.9f)};
  mesh.add_quad(int4(0, 1, 2, 3), top_uv);
  /* The front (-Y) face, in its own island: u = 0.75 + 0.15 x, v = 0.25 + 0.15 z. */
  const float2 front_uv[4] = {
      float2(0.6f, 0.1f), float2(0.9f, 0.1f), float2(0.9f, 0.4f), float2(0.6f, 0.4f)};
  mesh.add_quad(int4(4, 5, 1, 0), front_uv);

  PaintMaterialDecalParams params;
  params.center = float3(0.0f, 0.0f, 1.0f);
  params.axis = 2;
  params.half_size = float2(4.0f, 4.0f);
  params.radius = 6.0f;

  constexpr int size = 128;
  PaintMaterialDecalMap map;
  ASSERT_TRUE(mesh.build(params, size, map));

  /* The seed's plane has X along the world X and Y along the world Y (`N x X`, `N = +Z`). The top
   * face reads `(x, y)`; the front face hangs below the edge `y = -1`, so a point at height `z` is
   * `1 - z` past it: `(x, -2 + z)`. */
  int top_checked = 0;
  int front_checked = 0;
  constexpr float margin = 0.02f;
  for (const int y : IndexRange(size)) {
    for (const int x : IndexRange(size)) {
      const float u = (float(x) + 0.5f) / size;
      const float v = (float(y) + 0.5f) / size;
      const int64_t index = int64_t(y) * size + x;
      float2 expected_plane;
      if (u > 0.1f + margin && u < 0.4f - margin && v > 0.6f + margin && v < 0.9f - margin) {
        expected_plane = float2((u - 0.25f) / 0.15f, (v - 0.75f) / 0.15f);
        top_checked++;
      }
      else if (u > 0.6f + margin && u < 0.9f - margin && v > 0.1f + margin && v < 0.4f - margin) {
        expected_plane = float2((u - 0.75f) / 0.15f, -2.0f + (v - 0.25f) / 0.15f);
        front_checked++;
      }
      else {
        continue;
      }
      ASSERT_GE(map.patch_tri[index], 0);
      EXPECT_NEAR(map.q[index].x, expected_plane.x / params.half_size.x, 1e-3f);
      EXPECT_NEAR(map.q[index].y, expected_plane.y / params.half_size.y, 1e-3f);
    }
  }
  EXPECT_GT(top_checked, 0);
  EXPECT_GT(front_checked, 0);
  EXPECT_EQ(map.patch_tris_num, 4);
  /* Texels outside both islands and their padding stay empty. */
  EXPECT_EQ(map.patch_tri[0], -1);
}

/* The inverse map returns the tile UV of the surface point the forward map put at a map coordinate;
 * on a flat frame whose UV is the frame square the two are each other's inverse. */
TEST(paint_material_decal_map, inverse_roundtrip_plane)
{
  DecalTestMesh mesh;
  mesh.positions = {float3(-1, -1, 0), float3(1, -1, 0), float3(1, 1, 0), float3(-1, 1, 0)};
  const float2 uv[4] = {float2(0, 0), float2(1, 0), float2(1, 1), float2(0, 1)};
  mesh.add_quad(int4(0, 1, 2, 3), uv);

  PaintMaterialDecalParams params;
  params.center = float3(0.1f, -0.2f, 0.0f);
  params.axis = 2;
  params.half_size = float2(2.0f, 1.5f);
  params.radius = 4.0f;

  constexpr int size = 64;
  PaintMaterialDecalMap map;
  ASSERT_TRUE(mesh.build(params, size, map));
  PaintMaterialDecalInverseMap inverse;
  ASSERT_TRUE(BKE_paint_material_decal_inverse_map_build(map, size, inverse));

  for (const int y : IndexRange(size)) {
    for (const int x : IndexRange(size)) {
      const int64_t index = int64_t(y) * size + x;
      if (map.patch_tri[index] < 0) {
        continue;
      }
      const float2 texel_uv((float(x) + 0.5f) / size, (float(y) + 0.5f) / size);
      float2 tile_uv;
      int tri = -1;
      ASSERT_TRUE(BKE_paint_material_decal_inverse_lookup(
          map, inverse, map.q[index], tile_uv, tri));
      EXPECT_NEAR(tile_uv.x, texel_uv.x, 1.5f / size);
      EXPECT_NEAR(tile_uv.y, texel_uv.y, 1.5f / size);

      float2 q;
      ASSERT_TRUE(BKE_paint_material_decal_forward_lookup(map, texel_uv, q, tri));
      EXPECT_NEAR(q.x, map.q[index].x, 1.5f / size);
      EXPECT_NEAR(q.y, map.q[index].y, 1.5f / size);
    }
  }
}

/* A Decal-space Mirror on a flat frame reads the same as a UV Mirror: the frame square is the UV
 * square, so folding around the frame center is folding around the UV center. */
TEST(paint_material_decal_map, effect_space_mirror_matches_uv_on_plane)
{
  DecalTestMesh mesh;
  mesh.positions = {float3(-1, -1, 0), float3(1, -1, 0), float3(1, 1, 0), float3(-1, 1, 0)};
  const float2 uv[4] = {float2(0, 0), float2(1, 0), float2(1, 1), float2(0, 1)};
  mesh.add_quad(int4(0, 1, 2, 3), uv);

  PaintMaterialDecalParams params;
  params.axis = 2;
  params.half_size = float2(1.0f, 1.0f);
  params.radius = 2.0f;

  constexpr int size = 64;
  PaintMaterialDecalMap map;
  ASSERT_TRUE(mesh.build(params, size, map));
  PaintMaterialDecalInverseMap inverse;
  ASSERT_TRUE(BKE_paint_material_decal_inverse_map_build(map, size, inverse));

  CompositeMappingTransform mirror;
  mirror.mirror_u = true;
  mirror.mirror_center_u = 0.5f;

  CompositeEffectSpaceChain chain;
  chain.steps[chain.steps_num].transform = mirror;
  chain.steps[chain.steps_num].decal = true;
  chain.steps_num++;
  chain.decal_map = &map;
  chain.inverse_map = &inverse;

  CompositeMappingChain uv_chain;
  uv_chain.steps[uv_chain.steps_num++] = mirror;

  for (const int y : IndexRange(size)) {
    for (const int x : IndexRange(size)) {
      const int64_t index = int64_t(y) * size + x;
      if (map.patch_tri[index] < 0) {
        continue;
      }
      const float2 texel_uv((float(x) + 0.5f) / size, (float(y) + 0.5f) / size);
      float2 decal_uv;
      ASSERT_EQ(composite_effect_space_resolve(chain, texel_uv, index, decal_uv, nullptr),
                CompositeEffectSpaceResult::Mapped);
      float uv_u;
      float uv_v;
      ASSERT_TRUE(uv_chain.apply(texel_uv.x, texel_uv.y, uv_u, uv_v));
      EXPECT_NEAR(decal_uv.x, uv_u, 1.5f / size);
      EXPECT_NEAR(decal_uv.y, uv_v, 1.5f / size);
    }
  }
}

/* The order of mixed spaces is the list's: a Decal fold followed by a UV move reads the same as
 * the same two steps composed in UV on a flat frame (the conversions leave and re-enter the map). */
TEST(paint_material_decal_map, effect_space_mixes_uv_and_decal_in_order)
{
  DecalTestMesh mesh;
  mesh.positions = {float3(-1, -1, 0), float3(1, -1, 0), float3(1, 1, 0), float3(-1, 1, 0)};
  const float2 uv[4] = {float2(0, 0), float2(1, 0), float2(1, 1), float2(0, 1)};
  mesh.add_quad(int4(0, 1, 2, 3), uv);

  PaintMaterialDecalParams params;
  params.axis = 2;
  params.half_size = float2(1.0f, 1.0f);
  params.radius = 2.0f;

  constexpr int size = 64;
  PaintMaterialDecalMap map;
  ASSERT_TRUE(mesh.build(params, size, map));
  PaintMaterialDecalInverseMap inverse;
  ASSERT_TRUE(BKE_paint_material_decal_inverse_map_build(map, size, inverse));

  CompositeMappingTransform mirror;
  mirror.mirror_u = true;
  mirror.mirror_center_u = 0.5f;
  CompositeMappingTransform move;
  move.offset_u = 0.25f;

  CompositeEffectSpaceChain chain;
  chain.steps[chain.steps_num].transform = mirror;
  chain.steps[chain.steps_num].decal = true;
  chain.steps_num++;
  chain.steps[chain.steps_num].transform = move;
  chain.steps[chain.steps_num].decal = false;
  chain.steps_num++;
  chain.decal_map = &map;
  chain.inverse_map = &inverse;

  CompositeMappingChain uv_chain;
  uv_chain.steps[uv_chain.steps_num++] = mirror;
  uv_chain.steps[uv_chain.steps_num++] = move;

  for (const int y : IndexRange(size)) {
    for (const int x : IndexRange(size)) {
      const int64_t index = int64_t(y) * size + x;
      if (map.patch_tri[index] < 0) {
        continue;
      }
      const float2 texel_uv((float(x) + 0.5f) / size, (float(y) + 0.5f) / size);
      float2 decal_uv;
      ASSERT_EQ(composite_effect_space_resolve(chain, texel_uv, index, decal_uv, nullptr),
                CompositeEffectSpaceResult::Mapped);
      float uv_u;
      float uv_v;
      ASSERT_TRUE(uv_chain.apply(texel_uv.x, texel_uv.y, uv_u, uv_v));
      /* The composed lookup repeats at its end; the resolver leaves the wrap to the sampler. */
      EXPECT_NEAR(decal_uv.x - std::floor(decal_uv.x), uv_u, 1.5f / size);
      EXPECT_NEAR(decal_uv.y - std::floor(decal_uv.y), uv_v, 1.5f / size);
    }
  }
}

/* A Decal step keeps the content on the surface across a UV seam: a move that starts on the top
 * face and lands on the front face reads the front face's UV, not a mirrored top-face island. */
TEST(paint_material_decal_map, effect_space_crosses_cube_seam)
{
  DecalTestMesh mesh;
  mesh.positions = {float3(-1, -1, 1),
                    float3(1, -1, 1),
                    float3(1, 1, 1),
                    float3(-1, 1, 1),
                    float3(-1, -1, -1),
                    float3(1, -1, -1)};
  const float2 top_uv[4] = {
      float2(0.1f, 0.6f), float2(0.4f, 0.6f), float2(0.4f, 0.9f), float2(0.1f, 0.9f)};
  mesh.add_quad(int4(0, 1, 2, 3), top_uv);
  const float2 front_uv[4] = {
      float2(0.6f, 0.1f), float2(0.9f, 0.1f), float2(0.9f, 0.4f), float2(0.6f, 0.4f)};
  mesh.add_quad(int4(4, 5, 1, 0), front_uv);

  PaintMaterialDecalParams params;
  params.center = float3(0.0f, 0.0f, 1.0f);
  params.axis = 2;
  params.half_size = float2(4.0f, 4.0f);
  params.radius = 6.0f;

  constexpr int size = 128;
  PaintMaterialDecalMap map;
  ASSERT_TRUE(mesh.build(params, size, map));
  PaintMaterialDecalInverseMap inverse;
  ASSERT_TRUE(BKE_paint_material_decal_inverse_map_build(map, size, inverse));

  /* Half a frame down in the unfolded plane: the top face's center (`q = (0, 0)` in the frame's
   * units) lands at `y = -2`, the front face's middle. */
  CompositeMappingTransform move;
  move.offset_v = -0.25f;

  CompositeEffectSpaceChain chain;
  chain.steps[chain.steps_num].transform = move;
  chain.steps[chain.steps_num].decal = true;
  chain.steps_num++;
  chain.decal_map = &map;
  chain.inverse_map = &inverse;

  const int x = 32;
  const int y = 96;
  const int64_t index = int64_t(y) * size + x;
  ASSERT_GE(map.patch_tri[index], 0);
  const float2 texel_uv((float(x) + 0.5f) / size, (float(y) + 0.5f) / size);
  float2 src_uv;
  ASSERT_EQ(composite_effect_space_resolve(chain, texel_uv, index, src_uv, nullptr),
            CompositeEffectSpaceResult::Mapped);
  /* The front face: `u = 0.75 + 0.15 x`, `v = 0.25 + 0.15 z`, read at its center. */
  EXPECT_NEAR(src_uv.x, 0.75f, 0.01f);
  EXPECT_NEAR(src_uv.y, 0.25f, 0.01f);
}

/* The Decal params of a frame derived from a UV point, as the Decal map's cache builds them. */
static PaintMaterialDecalParams decal_params_from_uv_frame(const PaintMaterialDecalUvFrame &frame)
{
  PaintMaterialDecalParams params;
  float rot[3][3];
  eul_to_mat3(rot, frame.rotation);
  params.center = frame.center;
  for (const int i : IndexRange(3)) {
    params.frame_axes[i] = float3(rot[i]);
  }
  params.axis = 2;
  params.half_size = frame.half_size;
  params.radius = frame.radius;
  return params;
}

/* The frame of a UV point is derived, not placed: its center is the surface point, its half extent
 * the surface length of one UV unit over two, and its axes follow the UV directions. */
TEST(paint_material_decal_map, frame_from_uv_on_plane)
{
  DecalTestMesh mesh;
  mesh.positions = {float3(-1, -1, 0), float3(1, -1, 0), float3(1, 1, 0), float3(-1, 1, 0)};
  const float2 uv[4] = {float2(0, 0), float2(1, 0), float2(1, 1), float2(0, 1)};
  mesh.add_quad(int4(0, 1, 2, 3), uv);

  PaintMaterialDecalUvFrame frame;
  ASSERT_TRUE(BKE_paint_material_decal_frame_from_uv_mesh(
      mesh.corner_tris, mesh.corner_verts, mesh.positions, mesh.uvs, {}, 1001, float2(0.3f, 0.6f), frame));
  EXPECT_NEAR(frame.center.x, -0.4f, 1e-5f);
  EXPECT_NEAR(frame.center.y, 0.2f, 1e-5f);
  EXPECT_NEAR(frame.center.z, 0.0f, 1e-5f);
  EXPECT_NEAR(frame.half_size.x, 1.0f, 1e-5f);
  EXPECT_NEAR(frame.half_size.y, 1.0f, 1e-5f);
  /* The U axis is the world X, so the frame is not turned. */
  float rot[3][3];
  eul_to_mat3(rot, frame.rotation);
  EXPECT_NEAR(rot[0][0], 1.0f, 1e-5f);
  EXPECT_NEAR(rot[1][1], 1.0f, 1e-5f);
  /* The patch reaches the whole object. */
  EXPECT_GE(frame.radius, 2.0f * std::sqrt(2.0f) - 1e-4f);
}

/* A Decal Mirror whose center is an arbitrary UV point folds like the UV Mirror around that point,
 * while the effect's frame is derived from it: the step's own center is the frame's (0.5, 0.5). */
TEST(paint_material_decal_map, effect_space_mirror_about_uv_point_matches_uv)
{
  DecalTestMesh mesh;
  mesh.positions = {float3(-1, -1, 0), float3(1, -1, 0), float3(1, 1, 0), float3(-1, 1, 0)};
  const float2 uv[4] = {float2(0, 0), float2(1, 0), float2(1, 1), float2(0, 1)};
  mesh.add_quad(int4(0, 1, 2, 3), uv);

  const float2 center_uv(0.3f, 0.6f);
  PaintMaterialDecalUvFrame frame;
  ASSERT_TRUE(BKE_paint_material_decal_frame_from_uv_mesh(
      mesh.corner_tris, mesh.corner_verts, mesh.positions, mesh.uvs, {}, 1001, center_uv, frame));

  constexpr int size = 64;
  PaintMaterialDecalMap map;
  ASSERT_TRUE(mesh.build(decal_params_from_uv_frame(frame), size, map));
  PaintMaterialDecalInverseMap inverse;
  ASSERT_TRUE(BKE_paint_material_decal_inverse_map_build(map, size, inverse));

  CompositeMappingTransform decal_mirror;
  decal_mirror.mirror_u = true;
  decal_mirror.mirror_center_u = 0.5f;
  CompositeMappingTransform uv_mirror;
  uv_mirror.mirror_u = true;
  uv_mirror.mirror_center_u = center_uv.x;

  CompositeEffectSpaceChain chain;
  chain.steps[chain.steps_num].transform = decal_mirror;
  chain.steps[chain.steps_num].decal = true;
  chain.steps_num++;
  chain.decal_map = &map;
  chain.inverse_map = &inverse;

  CompositeMappingChain uv_chain;
  uv_chain.steps[uv_chain.steps_num++] = uv_mirror;

  int checked = 0;
  for (const int y : IndexRange(size)) {
    for (const int x : IndexRange(size)) {
      const int64_t index = int64_t(y) * size + x;
      const float2 texel_uv((float(x) + 0.5f) / size, (float(y) + 0.5f) / size);
      /* Inside the island on both sides of the fold: the mirrored source stays on the quad. */
      const float source_u = 2.0f * center_uv.x - texel_uv.x;
      if (source_u < 0.03f || source_u > 0.97f) {
        continue;
      }
      ASSERT_GE(map.patch_tri[index], 0);
      float2 decal_uv;
      ASSERT_EQ(composite_effect_space_resolve(chain, texel_uv, index, decal_uv, nullptr),
                CompositeEffectSpaceResult::Mapped);
      float uv_u;
      float uv_v;
      ASSERT_TRUE(uv_chain.apply(texel_uv.x, texel_uv.y, uv_u, uv_v));
      EXPECT_NEAR(decal_uv.x, uv_u, 1.5f / size);
      EXPECT_NEAR(decal_uv.y, uv_v, 1.5f / size);
      checked++;
    }
  }
  EXPECT_GT(checked, 0);
}

/* A Mirror center near a cube edge: the fold reads across the UV seam, so a texel on the top face
 * takes the content of the front face where the surface continues, not an unrelated island. */
TEST(paint_material_decal_map, effect_space_mirror_about_uv_point_crosses_seam)
{
  DecalTestMesh mesh;
  mesh.positions = {float3(-1, -1, 1),
                    float3(1, -1, 1),
                    float3(1, 1, 1),
                    float3(-1, 1, 1),
                    float3(-1, -1, -1),
                    float3(1, -1, -1)};
  const float2 top_uv[4] = {
      float2(0.1f, 0.6f), float2(0.4f, 0.6f), float2(0.4f, 0.9f), float2(0.1f, 0.9f)};
  mesh.add_quad(int4(0, 1, 2, 3), top_uv);
  const float2 front_uv[4] = {
      float2(0.6f, 0.1f), float2(0.9f, 0.1f), float2(0.9f, 0.4f), float2(0.6f, 0.4f)};
  mesh.add_quad(int4(4, 5, 1, 0), front_uv);

  /* On the top face at `y = -2/3`, a third of the way to the front edge `y = -1`. */
  const float2 center_uv(0.25f, 0.65f);
  PaintMaterialDecalUvFrame frame;
  ASSERT_TRUE(BKE_paint_material_decal_frame_from_uv_mesh(
      mesh.corner_tris, mesh.corner_verts, mesh.positions, mesh.uvs, {}, 1001, center_uv, frame));
  EXPECT_NEAR(frame.center.y, -2.0f / 3.0f, 1e-4f);
  EXPECT_NEAR(frame.center.z, 1.0f, 1e-4f);

  constexpr int size = 128;
  PaintMaterialDecalMap map;
  ASSERT_TRUE(mesh.build(decal_params_from_uv_frame(frame), size, map));
  PaintMaterialDecalInverseMap inverse;
  ASSERT_TRUE(BKE_paint_material_decal_inverse_map_build(map, size, inverse));

  CompositeMappingTransform decal_mirror;
  decal_mirror.mirror_v = true;
  decal_mirror.mirror_center_v = 0.5f;
  CompositeEffectSpaceChain chain;
  chain.steps[chain.steps_num].transform = decal_mirror;
  chain.steps[chain.steps_num].decal = true;
  chain.steps_num++;
  chain.decal_map = &map;
  chain.inverse_map = &inverse;

  /* The top face's texel at `x = 0, y = -0.2` (`u = 0.25`, `v = 0.72`) is `0.467` above the center;
   * its mirror is `0.467` below it, `y = -1.133`: `0.133` down the front face, at height `0.867`. */
  const float texel_u = 0.25f;
  const float texel_v = 0.75f + 0.15f * -0.2f;
  const int x = int(texel_u * size);
  const int y = int(texel_v * size);
  const int64_t index = int64_t(y) * size + x;
  ASSERT_GE(map.patch_tri[index], 0);
  float2 src_uv;
  ASSERT_EQ(composite_effect_space_resolve(chain,
                                           float2((float(x) + 0.5f) / size, (float(y) + 0.5f) / size),
                                           index,
                                           src_uv,
                                           nullptr),
            CompositeEffectSpaceResult::Mapped);
  EXPECT_NEAR(src_uv.x, 0.75f, 0.015f);
  EXPECT_NEAR(src_uv.y, 0.25f + 0.15f * 0.867f, 0.015f);
}

/* The Decal Mirror keeps the same half as the UV Mirror, on U and on V alone and on both, whichever
 * way the island lies: the UV fold keeps the side past the center along +U / +V, so the Decal fold
 * must keep the side along the UV's own directions (the frame's axes follow the island's tangent and
 * bitangent, also for a turned or a mirrored island and for a quad wound the other way). The content
 * of the kept half is what a stroke on that side shows after the fold, so it is checked by where the
 * source lands (the larger-UV side of the center), not only by agreeing with the UV chain. */
TEST(paint_material_decal_map, effect_space_mirror_keeps_the_uv_half_on_each_axis)
{
  /* The island's UV of a plane point (`p` in `[0, 1]` per axis): identity, turned a quarter, and
   * mirrored in U and in V. */
  const auto uv_of = [](const int variant, const float2 &p) {
    switch (variant) {
      case 0:
        return p;
      case 1:
        return float2(1.0f - p.y, p.x);
      case 2:
        return float2(1.0f - p.x, p.y);
      default:
        return float2(p.x, 1.0f - p.y);
    }
  };
  const float2 center_uv(0.3f, 0.6f);
  /* U, V, and whether the fold is the negative direction (keeps the smaller side). */
  const bool mirror_axes[6][3] = {{true, false, false},
                                  {false, true, false},
                                  {true, true, false},
                                  {true, false, true},
                                  {false, true, true},
                                  {true, true, true}};

  for (const int variant : IndexRange(4)) {
    for (const bool reversed : {false, true}) {
      DecalTestMesh mesh;
      mesh.positions = {float3(-1, -1, 0), float3(1, -1, 0), float3(1, 1, 0), float3(-1, 1, 0)};
      const float2 plane_uv[4] = {float2(0, 0), float2(1, 0), float2(1, 1), float2(0, 1)};
      /* A quad wound the other way has the opposite corner-order normal: the unfold's V flip. */
      const int4 verts = reversed ? int4(3, 2, 1, 0) : int4(0, 1, 2, 3);
      float2 uv[4];
      for (const int i : IndexRange(4)) {
        uv[i] = uv_of(variant, plane_uv[verts[i]]);
      }
      mesh.add_quad(verts, uv);

      PaintMaterialDecalUvFrame frame;
      ASSERT_TRUE(BKE_paint_material_decal_frame_from_uv_mesh(mesh.corner_tris,
                                                              mesh.corner_verts,
                                                              mesh.positions,
                                                              mesh.uvs,
                                                              {},
                                                              1001,
                                                              center_uv,
                                                              frame));
      constexpr int size = 64;
      PaintMaterialDecalMap map;
      ASSERT_TRUE(mesh.build(decal_params_from_uv_frame(frame), size, map));
      PaintMaterialDecalInverseMap inverse;
      ASSERT_TRUE(BKE_paint_material_decal_inverse_map_build(map, size, inverse));

      for (const auto &axes : mirror_axes) {
        SCOPED_TRACE(::testing::Message() << "variant=" << variant << " reversed=" << reversed
                                          << " mirror u=" << axes[0] << " v=" << axes[1]
                                          << " negative=" << axes[2]);
        CompositeMappingTransform decal_mirror;
        decal_mirror.mirror_u = axes[0];
        decal_mirror.mirror_v = axes[1];
        decal_mirror.mirror_negative_u = axes[0] && axes[2];
        decal_mirror.mirror_negative_v = axes[1] && axes[2];
        decal_mirror.mirror_center_u = 0.5f;
        decal_mirror.mirror_center_v = 0.5f;
        CompositeMappingTransform uv_mirror = decal_mirror;
        uv_mirror.mirror_center_u = center_uv.x;
        uv_mirror.mirror_center_v = center_uv.y;

        CompositeEffectSpaceChain chain;
        chain.steps[chain.steps_num].transform = decal_mirror;
        chain.steps[chain.steps_num].decal = true;
        chain.steps_num++;
        chain.decal_map = &map;
        chain.inverse_map = &inverse;
        CompositeMappingChain uv_chain;
        uv_chain.steps[uv_chain.steps_num++] = uv_mirror;

        int checked = 0;
        constexpr float eps = 1.5f / size;
        for (const int y : IndexRange(size)) {
          for (const int x : IndexRange(size)) {
            const int64_t index = int64_t(y) * size + x;
            const float2 texel_uv((float(x) + 0.5f) / size, (float(y) + 0.5f) / size);
            float uv_u;
            float uv_v;
            ASSERT_TRUE(uv_chain.apply(texel_uv.x, texel_uv.y, uv_u, uv_v));
            /* Inside the island on both sides of the fold: the mirrored source stays on the quad. */
            if (uv_u < 0.03f || uv_u > 0.97f || uv_v < 0.03f || uv_v > 0.97f) {
              continue;
            }
            ASSERT_GE(map.patch_tri[index], 0);
            float2 decal_uv;
            ASSERT_EQ(composite_effect_space_resolve(chain, texel_uv, index, decal_uv, nullptr),
                      CompositeEffectSpaceResult::Mapped);
            EXPECT_NEAR(decal_uv.x, uv_u, eps);
            EXPECT_NEAR(decal_uv.y, uv_v, eps);
            /* The kept half is the one past the center along +U / +V; no source is on the other. */
            if (axes[0]) {
              if (axes[2]) {
                EXPECT_LE(decal_uv.x, center_uv.x + eps);
              }
              else {
                EXPECT_GE(decal_uv.x, center_uv.x - eps);
              }
            }
            if (axes[1]) {
              if (axes[2]) {
                EXPECT_LE(decal_uv.y, center_uv.y + eps);
              }
              else {
                EXPECT_GE(decal_uv.y, center_uv.y - eps);
              }
            }
            checked++;
          }
        }
        EXPECT_GT(checked, 0);
      }
    }
  }
}

/* The positive fold keeps the larger side of the center and the negative one the smaller side, per
 * axis, so on content that is not symmetric (the coordinate itself) the two read mirrored points; a
 * Normal's side sign flips with the direction. */
TEST(paint_material_composite_mapping, mirror_negative_keeps_the_opposite_half_per_axis)
{
  PaintMaterialCompositeMapping mapping;
  mapping.enabled = true;
  mapping.mirror_center[0] = 0.4f;
  mapping.mirror_center[1] = 0.6f;

  const auto fold = [&](const int16_t axes, const float u, const float v, float2 &r_side) {
    mapping.mirror_axes = axes;
    const CompositeMappingTransform transform(mapping);
    float ru = 0.0f;
    float rv = 0.0f;
    EXPECT_TRUE(transform.apply(u, v, ru, rv));
    r_side = float2(transform.side_u(u), transform.side_v(v));
    return float2(ru, rv);
  };
  constexpr int16_t U = MA_PAINT_LAYER_MAPPING_MIRROR_U;
  constexpr int16_t V = MA_PAINT_LAYER_MAPPING_MIRROR_V;
  constexpr int16_t UN = MA_PAINT_LAYER_MAPPING_MIRROR_U_NEGATIVE;
  constexpr int16_t VN = MA_PAINT_LAYER_MAPPING_MIRROR_V_NEGATIVE;

  float2 side;
  /* U "+": the smaller side (0.1) reads the larger one (0.7); the larger side stays. */
  EXPECT_NEAR(fold(U, 0.1f, 0.3f, side).x, 0.7f, 1e-5f);
  EXPECT_EQ(side.x, -1.0f);
  EXPECT_NEAR(fold(U, 0.7f, 0.3f, side).x, 0.7f, 1e-5f);
  EXPECT_EQ(side.x, 1.0f);
  /* U "-": the larger side (0.7) reads the smaller one (0.1); the smaller side stays; sides flip. */
  EXPECT_NEAR(fold(U | UN, 0.7f, 0.3f, side).x, 0.1f, 1e-5f);
  EXPECT_EQ(side.x, -1.0f);
  EXPECT_NEAR(fold(U | UN, 0.1f, 0.3f, side).x, 0.1f, 1e-5f);
  EXPECT_EQ(side.x, 1.0f);
  /* V is independent of U: "-" on V only leaves U on its original fold. */
  const float2 mixed = fold(U | V | VN, 0.1f, 0.9f, side);
  EXPECT_NEAR(mixed.x, 0.7f, 1e-5f);
  EXPECT_NEAR(mixed.y, 0.3f, 1e-5f);
  EXPECT_EQ(side.x, -1.0f);
  EXPECT_EQ(side.y, -1.0f);
  /* U and V both "-" mirror the "+" result through the center on each axis. */
  const float2 positive = fold(U | V, 0.2f, 0.5f, side);
  const float2 negative = fold(U | V | UN | VN, 0.2f, 0.5f, side);
  EXPECT_NEAR(negative.x, 2.0f * 0.4f - positive.x, 1e-5f);
  EXPECT_NEAR(negative.y, 2.0f * 0.6f - positive.y, 1e-5f);
  /* A direction bit without its axis does nothing. */
  EXPECT_NEAR(fold(UN, 0.1f, 0.3f, side).x, 0.1f, 1e-5f);
}

/* The cached resolve field equals a texel-by-texel resolve; a second request (another channel) hits
 * the cache and reads the same values; a changed step number is a miss with the new values. */
TEST(paint_material_decal_map, effect_space_field_matches_resolve_and_invalidates)
{
  DecalTestMesh mesh;
  mesh.positions = {float3(-1, -1, 0), float3(1, -1, 0), float3(1, 1, 0), float3(-1, 1, 0)};
  const float2 uv[4] = {float2(0, 0), float2(1, 0), float2(1, 1), float2(0, 1)};
  mesh.add_quad(int4(0, 1, 2, 3), uv);

  PaintMaterialDecalParams params;
  params.axis = 2;
  params.half_size = float2(1.0f, 1.0f);
  params.radius = 2.0f;

  constexpr int size = 32;
  PaintMaterialDecalMap map;
  ASSERT_TRUE(mesh.build(params, size, map));
  /* A hand-built map has an empty key; a distinct one keeps the test off other tests' entries. */
  map.key.frame_hash = 0x5eed1234abcdULL;
  PaintMaterialDecalInverseMap inverse;
  ASSERT_TRUE(BKE_paint_material_decal_inverse_map_build(map, size, inverse));

  CompositeMappingTransform step;
  step.mirror_u = true;
  step.offset_u = 0.1f;

  CompositeEffectSpaceChain chain;
  chain.steps[chain.steps_num].transform = step;
  chain.steps[chain.steps_num].decal = true;
  chain.steps_num++;
  chain.decal_map = &map;
  chain.inverse_map = &inverse;

  const auto expect_field_equals_resolve = [&](const CompositeEffectSpaceField &field) {
    for (const int y : IndexRange(size)) {
      for (const int x : IndexRange(size)) {
        const int64_t index = int64_t(y) * size + x;
        const float2 texel_uv((float(x) + 0.5f) / size, (float(y) + 0.5f) / size);
        float2 direct(0.0f);
        const CompositeEffectSpaceResult result = composite_effect_space_resolve(
            chain, texel_uv, index, direct, nullptr);
        ASSERT_EQ(int(field.status[index]), int(result));
        if (result == CompositeEffectSpaceResult::Mapped) {
          ASSERT_EQ(field.src_uv[index].x, direct.x);
          ASSERT_EQ(field.src_uv[index].y, direct.y);
        }
      }
    }
  };

  bool hit = true;
  double build_seconds = 0.0;
  const std::shared_ptr<const CompositeEffectSpaceField> first = composite_effect_space_field_get(
      chain, size, size, hit, build_seconds);
  ASSERT_TRUE(first != nullptr);
  EXPECT_FALSE(hit);
  expect_field_equals_resolve(*first);

  const std::shared_ptr<const CompositeEffectSpaceField> second = composite_effect_space_field_get(
      chain, size, size, hit, build_seconds);
  EXPECT_TRUE(hit);
  EXPECT_EQ(first.get(), second.get());

  chain.steps[0].transform.offset_u = 0.2f;
  const std::shared_ptr<const CompositeEffectSpaceField> moved = composite_effect_space_field_get(
      chain, size, size, hit, build_seconds);
  ASSERT_TRUE(moved != nullptr);
  EXPECT_FALSE(hit);
  EXPECT_NE(first.get(), moved.get());
  expect_field_equals_resolve(*moved);
}

}  // namespace blender::bke::tests
