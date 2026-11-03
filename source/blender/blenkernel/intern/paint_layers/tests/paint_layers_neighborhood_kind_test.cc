/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * The parameters and the RNA of the neighborhood effects (Sharpen, High Pass, Edge Detect,
 * Emboss, Erode / Dilate): their schema, their roles and the enum name each reads back.
 */

#include "testing/testing.h"

#include "BKE_blender.hh"
#include "BKE_global.hh"
#include "BKE_gtest_base.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_paint_layers.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "DNA_material_types.h"

#include "../paint_layers_intern.hh"
#include "../paint_layers_kind.hh"

namespace blender::bke::tests {

class PaintLayersNeighborhoodKindTest : public bke::BlenderGTestBase {
 public:
  Main *bmain = nullptr;

  void SetUp() override
  {
    paint_layer_anchor_point_slice_cache_clear();
    bmain = BKE_main_new();
    BKE_blender_globals_main_replace(bmain);
  }

  void TearDown() override
  {
    paint_layer_anchor_point_slice_cache_clear();
    G_MAIN = nullptr;
    BKE_main_free(bmain);
  }
};

/** The neighborhood roles a Sharpen, a High Pass, an Edge Detect, an Emboss and an Erode / Dilate
 * share with the Blur: they correct a row's colour only, and the row is baked for the live graph. */
static void expect_neighborhood_kind_common(MaterialPaintLayer &effect)
{
  EXPECT_EQ(effect.role, MA_PAINT_LAYER_ROLE_EFFECT);
  EXPECT_TRUE(paint_layer_kind_has_flag(effect, PAINT_LAYER_KIND_ACTS_ON_BELOW));
  EXPECT_TRUE(paint_layer_kind_has_flag(effect, PAINT_LAYER_KIND_NEEDS_BAKE));
  effect.role = MA_PAINT_LAYER_ROLE_LAYER;
  EXPECT_FALSE(paint_layer_kind_role_allowed(effect));
  effect.role = MA_PAINT_LAYER_ROLE_MASK_ITEM;
  EXPECT_FALSE(paint_layer_kind_role_allowed(effect));
  effect.role = MA_PAINT_LAYER_ROLE_EFFECT;
}

/** The RNA enum names the kind: a filter row reads back \a identifier. */
static void expect_rna_filter_identifier(Material *ma,
                                         MaterialPaintLayer *effect,
                                         const char *identifier)
{
  PointerRNA effect_ptr = RNA_pointer_create_discrete(&ma->id, RNA_MaterialPaintLayer, effect);
  PropertyRNA *filter_prop = RNA_struct_find_property(&effect_ptr, "filter_type");
  ASSERT_NE(filter_prop, nullptr) << identifier;
  const int filter_value = RNA_property_enum_get(&effect_ptr, filter_prop);
  const char *read = nullptr;
  ASSERT_TRUE(RNA_property_enum_identifier(nullptr, &effect_ptr, filter_prop, filter_value, &read));
  EXPECT_STREQ(read, identifier);
}

TEST_F(PaintLayersNeighborhoodKindTest, blur_kind_is_a_correction_with_an_enum_and_an_int_schema)
{
  Material *ma = BKE_material_add(bmain, "RnaBlurParamMat");
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "A", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  MaterialPaintLayer *blur = BKE_paint_layers_filter_add(*ma, row, MA_PAINT_LAYER_FILTER_BLUR);
  ASSERT_NE(blur, nullptr);
  expect_neighborhood_kind_common(*blur);

  /* The schema: a shape chosen from two kernels, and a whole-texel radius of 1..64. */
  EXPECT_EQ(paint_layer_kind_param_type(*blur, PAINT_LAYER_BLUR_SHAPE),
            PaintLayerKindParamType::Enum);
  EXPECT_EQ(paint_layer_kind_param_int_get(*blur, PAINT_LAYER_BLUR_SHAPE), PAINT_LAYER_BLUR_GAUSSIAN)
      << "a new blur starts gaussian";
  EXPECT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_SHAPE, 5));
  EXPECT_EQ(paint_layer_kind_param_int_get(*blur, PAINT_LAYER_BLUR_SHAPE), PAINT_LAYER_BLUR_GAUSSIAN)
      << "the shape clamps to the two kernels the schema names";

  EXPECT_EQ(paint_layer_kind_param_type(*blur, PAINT_LAYER_BLUR_RADIUS),
            PaintLayerKindParamType::Int);
  EXPECT_EQ(paint_layer_kind_param_int_get(*blur, PAINT_LAYER_BLUR_RADIUS), 4);
  EXPECT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_RADIUS, 0));
  EXPECT_EQ(paint_layer_kind_param_int_get(*blur, PAINT_LAYER_BLUR_RADIUS), 1);
  EXPECT_TRUE(paint_layer_kind_param_int_set(*blur, PAINT_LAYER_BLUR_RADIUS, 100));
  EXPECT_EQ(paint_layer_kind_param_int_get(*blur, PAINT_LAYER_BLUR_RADIUS), 64);
  EXPECT_FALSE(paint_layer_kind_param_set(*blur, PAINT_LAYER_BLUR_RADIUS, 2.0f))
      << "the radius is an integer parameter, not a float one";

  expect_rna_filter_identifier(ma, blur, "BLUR");

  /* The collection lists exactly the schema's two parameters. */
  PointerRNA blur_ptr = RNA_pointer_create_discrete(&ma->id, RNA_MaterialPaintLayer, blur);
  PropertyRNA *params = RNA_struct_find_property(&blur_ptr, "kind_params");
  ASSERT_NE(params, nullptr);
  EXPECT_EQ(RNA_property_collection_length(&blur_ptr, params), 2);
  PointerRNA shape_ptr;
  ASSERT_TRUE(RNA_property_collection_lookup_string(&blur_ptr, params, "shape", &shape_ptr));
  PropertyRNA *label = RNA_struct_find_property(&shape_ptr, "label");
  ASSERT_NE(label, nullptr);
  EXPECT_EQ(RNA_property_string_get(&shape_ptr, label), "Shape");
  PointerRNA radius_ptr;
  ASSERT_TRUE(RNA_property_collection_lookup_string(&blur_ptr, params, "radius", &radius_ptr));
  label = RNA_struct_find_property(&radius_ptr, "label");
  ASSERT_NE(label, nullptr);
  EXPECT_EQ(RNA_property_string_get(&radius_ptr, label), "Radius");
}

