/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "BLI_index_range.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector_types.hh"

#include "BKE_attribute.hh"
#include "BKE_customdata.hh"
#include "BKE_lib_id.hh"
#include "BKE_mesh.hh"
#include "BKE_object.hh"
#include "BKE_paint_layers_composite.hh"
#include "BKE_paint_layers_gradient.hh"
#include "BKE_paint_layers_gradient_surface.hh"

#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"

#include "paint_layers_test_util.hh"

namespace blender::bke::tests {

class PaintLayersGradientSurface : public PaintLayersTestBase {
 public:
  Object *object = nullptr;
  Mesh *mesh = nullptr;

  void SetUp() override
  {
    PaintLayersTestBase::SetUp();
    create_quad();
  }

  void TearDown() override
  {
    BKE_paint_layers_gradient_surface_cache_clear();
    PaintLayersTestBase::TearDown();
  }

  /**
   * A 2x2 quad in the X/Y plane with UVs covering the lower-left quarter of the unit square.
   * The gap makes "outside a UV island" observable, and the size stays well inside the square so
   * the boundary texels are not confused with the square's edge.
   */
  void create_quad()
  {
    object = BKE_object_add_only_object(bmain, OB_MESH, "SurfaceObject");
    mesh = BKE_mesh_add(bmain, "SurfaceMesh");
    object->data = id_cast<ID *>(mesh);

    Mesh *source = BKE_mesh_new_nomain(4, 0, 1, 4);
    MutableSpan<float3> positions = source->vert_positions_for_write();
    positions[0] = float3(0.0f, 0.0f, 0.0f);
    positions[1] = float3(2.0f, 0.0f, 0.0f);
    positions[2] = float3(2.0f, 2.0f, 0.0f);
    positions[3] = float3(0.0f, 2.0f, 0.0f);

    MutableSpan<int> face_offsets = source->face_offsets_for_write();
    face_offsets[0] = 0;
    face_offsets[1] = 4;
    MutableSpan<int> corner_verts = source->corner_verts_for_write();
    corner_verts[0] = 0;
    corner_verts[1] = 1;
    corner_verts[2] = 2;
    corner_verts[3] = 3;

    bke::SpanAttributeWriter<float2> uvs =
        source->attributes_for_write().lookup_or_add_for_write_span<float2>(
            "UVMap", bke::AttrDomain::Corner);
    uvs.span[0] = float2(0.0f, 0.0f);
    uvs.span[1] = float2(0.5f, 0.0f);
    uvs.span[2] = float2(0.5f, 0.5f);
    uvs.span[3] = float2(0.0f, 0.5f);
    uvs.finish();

    source->tag_topology_changed();
    source->tag_positions_changed();
    BKE_mesh_nomain_to_mesh(source, mesh, object);

    /* Custom texture space, so the expected generated box does not depend on the auto calculation
     * and matches what the shader's Generated output reads. A flat Z extent exercises the
     * one-unit fallback. */
    mesh->texspace_flag = 0;
    mesh->texspace_location[0] = 1.0f;
    mesh->texspace_location[1] = 1.0f;
    mesh->texspace_location[2] = 0.0f;
    mesh->texspace_size[0] = 1.0f;
    mesh->texspace_size[1] = 1.0f;
    mesh->texspace_size[2] = 0.0f;
  }

