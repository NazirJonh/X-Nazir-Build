/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The projection row tests split out of #paint_layers_description_test.cc: what supports a
 * projection, the frame values and their normalization, the payload's life cycle, and the frame
 * orientation helpers.
 */

#include "testing/testing.h"

#include <cmath>
#include <limits>

#include "MEM_guardedalloc.h"

#include "BKE_appdir.hh"
#include "BKE_attribute.hh"
#include "BKE_blender.hh"
#include "BKE_context.hh"
#include "BKE_global.hh"
#include "BKE_gtest_base.hh"
#include "BKE_idprop.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_lib_query.hh"
#include "BKE_paint_layers_gradient.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_object.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_composite.hh"
#include "BKE_paint_layers_edit.hh"
#include "BKE_paint_material_composite.hh"
#include "BKE_report.hh"

#include "../paint_layers_intern.hh"
#include "../paint_layers_kind.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "BLI_fileops.h"
#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_math_base.h"
#include "BLI_math_matrix.h"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector_types.hh"
#include "BLI_path_utils.hh"
#include "BLI_string.h"
#include "BLI_utildefines.h"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "BLO_readfile.hh"
#include "BLO_writefile.hh"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include <string>

#include "paint_layers_description_test_fixture.hh"
#include "paint_layers_graph_eval_test_fixture.hh"

namespace blender::bke::tests {

/* -------------------------------------------------------------------- */
/** \name Row projection
 * \{ */

TEST_F(PaintLayersDescription, projection_is_supported_by_a_fill_that_reads_a_map)
{
  Material &ma = *BKE_material_add(bmain, "ProjectionSupportMat");

  MaterialPaintLayer *bare = BKE_paint_layers_add(ma, "Bare", nullptr, PaintLayerPlace::Above);
  ASSERT_TRUE(BKE_paint_layers_set_source(ma, bare, MA_PAINT_LAYER_SOURCE_CONSTANT));
  EXPECT_FALSE(BKE_paint_layers_projection_supported(ma, *bare));
  EXPECT_FALSE(BKE_paint_layers_projection_set_space(
      ma, bare, MA_PAINT_LAYER_MAPPING_SPACE_PLANAR));

  MaterialPaintLayer *fill = mapping_fill_with_map(*bmain, ma, "Projected");
  EXPECT_TRUE(BKE_paint_layers_projection_supported(ma, *fill));
  EXPECT_FALSE(BKE_paint_layers_projection_applies(ma, *fill))
      << "supported is not switched on";
  EXPECT_TRUE(fill->projection == nullptr) << "the payload is on demand";

  ASSERT_TRUE(BKE_paint_layers_projection_set_space(
      ma, fill, MA_PAINT_LAYER_MAPPING_SPACE_TRIPLANAR));
  EXPECT_EQ(fill->mapping.space, MA_PAINT_LAYER_MAPPING_SPACE_TRIPLANAR);
  EXPECT_NE(fill->projection, nullptr);
  EXPECT_TRUE(BKE_paint_layers_projection_applies(ma, *fill));
  EXPECT_TRUE(BKE_paint_layers_projection_clip_get(*fill));
  EXPECT_EQ(BKE_paint_layers_projection_axis_get(*fill), MA_PAINT_LAYER_PROJECTION_AXIS_Z);

  /* A painted map is written by strokes at the raw UV: no projection either. */
  MaterialPaintLayer *paint = BKE_paint_layers_add(ma, "Paint", nullptr, PaintLayerPlace::Above);
  Image *paint_map = static_cast<Image *>(BKE_id_new(bmain, ID_IM, "ProjectedPaintMap"));
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      ma, paint, PAINT_MATERIAL_CHANNEL_BASE_COLOR, paint_map));
  EXPECT_FALSE(BKE_paint_layers_projection_supported(ma, *paint));
  EXPECT_FALSE(BKE_paint_layers_projection_set_space(
      ma, paint, MA_PAINT_LAYER_MAPPING_SPACE_BOX));
  EXPECT_EQ(paint->mapping.space, MA_PAINT_LAYER_MAPPING_SPACE_UV);
}

/** A Gradient row owns no map: the projection replaces the UV its line reads, which only means
 * something for a UV line. */
TEST_F(PaintLayersDescription, projection_is_supported_by_a_uv_gradient_row_only)
{
  Material &ma = *BKE_material_add(bmain, "ProjectionGradientMat");
  MaterialPaintLayer *gradient = BKE_paint_layers_gradient_add(
      ma, nullptr, nullptr, PaintLayerPlace::Above);
  ASSERT_NE(gradient, nullptr);
  ASSERT_NE(gradient->gradient, nullptr);
  /* A new Gradient reads the object space: its line has coordinates of its own and a frame has
   * nothing to replace there. */
  EXPECT_FALSE(BKE_paint_layers_projection_supported(ma, *gradient));
  EXPECT_FALSE(BKE_paint_layers_projection_set_space(
      ma, gradient, MA_PAINT_LAYER_MAPPING_SPACE_PLANAR));
  EXPECT_EQ(gradient->mapping.space, MA_PAINT_LAYER_MAPPING_SPACE_UV);

  ASSERT_TRUE(BKE_paint_layers_gradient_set_space(
      ma, *gradient, MA_PAINT_LAYER_GRADIENT_SPACE_UV));
  EXPECT_TRUE(BKE_paint_layers_projection_supported(ma, *gradient));
  ASSERT_TRUE(BKE_paint_layers_projection_set_space(
      ma, gradient, MA_PAINT_LAYER_MAPPING_SPACE_PLANAR));
  EXPECT_TRUE(BKE_paint_layers_projection_applies(ma, *gradient));

  /* Back to the object space: the line re-owns its coordinates, so the projection stops applying
   * even though the payload is still there, and comes back with the space. */
  ASSERT_TRUE(BKE_paint_layers_gradient_set_space(
      ma, *gradient, MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT));
  EXPECT_FALSE(BKE_paint_layers_projection_supported(ma, *gradient));
  EXPECT_FALSE(BKE_paint_layers_projection_applies(ma, *gradient));
  EXPECT_TRUE(BKE_paint_layers_gradient_set_space(
      ma, *gradient, MA_PAINT_LAYER_GRADIENT_SPACE_UV));
  EXPECT_TRUE(BKE_paint_layers_projection_applies(ma, *gradient));

  /* A Gradient mask item follows the row its stack hangs on; it holds no frame of its own. */
  ASSERT_NE(BKE_paint_layers_mask_add(ma, gradient, 1.0f), nullptr);
  MaterialPaintLayer *item = BKE_paint_layers_mask_item_add(
      ma, gradient, MA_PAINT_LAYER_SOURCE_GENERATOR, "Mask Gradient", 1.0f);
  ASSERT_NE(item, nullptr);
  ASSERT_TRUE(BKE_paint_layers_gradient_set_space(ma, *item, MA_PAINT_LAYER_GRADIENT_SPACE_UV));
  EXPECT_FALSE(BKE_paint_layers_projection_supported(ma, *item));
  EXPECT_FALSE(BKE_paint_layers_projection_set_space(
      ma, item, MA_PAINT_LAYER_MAPPING_SPACE_PLANAR));
}

TEST_F(PaintLayersDescription, projection_frame_values_are_normalized_and_value_only)
{
  Material &ma = *BKE_material_add(bmain, "ProjectionValuesMat");
  MaterialPaintLayer *fill = mapping_fill_with_map(*bmain, ma, "Projected");
  ASSERT_TRUE(BKE_paint_layers_projection_set_space(
      ma, fill, MA_PAINT_LAYER_MAPPING_SPACE_BOX));

  ma.paint_layers_flag &= ~MA_PAINT_LAYERS_REGEN;
  const float center[3] = {1.0f, 2.0f, 3.0f};
  const float size[3] = {2.0f, 0.0f, -4.0f};
  const float rotation[3] = {0.1f, 0.2f, 0.3f};
  ASSERT_TRUE(BKE_paint_layers_projection_set_values_euler(ma, fill, center, size, rotation));
  EXPECT_EQ(ma.paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0)
      << "a frame move never rebuilds the graph";
  float stored_euler[3];
  BKE_paint_layers_projection_frame_euler(
      fill->projection->normal, fill->projection->roll, stored_euler);
  for (int i = 0; i < 3; i++) {
    EXPECT_FLOAT_EQ(fill->projection->center[i], center[i]);
    EXPECT_FLOAT_EQ(stored_euler[i], rotation[i]);
  }
  EXPECT_FLOAT_EQ(fill->projection->size[0], 2.0f);
  EXPECT_FLOAT_EQ(fill->projection->size[1], 1.0f);
  EXPECT_FLOAT_EQ(fill->projection->size[2], -4.0f);
  EXPECT_FLOAT_EQ(BKE_paint_layers_projection_size_normalize(0.0f), 1.0f);
  EXPECT_FLOAT_EQ(BKE_paint_layers_projection_size_normalize(1e-6f),
                  PAINT_LAYER_PROJECTION_SIZE_MIN);

  /* A zero or unknown frame cannot be written, and the clip toggle is a value too. */
  const float bad[3] = {0.0f, 0.0f, std::numeric_limits<float>::infinity()};
  EXPECT_FALSE(BKE_paint_layers_projection_set_center(ma, fill, bad));
  ma.paint_layers_flag &= ~MA_PAINT_LAYERS_REGEN;
  ASSERT_TRUE(BKE_paint_layers_projection_set_clip(ma, fill, false));
  EXPECT_FALSE(BKE_paint_layers_projection_clip_get(*fill));
  EXPECT_EQ(ma.paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0)
      << "the clip toggle is a group value, not topology";
  ASSERT_TRUE(BKE_paint_layers_projection_set_axis(ma, fill, MA_PAINT_LAYER_PROJECTION_AXIS_X));
  EXPECT_NE(ma.paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0) << "the axis changes the UV math";
}

TEST_F(PaintLayersDescription, projection_frame_inside_matches_the_graph_rule)
{
  /* Why the CPU calls this: the generated graph builds the same two Multiply Add factors per axis
   * (see `projection_axis_factor`), one inside the frame and fading over
   * #PAINT_LAYER_PROJECTION_EDGE_SOFT, so a frame-space point on either side of the frame has to
   * answer the same on the CPU. */
  const float inside[3] = {0.0f, 0.99f, -0.99f};
  const float on_edge[3] = {1.0f, 0.0f, 0.0f};
  const float halfway_out[3] = {1.0f + 0.25f * PAINT_LAYER_PROJECTION_EDGE_SOFT, 0.0f, 0.0f};
  const float outside[3] = {1.01f, 0.0f, 0.0f};
  EXPECT_FLOAT_EQ(BKE_paint_layers_projection_frame_factor(inside), 1.0f);
  EXPECT_FLOAT_EQ(BKE_paint_layers_projection_frame_factor(on_edge), 0.0f);
  EXPECT_NEAR(BKE_paint_layers_projection_frame_factor(halfway_out), 0.75f, 1e-5f);
  EXPECT_FLOAT_EQ(BKE_paint_layers_projection_frame_factor(outside), 0.0f);
}