TEST_F(PaintLayersNeighborhoodKindTest,
       sharpen_kind_is_a_correction_with_an_int_and_a_float_schema)
{
  Material *ma = BKE_material_add(bmain, "RnaSharpenParamMat");
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "A", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  MaterialPaintLayer *sharpen = BKE_paint_layers_filter_add(
      *ma, row, MA_PAINT_LAYER_FILTER_SHARPEN);
  ASSERT_NE(sharpen, nullptr);
  expect_neighborhood_kind_common(*sharpen);

  /* The schema: a whole-texel radius of 1..64, and the unsharp weight of 0..2. */
  EXPECT_EQ(paint_layer_kind_param_type(*sharpen, PAINT_LAYER_SHARPEN_RADIUS),
            PaintLayerKindParamType::Int);
  EXPECT_EQ(paint_layer_kind_param_int_get(*sharpen, PAINT_LAYER_SHARPEN_RADIUS), 2);
  EXPECT_TRUE(paint_layer_kind_param_int_set(*sharpen, PAINT_LAYER_SHARPEN_RADIUS, 0));
  EXPECT_EQ(paint_layer_kind_param_int_get(*sharpen, PAINT_LAYER_SHARPEN_RADIUS), 1);
  EXPECT_TRUE(paint_layer_kind_param_int_set(*sharpen, PAINT_LAYER_SHARPEN_RADIUS, 100));
  EXPECT_EQ(paint_layer_kind_param_int_get(*sharpen, PAINT_LAYER_SHARPEN_RADIUS), 64);
  EXPECT_FALSE(paint_layer_kind_param_set(*sharpen, PAINT_LAYER_SHARPEN_RADIUS, 2.0f))
      << "the radius is an integer parameter, not a float one";

  EXPECT_EQ(paint_layer_kind_param_type(*sharpen, PAINT_LAYER_SHARPEN_AMOUNT),
            PaintLayerKindParamType::Float);
  EXPECT_FLOAT_EQ(paint_layer_kind_param_get(*sharpen, PAINT_LAYER_SHARPEN_AMOUNT), 0.5f);
  EXPECT_TRUE(paint_layer_kind_param_set(*sharpen, PAINT_LAYER_SHARPEN_AMOUNT, 5.0f));
  EXPECT_FLOAT_EQ(paint_layer_kind_param_get(*sharpen, PAINT_LAYER_SHARPEN_AMOUNT), 2.0f);
  EXPECT_TRUE(paint_layer_kind_param_set(*sharpen, PAINT_LAYER_SHARPEN_AMOUNT, -1.0f));
  EXPECT_FLOAT_EQ(paint_layer_kind_param_get(*sharpen, PAINT_LAYER_SHARPEN_AMOUNT), 0.0f);
  EXPECT_FALSE(paint_layer_kind_param_int_set(*sharpen, PAINT_LAYER_SHARPEN_AMOUNT, 1))
      << "the amount is a float parameter, not an integer one";

  expect_rna_filter_identifier(ma, sharpen, "SHARPEN");

  /* The collection lists exactly the schema's two parameters, in the schema's order. */
  PointerRNA sharpen_ptr = RNA_pointer_create_discrete(&ma->id, RNA_MaterialPaintLayer, sharpen);
  PropertyRNA *params = RNA_struct_find_property(&sharpen_ptr, "kind_params");
  ASSERT_NE(params, nullptr);
  EXPECT_EQ(RNA_property_collection_length(&sharpen_ptr, params), 2);
  PointerRNA radius_ptr;
  ASSERT_TRUE(RNA_property_collection_lookup_string(&sharpen_ptr, params, "radius", &radius_ptr));
  PropertyRNA *label = RNA_struct_find_property(&radius_ptr, "label");
  ASSERT_NE(label, nullptr);
  EXPECT_EQ(RNA_property_string_get(&radius_ptr, label), "Radius");
  PointerRNA amount_ptr;
  ASSERT_TRUE(RNA_property_collection_lookup_string(&sharpen_ptr, params, "amount", &amount_ptr));
  label = RNA_struct_find_property(&amount_ptr, "label");
  ASSERT_NE(label, nullptr);
  EXPECT_EQ(RNA_property_string_get(&amount_ptr, label), "Amount");
}