  /** A standalone triangle with authored UVs, for shapes and degeneracy the quad cannot express. */
  Object *create_triangle(const char *name,
                          const char *mesh_name,
                          const float2 uvs[3],
                          Mesh **r_mesh)
  {
    Object *triangle_object = BKE_object_add_only_object(bmain, OB_MESH, name);
    Mesh *triangle_mesh = BKE_mesh_add(bmain, mesh_name);
    triangle_object->data = id_cast<ID *>(triangle_mesh);

    Mesh *source = BKE_mesh_new_nomain(3, 0, 1, 3);
    MutableSpan<float3> positions = source->vert_positions_for_write();
    positions[0] = float3(0.0f, 0.0f, 0.0f);
    positions[1] = float3(2.0f, 0.0f, 0.0f);
    positions[2] = float3(2.0f, 2.0f, 0.0f);

    MutableSpan<int> face_offsets = source->face_offsets_for_write();
    face_offsets[0] = 0;
    face_offsets[1] = 3;
    MutableSpan<int> corner_verts = source->corner_verts_for_write();
    corner_verts[0] = 0;
    corner_verts[1] = 1;
    corner_verts[2] = 2;

    bke::SpanAttributeWriter<float2> uv_layer =
        source->attributes_for_write().lookup_or_add_for_write_span<float2>(
            "UVMap", bke::AttrDomain::Corner);
    uv_layer.span[0] = uvs[0];
    uv_layer.span[1] = uvs[1];
    uv_layer.span[2] = uvs[2];
    uv_layer.finish();

    source->tag_topology_changed();
    source->tag_positions_changed();
    BKE_mesh_nomain_to_mesh(source, triangle_mesh, triangle_object);

    triangle_mesh->texspace_flag = 0;
    triangle_mesh->texspace_location[0] = 1.0f;
    triangle_mesh->texspace_location[1] = 1.0f;
    triangle_mesh->texspace_location[2] = 0.0f;
    triangle_mesh->texspace_size[0] = 1.0f;
    triangle_mesh->texspace_size[1] = 1.0f;
    triangle_mesh->texspace_size[2] = 0.0f;

    *r_mesh = triangle_mesh;
    return triangle_object;
  }
};

TEST_F(PaintLayersGradientSurface, rasterizes_positions_and_marks_island_texels)
{
  auto surface = BKE_paint_layers_gradient_surface_get(*object, *mesh, "UVMap", 256);
  ASSERT_NE(surface, nullptr);
  EXPECT_EQ(surface->width, 256);
  EXPECT_EQ(surface->height, 256);
  EXPECT_EQ(surface->positions.size(), 256 * 256);
  EXPECT_EQ(surface->valid.size(), 256 * 256);

  float3 position;
  ASSERT_TRUE(surface->sample(float2(0.25f, 0.25f), position));
  EXPECT_NEAR(position.x, 1.0f, 0.03f);
  EXPECT_NEAR(position.y, 1.0f, 0.03f);
  EXPECT_NEAR(position.z, 0.0f, 1e-5f);

  ASSERT_TRUE(surface->sample(float2(0.1f, 0.1f), position));
  EXPECT_NEAR(position.x, 0.4f, 0.03f);
  EXPECT_NEAR(position.y, 0.4f, 0.03f);

  /* The UV island stops at 0.5 on both axes; the rest of the square is invalid. */
  EXPECT_FALSE(surface->sample(float2(0.75f, 0.75f), position));
  EXPECT_FALSE(surface->sample(float2(-0.1f, 0.25f), position));
}

TEST_F(PaintLayersGradientSurface, generated_bbox_comes_from_texture_space)
{
  auto surface = BKE_paint_layers_gradient_surface_get(*object, *mesh, "UVMap", 256);
  ASSERT_NE(surface, nullptr);
  ASSERT_TRUE(surface->generated_bbox_valid);

  EXPECT_NEAR(surface->generated_bbox_min.x, 0.0f, 1e-6f);
  EXPECT_NEAR(surface->generated_bbox_min.y, 0.0f, 1e-6f);
  /* A zero texspace extent is widened by one unit around the centre. */
  EXPECT_NEAR(surface->generated_bbox_min.z, -1.0f, 1e-6f);
  EXPECT_NEAR(surface->generated_bbox_max.x, 2.0f, 1e-6f);
  EXPECT_NEAR(surface->generated_bbox_max.y, 2.0f, 1e-6f);
  EXPECT_NEAR(surface->generated_bbox_max.z, 1.0f, 1e-6f);

  float3 generated;
  ASSERT_TRUE(surface->sample_generated(float2(0.25f, 0.25f), generated));
  EXPECT_NEAR(generated.x, 0.5f, 0.02f);
  EXPECT_NEAR(generated.y, 0.5f, 0.02f);
  EXPECT_NEAR(generated.z, 0.5f, 1e-5f);
}

TEST_F(PaintLayersGradientSurface, generated_positions_come_from_the_orco_layer)
{
  auto *orco = static_cast<float3 *>(
      CustomData_add_layer(&mesh->vert_data, CD_ORCO, CD_CONSTRUCT, mesh->verts_num));
  ASSERT_NE(orco, nullptr);
  for (int i = 0; i < mesh->verts_num; i++) {
    orco[i] = float3(-1.0f, -1.0f, 0.0f);
  }

  auto surface = BKE_paint_layers_gradient_surface_get(*object, *mesh, "UVMap", 256);
  ASSERT_NE(surface, nullptr);
  ASSERT_EQ(surface->generated_positions.size(), 256 * 256);

  /* The shader's Generated is `orco * 0.5 + 0.5`; the texspace bbox would have said 0.5 here. */
  float3 generated;
  ASSERT_TRUE(surface->sample_generated(float2(0.25f, 0.25f), generated));
  EXPECT_NEAR(generated.x, 0.0f, 1e-6f);
  EXPECT_NEAR(generated.y, 0.0f, 1e-6f);
  EXPECT_NEAR(generated.z, 0.5f, 1e-6f);
}

TEST_F(PaintLayersGradientSurface, clamps_the_requested_size)
{
  auto small = BKE_paint_layers_gradient_surface_get(*object, *mesh, "UVMap", 1);
  ASSERT_NE(small, nullptr);
  EXPECT_EQ(small->width, 256);
  EXPECT_EQ(small->height, 256);

  auto large = BKE_paint_layers_gradient_surface_get(*object, *mesh, "UVMap", 10000);
  ASSERT_NE(large, nullptr);
  EXPECT_EQ(large->width, 1024);
  EXPECT_EQ(large->height, 1024);
  EXPECT_NE(small.get(), large.get());
}

TEST_F(PaintLayersGradientSurface, repeated_get_returns_the_cached_surface)
{
  auto first = BKE_paint_layers_gradient_surface_get(*object, *mesh, "UVMap", 256);
  auto second = BKE_paint_layers_gradient_surface_get(*object, *mesh, "UVMap", 256);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(first.get(), second.get());
  EXPECT_EQ(first->key, second->key);

  /* A missing UV map is not cached as an empty success. */
  EXPECT_FALSE(BKE_paint_layers_gradient_surface_get(*object, *mesh, "Missing", 256));
}

TEST_F(PaintLayersGradientSurface, clearing_for_an_object_forces_a_rebuild)
{
  auto first = BKE_paint_layers_gradient_surface_get(*object, *mesh, "UVMap", 256);
  ASSERT_NE(first, nullptr);

  BKE_paint_layers_gradient_surface_cache_clear_for_object(object->id.session_uid);
  auto second = BKE_paint_layers_gradient_surface_get(*object, *mesh, "UVMap", 256);
  ASSERT_NE(second, nullptr);

  /* The key is the same identity, but the cache entry is a new surface with new content. */
  EXPECT_NE(first.get(), second.get());
  EXPECT_NE(first->revision, second->revision);
}

TEST_F(PaintLayersGradientSurface, a_rebuilt_surface_changes_the_composite_stack_hash)
{
  MaterialPaintLayer *gradient = BKE_paint_layers_gradient_add(
      *ma, "Gradient", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(gradient, nullptr);
  ASSERT_TRUE(BKE_paint_layers_gradient_set_space(
      *ma, *gradient, MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT));
  const float start[3] = {0.0f, 0.0f, 0.0f};
  const float end[3] = {1.0f, 0.0f, 0.0f};
  ASSERT_TRUE(BKE_paint_layers_gradient_set_line(*ma, *gradient, start, end));
  ma->active_layer_marker = {};
  ma->paint_layers_image_size = 4;

  auto first = BKE_paint_layers_gradient_surface_get(*object, *mesh, "UVMap", 256);
  ASSERT_NE(first, nullptr);

  Vector<PaintMaterialCompositeImageLayer> first_layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, first_layers, false, first));
  const uint64_t first_hash = BKE_paint_material_composite_stack_hash(first_layers);