TEST_F(PaintLayersDescription, projection_facing_weight_matches_the_graph_rule)
{
  /* The graph runs the frame-space normal component through one clamped Multiply Add and the
   * t * t * (3 - 2 t) hermite (see `projection_facing_hermite_entry`); the CPU repeats both in
   * #BKE_paint_layers_projection_facing_weight. The scale comes from the one helper the value
   * sync and the bake share, so the sign it carries is the side the fade keeps. */
  const float angle = 80.0f;
  const float cos_min = cosf(DEG2RADF(angle));
  const float cos_max = cosf(DEG2RADF(angle - PAINT_LAYER_PROJECTION_ANGLE_BAND));

  /* Without Flip: the frame's +Z looks at the observer, so the faces whose normal aligns with
   * +Z (the positive component) are kept at full weight. */
  float scale = 0.0f;
  float offset = 0.0f;
  BKE_paint_layers_projection_facing_weights(angle, false, scale, offset);
  EXPECT_GT(scale, 0.0f) << "without flip, positive component is the kept side";
  EXPECT_FLOAT_EQ(BKE_paint_layers_projection_facing_weight(scale, offset, 1.0f), 1.0f);
  EXPECT_NEAR(BKE_paint_layers_projection_facing_weight(scale, offset, cos_max), 1.0f, 1e-5f);
  EXPECT_NEAR(BKE_paint_layers_projection_facing_weight(scale, offset, cos_min), 0.0f, 1e-5f);
  EXPECT_FLOAT_EQ(BKE_paint_layers_projection_facing_weight(scale, offset, -1.0f), 0.0f);
  /* The band is symmetric: the fading rim's midpoint sits halfway up the hermite. */
  EXPECT_NEAR(BKE_paint_layers_projection_facing_weight(
                  scale, offset, 0.5f * (cos_min + cos_max)),
              0.5f,
              1e-4f);

  /* Flip mirrors the weight onto the other side of the surface without moving the limit. */
  BKE_paint_layers_projection_facing_weights(angle, true, scale, offset);
  EXPECT_LT(scale, 0.0f);
  EXPECT_FLOAT_EQ(BKE_paint_layers_projection_facing_weight(scale, offset, -1.0f), 1.0f);
  EXPECT_NEAR(BKE_paint_layers_projection_facing_weight(scale, offset, -cos_max), 1.0f, 1e-5f);
  EXPECT_NEAR(BKE_paint_layers_projection_facing_weight(scale, offset, -cos_min), 0.0f, 1e-5f);
  EXPECT_FLOAT_EQ(BKE_paint_layers_projection_facing_weight(scale, offset, 1.0f), 0.0f);
}

TEST_F(PaintLayersDescription, projection_angle_is_stored_away_from_the_file_marker)
{
  Material &ma = *BKE_material_add(bmain, "ProjectionAngleMat");
  MaterialPaintLayer *fill = mapping_fill_with_map(*bmain, ma, "Projected");
  ASSERT_TRUE(BKE_paint_layers_projection_set_space(
      ma, fill, MA_PAINT_LAYER_MAPPING_SPACE_PLANAR));

  /* A zero limit would fade the whole projection out, so a stored limit never reaches it; a half
   * turn or more is the widest limit there is. */
  ASSERT_TRUE(BKE_paint_layers_projection_set_angle(ma, fill, 0.0f));
  EXPECT_FLOAT_EQ(fill->projection->angle, 1.0f);
  ASSERT_TRUE(BKE_paint_layers_projection_set_angle(ma, fill, -30.0f));
  EXPECT_FLOAT_EQ(fill->projection->angle, 1.0f);
  ASSERT_TRUE(BKE_paint_layers_projection_set_angle(ma, fill, 240.0f));
  EXPECT_FLOAT_EQ(fill->projection->angle, 180.0f);
  ASSERT_TRUE(BKE_paint_layers_projection_set_angle(ma, fill, 55.0f));
  EXPECT_FLOAT_EQ(BKE_paint_layers_projection_angle_get(*fill), 55.0f);
}

TEST_F(PaintLayersDescription, projection_angle_zero_reads_back_as_the_default)
{
  /* A zero limit would fade the whole projection out; the normalized read maps it, a negative
   * value and anything past a half turn to the default. */
  EXPECT_EQ(BKE_paint_layers_projection_angle_normalize(0.0f),
            PAINT_LAYER_PROJECTION_ANGLE_DEFAULT);
  EXPECT_EQ(BKE_paint_layers_projection_angle_normalize(-4.0f),
            PAINT_LAYER_PROJECTION_ANGLE_DEFAULT);
  EXPECT_EQ(BKE_paint_layers_projection_angle_normalize(250.0f),
            PAINT_LAYER_PROJECTION_ANGLE_DEFAULT);
  EXPECT_EQ(BKE_paint_layers_projection_angle_normalize(65.0f), 65.0f);
}

TEST_F(PaintLayersDescription, projection_mode_table_covers_every_space)
{
  for (int space = 0; space < MA_PAINT_LAYER_MAPPING_SPACE_NUM; space++) {
    EXPECT_EQ(BKE_paint_layers_projection_mode_info(int16_t(space)).space, space);
  }
  EXPECT_FALSE(BKE_paint_layers_projection_mode_info(MA_PAINT_LAYER_MAPPING_SPACE_UV).uses_axis);
  EXPECT_TRUE(BKE_paint_layers_projection_mode_info(MA_PAINT_LAYER_MAPPING_SPACE_PLANAR).uses_axis);
  EXPECT_TRUE(
      BKE_paint_layers_projection_mode_info(MA_PAINT_LAYER_MAPPING_SPACE_CYLINDER).uses_axis);
  /* Every mode names a map read count and a controller display; only the table decides who blends
   * faces and who reads an atlas. */
  int blend_modes = 0;
  int cube_modes = 0;
  for (int space = 0; space < MA_PAINT_LAYER_MAPPING_SPACE_NUM; space++) {
    const ProjectionModeInfo &info = BKE_paint_layers_projection_mode_info(int16_t(space));
    EXPECT_TRUE(info.samples_num == 1 || info.samples_num == 3) << "space " << space;
    EXPECT_NE(info.controller_display, 0) << "space " << space;
    blend_modes += info.uses_blend ? 1 : 0;
    cube_modes += info.uses_cube_layout ? 1 : 0;
    /* Exactly the kinds that fill the frame's extents are volumes. */
    const bool volume_expected = ELEM(space,
                                      MA_PAINT_LAYER_MAPPING_SPACE_BOX,
                                      MA_PAINT_LAYER_MAPPING_SPACE_TRIPLANAR,
                                      MA_PAINT_LAYER_MAPPING_SPACE_CUBE,
                                      MA_PAINT_LAYER_MAPPING_SPACE_CYLINDER,
                                      MA_PAINT_LAYER_MAPPING_SPACE_SPHERE);
    EXPECT_EQ(info.is_volume, volume_expected) << "space " << space;
  }
  EXPECT_EQ(blend_modes, 1);
  EXPECT_EQ(cube_modes, 1);
  EXPECT_EQ(BKE_paint_layers_projection_mode_info(MA_PAINT_LAYER_MAPPING_SPACE_CYLINDER).cage_style,
            PROJECTION_CAGE_CYLINDER);
  EXPECT_EQ(BKE_paint_layers_projection_mode_info(MA_PAINT_LAYER_MAPPING_SPACE_SPHERE).cage_style,
            PROJECTION_CAGE_SPHERE);
  EXPECT_TRUE(BKE_paint_layers_projection_mode_info(MA_PAINT_LAYER_MAPPING_SPACE_TRIPLANAR)
                  .uses_blend);
  EXPECT_EQ(BKE_paint_layers_projection_mode_info(MA_PAINT_LAYER_MAPPING_SPACE_BOX).samples_num, 3);
  /* A stored value outside the table reads as UV, which has no frame. */
  EXPECT_EQ(BKE_paint_layers_projection_mode_info(MA_PAINT_LAYER_MAPPING_SPACE_NUM).space,
            MA_PAINT_LAYER_MAPPING_SPACE_UV);
  EXPECT_EQ(BKE_paint_layers_projection_mode_info(-1).space, MA_PAINT_LAYER_MAPPING_SPACE_UV);
}

TEST_F(PaintLayersDescription, projection_snap_and_controller_scale_are_off_and_value_only)
{
  Material &ma = *BKE_material_add(bmain, "ProjectionOptionsMat");
  MaterialPaintLayer *fill = mapping_fill_with_map(*bmain, ma, "Projected");
  ASSERT_TRUE(BKE_paint_layers_projection_set_space(
      ma, fill, MA_PAINT_LAYER_MAPPING_SPACE_PLANAR));

  /* Both options keep the decal behaviour of every earlier frame until the user asks for them. */
  EXPECT_FALSE(BKE_paint_layers_projection_snap_get(*fill));
  EXPECT_FALSE(BKE_paint_layers_projection_size_from_controller_get(*fill));
  ASSERT_TRUE(BKE_paint_layers_projection_set_snap(ma, fill, true));
  ASSERT_TRUE(BKE_paint_layers_projection_set_size_from_controller(ma, fill, true));
  EXPECT_TRUE(BKE_paint_layers_projection_snap_get(*fill));
  EXPECT_TRUE(BKE_paint_layers_projection_size_from_controller_get(*fill));

  const float center[3] = {0.0f, 0.0f, 0.0f};
  const float size[3] = {2.0f, 3.0f, 4.0f};
  const float rotation[3] = {0.0f, 0.0f, 0.0f};
  ASSERT_TRUE(BKE_paint_layers_projection_set_values_euler(ma, fill, center, size, rotation));

  /* The frame reads one on every axis while the controller carries the scale, but the inline
   * extents stay stored so turning the option off restores them. */
  float effective[3];
  BKE_paint_layers_projection_size_effective(*fill, effective);
  EXPECT_FLOAT_EQ(effective[0], 1.0f);
  EXPECT_FLOAT_EQ(effective[1], 1.0f);
  EXPECT_FLOAT_EQ(effective[2], 1.0f);
  EXPECT_FLOAT_EQ(fill->projection->size[0], 2.0f);
  EXPECT_FLOAT_EQ(fill->projection->size[1], 3.0f);
  EXPECT_FLOAT_EQ(fill->projection->size[2], 4.0f);

  ASSERT_TRUE(BKE_paint_layers_projection_set_size_from_controller(ma, fill, false));
  BKE_paint_layers_projection_size_effective(*fill, effective);
  EXPECT_FLOAT_EQ(effective[0], 2.0f);
  EXPECT_FLOAT_EQ(effective[1], 3.0f);
  EXPECT_FLOAT_EQ(effective[2], 4.0f);
}