TEST_F(PaintLayersNeighborhoodKindTest,
       high_pass_kind_is_a_correction_with_an_int_and_a_float_schema)
{
  Material *ma = BKE_material_add(bmain, "RnaHighPassParamMat");
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "A", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  MaterialPaintLayer *high_pass = BKE_paint_layers_filter_add(
      *ma, row, MA_PAINT_LAYER_FILTER_HIGH_PASS);
  ASSERT_NE(high_pass, nullptr);
  expect_neighborhood_kind_common(*high_pass);

  /* The schema: a whole-texel radius of 1..64 (four, wider than the Sharpen's), and the detail's
   * weight of 0..2, one keeping it all and zero flattening to the mid grey. */
  EXPECT_EQ(paint_layer_kind_param_type(*high_pass, PAINT_LAYER_HIGH_PASS_RADIUS),
            PaintLayerKindParamType::Int);
  EXPECT_EQ(paint_layer_kind_param_int_get(*high_pass, PAINT_LAYER_HIGH_PASS_RADIUS), 4);
  EXPECT_TRUE(paint_layer_kind_param_int_set(*high_pass, PAINT_LAYER_HIGH_PASS_RADIUS, 0));
  EXPECT_EQ(paint_layer_kind_param_int_get(*high_pass, PAINT_LAYER_HIGH_PASS_RADIUS), 1);
  EXPECT_TRUE(paint_layer_kind_param_int_set(*high_pass, PAINT_LAYER_HIGH_PASS_RADIUS, 100));
  EXPECT_EQ(paint_layer_kind_param_int_get(*high_pass, PAINT_LAYER_HIGH_PASS_RADIUS), 100);
  EXPECT_TRUE(paint_layer_kind_param_int_set(*high_pass, PAINT_LAYER_HIGH_PASS_RADIUS, 1000));
  EXPECT_EQ(paint_layer_kind_param_int_get(*high_pass, PAINT_LAYER_HIGH_PASS_RADIUS), 256);

  EXPECT_EQ(paint_layer_kind_param_type(*high_pass, PAINT_LAYER_HIGH_PASS_CONTRAST),
            PaintLayerKindParamType::Float);
  EXPECT_FLOAT_EQ(paint_layer_kind_param_get(*high_pass, PAINT_LAYER_HIGH_PASS_CONTRAST), 1.0f);
  EXPECT_TRUE(paint_layer_kind_param_set(*high_pass, PAINT_LAYER_HIGH_PASS_CONTRAST, 5.0f));
  EXPECT_FLOAT_EQ(paint_layer_kind_param_get(*high_pass, PAINT_LAYER_HIGH_PASS_CONTRAST), 5.0f);
  EXPECT_TRUE(paint_layer_kind_param_set(*high_pass, PAINT_LAYER_HIGH_PASS_CONTRAST, 50.0f));
  EXPECT_FLOAT_EQ(paint_layer_kind_param_get(*high_pass, PAINT_LAYER_HIGH_PASS_CONTRAST), 10.0f);

  EXPECT_EQ(paint_layer_kind_param_type(*high_pass, PAINT_LAYER_HIGH_PASS_OUTPUT),
            PaintLayerKindParamType::Enum);
  EXPECT_EQ(paint_layer_kind_param_int_get(*high_pass, PAINT_LAYER_HIGH_PASS_OUTPUT),
            PAINT_LAYER_HIGH_PASS_OUTPUT_COLOR)
      << "a new high pass keeps the colour";
  EXPECT_TRUE(paint_layer_kind_param_int_set(*high_pass, PAINT_LAYER_HIGH_PASS_OUTPUT, 5));
  EXPECT_EQ(paint_layer_kind_param_int_get(*high_pass, PAINT_LAYER_HIGH_PASS_OUTPUT),
            PAINT_LAYER_HIGH_PASS_OUTPUT_GRAYSCALE)
      << "the output clamps to the two choices the schema names";

  expect_rna_filter_identifier(ma, high_pass, "HIGH_PASS");
  PointerRNA high_pass_ptr = RNA_pointer_create_discrete(
      &ma->id, RNA_MaterialPaintLayer, high_pass);
  PropertyRNA *params = RNA_struct_find_property(&high_pass_ptr, "kind_params");
  ASSERT_NE(params, nullptr);
  EXPECT_EQ(RNA_property_collection_length(&high_pass_ptr, params), 3);
}