  /* A repeated get is a cache hit and must not move the stack hash. */
  auto same = BKE_paint_layers_gradient_surface_get(*object, *mesh, "UVMap", 256);
  ASSERT_NE(same, nullptr);
  EXPECT_EQ(same.get(), first.get());
  EXPECT_EQ(same->revision, first->revision);

  BKE_paint_layers_gradient_surface_cache_clear_for_object(object->id.session_uid);
  auto rebuilt = BKE_paint_layers_gradient_surface_get(*object, *mesh, "UVMap", 256);
  ASSERT_NE(rebuilt, nullptr);
  EXPECT_NE(rebuilt->revision, first->revision);

  Vector<PaintMaterialCompositeImageLayer> rebuilt_layers;
  ASSERT_TRUE(BKE_paint_layers_composite_image_layers(
      *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, rebuilt_layers, false, rebuilt));
  const uint64_t rebuilt_hash = BKE_paint_material_composite_stack_hash(rebuilt_layers);
  EXPECT_NE(first_hash, rebuilt_hash);
}

TEST_F(PaintLayersGradientSurface, a_triangle_partly_outside_uv_marks_only_inside_texels)
{
  const float2 uvs[3] = {
      float2(-0.5f, 0.0f), float2(0.5f, 0.0f), float2(0.5f, 0.5f)};
  Mesh *triangle_mesh = nullptr;
  Object *triangle = create_triangle("PartialTriangle", "PartialTriangleMesh", uvs, &triangle_mesh);
  ASSERT_NE(triangle, nullptr);

  auto surface = BKE_paint_layers_gradient_surface_get(*triangle, *triangle_mesh, "UVMap", 256);
  ASSERT_NE(surface, nullptr);

  float3 position;
  /* Lower-left of the triangle, inside the square and inside the island. */
  EXPECT_TRUE(surface->sample(float2(0.1f, 0.05f), position));
  /* Above the diagonal: inside the UV square but outside the triangle. */
  EXPECT_FALSE(surface->sample(float2(0.45f, 0.49f), position));

  int valid_count = 0;
  for (const uint8_t valid : surface->valid) {
    valid_count += valid != 0 ? 1 : 0;
  }
  EXPECT_GT(valid_count, 0);
  EXPECT_LT(valid_count, 256 * 256);
}