TEST_F(PaintLayersDescription, projection_aspect_factors_follow_the_image)
{
  /* Ucupaint's decal Scale: the footprint of the image's wider axis grows by the aspect, so a
   * non-square image is not stretched; a square image and an unknown one read one. */
  float factors[2] = {0.0f, 0.0f};
  BKE_paint_layers_projection_aspect_factors(nullptr, factors);
  EXPECT_FLOAT_EQ(factors[0], 1.0f);
  EXPECT_FLOAT_EQ(factors[1], 1.0f);

  const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *wide = BKE_image_add_generated(
      bmain, 2000, 1000, "AspectWide", 32, false, IMA_GENTYPE_BLANK, color, false, true, false);
  ASSERT_NE(wide, nullptr);
  BKE_paint_layers_projection_aspect_factors(wide, factors);
  EXPECT_FLOAT_EQ(factors[0], 0.5f);
  EXPECT_FLOAT_EQ(factors[1], 1.0f);

  Image *square = BKE_image_add_generated(
      bmain, 512, 512, "AspectSquare", 32, false, IMA_GENTYPE_BLANK, color, false, true, false);
  ASSERT_NE(square, nullptr);
  BKE_paint_layers_projection_aspect_factors(square, factors);
  EXPECT_FLOAT_EQ(factors[0], 1.0f);
  EXPECT_FLOAT_EQ(factors[1], 1.0f);

  /* A tall image stretches its Y footprint instead, on the same rule. */
  Image *tall = BKE_image_add_generated(
      bmain, 1000, 2000, "AspectTall", 32, false, IMA_GENTYPE_BLANK, color, false, true, false);
  ASSERT_NE(tall, nullptr);
  BKE_paint_layers_projection_aspect_factors(tall, factors);
  EXPECT_FLOAT_EQ(factors[0], 1.0f);
  EXPECT_FLOAT_EQ(factors[1], 0.5f);
}

TEST_F(PaintLayersDescription, projection_payload_copies_with_the_row_and_clears_with_uv)
{
  Material *ma = BKE_material_add(bmain, "ProjectionCopyMat");
  MaterialPaintLayer *fill = mapping_fill_with_map(*bmain, *ma, "Projected");
  ASSERT_TRUE(BKE_paint_layers_projection_set_space(
      *ma, fill, MA_PAINT_LAYER_MAPPING_SPACE_CYLINDER));
  const float center[3] = {0.5f, -1.0f, 2.0f};
  const float size[3] = {3.0f, 4.0f, 5.0f};
  const float rotation[3] = {0.25f, 0.5f, 0.75f};
  ASSERT_TRUE(BKE_paint_layers_projection_set_values_euler(
      *ma, fill, center, size, rotation));
  ASSERT_TRUE(BKE_paint_layers_projection_set_blend(*ma, fill, 0.5f));

  Material *copy = id_cast<Material *>(BKE_id_copy(bmain, &ma->id));
  ASSERT_NE(copy, nullptr);
  const MaterialPaintLayer *copy_layer = paint_layers_first(*copy);
  ASSERT_NE(copy_layer, nullptr);
  ASSERT_NE(copy_layer->projection, nullptr);
  EXPECT_EQ(copy_layer->mapping.space, MA_PAINT_LAYER_MAPPING_SPACE_CYLINDER);
  EXPECT_FLOAT_EQ(copy_layer->projection->center[2], 2.0f);
  EXPECT_FLOAT_EQ(copy_layer->projection->size[1], 4.0f);
  EXPECT_FLOAT_EQ(copy_layer->projection->blend, 0.5f);
  BKE_id_free(bmain, copy);

  MaterialPaintLayer *duplicate = BKE_paint_layers_duplicate(*bmain, *ma, fill);
  ASSERT_NE(duplicate, nullptr);
  ASSERT_NE(duplicate->projection, nullptr);
  EXPECT_EQ(duplicate->mapping.space, MA_PAINT_LAYER_MAPPING_SPACE_CYLINDER);
  /* Freeing a row with a frame releases the payload with it. */
  ASSERT_TRUE(BKE_paint_layers_remove(*ma, duplicate));
  EXPECT_EQ(paint_layers_first(*ma), fill);

  /* Returning to UV frees the frame; the row keeps its mapping values. */
  ASSERT_TRUE(BKE_paint_layers_projection_set_space(
      *ma, fill, MA_PAINT_LAYER_MAPPING_SPACE_UV));
  EXPECT_EQ(fill->projection, nullptr);
  EXPECT_FALSE(BKE_paint_layers_projection_applies(*ma, *fill));
}

TEST_F(PaintLayersDescription, projection_space_round_trip_resets_the_controller)
{
  Material &ma = *BKE_material_add(bmain, "ProjectionRoundTripStateMat");
  MaterialPaintLayer *fill = mapping_fill_with_map(*bmain, ma, "Projected");
  Object *controller = BKE_object_add_only_object(bmain, OB_EMPTY, "ProjectionController");
  ASSERT_NE(controller, nullptr);

  ASSERT_TRUE(BKE_paint_layers_projection_set_space(
      ma, fill, MA_PAINT_LAYER_MAPPING_SPACE_PLANAR));
  ASSERT_NE(fill->projection, nullptr);
  ASSERT_TRUE(BKE_paint_layers_projection_set_object(ma, fill, controller));
  EXPECT_EQ(fill->projection_object, controller);

  /* UV frees the frame and lets the controller go with it. */
  ASSERT_TRUE(BKE_paint_layers_projection_set_space(
      ma, fill, MA_PAINT_LAYER_MAPPING_SPACE_UV));
  EXPECT_EQ(fill->projection, nullptr);
  EXPECT_EQ(fill->projection_object, nullptr);

  /* Switching back allocates a fresh frame with defaults and no controller. */
  ASSERT_TRUE(BKE_paint_layers_projection_set_space(
      ma, fill, MA_PAINT_LAYER_MAPPING_SPACE_PLANAR));
  ASSERT_NE(fill->projection, nullptr);
  EXPECT_EQ(fill->projection_object, nullptr);
  EXPECT_TRUE(BKE_paint_layers_projection_clip_get(*fill));
  EXPECT_EQ(BKE_paint_layers_projection_axis_get(*fill), MA_PAINT_LAYER_PROJECTION_AXIS_Z);
}

TEST_F(PaintLayersDescription, projection_duplicate_owns_its_payload_and_takes_a_controller_user)
{
  Material &ma = *BKE_material_add(bmain, "ProjectionDuplicateMat");
  MaterialPaintLayer *fill = mapping_fill_with_map(*bmain, ma, "Projected");
  Object *controller = BKE_object_add_only_object(bmain, OB_EMPTY, "ProjectionController");
  ASSERT_NE(controller, nullptr);
  ASSERT_TRUE(BKE_paint_layers_projection_set_space(
      ma, fill, MA_PAINT_LAYER_MAPPING_SPACE_PLANAR));
  ASSERT_TRUE(BKE_paint_layers_projection_set_object(ma, fill, controller));
  const int users = controller->id.us;

  MaterialPaintLayer *copy = BKE_paint_layers_duplicate(*bmain, ma, fill);
  ASSERT_NE(copy, nullptr);
  ASSERT_NE(copy->projection, nullptr);
  EXPECT_NE(copy->projection, fill->projection);
  EXPECT_EQ(copy->projection_object, controller);
  EXPECT_EQ(controller->id.us, users + 1);
}

TEST_F(PaintLayersDescription, projection_round_trips_through_a_file)
{
  Material *ma = BKE_material_add(bmain, "ProjectionRoundTripMat");
  ma->paint_layers_flag = MA_PAINT_LAYERED;
  id_us_plus(&ma->id);

  Image *image = static_cast<Image *>(BKE_id_new(bmain, ID_IM, "ProjectionRoundTripImage"));
  id_us_plus(&image->id);

  MaterialPaintLayer *layer = paint_layer_add(*ma, "Projected");
  ASSERT_TRUE(BKE_paint_layers_set_source(*ma, layer, MA_PAINT_LAYER_SOURCE_CONSTANT));
  paint_layer_channel_add(*layer, *image);
  ASSERT_TRUE(BKE_paint_layers_projection_set_space(
      *ma, layer, MA_PAINT_LAYER_MAPPING_SPACE_BOX));
  const float center[3] = {1.0f, -2.0f, 0.5f};
  const float size[3] = {2.0f, 3.0f, 4.0f};
  const float rotation[3] = {0.1f, 0.2f, 0.3f};
  ASSERT_TRUE(BKE_paint_layers_projection_set_values_euler(*ma, layer, center, size, rotation));
  ASSERT_TRUE(BKE_paint_layers_projection_set_blend(*ma, layer, 0.25f));
  ASSERT_TRUE(BKE_paint_layers_projection_set_clip(*ma, layer, false));

  char filepath[FILE_MAX];
  BLI_path_join(filepath,
                sizeof(filepath),
                BKE_tempdir_session(),
                "paint_layers_projection_round_trip.blend");
  BlendFileWriteParams write_params{};
  ASSERT_TRUE(BLO_write_file(bmain, filepath, 0, &write_params, nullptr));

  BlendFileReadReport read_report{};
  BlendFileData *bfd = BLO_read_from_file(filepath, BLO_READ_SKIP_NONE, &read_report);
  ASSERT_NE(bfd, nullptr);

  Material *reloaded = nullptr;
  for (Material &candidate : bfd->main->materials) {
    if (STREQ(candidate.id.name + 2, "ProjectionRoundTripMat")) {
      reloaded = &candidate;
      break;
    }
  }
  ASSERT_NE(reloaded, nullptr);
  MaterialPaintLayer *reloaded_layer = paint_layers_first(*reloaded);
  ASSERT_NE(reloaded_layer, nullptr);
  EXPECT_EQ(reloaded_layer->mapping.space, MA_PAINT_LAYER_MAPPING_SPACE_BOX);
  ASSERT_NE(reloaded_layer->projection, nullptr);
  EXPECT_FLOAT_EQ(reloaded_layer->projection->center[1], -2.0f);
  EXPECT_FLOAT_EQ(reloaded_layer->projection->size[2], 4.0f);
  float reloaded_euler[3];
  BKE_paint_layers_projection_frame_euler(
      reloaded_layer->projection->normal, reloaded_layer->projection->roll, reloaded_euler);
  EXPECT_NEAR(reloaded_euler[2], 0.3f, 1e-5f);
  EXPECT_FLOAT_EQ(reloaded_layer->projection->blend, 0.25f);
  EXPECT_FALSE(BKE_paint_layers_projection_clip_get(*reloaded_layer));
  EXPECT_EQ(reloaded_layer->projection_object, nullptr);
}

