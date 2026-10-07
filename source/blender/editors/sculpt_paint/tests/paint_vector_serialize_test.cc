/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "../shapes/paint_vector_document.hh"

#include "DNA_paint_vector_types.h"
#include "DNA_screen_types.h"

#include "BKE_mesh_sample.hh"
#include "BKE_paint_vector.hh"

#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"

#include "testing/testing.h"

namespace blender::ed::sculpt_paint::shape {

/* -------------------------------------------------------------------- */
/** \name Fixtures
 * \{ */

static VectorItem make_spline_item()
{
  VectorItem item;
  item.type = VectorItemType::Shape;
  item.shape.type = PAINT_SHAPE_LINE;
  item.shape.origin = float2(33.0f, 44.0f);
  item.shape.origin_is_custom = true;

  ShapeSpline spline;
  spline.is_bezier = true;
  spline.cyclic = true;
  ShapePoint a;
  a.co = float2(10.0f, 20.0f);
  a.handle_left = float2(1.0f, 2.0f);
  a.handle_right = float2(3.0f, 4.0f);
  a.corner = true;
  a.auto_handles = false;
  a.width_factor = 1.5f;
  spline.points.append(a);
  spline.points.append(ShapePoint{float2(30.0f, 40.0f), float2(5), float2(6), false, 0.5f, true});
  item.shape.splines.append(std::move(spline));

  item.space.type = ShapeSpaceType::ViewProjector;
  item.space.ref_tile = 1002;
  item.space.ref_tile_size = int2(512, 256);
  item.space.win_size = int2(1920, 1080);
  item.space.clip_start = 0.1f;
  item.space.clip_end = 250.0f;
  item.space.is_persp = true;
  item.space.persmat[0][0] = 1.25f;
  item.space.persmat[3][2] = -7.5f;
  item.space.viewinv[1][3] = 0.5f;
  return item;
}

static VectorItem make_parametric_item()
{
  VectorItem item;
  item.type = VectorItemType::Shape;
  item.shape.type = PAINT_SHAPE_ELLIPSE;
  item.shape.center = float2(60.0f, 70.0f);
  item.shape.half_size = float2(30.0f, 18.0f);
  item.shape.rotation = 0.75f;
  item.shape.corner_radius = float4(1.0f, 2.0f, 3.0f, 4.0f);
  item.shape.polygon_sides = 5;
  item.space.type = ShapeSpaceType::CanvasUV;
  item.space.ref_tile = 1001;
  item.space.ref_tile_size = int2(1024, 1024);
  return item;
}

static PaintShapeSettings test_settings()
{
  PaintShapeSettings settings = {};
  settings.stroke_width = 42.0f;
  settings.fill_type = PAINT_SHAPE_FILL_TEXTURE;
  return settings;
}

TEST(PaintVectorSerialize, RoundTripTextureSettings)
{
  /* The texture parts and the Curve Pattern are plain PODs inside the style snapshot: a
   * document round-trip must carry them verbatim. */
  PaintShapeSettings settings = test_settings();
  settings.fill_type = PAINT_SHAPE_FILL_TEXTURE;
  settings.stroke_type = PAINT_SHAPE_STROKE_TEXTURE;
  settings.fill_texture.mapping = PAINT_SHAPE_TEX_MAP_MASK;
  settings.fill_texture.fit_mode = PAINT_SHAPE_TEX_FIT_COVER;
  settings.fill_texture.anchor = PAINT_SHAPE_TEX_ANCHOR_CANVAS;
  settings.fill_texture.flag = PAINT_SHAPE_TEX_TILE_X | PAINT_SHAPE_TEX_MIRROR |
                               PAINT_SHAPE_TEX_NORMAL_FLIP_Y;
  settings.fill_texture.image_channel = 3;
  settings.fill_texture.scale[0] = 2.5f;
  settings.fill_texture.scale[1] = 0.5f;
  settings.fill_texture.offset[0] = 0.25f;
  settings.fill_texture.offset[1] = -0.125f;
  settings.fill_texture.repeat[0] = 3.0f;
  settings.fill_texture.repeat[1] = 4.0f;
  settings.fill_texture.angle = 0.125f;
  settings.fill_texture.tile_length = 64.0f;
  settings.fill_texture.height_mid = 0.5f;
  settings.fill_texture.opacity = 0.75f;
  settings.stroke_texture.source = PAINT_SHAPE_TEX_SRC_CURVE_PATTERN;
  settings.stroke_texture.mapping = PAINT_SHAPE_TEX_MAP_ALONG;
  settings.stroke_texture.flag = PAINT_SHAPE_TEX_WHOLE_REPEATS;
  settings.curve_pattern.crop_min[0] = -0.75f;
  settings.curve_pattern.crop_max[0] = 0.75f;
  settings.curve_pattern.resolution = 256;
  settings.curve_pattern.line_width = 0.02f;
  settings.curve_pattern.mode = PAINT_SHAPE_CURVE_PATTERN_BOTH;
  settings.curve_pattern.flag = PAINT_SHAPE_CURVE_PATTERN_WRAP_CROSSING |
                                PAINT_SHAPE_CURVE_PATTERN_USE_SDF_RELIEF;

  VectorDocument doc;
  doc.items.append(make_spline_item());
  PaintVector pv = {};
  paint_vector_from_document(pv, doc, settings);
  ASSERT_EQ(pv.items_num, 1);

  const PaintShapeSettings &style = pv.items[0].style;
  EXPECT_EQ(style.fill_type, PAINT_SHAPE_FILL_TEXTURE);
  EXPECT_EQ(style.stroke_type, PAINT_SHAPE_STROKE_TEXTURE);
  EXPECT_EQ(style.fill_texture.mapping, PAINT_SHAPE_TEX_MAP_MASK);
  EXPECT_EQ(style.fill_texture.fit_mode, PAINT_SHAPE_TEX_FIT_COVER);
  EXPECT_EQ(style.fill_texture.anchor, PAINT_SHAPE_TEX_ANCHOR_CANVAS);
  EXPECT_EQ(style.fill_texture.flag,
            PAINT_SHAPE_TEX_TILE_X | PAINT_SHAPE_TEX_MIRROR | PAINT_SHAPE_TEX_NORMAL_FLIP_Y);
  EXPECT_EQ(style.fill_texture.image_channel, 3);
  EXPECT_FLOAT_EQ(style.fill_texture.scale[0], 2.5f);
  EXPECT_FLOAT_EQ(style.fill_texture.scale[1], 0.5f);
  EXPECT_FLOAT_EQ(style.fill_texture.offset[0], 0.25f);
  EXPECT_FLOAT_EQ(style.fill_texture.offset[1], -0.125f);
  EXPECT_FLOAT_EQ(style.fill_texture.repeat[0], 3.0f);
  EXPECT_FLOAT_EQ(style.fill_texture.repeat[1], 4.0f);
  EXPECT_FLOAT_EQ(style.fill_texture.angle, 0.125f);
  EXPECT_FLOAT_EQ(style.fill_texture.tile_length, 64.0f);
  EXPECT_FLOAT_EQ(style.fill_texture.height_mid, 0.5f);
  EXPECT_FLOAT_EQ(style.fill_texture.opacity, 0.75f);
  EXPECT_EQ(style.stroke_texture.source, PAINT_SHAPE_TEX_SRC_CURVE_PATTERN);
  EXPECT_EQ(style.stroke_texture.mapping, PAINT_SHAPE_TEX_MAP_ALONG);
  EXPECT_EQ(int(style.stroke_texture.flag), int(PAINT_SHAPE_TEX_WHOLE_REPEATS));
  EXPECT_FLOAT_EQ(style.curve_pattern.crop_min[0], -0.75f);
  EXPECT_FLOAT_EQ(style.curve_pattern.crop_max[0], 0.75f);
  EXPECT_EQ(style.curve_pattern.resolution, 256);
  EXPECT_FLOAT_EQ(style.curve_pattern.line_width, 0.02f);
  EXPECT_EQ(style.curve_pattern.mode, PAINT_SHAPE_CURVE_PATTERN_BOTH);
  EXPECT_EQ(int(style.curve_pattern.flag),
            int(PAINT_SHAPE_CURVE_PATTERN_WRAP_CROSSING |
                PAINT_SHAPE_CURVE_PATTERN_USE_SDF_RELIEF));

  BKE_paint_vector_items_clear(pv);
}

static void expect_space_equal(const ShapeSpaceDesc &a, const ShapeSpaceDesc &b)
{
  EXPECT_EQ(int(a.type), int(b.type));
  EXPECT_EQ(a.ref_tile, b.ref_tile);
  EXPECT_EQ(a.ref_tile_size, b.ref_tile_size);
  EXPECT_EQ(a.win_size, b.win_size);
  EXPECT_FLOAT_EQ(a.clip_start, b.clip_start);
  EXPECT_FLOAT_EQ(a.clip_end, b.clip_end);
  EXPECT_EQ(a.is_persp, b.is_persp);
  for (int i = 0; i < 4; i++) {
    for (int j = 0; j < 4; j++) {
      EXPECT_FLOAT_EQ(a.persmat[i][j], b.persmat[i][j]);
      EXPECT_FLOAT_EQ(a.viewinv[i][j], b.viewinv[i][j]);
    }
  }
}

static void expect_item_equal(const VectorItem &a, const VectorItem &b)
{
  EXPECT_EQ(int(a.type), int(b.type));
  expect_space_equal(a.space, b.space);
  EXPECT_EQ(int(a.shape.type), int(b.shape.type));
  EXPECT_FLOAT_EQ(a.shape.origin.x, b.shape.origin.x);
  EXPECT_FLOAT_EQ(a.shape.origin.y, b.shape.origin.y);
  EXPECT_EQ(a.shape.origin_is_custom, b.shape.origin_is_custom);
  EXPECT_FLOAT_EQ(a.shape.center.x, b.shape.center.x);
  EXPECT_FLOAT_EQ(a.shape.center.y, b.shape.center.y);
  EXPECT_FLOAT_EQ(a.shape.half_size.x, b.shape.half_size.x);
  EXPECT_FLOAT_EQ(a.shape.half_size.y, b.shape.half_size.y);
  EXPECT_FLOAT_EQ(a.shape.rotation, b.shape.rotation);
  EXPECT_EQ(a.shape.polygon_sides, b.shape.polygon_sides);
  for (int i = 0; i < 4; i++) {
    EXPECT_FLOAT_EQ(a.shape.corner_radius[i], b.shape.corner_radius[i]);
  }
  ASSERT_EQ(a.shape.splines.size(), b.shape.splines.size());
  for (const int si : a.shape.splines.index_range()) {
    const ShapeSpline &sa = a.shape.splines[si];
    const ShapeSpline &sb = b.shape.splines[si];
    EXPECT_EQ(sa.cyclic, sb.cyclic);
    EXPECT_EQ(sa.is_bezier, sb.is_bezier);
    ASSERT_EQ(sa.points.size(), sb.points.size());
    for (const int pi : sa.points.index_range()) {
      EXPECT_EQ(sa.points[pi].co, sb.points[pi].co);
      EXPECT_EQ(sa.points[pi].handle_left, sb.points[pi].handle_left);
      EXPECT_EQ(sa.points[pi].handle_right, sb.points[pi].handle_right);
      EXPECT_FLOAT_EQ(sa.points[pi].width_factor, sb.points[pi].width_factor);
      EXPECT_EQ(sa.points[pi].corner, sb.points[pi].corner);
      EXPECT_EQ(sa.points[pi].auto_handles, sb.points[pi].auto_handles);
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Tests
 * \{ */

TEST(PaintVectorSerialize, RoundTripPreservesItems)
{
  VectorDocument doc;
  doc.items.append(make_spline_item());
  doc.items.append(make_parametric_item());
  doc.active = 1;
  for (VectorItem &item : doc.items) {
    item.style = vector_item_style_make(test_settings());
  }

  PaintVector pv = {};
  paint_vector_from_document(pv, doc);

  /* Each item's own style is written with it. */
  ASSERT_EQ(pv.items_num, 2);
  EXPECT_EQ(pv.active_item, 1);
  EXPECT_FLOAT_EQ(pv.items[0].style.stroke_width, 42.0f);
  EXPECT_EQ(pv.items[1].style.fill_type, PAINT_SHAPE_FILL_TEXTURE);
  EXPECT_EQ(pv.items[0].shape_type, PAINT_SHAPE_LINE);
  EXPECT_EQ(pv.items[1].shape_type, PAINT_SHAPE_ELLIPSE);

  VectorDocument out;
  paint_vector_to_document(pv, out);

  ASSERT_EQ(out.items.size(), 2);
  EXPECT_EQ(out.active, 1);
  expect_item_equal(doc.items[0], out.items[0]);
  expect_item_equal(doc.items[1], out.items[1]);

  BKE_paint_vector_items_clear(pv);
}

TEST(PaintVectorSerialize, SurfaceAnchoredProjectionAndBackface)
{
  ShapeSpaceDesc desc;
  desc.type = ShapeSpaceType::SurfaceAnchored;
  desc.anchor.co = float3(1.0f, 2.0f, 3.0f);
  desc.anchor.normal = float3(0.0f, 0.0f, 1.0f);
  desc.anchor.tangent = float3(1.0f, 0.0f, 0.0f);
  const std::unique_ptr<ShapeSpace> space = shape_space_create(desc);

  float2 p;
  /* A point on the plane one unit along the tangent is (1, 0) in plane coordinates. */
  ASSERT_TRUE(space->project_point(float3(2.0f, 2.0f, 3.0f), float3(0.0f, 0.0f, 1.0f), p));
  EXPECT_FLOAT_EQ(p.x, 1.0f);
  EXPECT_FLOAT_EQ(p.y, 0.0f);
  /* The anchor itself projects to the plane origin. */
  ASSERT_TRUE(space->project_point(float3(1.0f, 2.0f, 3.0f), float3(0.0f, 0.0f, 1.0f), p));
  EXPECT_FLOAT_EQ(p.x, 0.0f);
  EXPECT_FLOAT_EQ(p.y, 0.0f);
  /* A vertex whose normal faces away from the anchor normal is rejected. */
  EXPECT_FALSE(space->project_point(float3(1.0f, 2.0f, 3.0f), float3(0.0f, 0.0f, -1.0f), p));
}

TEST(PaintVectorSerialize, SurfaceAnchoredPixelScaleDepthAndRegion)
{
  ShapeSpaceDesc desc;
  desc.type = ShapeSpaceType::SurfaceAnchored;
  desc.anchor.co = float3(0.0f, 0.0f, 0.0f);
  desc.anchor.normal = float3(0.0f, 0.0f, 1.0f);
  desc.anchor.tangent = float3(1.0f, 0.0f, 0.0f);
  desc.anchor.px_per_unit = 10.0f;
  desc.anchor.max_depth = 1.0f;
  desc.persmat = float4x4::identity();
  desc.win_size = int2(200, 100);
  const std::unique_ptr<ShapeSpace> space = shape_space_create(desc);

  float2 p;
  /* One world unit along the tangent is 10 plane pixels; the stroke scale matches. */
  ASSERT_TRUE(space->project_point(float3(1.0f, 0.0f, 0.0f), float3(0.0f, 0.0f, 1.0f), p));
  EXPECT_FLOAT_EQ(p.x, 10.0f);
  EXPECT_FLOAT_EQ(p.y, 0.0f);
  EXPECT_FLOAT_EQ(space->units_per_world(float3(0.0f)), 10.0f);

  /* Depth: 2 units behind the plane is rejected, 0.5 is accepted. */
  EXPECT_FALSE(space->project_point(float3(0.0f, 0.0f, -2.0f), float3(0.0f, 0.0f, 1.0f), p));
  ASSERT_TRUE(space->project_point(float3(0.0f, 0.0f, -0.5f), float3(0.0f, 0.0f, 1.0f), p));

  /* Plane pixels -> object -> world -> region through the *current* view. */
  const float4x4 identity = float4x4::identity();
  ARegion region{};
  RegionView3D rv3d{};
  region.regiondata = &rv3d;
  region.winx = 200;
  region.winy = 100;
  for (int i = 0; i < 4; i++) {
    for (int j = 0; j < 4; j++) {
      rv3d.persmat[i][j] = identity[i][j];
    }
  }

  float2 region_px;
  ASSERT_TRUE(space->to_region(float2(10.0f, 0.0f), region, identity, region_px));
  EXPECT_FLOAT_EQ(region_px.x, 200.0f); /* object (1,0,0) -> ndc (1,0) */
  EXPECT_FLOAT_EQ(region_px.y, 50.0f);

  /* A different view moves the same surface point to different region pixels; recovering the
   * object point from the new view gives the same (1,0,0). */
  float4x4 shifted = identity;
  shifted[3][0] = 0.5f;
  for (int i = 0; i < 4; i++) {
    for (int j = 0; j < 4; j++) {
      rv3d.persmat[i][j] = shifted[i][j];
    }
  }
  float2 region_b;
  ASSERT_TRUE(space->to_region(float2(10.0f, 0.0f), region, identity, region_b));
  EXPECT_FLOAT_EQ(region_b.x, 250.0f);
  /* Undo the view offset: world_x = region/100 - 1 (identity object matrix) minus the shift. */
  EXPECT_FLOAT_EQ(region_b.x / 100.0f - 1.0f - shifted[3][0], 1.0f);
}

TEST(PaintVectorSerialize, SurfaceAnchorRoundTripsThroughId)
{
  VectorDocument doc;
  VectorItem item;
  item.shape.type = PAINT_SHAPE_LINE;
  item.space.type = ShapeSpaceType::SurfaceAnchored;
  item.space.anchor.co = float3(1.0f, 2.0f, 3.0f);
  item.space.anchor.normal = float3(0.0f, 1.0f, 0.0f);
  item.space.anchor.tangent = float3(0.0f, 0.0f, 1.0f);
  item.space.anchor.surface_uv = float2(0.25f, 0.75f);
  item.space.anchor.tri = 7;
  item.space.anchor.has_surface_uv = true;
  item.space.anchor.px_per_unit = 12.5f;
  item.space.anchor.max_depth = 3.0f;
  BLI_strncpy(item.space.anchor.surface_uv_map, "UVMap", sizeof(item.space.anchor.surface_uv_map));
  doc.items.append(std::move(item));

  PaintVector pv = {};
  paint_vector_from_document(pv, doc);
  VectorDocument out;
  paint_vector_to_document(pv, out);

  ASSERT_EQ(out.items.size(), 1);
  EXPECT_EQ(out.items[0].space.type, ShapeSpaceType::SurfaceAnchored);
  const SurfaceAnchor &a = out.items[0].space.anchor;
  EXPECT_EQ(a.co, float3(1.0f, 2.0f, 3.0f));
  EXPECT_EQ(a.normal, float3(0.0f, 1.0f, 0.0f));
  EXPECT_EQ(a.tangent, float3(0.0f, 0.0f, 1.0f));
  EXPECT_EQ(a.surface_uv, float2(0.25f, 0.75f));
  EXPECT_EQ(a.tri, 7);
  EXPECT_TRUE(a.has_surface_uv);
  EXPECT_FLOAT_EQ(a.px_per_unit, 12.5f);
  EXPECT_FLOAT_EQ(a.max_depth, 3.0f);
  EXPECT_STREQ(a.surface_uv_map, "UVMap");

  BKE_paint_vector_items_clear(pv);
}

TEST(PaintVectorSerialize, SurfaceAnchorRestoreCoreFollowsPositions)
{
  const Vector<float2> uv = {float2(0, 0), float2(1, 0), float2(0, 1),
                             float2(1, 0), float2(1, 1), float2(0, 1)};
  const Vector<int3> tris = {int3(0, 1, 2), int3(3, 4, 5)};
  const Vector<int> corner_verts = {0, 1, 2, 1, 3, 2};
  Vector<float3> positions = {float3(0, 0, 0), float3(1, 0, 0), float3(0, 1, 0), float3(1, 1, 0)};

  SurfaceAnchor anchor;
  anchor.surface_uv = float2(0.25f, 0.25f);
  ASSERT_TRUE(surface_anchor_restore_from_mesh(uv, tris, corner_verts, positions, anchor));
  EXPECT_NEAR(anchor.co.x, 0.25f, 1e-4f);
  EXPECT_NEAR(anchor.co.y, 0.25f, 1e-4f);
  EXPECT_EQ(anchor.tri, 0);

  /* Moving a vertex moves the restored anchor with the surface. */
  positions[1] = float3(2.0f, 0.0f, 0.0f);
  SurfaceAnchor moved;
  moved.surface_uv = anchor.surface_uv;
  ASSERT_TRUE(surface_anchor_restore_from_mesh(uv, tris, corner_verts, positions, moved));
  EXPECT_NEAR(moved.co.x, 0.5f, 1e-4f);
  EXPECT_NEAR(moved.co.y, 0.25f, 1e-4f);

  /* Degenerate hit triangle -> fallback. */
  Vector<float3> degenerate = {
      float3(0, 0, 0), float3(0, 0, 0), float3(0, 0, 0), float3(1, 1, 0)};
  SurfaceAnchor deg;
  deg.surface_uv = float2(0.25f, 0.25f);
  EXPECT_FALSE(surface_anchor_restore_from_mesh(uv, tris, corner_verts, degenerate, deg));

  /* Ambiguous UV (a second triangle covers the same UV point) -> fallback. */
  Vector<float2> dup_uv = uv;
  dup_uv.append(float2(0, 0));
  dup_uv.append(float2(1, 0));
  dup_uv.append(float2(0, 1));
  Vector<int3> dup_tris = tris;
  dup_tris.append(int3(6, 7, 8));
  Vector<int> dup_cv = corner_verts;
  dup_cv.append(0);
  dup_cv.append(1);
  dup_cv.append(2);
  SurfaceAnchor amb;
  amb.surface_uv = float2(0.25f, 0.25f);
  EXPECT_FALSE(surface_anchor_restore_from_mesh(dup_uv, dup_tris, dup_cv, positions, amb));
}

TEST(PaintVectorSerialize, SurfaceAnchorBarycentricRoundTripsFromPosition)
{
  /* Non-degenerate triangle with distinct vertices: a swapped barycentric order moves the restored
   * point, so this pins the convention that `bary[i]` weighs `tri[i]` (what the creation code in
   * `sculpt_paint_shape_ops.cc` must produce and #surface_anchor_restore consumes). */
  const Vector<float2> uv = {float2(0, 0), float2(1, 0), float2(0, 1)};
  const Vector<int3> tris = {int3(0, 1, 2)};
  const Vector<int> corner_verts = {0, 1, 2};
  const Vector<float3> positions = {float3(0, 0, 0), float3(1, 0, 0), float3(0, 2, 0)};
  const float3 point(0.25f, 0.5f, 0.0f);

  SurfaceAnchor anchor;
  anchor.tri = 0;
  anchor.tri_corners = tris[0];
  anchor.bary = bke::mesh_surface_sample::compute_bary_coord_in_triangle(
      positions, corner_verts, tris[0], point);
  /* v0 = (0, 0), v1 = (1, 0), v2 = (0, 2): the point is `0.5*v0 + 0.25*v1 + 0.25*v2`. */
  EXPECT_NEAR(anchor.bary.x, 0.5f, 1e-5f);
  EXPECT_NEAR(anchor.bary.y, 0.25f, 1e-5f);
  EXPECT_NEAR(anchor.bary.z, 0.25f, 1e-5f);

  SurfaceAnchor restored = anchor;
  ASSERT_TRUE(surface_anchor_restore_from_mesh(uv, tris, corner_verts, positions, restored));
  EXPECT_NEAR(restored.co.x, point.x, 1e-5f);
  EXPECT_NEAR(restored.co.y, point.y, 1e-5f);
  EXPECT_NEAR(restored.co.z, point.z, 1e-5f);
}

TEST(PaintVectorSerialize, SurfaceAnchorFrameMatchesSpaceProjection)
{
  /* The unified-basis contract (B6c): on an inclined surface whose stored tangent keeps a normal
   * component (the view-horizontal tangent of a tilted face), the gesture mapping through
   * #surface_anchor_frame must produce exactly the plane pixels
   * #SurfaceAnchoredSpace::project_point assigns -- the drawn contour, the cage and the bake
   * cannot disagree. */
  const float3 normal = math::normalize(float3(0.2f, -0.3f, 0.9f));
  const float3 tangent = math::normalize(float3(1.0f, 0.0f, 1.0f));
  EXPECT_GT(math::abs(math::dot(tangent, normal)), 1e-4f)
      << "The test tangent must actually keep a normal component";

  const SurfaceAnchorFrame frame = surface_anchor_frame(normal, tangent);
  EXPECT_NEAR(math::length(frame.tangent), 1.0f, 1e-6f);
  EXPECT_NEAR(math::length(frame.bitangent), 1.0f, 1e-6f);
  EXPECT_NEAR(math::dot(frame.normal, frame.tangent), 0.0f, 1e-6f);
  EXPECT_NEAR(math::dot(frame.normal, frame.bitangent), 0.0f, 1e-6f);
  EXPECT_NEAR(math::dot(frame.tangent, frame.bitangent), 0.0f, 1e-6f);

  ShapeSpaceDesc desc;
  desc.type = ShapeSpaceType::SurfaceAnchored;
  desc.anchor.co = float3(1.0f, 2.0f, 3.0f);
  desc.anchor.normal = normal;
  desc.anchor.tangent = tangent;
  desc.anchor.px_per_unit = 42.0f;
  const std::unique_ptr<ShapeSpace> space = shape_space_create(desc);

  const Vector<float3> samples = {desc.anchor.co + frame.tangent * 0.5f + frame.bitangent * 0.25f,
                                  desc.anchor.co - frame.tangent * 2.0f,
                                  desc.anchor.co + frame.bitangent * 1.5f,
                                  desc.anchor.co + frame.tangent * 0.75f - frame.bitangent * 0.5f};
  for (const float3 &co : samples) {
    float2 projected;
    ASSERT_TRUE(space->project_point(co, frame.normal, projected));
    /* The gesture mapping: dot with the frame axes, scaled by px_per_unit (what
     * #paint_shape_session_event_to_shape_px and the creation mapping compute). */
    const float3 d = co - desc.anchor.co;
    const float2 gesture = float2(math::dot(d, frame.tangent), math::dot(d, frame.bitangent)) *
                           desc.anchor.px_per_unit;
    EXPECT_NEAR(projected.x, gesture.x, 1e-4f);
    EXPECT_NEAR(projected.y, gesture.y, 1e-4f);
  }
}

TEST(PaintVectorSerialize, SurfaceAnchorRestoreTriPathFollowsDeformation)
{
  const Vector<float2> uv = {float2(0, 0), float2(1, 0), float2(0, 1)};
  const Vector<int3> tris = {int3(0, 1, 2)};
  const Vector<int> corner_verts = {0, 1, 2};
  Vector<float3> positions = {float3(0, 0, 0), float3(1, 0, 0), float3(0, 2, 0)};
  const float3 point(0.25f, 0.5f, 0.0f);

  SurfaceAnchor anchor;
  anchor.tri = 0;
  anchor.tri_corners = tris[0];
  anchor.bary = bke::mesh_surface_sample::compute_bary_coord_in_triangle(
      positions, corner_verts, tris[0], point);
  anchor.surface_uv = float2(0.25f, 0.25f);
  anchor.has_surface_uv = true;

  /* The preferred path needs no UV data at all: a mesh without a UV map still restores. */
  SurfaceAnchor no_uv = anchor;
  no_uv.has_surface_uv = false;
  ASSERT_TRUE(surface_anchor_restore_from_mesh(
      Span<float2>(), tris, corner_verts, positions, no_uv));
  EXPECT_NEAR(no_uv.co.x, point.x, 1e-5f);
  EXPECT_NEAR(no_uv.co.y, point.y, 1e-5f);

  /* A deformation (a moved vertex) moves the restored anchor with the surface. */
  positions[1] = float3(2.0f, 0.0f, 0.0f);
  SurfaceAnchor moved = anchor;
  ASSERT_TRUE(surface_anchor_restore_from_mesh(uv, tris, corner_verts, positions, moved));
  EXPECT_NEAR(moved.co.x, 0.5f, 1e-5f);
  EXPECT_NEAR(moved.co.y, 0.5f, 1e-5f);

  /* Topology change (the stored corner indices no longer match): the saved UV takes over and
   * refreshes the triangle cache. */
  const Vector<int3> rebuilt_tris = {int3(3, 4, 5)};
  const Vector<int> rebuilt_corner_verts = {0, 1, 2, 0, 1, 2};
  const Vector<float2> rebuilt_uv = {float2(0, 0),
                                     float2(1, 0),
                                     float2(0, 1),
                                     float2(0, 0),
                                     float2(1, 0),
                                     float2(0, 1)};
  SurfaceAnchor rebuilt = anchor;
  ASSERT_TRUE(surface_anchor_restore_from_mesh(
      rebuilt_uv, rebuilt_tris, rebuilt_corner_verts, positions, rebuilt));
  EXPECT_EQ(rebuilt.tri, 0);
  EXPECT_EQ(rebuilt.tri_corners, int3(3, 4, 5));
  EXPECT_NEAR(rebuilt.co.x, 0.5f, 1e-5f);
  EXPECT_NEAR(rebuilt.co.y, 0.5f, 1e-5f);

  /* Topology changed and no UV: unrestorable -- the anchor is untouched (the Apply refusal). */
  SurfaceAnchor stuck = anchor;
  stuck.has_surface_uv = false;
  const float3 stuck_co = stuck.co;
  EXPECT_FALSE(surface_anchor_restore_from_mesh(
      rebuilt_uv, rebuilt_tris, rebuilt_corner_verts, positions, stuck));
  EXPECT_EQ(stuck.co, stuck_co);
}

TEST(PaintVectorSerialize, SurfaceAnchorPxPerUnitScalesWithObject)
{
  const float3 tangent(1.0f, 0.0f, 0.0f);
  const float3 bitangent(0.0f, 1.0f, 0.0f);
  const float base = 10.0f;

  /* Unit object: the object-space scale equals the world scale. */
  EXPECT_FLOAT_EQ(
      surface_anchor_px_per_unit(base, float4x4::identity(), tangent, bitangent), base);

  /* Object scale 2: an 8 px stroke stays 8 object units, so the object-space px_per_unit doubles
   * and the shape keeps its on-screen size. */
  float4x4 scale2 = float4x4::identity();
  scale2[0][0] = 2.0f;
  scale2[1][1] = 2.0f;
  scale2[2][2] = 2.0f;
  EXPECT_FLOAT_EQ(surface_anchor_px_per_unit(base, scale2, tangent, bitangent), base * 2.0f);
}

TEST(PaintVectorSerialize, RepeatedWriteReplacesWithoutDuplicating)
{
  VectorDocument doc;
  doc.items.append(make_parametric_item());
  doc.active = 0;

  PaintVector pv = {};
  paint_vector_from_document(pv, doc);
  /* Writing the same document again must replace the items, not append a second copy. */
  paint_vector_from_document(pv, doc);
  EXPECT_EQ(pv.items_num, 1);
  EXPECT_EQ(pv.items[0].points_num, 0);
  EXPECT_EQ(pv.items[0].splines_num, 0);

  /* A document with fewer items shrinks the ID and clamps the active index. */
  VectorDocument empty;
  paint_vector_from_document(pv, empty);
  EXPECT_EQ(pv.items_num, 0);
  EXPECT_EQ(pv.active_item, 0);

  BKE_paint_vector_items_clear(pv);
}

/** \} */

}  // namespace blender::ed::sculpt_paint::shape