TEST_F(PaintLayersGradientSurface, a_degenerate_uv_triangle_marks_nothing)
{
  const float2 uvs[3] = {
      float2(0.25f, 0.25f), float2(0.25f, 0.25f), float2(0.25f, 0.25f)};
  Mesh *triangle_mesh = nullptr;
  Object *triangle = create_triangle("DegenerateTriangle", "DegenerateTriangleMesh", uvs, &triangle_mesh);
  ASSERT_NE(triangle, nullptr);

  auto surface = BKE_paint_layers_gradient_surface_get(*triangle, *triangle_mesh, "UVMap", 256);
  ASSERT_NE(surface, nullptr);

  float3 position;
  EXPECT_FALSE(surface->sample(float2(0.25f, 0.25f), position));
  for (const uint8_t valid : surface->valid) {
    EXPECT_EQ(valid, 0);
  }
}

TEST_F(PaintLayersGradientSurface, clearing_one_object_keeps_another_objects_surface)
{
  Object *other = BKE_object_add_only_object(bmain, OB_MESH, "OtherSurfaceObject");
  other->data = id_cast<ID *>(mesh);
  id_us_plus(&mesh->id);

  auto first = BKE_paint_layers_gradient_surface_get(*object, *mesh, "UVMap", 256);
  auto other_surface = BKE_paint_layers_gradient_surface_get(*other, *mesh, "UVMap", 256);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(other_surface, nullptr);
  EXPECT_NE(first.get(), other_surface.get());

  BKE_paint_layers_gradient_surface_cache_clear_for_object(object->id.session_uid);

  auto other_again = BKE_paint_layers_gradient_surface_get(*other, *mesh, "UVMap", 256);
  ASSERT_NE(other_again, nullptr);
  EXPECT_EQ(other_surface.get(), other_again.get());

  /* The cleared object rebuilds rather than returning the evicted pointer. */
  auto first_again = BKE_paint_layers_gradient_surface_get(*object, *mesh, "UVMap", 256);
  ASSERT_NE(first_again, nullptr);
  EXPECT_NE(first.get(), first_again.get());
}