TEST_F(PaintLayersDescription, projection_old_file_reads_the_facing_defaults)
{
  /* A file written before the facing limit and its flags existed carries the zeroed angle and
   * none of the flags: the read has to give it the defaults the payload gives a new frame,
   * except the flip, which stays off so the arrow still enters the surface. */
  Material *ma = BKE_material_add(bmain, "ProjectionFacingMigrationMat");
  ma->paint_layers_flag = MA_PAINT_LAYERED;
  id_us_plus(&ma->id);

  Image *image = static_cast<Image *>(BKE_id_new(bmain, ID_IM, "ProjectionFacingMigrationImage"));
  id_us_plus(&image->id);

  MaterialPaintLayer *layer = paint_layer_add(*ma, "Projected");
  ASSERT_TRUE(BKE_paint_layers_set_source(*ma, layer, MA_PAINT_LAYER_SOURCE_CONSTANT));
  paint_layer_channel_add(*layer, *image);
  ASSERT_TRUE(BKE_paint_layers_projection_set_space(
      *ma, layer, MA_PAINT_LAYER_MAPPING_SPACE_PLANAR));
  layer->projection->angle = 0.0f;
  layer->projection->flag = MA_PAINT_LAYER_PROJECTION_CLIP;

  char filepath[FILE_MAX];
  BLI_path_join(filepath,
                sizeof(filepath),
                BKE_tempdir_session(),
                "paint_layers_projection_facing_migration.blend");
  BlendFileWriteParams write_params{};
  ASSERT_TRUE(BLO_write_file(bmain, filepath, 0, &write_params, nullptr));

  BlendFileReadReport read_report{};
  BlendFileData *bfd = BLO_read_from_file(filepath, BLO_READ_SKIP_NONE, &read_report);
  ASSERT_NE(bfd, nullptr);

  Material *reloaded = nullptr;
  for (Material &candidate : bfd->main->materials) {
    if (STREQ(candidate.id.name + 2, "ProjectionFacingMigrationMat")) {
      reloaded = &candidate;
      break;
    }
  }
  ASSERT_NE(reloaded, nullptr);
  MaterialPaintLayer *reloaded_layer = paint_layers_first(*reloaded);
  ASSERT_NE(reloaded_layer, nullptr);
  ASSERT_NE(reloaded_layer->projection, nullptr);
  EXPECT_FLOAT_EQ(reloaded_layer->projection->angle, PAINT_LAYER_PROJECTION_ANGLE_DEFAULT);
  EXPECT_TRUE(BKE_paint_layers_projection_depth_get(*reloaded_layer));
  EXPECT_TRUE(BKE_paint_layers_projection_fit_aspect_get(*reloaded_layer));
  EXPECT_TRUE(BKE_paint_layers_projection_facing_get(*reloaded_layer));
  EXPECT_FALSE(BKE_paint_layers_projection_facing_invert_get(*reloaded_layer));
  /* The two newer bits were not in the file: both read as their off state. */
  EXPECT_FALSE(BKE_paint_layers_projection_snap_get(*reloaded_layer));
  EXPECT_FALSE(BKE_paint_layers_projection_size_from_controller_get(*reloaded_layer));

  BLO_blendfiledata_free(bfd);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Projection frame orientation helpers (WP9)
 * \{ */

TEST_F(PaintLayersDescription, projection_frame_matrix_orthonormal_and_round_trips)
{
  /* For a set of representative normals and rolls, frame_matrix produces an orthonormal matrix
   * with det=+1, and frame_from_matrix recovers the normal+roll. */
  const float pi = M_PI;
  /* Normals: ±axes, diagonal, near-threshold (|n·Z| just below and just above 0.999). */
  const float near_below[3] = {0.0f, sqrtf(1.0f - 0.998f * 0.998f), 0.998f};  /* dot≈ 0.998 < 0.999. */
  const float near_above[3] = {0.0f, sqrtf(1.0f - 0.9995f * 0.9995f), 0.9995f};  /* dot≈ 0.9995 > 0.999. */
  const float diag[3] = {0.577f, 0.577f, 0.577f};
  const float nx[3] = {1.0f, 0.0f, 0.0f};
  const float nnx[3] = {-1.0f, 0.0f, 0.0f};
  const float ny[3] = {0.0f, 1.0f, 0.0f};
  const float nny[3] = {0.0f, -1.0f, 0.0f};
  const float nz[3] = {0.0f, 0.0f, 1.0f};
  const float nnz[3] = {0.0f, 0.0f, -1.0f};
  const float rolls[] = {0.0f, pi / 2.0f, -pi / 2.0f, pi, 0.3f};
  const int num_rolls = 5;
  const float *normals[] = {nx, nnx, ny, nny, nz, nnz, diag, near_below, near_above};
  const int num_normals = 9;
  for (const int ni : IndexRange(num_normals)) {
    for (const int ri : IndexRange(num_rolls)) {
      const float *n = normals[ni];
      const float r = rolls[ri];
      float mat[3][3];
      BKE_paint_layers_projection_frame_matrix(n, r, mat);
      /* Orthonormality: rows unit and perpendicular. */
      for (const int i : IndexRange(3)) {
        const float len_sq = mat[i][0] * mat[i][0] + mat[i][1] * mat[i][1] +
                             mat[i][2] * mat[i][2];
        EXPECT_NEAR(len_sq, 1.0f, 1e-5f)
            << "normal[" << ni << "] roll=" << r << " row " << i << " not unit";
        for (const int j : IndexRange(i + 1, 3)) {
          const float dot_val = mat[i][0] * mat[j][0] + mat[i][1] * mat[j][1] +
                                mat[i][2] * mat[j][2];
          EXPECT_NEAR(dot_val, 0.0f, 1e-5f)
              << "normal[" << ni << "] rows " << i << "," << j << " not perpendicular";
        }
      }
      /* det = +1: cross(row0, row1) dot row2 = 1. */
      float cr[3];
      cross_v3_v3v3(cr, mat[0], mat[1]);
      const float det_val = dot_v3v3(cr, mat[2]);
      EXPECT_NEAR(det_val, 1.0f, 1e-5f)
          << "normal[" << ni << "] roll=" << r << " det not +1";
      /* Axis 2 (`mat[2]`) = the normalized normal. */
      float expected[3];
      copy_v3_v3(expected, n);
      normalize_v3(expected);
      EXPECT_NEAR(mat[2][0], expected[0], 1e-5f);
      EXPECT_NEAR(mat[2][1], expected[1], 1e-5f);
      EXPECT_NEAR(mat[2][2], expected[2], 1e-5f);
      /* Round-trip. */
      float rec_n[3];
      float rec_r;
      BKE_paint_layers_projection_frame_from_matrix(mat, rec_n, &rec_r);
      for (const int i : IndexRange(3)) {
        EXPECT_NEAR(rec_n[i], expected[i], 1e-5f) << "normal round-trip axis " << i;
      }
      /* Compare roll as angle mod 2*pi. */
      EXPECT_NEAR(sinf(rec_r), sinf(r), 1e-5f);
      EXPECT_NEAR(cosf(rec_r), cosf(r), 1e-5f);
    }
  }
}

TEST_F(PaintLayersDescription, projection_frame_euler_agrees_with_frame_matrix)
{
  /* frame_euler gives the same rotation as frame_matrix when fed to eul_to_mat3. */
  const float normals[][3] = {
      {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.6f, 0.0f, 0.8f}};
  const float rolls[] = {0.0f, M_PI / 4.0f, -M_PI / 2.0f};
  for (const auto &n : normals) {
    for (const float roll : rolls) {
      float mat_direct[3][3];
      BKE_paint_layers_projection_frame_matrix(n, roll, mat_direct);
      float euler[3];
      BKE_paint_layers_projection_frame_euler(n, roll, euler);
      float mat_from_euler[3][3];
      eul_to_mat3(mat_from_euler, euler);
      for (const int i : IndexRange(3)) {
        for (const int j : IndexRange(3)) {
          EXPECT_NEAR(mat_direct[i][j], mat_from_euler[i][j], 1e-5f)
              << " mismatch at (" << i << "," << j << ")";
        }
      }
    }
  }
}

TEST_F(PaintLayersDescription, projection_frame_y_points_up_for_horizontal_normal)
{
  /* With roll=0 and normal = +X, column 1 (the frame Y axis) should point in the +Z direction
   * (the reference up), because the image top faces up for a wall projected along +X. */
  const float normal[3] = {1.0f, 0.0f, 0.0f};
  float mat[3][3];
  BKE_paint_layers_projection_frame_matrix(normal, 0.0f, mat);
  /* `mat[1]` = Y axis in parent space. */
  EXPECT_NEAR(mat[1][0], 0.0f, 1e-5f);
  EXPECT_NEAR(mat[1][1], 0.0f, 1e-5f);
  /* +Z or -Z depending on cross product orientation; the important thing is it's unit in Z. */
  EXPECT_GT(fabsf(mat[1][2]), 0.99f) << "Y axis should be nearly vertical for horizontal normal";
}

TEST_F(PaintLayersDescription, projection_set_orientation_normalizes_and_rejects_invalid)
{
  Material &ma = *BKE_material_add(bmain, "SetOrientMat");
  MaterialPaintLayer *fill = mapping_fill_with_map(*bmain, ma, "OrientFill");
  ASSERT_TRUE(BKE_paint_layers_projection_set_space(
      ma, fill, MA_PAINT_LAYER_MAPPING_SPACE_PLANAR));

  /* Non-normalized normal is normalized before storing. */
  const float long_n[3] = {0.0f, 0.0f, 5.0f};
  ASSERT_TRUE(BKE_paint_layers_projection_set_orientation(ma, fill, long_n, 0.0f));
  EXPECT_NEAR(fill->projection->normal[2], 1.0f, 1e-6f);

  /* Zero normal becomes (0,0,1). */
  const float zero_n[3] = {0.0f, 0.0f, 0.0f};
  ASSERT_TRUE(BKE_paint_layers_projection_set_orientation(ma, fill, zero_n, 1.0f));
  EXPECT_NEAR(fill->projection->normal[0], 0.0f, 1e-6f);
  EXPECT_NEAR(fill->projection->normal[2], 1.0f, 1e-6f);
  EXPECT_NEAR(fill->projection->roll, 1.0f, 1e-6f);

  /* NaN rejected, values unchanged. */
  const float nan_n[3] = {std::numeric_limits<float>::quiet_NaN(), 0.0f, 1.0f};
  EXPECT_FALSE(BKE_paint_layers_projection_set_orientation(ma, fill, nan_n, 0.0f));
  EXPECT_NEAR(fill->projection->roll, 1.0f, 1e-6f);

  /* Inf in roll rejected. */
  const float good_n[3] = {1.0f, 0.0f, 0.0f};
  EXPECT_FALSE(BKE_paint_layers_projection_set_orientation(
      ma, fill, good_n, std::numeric_limits<float>::infinity()));
}

TEST_F(PaintLayersDescription, projection_set_roll_wraps_to_pm_pi)
{
  Material &ma = *BKE_material_add(bmain, "SetRollMat");
  MaterialPaintLayer *fill = mapping_fill_with_map(*bmain, ma, "RollFill");
  ASSERT_TRUE(BKE_paint_layers_projection_set_space(
      ma, fill, MA_PAINT_LAYER_MAPPING_SPACE_PLANAR));

  /* 3pi/2 wraps to -pi/2. */
  ASSERT_TRUE(BKE_paint_layers_projection_set_roll(ma, fill, 3.0f * M_PI / 2.0f));
  EXPECT_NEAR(fill->projection->roll, -M_PI / 2.0f, 1e-5f);

  /* -3pi/2 wraps to pi/2. */
  ASSERT_TRUE(BKE_paint_layers_projection_set_roll(ma, fill, -3.0f * M_PI / 2.0f));
  EXPECT_NEAR(fill->projection->roll, M_PI / 2.0f, 1e-5f);

  /* pi stays pi. */
  ASSERT_TRUE(BKE_paint_layers_projection_set_roll(ma, fill, M_PI));
  EXPECT_NEAR(fill->projection->roll, M_PI, 1e-5f);
}

TEST_F(PaintLayersDescription, projection_set_values_rejects_foreign_and_normalizes_size)
{
  Material &ma = *BKE_material_add(bmain, "SetValuesMat");
  MaterialPaintLayer *fill = mapping_fill_with_map(*bmain, ma, "ValuesFill");
  ASSERT_TRUE(BKE_paint_layers_projection_set_space(
      ma, fill, MA_PAINT_LAYER_MAPPING_SPACE_PLANAR));

  /* Size normalization: zero axis reads as 1.0. */
  const float center[3] = {0.0f, 0.0f, 0.0f};
  const float size[3] = {0.0f, -4.0f, 2.0f};
  const float normal[3] = {0.0f, 0.0f, 1.0f};
  ASSERT_TRUE(BKE_paint_layers_projection_set_values(ma, fill, center, size, normal, 0.0f));
  EXPECT_NEAR(fill->projection->size[0], 1.0f, 1e-6f);
  EXPECT_NEAR(fabsf(fill->projection->size[1]), 4.0f, 1e-3f);

  /* A layer not owned by ma returns false. */
  Material &other = *BKE_material_add(bmain, "OtherMat");
  EXPECT_FALSE(BKE_paint_layers_projection_set_values(
      other, fill, center, size, normal, 0.0f));
}

TEST_F(PaintLayersDescription, projection_set_rotation_wrapper)
{
  /* set_rotation(Euler) stores normal+roll; frame_euler round-trips to the same matrix. */
  Material &ma = *BKE_material_add(bmain, "SetRotWrapMat");
  MaterialPaintLayer *fill = mapping_fill_with_map(*bmain, ma, "RotWrapFill");
  ASSERT_TRUE(BKE_paint_layers_projection_set_space(
      ma, fill, MA_PAINT_LAYER_MAPPING_SPACE_PLANAR));

  const float euler[3] = {0.4f, -0.3f, 1.2f};
  ASSERT_TRUE(BKE_paint_layers_projection_set_rotation(ma, fill, euler));

  /* Compare matrices rather than angles (Euler is ambiguous). */
  float mat_orig[3][3];
  eul_to_mat3(mat_orig, euler);
  float mat_stored[3][3];
  BKE_paint_layers_projection_frame_matrix(
      fill->projection->normal, fill->projection->roll, mat_stored);
  for (const int i : IndexRange(3)) {
    for (const int j : IndexRange(3)) {
      EXPECT_NEAR(mat_orig[i][j], mat_stored[i][j], 1e-5f)
          << "(" << i << "," << j << ")";
    }
  }
}

TEST_F(PaintLayersDescription, projection_set_rotation_round_trips_near_the_poles)
{
  /* Euler triples whose frame +Z is on or next to the pole (the +/-0.999 reference switch of the
   * frame basis): the stored normal+roll must give back the same rotation. */
  Material &ma = *BKE_material_add(bmain, "PoleRotMat");
  MaterialPaintLayer *fill = mapping_fill_with_map(*bmain, ma, "PoleRotFill");
  ASSERT_TRUE(BKE_paint_layers_projection_set_space(
      ma, fill, MA_PAINT_LAYER_MAPPING_SPACE_PLANAR));

  const float eulers[][3] = {{0.0f, 0.0f, 0.0f},
                             {0.0f, 0.0f, 1.0f},
                             {0.03f, 0.02f, 0.5f},
                             {0.04f, -0.03f, -2.0f},
                             {float(M_PI), 0.0f, 0.3f},
                             {float(M_PI) - 0.03f, 0.02f, 0.3f}};
  for (const auto &euler : eulers) {
    ASSERT_TRUE(BKE_paint_layers_projection_set_rotation(ma, fill, euler));
    float expected[3][3];
    eul_to_mat3(expected, euler);
    float read_euler[3];
    float normal[3];
    BKE_paint_layers_projection_normal_get(*fill, normal);
    BKE_paint_layers_projection_frame_euler(normal, fill->projection->roll, read_euler);
    float actual[3][3];
    eul_to_mat3(actual, read_euler);
    for (const int i : IndexRange(3)) {
      for (const int j : IndexRange(3)) {
        EXPECT_NEAR(expected[i][j], actual[i][j], 1e-4f)
            << "euler " << euler[0] << " " << euler[1] << " " << euler[2] << " at (" << i << ","
            << j << ")";
      }
    }
  }
}

TEST_F(PaintLayersDescription, projection_normal_get_reads_zero_as_up)
{
  Material &ma = *BKE_material_add(bmain, "NormalGetMat");
  MaterialPaintLayer *fill = mapping_fill_with_map(*bmain, ma, "NormalGetFill");
  ASSERT_TRUE(BKE_paint_layers_projection_set_space(
      ma, fill, MA_PAINT_LAYER_MAPPING_SPACE_PLANAR));
  float normal[3];
  /* A payload that never went through a setter (a damaged file) may hold a zero or a long normal. */
  zero_v3(fill->projection->normal);
  BKE_paint_layers_projection_normal_get(*fill, normal);
  EXPECT_NEAR(normal[2], 1.0f, 1e-6f);
  fill->projection->normal[0] = 3.0f;
  BKE_paint_layers_projection_normal_get(*fill, normal);
  EXPECT_NEAR(len_v3(normal), 1.0f, 1e-6f);
  EXPECT_NEAR(normal[0], 1.0f, 1e-6f);
}

TEST_F(PaintLayersDescription, projection_cube_layout_unknown_value_reads_as_default)
{
  Material &ma = *BKE_material_add(bmain, "CubeLayoutMat");
  MaterialPaintLayer *fill = mapping_fill_with_map(*bmain, ma, "CubeLayoutFill");
  ASSERT_TRUE(BKE_paint_layers_projection_set_space(
      ma, fill, MA_PAINT_LAYER_MAPPING_SPACE_CUBE));
  EXPECT_TRUE(BKE_paint_layers_projection_set_cube_layout(
      ma, fill, MA_PAINT_LAYER_PROJECTION_CUBE_CROSS_4X3));
  EXPECT_FALSE(BKE_paint_layers_projection_set_cube_layout(
      ma, fill, MA_PAINT_LAYER_PROJECTION_CUBE_LAYOUT_NUM));
  EXPECT_FALSE(BKE_paint_layers_projection_set_cube_layout(ma, fill, -1));
  fill->projection->cube_layout = 99;
  EXPECT_EQ(BKE_paint_layers_projection_cube_layout_get(*fill),
            MA_PAINT_LAYER_PROJECTION_CUBE_CROSS_4X3);
  /* The table lookup agrees: an unknown layout is the first row. */
  EXPECT_EQ(&BKE_paint_layers_projection_cube_layout_info(99),
            &BKE_paint_layers_projection_cube_layout_info(MA_PAINT_LAYER_PROJECTION_CUBE_CROSS_4X3));
}

TEST_F(PaintLayersDescription, projection_preset_decal_resets_angle_and_flat_keeps_it)
{
  Material &ma = *BKE_material_add(bmain, "PresetAngleMat");
  MaterialPaintLayer *fill = mapping_fill_with_map(*bmain, ma, "PresetAngleFill");
  ASSERT_TRUE(BKE_paint_layers_projection_set_space(
      ma, fill, MA_PAINT_LAYER_MAPPING_SPACE_PLANAR));
  ASSERT_TRUE(BKE_paint_layers_projection_set_angle(ma, fill, 45.0f));
  /* The angle is not part of what the preset getter reads. */
  EXPECT_EQ(BKE_paint_layers_projection_preset_get(*fill), PAINT_LAYER_PROJ_PRESET_DECAL);
  ASSERT_TRUE(BKE_paint_layers_projection_apply_preset(ma, fill, PAINT_LAYER_PROJ_PRESET_FLAT));
  EXPECT_EQ(BKE_paint_layers_projection_preset_get(*fill), PAINT_LAYER_PROJ_PRESET_FLAT);
  EXPECT_NEAR(BKE_paint_layers_projection_angle_get(*fill), 45.0f, 1e-5f);
  ASSERT_TRUE(BKE_paint_layers_projection_apply_preset(ma, fill, PAINT_LAYER_PROJ_PRESET_DECAL));
  EXPECT_EQ(BKE_paint_layers_projection_preset_get(*fill), PAINT_LAYER_PROJ_PRESET_DECAL);
  EXPECT_NEAR(BKE_paint_layers_projection_angle_get(*fill),
              PAINT_LAYER_PROJECTION_ANGLE_DEFAULT,
              1e-5f);
}

TEST_F(PaintLayersDescription, projection_ensure_defaults)
{
  Material &ma = *BKE_material_add(bmain, "EnsureDefaultsMat");
  MaterialPaintLayer *fill = mapping_fill_with_map(*bmain, ma, "DefaultsFill");
  ASSERT_TRUE(BKE_paint_layers_projection_set_space(
      ma, fill, MA_PAINT_LAYER_MAPPING_SPACE_PLANAR));
  ASSERT_NE(fill->projection, nullptr);
  EXPECT_NEAR(fill->projection->normal[0], 0.0f, 1e-6f);
  EXPECT_NEAR(fill->projection->normal[1], 0.0f, 1e-6f);
  EXPECT_NEAR(fill->projection->normal[2], 1.0f, 1e-6f);
  EXPECT_NEAR(fill->projection->roll, 0.0f, 1e-6f);
  /* frame_matrix(0,0,1; 0) gives the identity. */
  float mat[3][3];
  BKE_paint_layers_projection_frame_matrix(fill->projection->normal, fill->projection->roll, mat);
  for (const int i : IndexRange(3)) {
    for (const int j : IndexRange(3)) {
      EXPECT_NEAR(mat[i][j], (i == j) ? 1.0f : 0.0f, 1e-5f);
    }
  }
}

TEST_F(PaintLayersDescription, projection_normal_roll_blend_round_trip)
{
  /* Write a row with normal=(1,0,0) roll=0.7, save/read .blend, verify values. */
  Material *ma = BKE_material_add(bmain, "NormalRollRoundTripMat");
  ma->paint_layers_flag = MA_PAINT_LAYERED;
  id_us_plus(&ma->id);
  Image *image = static_cast<Image *>(BKE_id_new(bmain, ID_IM, "NormalRollRTImg"));
  id_us_plus(&image->id);
  MaterialPaintLayer *layer = paint_layer_add(*ma, "NRRTRow");
  ASSERT_TRUE(BKE_paint_layers_set_source(*ma, layer, MA_PAINT_LAYER_SOURCE_CONSTANT));
  paint_layer_channel_add(*layer, *image);
  ASSERT_TRUE(BKE_paint_layers_projection_set_space(
      *ma, layer, MA_PAINT_LAYER_MAPPING_SPACE_PLANAR));

  const float center[3] = {0.5f, -1.0f, 2.0f};
  const float size[3] = {2.0f, 3.0f, 4.0f};
  const float normal[3] = {1.0f, 0.0f, 0.0f};
  const float roll = 0.7f;
  ASSERT_TRUE(BKE_paint_layers_projection_set_values(*ma, layer, center, size, normal, roll));

  char filepath[FILE_MAX];
  BLI_path_join(filepath, sizeof(filepath), BKE_tempdir_session(),
                "normal_roll_round_trip.blend");
  BlendFileWriteParams write_params{};
  ASSERT_TRUE(BLO_write_file(bmain, filepath, 0, &write_params, nullptr));

  BlendFileReadReport read_report{};
  BlendFileData *bfd = BLO_read_from_file(filepath, BLO_READ_SKIP_NONE, &read_report);
  ASSERT_NE(bfd, nullptr);
  Material *reloaded = nullptr;
  for (Material &candidate : bfd->main->materials) {
    if (STREQ(candidate.id.name + 2, "NormalRollRoundTripMat")) {
      reloaded = &candidate;
      break;
    }
  }
  ASSERT_NE(reloaded, nullptr);
  MaterialPaintLayer *rl = paint_layers_first(*reloaded);
  ASSERT_NE(rl, nullptr);
  ASSERT_NE(rl->projection, nullptr);
  EXPECT_NEAR(rl->projection->normal[0], 1.0f, 1e-6f);
  EXPECT_NEAR(rl->projection->normal[1], 0.0f, 1e-6f);
  EXPECT_NEAR(rl->projection->normal[2], 0.0f, 1e-6f);
  EXPECT_NEAR(rl->projection->roll, 0.7f, 1e-6f);
  EXPECT_NEAR(rl->projection->center[0], 0.5f, 1e-6f);
  BLO_blendfiledata_free(bfd);
}

TEST_F(PaintLayersDescription, projection_preset_decal_survives_snap_and_size_from_controller)
{
  Material &ma = *BKE_material_add(bmain, "PresetSnapMat");
  MaterialPaintLayer *fill = mapping_fill_with_map(*bmain, ma, "PresetSnapFill");
  ASSERT_TRUE(BKE_paint_layers_projection_set_space(
      ma, fill, MA_PAINT_LAYER_MAPPING_SPACE_PLANAR));
  ASSERT_TRUE(BKE_paint_layers_projection_apply_preset(ma, fill, PAINT_LAYER_PROJ_PRESET_DECAL));
  EXPECT_EQ(BKE_paint_layers_projection_preset_get(*fill), PAINT_LAYER_PROJ_PRESET_DECAL);

  /* Turning on SNAP and SIZE_FROM_CONTROLLER does not change the preset to CUSTOM. */
  ASSERT_TRUE(BKE_paint_layers_projection_set_snap(ma, fill, true));
  ASSERT_TRUE(BKE_paint_layers_projection_set_size_from_controller(ma, fill, true));
  EXPECT_EQ(BKE_paint_layers_projection_preset_get(*fill), PAINT_LAYER_PROJ_PRESET_DECAL);
}

TEST_F(PaintLayersDescription, projection_preset_flat_turns_off_four_flags)
{
  Material &ma = *BKE_material_add(bmain, "PresetFlatMat");
  MaterialPaintLayer *fill = mapping_fill_with_map(*bmain, ma, "PresetFlatFill");
  ASSERT_TRUE(BKE_paint_layers_projection_set_space(
      ma, fill, MA_PAINT_LAYER_MAPPING_SPACE_PLANAR));
  /* Start from Decal (all 4 on) and switch to Flat. */
  ASSERT_TRUE(BKE_paint_layers_projection_apply_preset(ma, fill, PAINT_LAYER_PROJ_PRESET_DECAL));
  ASSERT_TRUE(BKE_paint_layers_projection_set_facing_invert(ma, fill, true));
  ASSERT_TRUE(BKE_paint_layers_projection_apply_preset(ma, fill, PAINT_LAYER_PROJ_PRESET_FLAT));
  EXPECT_FALSE(BKE_paint_layers_projection_clip_get(*fill));
  EXPECT_FALSE(BKE_paint_layers_projection_depth_get(*fill));
  EXPECT_FALSE(BKE_paint_layers_projection_fit_aspect_get(*fill));
  EXPECT_FALSE(BKE_paint_layers_projection_facing_get(*fill));
  EXPECT_FALSE(BKE_paint_layers_projection_facing_invert_get(*fill));
  EXPECT_EQ(BKE_paint_layers_projection_preset_get(*fill), PAINT_LAYER_PROJ_PRESET_FLAT);
}

TEST_F(PaintLayersDescription, projection_preset_decal_turns_on_four_flags)
{
  Material &ma = *BKE_material_add(bmain, "PresetDecalMat");
  MaterialPaintLayer *fill = mapping_fill_with_map(*bmain, ma, "PresetDecalFill");
  ASSERT_TRUE(BKE_paint_layers_projection_set_space(
      ma, fill, MA_PAINT_LAYER_MAPPING_SPACE_PLANAR));
  ASSERT_TRUE(BKE_paint_layers_projection_apply_preset(ma, fill, PAINT_LAYER_PROJ_PRESET_FLAT));
  ASSERT_TRUE(BKE_paint_layers_projection_apply_preset(ma, fill, PAINT_LAYER_PROJ_PRESET_DECAL));
  EXPECT_TRUE(BKE_paint_layers_projection_clip_get(*fill));
  EXPECT_TRUE(BKE_paint_layers_projection_depth_get(*fill));
  EXPECT_TRUE(BKE_paint_layers_projection_fit_aspect_get(*fill));
  EXPECT_TRUE(BKE_paint_layers_projection_facing_get(*fill));
  EXPECT_FALSE(BKE_paint_layers_projection_facing_invert_get(*fill));
  EXPECT_NEAR(BKE_paint_layers_projection_angle_get(*fill),
              PAINT_LAYER_PROJECTION_ANGLE_DEFAULT, 1e-5f);
}

TEST_F(PaintLayersDescription, projection_preset_custom_on_manual_change)
{
  Material &ma = *BKE_material_add(bmain, "PresetCustomMat");
  MaterialPaintLayer *fill = mapping_fill_with_map(*bmain, ma, "PresetCustomFill");
  ASSERT_TRUE(BKE_paint_layers_projection_set_space(
      ma, fill, MA_PAINT_LAYER_MAPPING_SPACE_PLANAR));
  ASSERT_TRUE(BKE_paint_layers_projection_apply_preset(ma, fill, PAINT_LAYER_PROJ_PRESET_DECAL));
  /* Manually turn off CLIP -> no longer Decal or Flat. */
  ASSERT_TRUE(BKE_paint_layers_projection_set_clip(ma, fill, false));
  EXPECT_EQ(BKE_paint_layers_projection_preset_get(*fill), PAINT_LAYER_PROJ_PRESET_CUSTOM);
  /* apply_preset(CUSTOM) does not change anything. */
  const int16_t before = fill->projection->flag;
  ASSERT_TRUE(BKE_paint_layers_projection_apply_preset(ma, fill, PAINT_LAYER_PROJ_PRESET_CUSTOM));
  EXPECT_EQ(fill->projection->flag, before);
}

TEST_F(PaintLayersDescription, projection_orientation_from_surface)
{
  /* ±X, ±Y, ±Z, diagonal surface normals → orientation_from_surface gives r_normal = +surface,
   * roll = 0. frame_matrix(r_normal, 0) is orthonormal with det=+1. */
  const float normals[][3] = {
      {1.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
      {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f},
      {0.577f, 0.577f, 0.577f}};
  for (const auto &sn : normals) {
    float r_normal[3];
    float r_roll;
    BKE_paint_layers_projection_orientation_from_surface(sn, r_normal, &r_roll);
    /* r_normal = normalize(surface_normal). */
    float expected[3];
    copy_v3_v3(expected, sn);
    normalize_v3(expected);
    for (const int i : IndexRange(3)) {
      EXPECT_NEAR(r_normal[i], expected[i], 1e-5f);
    }
    EXPECT_NEAR(r_roll, 0.0f, 1e-6f);
    /* Orthonormality of frame_matrix. */
    float mat[3][3];
    BKE_paint_layers_projection_frame_matrix(r_normal, r_roll, mat);
    float cr[3];
    cross_v3_v3v3(cr, mat[0], mat[1]);
    EXPECT_NEAR(dot_v3v3(cr, mat[2]), 1.0f, 1e-5f) << "det not +1";
  }
}

TEST_F(PaintLayersDescription, projection_orientation_from_surface_wall_x_axis_right)
{
  /* A vertical wall with surface_normal = +X: observer at +X looking in -X direction.
   * Frame X should be to the observer's right = +Y world (no mirror). */
  const float sn[3] = {1.0f, 0.0f, 0.0f};
  float r_normal[3];
  float r_roll;
  BKE_paint_layers_projection_orientation_from_surface(sn, r_normal, &r_roll);
  float mat[3][3];
  BKE_paint_layers_projection_frame_matrix(r_normal, r_roll, mat);
  /* `mat[0]` = frame X axis in parent space. */
  const float *frame_x = mat[0];
  /* Observer right = +Y for wall at +X. */
  EXPECT_NEAR(frame_x[0], 0.0f, 1e-5f);
  EXPECT_NEAR(frame_x[1], 1.0f, 1e-5f) << "frame X should be +Y (not mirrored)";
  EXPECT_NEAR(frame_x[2], 0.0f, 1e-5f);
}

TEST_F(PaintLayersDescription, projection_facing_weight_positive_normal_toward_z)
{
  /* A face whose normal equals the frame +Z (dot = +1) must have weight = 1 without Flip.
   * frame_Z = normalize(surface_normal) from orientation_from_surface. */
  const float surface_normal[3] = {0.0f, 0.0f, 1.0f};
  float r_normal[3];
  float r_roll;
  BKE_paint_layers_projection_orientation_from_surface(surface_normal, r_normal, &r_roll);
  float scale, offset;
  BKE_paint_layers_projection_facing_weights(80.0f, false, scale, offset);
  /* component = dot(face_world_normal, frame_Z) = dot(surface_normal, r_normal) ≈ 1. */
  const float component = dot_v3v3(surface_normal, r_normal);
  const float weight = BKE_paint_layers_projection_facing_weight(scale, offset, component);
  EXPECT_NEAR(weight, 1.0f, 1e-4f) << "face looking at +Z should have weight 1";
}

TEST_F(PaintLayersDescription, projection_flip_column_signs)
{
  /* Flip: negate columns X and Z of frame_matrix(n, roll), keep Y. Then
   * frame_from_matrix gives the new normal/roll. Verify column properties. */
  const float n[3] = {0.6f, 0.0f, 0.8f};
  const float roll = 0.4f;
  float mat_orig[3][3];
  BKE_paint_layers_projection_frame_matrix(n, roll, mat_orig);
  /* Build flipped: negate axes 0 and 2. */
  float mat_flip[3][3];
  copy_m3_m3(mat_flip, mat_orig);
  negate_v3(mat_flip[0]);
  negate_v3(mat_flip[2]);
  float flipped_n[3], flipped_roll;
  BKE_paint_layers_projection_frame_from_matrix(mat_flip, flipped_n, &flipped_roll);
  /* Check: new normal = -original normal (column 2 negated). */
  for (const int i : IndexRange(3)) {
    EXPECT_NEAR(flipped_n[i], -n[i] / len_v3(n), 1e-5f) << "Z column sign";
  }
  /* Check: column 1 (Y) unchanged: frame_matrix(flipped_n, flipped_roll)[col1] == mat_orig[col1]. */
  float mat_verify[3][3];
  BKE_paint_layers_projection_frame_matrix(flipped_n, flipped_roll, mat_verify);
  for (const int i : IndexRange(3)) {
    EXPECT_NEAR(mat_verify[1][i], mat_orig[1][i], 1e-5f) << "Y axis preserved";
    EXPECT_NEAR(mat_verify[0][i], -mat_orig[0][i], 1e-5f) << "X axis negated";
    EXPECT_NEAR(mat_verify[2][i], -mat_orig[2][i], 1e-5f) << "Z axis negated";
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Graph ↔ CPU parity of the projection modes
 * \{ */

/**
 * Every mapping space, read on both sides the way the product reads it: the generated graph at a
 * reference texel through the interpreter, the same row baked through the CPU composite into the
 * painted object's UV space. The flat grid -- object XY equal to its render UV, normal +Z -- makes
 * the interpreter's texel-centre object position the point the CPU bake samples, so a projected
 * lookup has to land on the same map texel.
 */
TEST_F(PaintLayersGraphEvalTest, every_projection_space_matches_the_cpu)
{
  Object *ob = BKE_object_add_only_object(bmain, OB_MESH, "ProjectionParityObject");
  Mesh *mesh = BKE_mesh_add(bmain, "ProjectionParityMesh");
  ob->data = id_cast<ID *>(mesh);
  BKE_object_material_assign(bmain, ob, ma, 1, BKE_MAT_ASSIGN_OBDATA);
  /* A quad spanning [0, 1]² at z = 0, its render UV equal to its XY. The edges are derived from
   * its face, since the projected render walks corner triangles and needs the corner/edge
   * topology to be consistent. */
  Mesh *source = BKE_mesh_new_nomain(4, 0, 1, 4);
  MutableSpan<float3> positions = source->vert_positions_for_write();
  positions[0] = float3(0.0f, 0.0f, 0.0f);
  positions[1] = float3(1.0f, 0.0f, 0.0f);
  positions[2] = float3(1.0f, 1.0f, 0.0f);
  positions[3] = float3(0.0f, 1.0f, 0.0f);
  MutableSpan<int> face_offsets = source->face_offsets_for_write();
  face_offsets[0] = 0;
  face_offsets[1] = 4;
  MutableSpan<int> corner_verts = source->corner_verts_for_write();
  corner_verts[0] = 0;
  corner_verts[1] = 1;
  corner_verts[2] = 2;
  corner_verts[3] = 3;
  bke::SpanAttributeWriter<float2> uvs =
      source->attributes_for_write().lookup_or_add_for_write_span<float2>("UVMap",
                                                                          bke::AttrDomain::Corner);
  uvs.span[0] = float2(0.0f, 0.0f);
  uvs.span[1] = float2(1.0f, 0.0f);
  uvs.span[2] = float2(1.0f, 1.0f);
  uvs.span[3] = float2(0.0f, 1.0f);
  uvs.finish();
  bke::mesh_calc_edges(*source, false, false);
  source->tag_topology_changed();
  source->tag_positions_changed();
  BKE_mesh_nomain_to_mesh(source, mesh, ob);

  /* Four distinct quadrants, so every mode's lookup lands somewhere visible. */
  Image *map = add_solid_image("ProjectionParityMap", 255, 0, 0, 255);
  {
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(map, nullptr, &lock);
    ASSERT_NE(ibuf, nullptr);
    uchar *pixels = ibuf->byte_data_for_write();
    const uchar red[4] = {255, 0, 0, 255};
    const uchar green[4] = {0, 255, 0, 255};
    const uchar blue[4] = {0, 0, 255, 255};
    const uchar white[4] = {255, 255, 255, 255};
    for (const int y : IndexRange(4)) {
      for (const int x : IndexRange(4)) {
        const uchar *rgba = (x < 2) ? ((y < 2) ? red : green) : ((y < 2) ? blue : white);
        uchar *pixel = pixels + (int64_t(y) * 4 + x) * 4;
        for (const int k : IndexRange(4)) {
          pixel[k] = rgba[k];
        }
      }
    }
    BKE_image_release_ibuf(map, ibuf, lock);
  }
  add_layer("Bottom", add_solid_image("Bottom", 200, 100, 50, 255));
  MaterialPaintLayer *top = add_layer("Top", map);
  /* A UV gradient mask on the projected row: the graph feeds the line the row's projected UV read
   * raw (no frame factor, no tiling), and the CPU renders the same line through the same frame.
   * The parity of the two is what pins that behaviour for every space. */
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, top, 1.0f), nullptr);
  MaterialPaintLayer *item = BKE_paint_layers_mask_item_add(
      *ma, top, MA_PAINT_LAYER_SOURCE_GENERATOR, "Parity Gradient", 1.0f);
  ASSERT_NE(item, nullptr);
  ASSERT_TRUE(BKE_paint_layers_gradient_set_space(*ma, *item, MA_PAINT_LAYER_GRADIENT_SPACE_UV));

  const int16_t spaces[6] = {MA_PAINT_LAYER_MAPPING_SPACE_PLANAR,
                             MA_PAINT_LAYER_MAPPING_SPACE_BOX,
                             MA_PAINT_LAYER_MAPPING_SPACE_TRIPLANAR,
                             MA_PAINT_LAYER_MAPPING_SPACE_CUBE,
                             MA_PAINT_LAYER_MAPPING_SPACE_CYLINDER,
                             MA_PAINT_LAYER_MAPPING_SPACE_SPHERE};
  for (const int16_t space : spaces) {
    ASSERT_TRUE(BKE_paint_layers_projection_set_space(*ma, top, space));
    /* Explicit frame values, so both sides read the same numbers: the frame spans [-1, 1]² in
     * object space, which keeps every reference texel inside its smooth edge. */
    const float center[3] = {0.0f, 0.0f, 0.0f};
    const float size[3] = {2.0f, 2.0f, 2.0f};
    const float rotation[3] = {0.0f, 0.0f, 0.0f};
    ASSERT_TRUE(BKE_paint_layers_projection_set_values(*ma, top, center, size, rotation));

    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
    Vector<PaintMaterialCompositeImageLayer> layers;
    ASSERT_TRUE(BKE_paint_layers_composite_image_layers(
        *ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers, false, nullptr, nullptr, 1001, ob));
    int width = 0, height = 0;
    ASSERT_TRUE(BKE_paint_material_composite_stack_dimensions(layers, width, height));
    ASSERT_EQ(width, 4) << "space " << space;
    ASSERT_EQ(height, 4) << "space " << space;
    Vector<float> linear(int64_t(width) * height * 4);
    float bottom[4];
    BKE_paint_layers_channel_bottom_color(PAINT_MATERIAL_CHANNEL_BASE_COLOR, bottom);
    ASSERT_TRUE(BKE_paint_material_composite_eval_images_linear(
        layers,
        PaintMaterialCompositeLinearTarget::frame(linear, width),
        nullptr,
        nullptr,
        bottom));

    for (const int y : IndexRange(4)) {
      for (const int x : IndexRange(4)) {
        GraphInterpreter interpreter;
        interpreter.tree = ma->paint_layers_tree;
        interpreter.x = x;
        interpreter.y = y;
        interpreter.object_mask_value = object_mask_value;
        const RGBA graph = interpreter.eval_result(
            result_name(PAINT_MATERIAL_CHANNEL_BASE_COLOR));
        const float *cpu = linear.data() + (int64_t(y) * width + x) * 4;
        EXPECT_NEAR(graph.r, cpu[0], 2e-3f)
            << "space " << space << " texel " << x << "," << y << " R";
        EXPECT_NEAR(graph.g, cpu[1], 2e-3f)
            << "space " << space << " texel " << x << "," << y << " G";
        EXPECT_NEAR(graph.b, cpu[2], 2e-3f)
            << "space " << space << " texel " << x << "," << y << " B";
      }
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Surface map
 * \{ */

TEST_F(PaintLayersGraphEvalTest, surface_map_matches_the_rasterizer_and_caches_by_content)
{
  Object *ob = BKE_object_add_only_object(bmain, OB_MESH, "SurfaceMapObject");
  Mesh *mesh = BKE_mesh_add(bmain, "SurfaceMapMesh");
  ob->data = id_cast<ID *>(mesh);
  BKE_object_material_assign(bmain, ob, ma, 1, BKE_MAT_ASSIGN_OBDATA);
  /* A quad over the lower-left quarter of the UV tile, lifted to z = 2 and scaled to 4 in XY, so a
   * texel's position differs from its UV. */
  Mesh *source = BKE_mesh_new_nomain(4, 0, 1, 4);
  MutableSpan<float3> positions = source->vert_positions_for_write();
  positions[0] = float3(0.0f, 0.0f, 2.0f);
  positions[1] = float3(4.0f, 0.0f, 2.0f);
  positions[2] = float3(4.0f, 4.0f, 2.0f);
  positions[3] = float3(0.0f, 4.0f, 2.0f);
  MutableSpan<int> face_offsets = source->face_offsets_for_write();
  face_offsets[0] = 0;
  face_offsets[1] = 4;
  MutableSpan<int> corner_verts = source->corner_verts_for_write();
  for (const int i : IndexRange(4)) {
    corner_verts[i] = i;
  }
  bke::SpanAttributeWriter<float2> uvs =
      source->attributes_for_write().lookup_or_add_for_write_span<float2>("UVMap",
                                                                          bke::AttrDomain::Corner);
  uvs.span[0] = float2(0.0f, 0.0f);
  uvs.span[1] = float2(0.5f, 0.0f);
  uvs.span[2] = float2(0.5f, 0.5f);
  uvs.span[3] = float2(0.0f, 0.5f);
  uvs.finish();
  bke::mesh_calc_edges(*source, false, false);
  source->tag_topology_changed();
  source->tag_positions_changed();
  BKE_mesh_nomain_to_mesh(source, mesh, ob);

  BKE_paint_material_surface_map_free();
  const int size = 16;
  const std::shared_ptr<const PaintMaterialSurfaceMap> map = BKE_paint_material_surface_map_get(
      ob, ma, size, size, 1001);
  ASSERT_NE(map, nullptr);
  ASSERT_EQ(map->position.size(), size * size);
  ASSERT_EQ(map->normal.size(), size * size);

  /* Texel (3, 2) lies inside the quarter: UV centre (3.5/16, 2.5/16) maps to XY = UV * 8. */
  const float4 p = map->position[2 * size + 3];
  const float4 n = map->normal[2 * size + 3];
  EXPECT_NEAR(p.x, 3.5f / 16.0f * 8.0f, 1e-4f);
  EXPECT_NEAR(p.y, 2.5f / 16.0f * 8.0f, 1e-4f);
  EXPECT_NEAR(p.z, 2.0f, 1e-4f);
  EXPECT_EQ(p.w, 1.0f);
  EXPECT_NEAR(n.z, 1.0f, 1e-4f);
  /* A texel far outside the island, beyond any padding, stays empty. */
  EXPECT_EQ(map->position[(size - 1) * size + (size - 1)].w, 0.0f);

  /* The same key answers from the cache; a moved vertex (same topology) is a new map. */
  EXPECT_EQ(BKE_paint_material_surface_map_get(ob, ma, size, size, 1001), map);
  mesh->vert_positions_for_write()[2].z = 3.0f;
  mesh->tag_positions_changed();
  const std::shared_ptr<const PaintMaterialSurfaceMap> moved = BKE_paint_material_surface_map_get(
      ob, ma, size, size, 1001);
  ASSERT_NE(moved, nullptr);
  EXPECT_NE(moved, map);
  EXPECT_NE(moved->key.content_hash, map->key.content_hash);

  /* Freeing drops the entry but not a holder's map. */
  BKE_paint_material_surface_map_free();
  EXPECT_EQ(moved->width, size);
  EXPECT_NE(BKE_paint_material_surface_map_get(ob, ma, size, size, 1001), moved);
  BKE_paint_material_surface_map_free();
}

TEST_F(PaintLayersGraphEvalTest, surface_map_releases_texels_and_rebuilds_on_demand_and_pads_islands)
{
  Object *ob = BKE_object_add_only_object(bmain, OB_MESH, "SurfaceReleaseObject");
  Mesh *mesh = BKE_mesh_add(bmain, "SurfaceReleaseMesh");
  ob->data = id_cast<ID *>(mesh);
  BKE_object_material_assign(bmain, ob, ma, 1, BKE_MAT_ASSIGN_OBDATA);
  /* The same quad over the lower-left quarter of the UV tile as the test above. */
  Mesh *source = BKE_mesh_new_nomain(4, 0, 1, 4);
  MutableSpan<float3> positions = source->vert_positions_for_write();
  positions[0] = float3(0.0f, 0.0f, 2.0f);
  positions[1] = float3(4.0f, 0.0f, 2.0f);
  positions[2] = float3(4.0f, 4.0f, 2.0f);
  positions[3] = float3(0.0f, 4.0f, 2.0f);
  MutableSpan<int> face_offsets = source->face_offsets_for_write();
  face_offsets[0] = 0;
  face_offsets[1] = 4;
  MutableSpan<int> corner_verts = source->corner_verts_for_write();
  for (const int i : IndexRange(4)) {
    corner_verts[i] = i;
  }
  bke::SpanAttributeWriter<float2> uvs =
      source->attributes_for_write().lookup_or_add_for_write_span<float2>("UVMap",
                                                                          bke::AttrDomain::Corner);
  uvs.span[0] = float2(0.0f, 0.0f);
  uvs.span[1] = float2(0.5f, 0.0f);
  uvs.span[2] = float2(0.5f, 0.5f);
  uvs.span[3] = float2(0.0f, 0.5f);
  uvs.finish();
  bke::mesh_calc_edges(*source, false, false);
  source->tag_topology_changed();
  source->tag_positions_changed();
  BKE_mesh_nomain_to_mesh(source, mesh, ob);

  BKE_paint_material_surface_map_free();
  const int size = 16;
  const std::shared_ptr<const PaintMaterialSurfaceMap> map = BKE_paint_material_surface_map_get(
      ob, ma, size, size, 1001);
  ASSERT_NE(map, nullptr);
  ASSERT_EQ(map->position.size(), size * size);

  /* Texel (8, 2) lies just outside the island (its centre is at 8.5 / 16 > 0.5): the padding owns
   * it and copies the position and normal of the island texel next to it. */
  const float4 padded_p = map->position[2 * size + 8];
  const float4 padded_n = map->normal[2 * size + 8];
  const float4 edge_p = map->position[2 * size + 7];
  const float4 edge_n = map->normal[2 * size + 7];
  EXPECT_EQ(edge_p.w, 1.0f);
  EXPECT_EQ(padded_p.w, 1.0f);
  EXPECT_NEAR(padded_p.x, edge_p.x, 1e-4f);
  EXPECT_NEAR(padded_p.y, edge_p.y, 1e-4f);
  EXPECT_NEAR(padded_p.z, edge_p.z, 1e-4f);
  EXPECT_NEAR(padded_n.z, edge_n.z, 1e-4f);

  /* Releasing the texels keeps the entry: a caller that does not need them gets it back as it is,
   * and one that does need them gets a rebuilt map. */
  BKE_paint_material_surface_map_texels_release(map);
  EXPECT_TRUE(map->texels_released);
  const std::shared_ptr<const PaintMaterialSurfaceMap> identity =
      BKE_paint_material_surface_map_get(ob, ma, size, size, 1001, /*need_texels=*/false);
  EXPECT_EQ(identity, map);
  const std::shared_ptr<const PaintMaterialSurfaceMap> rebuilt =
      BKE_paint_material_surface_map_get(ob, ma, size, size, 1001, /*need_texels=*/true);
  ASSERT_NE(rebuilt, nullptr);
  EXPECT_NE(rebuilt, map);
  EXPECT_FALSE(rebuilt->texels_released);
  EXPECT_EQ(rebuilt->position.size(), size * size);
  EXPECT_EQ(rebuilt->key.content_hash, map->key.content_hash);
  BKE_paint_material_surface_map_free();
}

TEST_F(PaintLayersGraphEvalTest, surface_map_keeps_entries_of_alternating_targets)
{
  Object *ob = BKE_object_add_only_object(bmain, OB_MESH, "SurfaceAlternateObject");
  Mesh *mesh = BKE_mesh_add(bmain, "SurfaceAlternateMesh");
  ob->data = id_cast<ID *>(mesh);
  BKE_object_material_assign(bmain, ob, ma, 1, BKE_MAT_ASSIGN_OBDATA);
  /* The same quad over the lower-left quarter of the UV tile as the tests above. */
  Mesh *source = BKE_mesh_new_nomain(4, 0, 1, 4);
  MutableSpan<float3> positions = source->vert_positions_for_write();
  positions[0] = float3(0.0f, 0.0f, 2.0f);
  positions[1] = float3(4.0f, 0.0f, 2.0f);
  positions[2] = float3(4.0f, 4.0f, 2.0f);
  positions[3] = float3(0.0f, 4.0f, 2.0f);
  MutableSpan<int> face_offsets = source->face_offsets_for_write();
  face_offsets[0] = 0;
  face_offsets[1] = 4;
  MutableSpan<int> corner_verts = source->corner_verts_for_write();
  for (const int i : IndexRange(4)) {
    corner_verts[i] = i;
  }
  bke::SpanAttributeWriter<float2> uvs =
      source->attributes_for_write().lookup_or_add_for_write_span<float2>("UVMap",
                                                                          bke::AttrDomain::Corner);
  uvs.span[0] = float2(0.0f, 0.0f);
  uvs.span[1] = float2(0.5f, 0.0f);
  uvs.span[2] = float2(0.5f, 0.5f);
  uvs.span[3] = float2(0.0f, 0.5f);
  uvs.finish();
  bke::mesh_calc_edges(*source, false, false);
  source->tag_topology_changed();
  source->tag_positions_changed();
  BKE_mesh_nomain_to_mesh(source, mesh, ob);

  BKE_paint_material_surface_map_free();
  /* Two targets of one object: the same tile at two canvas sizes. The store keeps an entry per
   * target, so alternating between them answers with the same instance instead of a rebuild --
   * what lets the GPU cache hold several maps and reuse their textures. */
  const int size = 16;
  const std::shared_ptr<const PaintMaterialSurfaceMap> first = BKE_paint_material_surface_map_get(
      ob, ma, size, size, 1001);
  ASSERT_NE(first, nullptr);
  const std::shared_ptr<const PaintMaterialSurfaceMap> second = BKE_paint_material_surface_map_get(
      ob, ma, size * 2, size * 2, 1001);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(BKE_paint_material_surface_map_get(ob, ma, size, size, 1001), first);

  /* Releasing the texels finds an entry that is not the last inserted one. */
  BKE_paint_material_surface_map_texels_release(second);
  EXPECT_TRUE(second->texels_released);
  EXPECT_TRUE(second->position.is_empty());

  /* A rebuilt map of one target replaces its stale instance: the release of the dropped map is a
   * no-op that leaves its arrays alone, and the identity moves to the new map. The content hash is
   * the mesh's, so the other target's entry is stale by it too and rebuilds at its next ask. */
  mesh->vert_positions_for_write()[2].z = 3.0f;
  mesh->tag_positions_changed();
  const std::shared_ptr<const PaintMaterialSurfaceMap> moved = BKE_paint_material_surface_map_get(
      ob, ma, size, size, 1001);
  ASSERT_NE(moved, nullptr);
  EXPECT_NE(moved, first);
  EXPECT_NE(moved->key.content_hash, first->key.content_hash);
  BKE_paint_material_surface_map_texels_release(first);
  EXPECT_FALSE(first->texels_released);
  EXPECT_EQ(first->position.size(), size * size);
  EXPECT_EQ(BKE_paint_material_surface_map_get(ob, ma, size, size, 1001, /*need_texels=*/false),
            moved);

  BKE_paint_material_surface_map_free();
  /* Freeing drops the entries but not a holder's map. */
  EXPECT_EQ(first->width, size);
  const std::shared_ptr<const PaintMaterialSurfaceMap> fresh = BKE_paint_material_surface_map_get(
      ob, ma, size, size, 1001);
  ASSERT_NE(fresh, nullptr);
  EXPECT_NE(fresh, moved);
  BKE_paint_material_surface_map_free();
}

TEST_F(PaintLayersGraphEvalTest, projected_effect_input_hash_follows_uv_adjust_and_frame)
{
  PaintMaterialCompositeImageLayer layer;
  layer.mapping.projection.present = true;
  const uint64_t base = BKE_paint_layers_effect_input_scalar_hash(layer);
  EXPECT_EQ(BKE_paint_layers_effect_input_scalar_hash(layer), base);

  PaintMaterialCompositeImageLayer frame_moved = layer;
  frame_moved.mapping.projection.center[0] = 0.25f;
  EXPECT_NE(BKE_paint_layers_effect_input_scalar_hash(frame_moved), base);

  PaintMaterialCompositeImageLayer offset_moved = layer;
  offset_moved.mapping.enabled = true;
  const uint64_t adjust_on = BKE_paint_layers_effect_input_scalar_hash(offset_moved);
  EXPECT_NE(adjust_on, base);
  offset_moved.mapping.offset[0] = 0.1f;
  const uint64_t offset_hash = BKE_paint_layers_effect_input_scalar_hash(offset_moved);
  EXPECT_NE(offset_hash, adjust_on);
  offset_moved.mapping.rotation = 0.5f;
  EXPECT_NE(BKE_paint_layers_effect_input_scalar_hash(offset_moved), offset_hash);
}

TEST_F(PaintLayersGraphEvalTest, gpu_unsupported_reason_for_projected_rows)
{
  const uchar rgba[4] = {255, 0, 0, 255};
  Image *image = add_image("ProjectedMap", 4, rgba);
  PaintMaterialCompositeImageLayer layer;
  layer.enabled = true;
  layer.opacity = 1.0f;
  layer.mapping.projection.present = true;

  /* A projected row without a map is not laid. */
  EXPECT_NE(BKE_paint_layers_gpu_unsupported_reason(layer), nullptr);

  /* A plain projected map row is. */
  layer.color_image = image;
  EXPECT_EQ(BKE_paint_layers_gpu_unsupported_reason(layer), nullptr);

  /* A tiled map is not. */
  image->source = IMA_SRC_TILED;
  EXPECT_NE(BKE_paint_layers_gpu_unsupported_reason(layer), nullptr);
}

/** \} */

}  // namespace blender::bke::tests