TEST_F(PaintLayersNeighborhoodKindTest, edge_detect_and_emboss_kinds_carry_no_parameters)
{
  Material *ma = BKE_material_add(bmain, "RnaEdgeEmbossParamMat");
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "A", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  MaterialPaintLayer *edge = BKE_paint_layers_filter_add(
      *ma, row, MA_PAINT_LAYER_FILTER_EDGE_DETECT);
  MaterialPaintLayer *emboss = BKE_paint_layers_filter_add(
      *ma, row, MA_PAINT_LAYER_FILTER_EMBOSS);
  ASSERT_NE(edge, nullptr);
  ASSERT_NE(emboss, nullptr);

  /* The first kinds without a schema: no parameter is listed, and the generic accessors refuse
   * any index. */
  for (MaterialPaintLayer *effect : {edge, emboss}) {
    expect_neighborhood_kind_common(*effect);
    const PaintLayerKindInfo *kind = paint_layer_kind_of(*effect);
    ASSERT_NE(kind, nullptr);
    EXPECT_EQ(kind->params_num, 0);
    EXPECT_EQ(kind->params, nullptr);

    EXPECT_FALSE(paint_layer_kind_param_set(*effect, 0, 0.5f));
    EXPECT_FALSE(paint_layer_kind_param_int_set(*effect, 0, 1));
    EXPECT_FLOAT_EQ(paint_layer_kind_param_get(*effect, 0), 0.0f);
    EXPECT_EQ(paint_layer_kind_param_int_get(*effect, 0), 0);

    PointerRNA effect_ptr = RNA_pointer_create_discrete(
        &ma->id, RNA_MaterialPaintLayer, effect);
    PropertyRNA *params = RNA_struct_find_property(&effect_ptr, "kind_params");
    ASSERT_NE(params, nullptr);
    EXPECT_EQ(RNA_property_collection_length(&effect_ptr, params), 0);
  }

  expect_rna_filter_identifier(ma, edge, "EDGE_DETECT");
  expect_rna_filter_identifier(ma, emboss, "EMBOSS");
}