TEST_F(PaintLayersGradientSurface, the_capacity_clears_the_cache)
{
  auto first = BKE_paint_layers_gradient_surface_get(*object, *mesh, "UVMap", 256);
  auto second = BKE_paint_layers_gradient_surface_get(*object, *mesh, "UVMap", 512);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);

  /* Not an LRU: the whole two-entry map is dropped before the third entry is inserted. */
  auto third = BKE_paint_layers_gradient_surface_get(*object, *mesh, "UVMap", 768);
  ASSERT_NE(third, nullptr);
  auto first_again = BKE_paint_layers_gradient_surface_get(*object, *mesh, "UVMap", 256);
  ASSERT_NE(first_again, nullptr);
  EXPECT_NE(first.get(), first_again.get());
}

TEST_F(PaintLayersGradientSurface, uv_and_object_points_round_trip_on_the_island)
{
  float3 object;
  ASSERT_TRUE(BKE_paint_layers_gradient_uv_to_object(*mesh, float2(0.25f, 0.25f), object));
  EXPECT_NEAR(object.x, 1.0f, 1e-5f);
  EXPECT_NEAR(object.y, 1.0f, 1e-5f);
  EXPECT_NEAR(object.z, 0.0f, 1e-5f);

  float2 uv;
  ASSERT_TRUE(BKE_paint_layers_gradient_object_to_uv(*mesh, object, uv));
  EXPECT_NEAR(uv.x, 0.25f, 1e-5f);
  EXPECT_NEAR(uv.y, 0.25f, 1e-5f);
}

TEST_F(PaintLayersGradientSurface, a_uv_point_outside_the_island_snaps_to_it)
{
  float3 object;
  ASSERT_TRUE(BKE_paint_layers_gradient_uv_to_object(*mesh, float2(0.9f, 0.25f), object));
  EXPECT_NEAR(object.x, 2.0f, 1e-5f);
  EXPECT_NEAR(object.y, 1.0f, 1e-5f);

  float2 uv;
  ASSERT_TRUE(BKE_paint_layers_gradient_object_to_uv(*mesh, object, uv));
  EXPECT_NEAR(uv.x, 0.5f, 1e-5f);
  EXPECT_NEAR(uv.y, 0.25f, 1e-5f);
}

TEST_F(PaintLayersGradientSurface, an_object_point_off_the_surface_loses_its_distance)
{
  float2 uv;
  ASSERT_TRUE(BKE_paint_layers_gradient_object_to_uv(*mesh, float3(1.0f, 1.0f, 5.0f), uv));
  EXPECT_NEAR(uv.x, 0.25f, 1e-5f);
  EXPECT_NEAR(uv.y, 0.25f, 1e-5f);
}