TEST_F(PaintLayersNeighborhoodKindTest, erode_dilate_kind_has_a_mode_enum_and_an_int_radius)
{
  Material *ma = BKE_material_add(bmain, "RnaMorphParamMat");
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "A", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  MaterialPaintLayer *morph = BKE_paint_layers_filter_add(
      *ma, row, MA_PAINT_LAYER_FILTER_ERODE_DILATE);
  ASSERT_NE(morph, nullptr);
  expect_neighborhood_kind_common(*morph);

  /* The schema: a direction chosen from two kernels, and a whole-texel radius of 1..64. */
  EXPECT_EQ(paint_layer_kind_param_type(*morph, PAINT_LAYER_ERODE_DILATE_MODE),
            PaintLayerKindParamType::Enum);
  EXPECT_EQ(paint_layer_kind_param_int_get(*morph, PAINT_LAYER_ERODE_DILATE_MODE),
            PAINT_LAYER_ERODE_DILATE_ERODE)
      << "a new morph starts eroding";
  EXPECT_TRUE(paint_layer_kind_param_int_set(*morph, PAINT_LAYER_ERODE_DILATE_MODE, 5));
  EXPECT_EQ(paint_layer_kind_param_int_get(*morph, PAINT_LAYER_ERODE_DILATE_MODE),
            PAINT_LAYER_ERODE_DILATE_DILATE)
      << "the mode clamps to the two directions the schema names";
  EXPECT_TRUE(paint_layer_kind_param_int_set(*morph, PAINT_LAYER_ERODE_DILATE_MODE,
                                             PAINT_LAYER_ERODE_DILATE_ERODE));

  EXPECT_EQ(paint_layer_kind_param_type(*morph, PAINT_LAYER_ERODE_DILATE_RADIUS),
            PaintLayerKindParamType::Int);
  EXPECT_EQ(paint_layer_kind_param_int_get(*morph, PAINT_LAYER_ERODE_DILATE_RADIUS), 2);
  EXPECT_TRUE(paint_layer_kind_param_int_set(*morph, PAINT_LAYER_ERODE_DILATE_RADIUS, 0));
  EXPECT_EQ(paint_layer_kind_param_int_get(*morph, PAINT_LAYER_ERODE_DILATE_RADIUS), 1);
  EXPECT_TRUE(paint_layer_kind_param_int_set(*morph, PAINT_LAYER_ERODE_DILATE_RADIUS, 100));
  EXPECT_EQ(paint_layer_kind_param_int_get(*morph, PAINT_LAYER_ERODE_DILATE_RADIUS), 64);

  expect_rna_filter_identifier(ma, morph, "ERODE_DILATE");

  /* The collection lists exactly the schema's two parameters, the mode first. */
  PointerRNA morph_ptr = RNA_pointer_create_discrete(&ma->id, RNA_MaterialPaintLayer, morph);
  PropertyRNA *params = RNA_struct_find_property(&morph_ptr, "kind_params");
  ASSERT_NE(params, nullptr);
  EXPECT_EQ(RNA_property_collection_length(&morph_ptr, params), 2);
  PointerRNA mode_ptr;
  ASSERT_TRUE(RNA_property_collection_lookup_string(&morph_ptr, params, "mode", &mode_ptr));
  PropertyRNA *label = RNA_struct_find_property(&mode_ptr, "label");
  ASSERT_NE(label, nullptr);
  EXPECT_EQ(RNA_property_string_get(&mode_ptr, label), "Mode");
  PointerRNA radius_ptr;
  ASSERT_TRUE(RNA_property_collection_lookup_string(&morph_ptr, params, "radius", &radius_ptr));
  label = RNA_struct_find_property(&radius_ptr, "label");
  ASSERT_NE(label, nullptr);
  EXPECT_EQ(RNA_property_string_get(&radius_ptr, label), "Radius");
}

}  // namespace blender::bke::tests