TEST_F(PaintLayersGradientSurface, switching_space_keeps_the_line_on_the_surface)
{
  MaterialPaintLayer *gradient = BKE_paint_layers_gradient_add(
      *ma, "Gradient", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(gradient, nullptr);
  ASSERT_TRUE(BKE_paint_layers_gradient_set_space(
      *ma, *gradient, MA_PAINT_LAYER_GRADIENT_SPACE_UV));
  const float start[3] = {0.0f, 0.25f, 0.0f};
  const float end[3] = {0.5f, 0.25f, 0.0f};
  ASSERT_TRUE(BKE_paint_layers_gradient_set_line(*ma, *gradient, start, end));

  bool converted = false;
  ASSERT_TRUE(BKE_paint_layers_gradient_convert_space(
      *ma, *gradient, object, mesh, MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT, converted));
  ASSERT_TRUE(converted);
  EXPECT_NEAR(gradient->gradient->start[0], 0.0f, 1e-5f);
  EXPECT_NEAR(gradient->gradient->start[1], 1.0f, 1e-5f);
  EXPECT_NEAR(gradient->gradient->end[0], 2.0f, 1e-5f);
  EXPECT_NEAR(gradient->gradient->end[1], 1.0f, 1e-5f);

  ASSERT_TRUE(BKE_paint_layers_gradient_convert_space(
      *ma, *gradient, object, mesh, MA_PAINT_LAYER_GRADIENT_SPACE_GENERATED, converted));
  ASSERT_TRUE(converted);
  EXPECT_NEAR(gradient->gradient->start[0], 0.0f, 1e-5f);
  EXPECT_NEAR(gradient->gradient->start[1], 0.5f, 1e-5f);
  EXPECT_NEAR(gradient->gradient->end[0], 1.0f, 1e-5f);
  EXPECT_NEAR(gradient->gradient->end[1], 0.5f, 1e-5f);

  ASSERT_TRUE(BKE_paint_layers_gradient_convert_space(
      *ma, *gradient, object, mesh, MA_PAINT_LAYER_GRADIENT_SPACE_UV, converted));
  ASSERT_TRUE(converted);
  EXPECT_NEAR(gradient->gradient->start[0], start[0], 1e-5f);
  EXPECT_NEAR(gradient->gradient->start[1], start[1], 1e-5f);
  EXPECT_NEAR(gradient->gradient->end[0], end[0], 1e-5f);
  EXPECT_NEAR(gradient->gradient->end[1], end[1], 1e-5f);
}

TEST_F(PaintLayersGradientSurface, switching_space_without_a_mesh_keeps_the_numbers)
{
  MaterialPaintLayer *gradient = BKE_paint_layers_gradient_add(
      *ma, "Gradient", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(gradient, nullptr);
  ASSERT_TRUE(BKE_paint_layers_gradient_set_space(
      *ma, *gradient, MA_PAINT_LAYER_GRADIENT_SPACE_UV));
  float start[3];
  float end[3];
  copy_v3_v3(start, gradient->gradient->start);
  copy_v3_v3(end, gradient->gradient->end);

  bool converted = false;
  ASSERT_TRUE(BKE_paint_layers_gradient_convert_space(
      *ma, *gradient, nullptr, nullptr, MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT, converted));
  EXPECT_FALSE(converted);
  EXPECT_EQ(gradient->gradient->space, MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT);
  for (const int i : IndexRange(3)) {
    EXPECT_FLOAT_EQ(gradient->gradient->start[i], start[i]);
    EXPECT_FLOAT_EQ(gradient->gradient->end[i], end[i]);
  }
}

TEST_F(PaintLayersGradientSurface, the_builder_counts_the_island_texels_once)
{
  auto surface = BKE_paint_layers_gradient_surface_get(*object, *mesh, "UVMap", 256);
  ASSERT_NE(surface, nullptr);
  ASSERT_GE(surface->valid_texel_count, 0) << "the rasterizer fills the count";
  EXPECT_TRUE(surface->has_valid_texels());

  /* The count is what the mask says, so a reader of either agrees. */
  int64_t marked = 0;
  for (const uint8_t valid : surface->valid) {
    marked += valid != 0 ? 1 : 0;
  }
  EXPECT_EQ(surface->valid_texel_count, marked);
}

/**
 * The padding of a bake surface marks texels valid, and the count has to include them: the preview
 * asks whether the surface covers anything, and a padded surface carries a position outside the
 * islands on purpose.
 */
TEST_F(PaintLayersGradientSurface, the_padding_of_a_bake_surface_is_part_of_the_count)
{
  auto plain = BKE_paint_layers_gradient_surface_build(*object, *mesh, "UVMap", 256, 0, false);
  auto padded = BKE_paint_layers_gradient_surface_build(*object, *mesh, "UVMap", 256, 4, false);
  ASSERT_NE(plain, nullptr);
  ASSERT_NE(padded, nullptr);
  EXPECT_GT(padded->valid_texel_count, plain->valid_texel_count);

  int64_t marked = 0;
  for (const uint8_t valid : padded->valid) {
    marked += valid != 0 ? 1 : 0;
  }
  EXPECT_EQ(padded->valid_texel_count, marked);
}

/** A surface with no UV island has a counted zero: the preview must not rescan it. */
TEST_F(PaintLayersGradientSurface, a_surface_covering_no_texel_counts_zero)
{
  const float2 uvs[3] = {
      float2(0.25f, 0.25f), float2(0.25f, 0.25f), float2(0.25f, 0.25f)};
  Mesh *triangle_mesh = nullptr;
  Object *triangle = create_triangle("EmptyTriangle", "EmptyTriangleMesh", uvs, &triangle_mesh);
  ASSERT_NE(triangle, nullptr);

  auto surface = BKE_paint_layers_gradient_surface_get(*triangle, *triangle_mesh, "UVMap", 256);
  ASSERT_NE(surface, nullptr);
  EXPECT_EQ(surface->valid_texel_count, 0);
  EXPECT_FALSE(surface->has_valid_texels());
}

/** A surface assembled by hand carries no count; the predicate then falls back to the mask. */
TEST(PaintMaterialCompositeSurface, has_valid_texels_scans_a_surface_built_by_hand)
{
  PaintMaterialCompositeSurface surface;
  surface.width = 2;
  surface.height = 2;
  surface.valid.resize(4, uint8_t(0));
  EXPECT_EQ(surface.valid_texel_count, -1) << "nobody counted this one";
  EXPECT_FALSE(surface.has_valid_texels());

  surface.valid[2] = 1;
  EXPECT_TRUE(surface.has_valid_texels());
}

TEST(PaintMaterialCompositeSurface, has_valid_texels_prefers_the_count_over_the_mask)
{
  PaintMaterialCompositeSurface surface;
  surface.width = 2;
  surface.height = 2;
  surface.valid.resize(4, uint8_t(0));
  /* A count of zero is an answer, not an invitation to look at the mask. */
  surface.valid_texel_count = 0;
  surface.valid[0] = 1;
  EXPECT_FALSE(surface.has_valid_texels());
}

TEST(PaintMaterialCompositeSurface, sample_generated_prefers_stored_orco)
{
  PaintMaterialCompositeSurface surface;
  surface.width = 2;
  surface.height = 2;
  surface.positions.resize(4, float3(0.5f));
  surface.valid.resize(4, uint8_t(1));
  surface.generated_positions.resize(4, float3(0.0f));
  surface.generated_positions[0] = float3(-1.0f, -1.0f, 0.0f);
  surface.generated_positions[3] = float3(1.0f, 1.0f, 1.0f);
  /* A contradictory bbox proves the orco path is used, not the fallback. */
  surface.generated_bbox_valid = true;
  surface.generated_bbox_min = float3(0.0f);
  surface.generated_bbox_max = float3(1.0f);

  float3 generated;
  ASSERT_TRUE(surface.sample_generated(float2(0.1f, 0.1f), generated));
  EXPECT_NEAR(generated.x, 0.0f, 1e-6f);
  EXPECT_NEAR(generated.y, 0.0f, 1e-6f);
  EXPECT_NEAR(generated.z, 0.5f, 1e-6f);

  ASSERT_TRUE(surface.sample_generated(float2(0.9f, 0.9f), generated));
  EXPECT_NEAR(generated.x, 1.0f, 1e-6f);
  EXPECT_NEAR(generated.y, 1.0f, 1e-6f);
  EXPECT_NEAR(generated.z, 1.0f, 1e-6f);
}

TEST(PaintMaterialCompositeSurface, sample_generated_falls_back_to_the_bbox_without_orco)
{
  PaintMaterialCompositeSurface surface;
  surface.width = 1;
  surface.height = 1;
  surface.positions.append(float3(0.5f, 0.5f, 0.0f));
  surface.valid.append(1);
  surface.generated_bbox_valid = true;
  surface.generated_bbox_min = float3(0.0f);
  surface.generated_bbox_max = float3(1.0f, 1.0f, 0.0f);

  float3 generated;
  ASSERT_TRUE(surface.sample_generated(float2(0.5f, 0.5f), generated));
  EXPECT_NEAR(generated.x, 0.5f, 1e-6f);
  EXPECT_NEAR(generated.y, 0.5f, 1e-6f);
  EXPECT_NEAR(generated.z, 0.5f, 1e-6f);
}

}  // namespace blender::bke::tests
