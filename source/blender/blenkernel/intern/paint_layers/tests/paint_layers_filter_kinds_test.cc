/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * The parameters and the RNA of the filter kinds: the CPU-computed effects (Blur, Sharpen, High
 * Pass, Edge Detect, Emboss, Erode / Dilate) and the pointwise filters added beside them (Exposure,
 * Deband / Dither, Normal Convert, Channel Mixer): their schema, their roles and the enum name each
 * reads back.
 */

#include "testing/testing.h"

#include "MEM_guardedalloc.h"

#include "BKE_blender.hh"
#include "BKE_global.hh"
#include "BKE_gtest_base.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_paint_layers.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "BLI_index_range.hh"

#include "DNA_material_types.h"

#include <string>

#include "../paint_layers_intern.hh"
#include "../paint_layers_kind.hh"

namespace blender::bke::tests {

class PaintLayersFilterKindsTest : public bke::BlenderGTestBase {
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

/** The roles a Sharpen, a High Pass, an Edge Detect, an Emboss and an Erode / Dilate share with the
 * Blur: they correct a row's colour only, and the row is baked for the live graph. */
static void expect_cpu_computed_kind_common(MaterialPaintLayer &effect)
{
  EXPECT_EQ(effect.role, MA_PAINT_LAYER_ROLE_EFFECT);
  EXPECT_TRUE(paint_layer_kind_has_flag(effect, PAINT_LAYER_KIND_ACTS_ON_BELOW));
  EXPECT_TRUE(paint_layer_kind_has_flag(effect, PAINT_LAYER_KIND_NEEDS_BAKE));
  effect.role = MA_PAINT_LAYER_ROLE_LAYER;
  EXPECT_FALSE(paint_layer_kind_role_allowed(effect));
  /* As a mask item it reads the grey the mask has up to it, through the mask effect's cache. */
  effect.role = MA_PAINT_LAYER_ROLE_MASK_ITEM;
  EXPECT_TRUE(paint_layer_kind_role_allowed(effect));
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

TEST_F(PaintLayersFilterKindsTest, blur_kind_is_a_correction_with_an_enum_and_an_int_schema)
{
  Material *ma = BKE_material_add(bmain, "RnaBlurParamMat");
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "A", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  MaterialPaintLayer *blur = BKE_paint_layers_filter_add(*ma, row, MA_PAINT_LAYER_FILTER_BLUR);
  ASSERT_NE(blur, nullptr);
  expect_cpu_computed_kind_common(*blur);

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

TEST_F(PaintLayersFilterKindsTest,
       sharpen_kind_is_a_correction_with_an_int_and_a_float_schema)
{
  Material *ma = BKE_material_add(bmain, "RnaSharpenParamMat");
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "A", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  MaterialPaintLayer *sharpen = BKE_paint_layers_filter_add(
      *ma, row, MA_PAINT_LAYER_FILTER_SHARPEN);
  ASSERT_NE(sharpen, nullptr);
  expect_cpu_computed_kind_common(*sharpen);

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

TEST_F(PaintLayersFilterKindsTest,
       high_pass_kind_is_a_correction_with_an_int_and_a_float_schema)
{
  Material *ma = BKE_material_add(bmain, "RnaHighPassParamMat");
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "A", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  MaterialPaintLayer *high_pass = BKE_paint_layers_filter_add(
      *ma, row, MA_PAINT_LAYER_FILTER_HIGH_PASS);
  ASSERT_NE(high_pass, nullptr);
  expect_cpu_computed_kind_common(*high_pass);

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

TEST_F(PaintLayersFilterKindsTest, edge_detect_and_emboss_kinds_carry_no_parameters)
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
    expect_cpu_computed_kind_common(*effect);
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

TEST_F(PaintLayersFilterKindsTest, erode_dilate_kind_has_a_mode_enum_and_an_int_radius)
{
  Material *ma = BKE_material_add(bmain, "RnaMorphParamMat");
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "A", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  MaterialPaintLayer *morph = BKE_paint_layers_filter_add(
      *ma, row, MA_PAINT_LAYER_FILTER_ERODE_DILATE);
  ASSERT_NE(morph, nullptr);
  expect_cpu_computed_kind_common(*morph);

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

TEST_F(PaintLayersFilterKindsTest, rna_lists_the_parameters_of_a_filter_and_clamps_an_edit)
{
  Material *ma = BKE_material_add(bmain, "RnaParamMat");
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "A", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  MaterialPaintLayer *levels = BKE_paint_layers_filter_add(
      *ma, row, MA_PAINT_LAYER_FILTER_LEVELS);
  ASSERT_NE(levels, nullptr);

  PointerRNA layer_ptr = RNA_pointer_create_discrete(&ma->id, RNA_MaterialPaintLayer, levels);
  PropertyRNA *params = RNA_struct_find_property(&layer_ptr, "kind_params");
  ASSERT_NE(params, nullptr);
  /* Listed before any edit, with the schema's range as the UI data. */
  EXPECT_EQ(RNA_property_collection_length(&layer_ptr, params), 5);

  PointerRNA gamma_ptr;
  ASSERT_TRUE(RNA_property_collection_lookup_string(&layer_ptr, params, "gamma", &gamma_ptr));
  PropertyRNA *value = RNA_struct_find_property(&gamma_ptr, "value");
  PropertyRNA *label = RNA_struct_find_property(&gamma_ptr, "label");
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(RNA_property_string_get(&gamma_ptr, label), "Gamma");
  EXPECT_FLOAT_EQ(RNA_property_float_get(&gamma_ptr, value), 1.0f);

  RNA_property_float_set(&gamma_ptr, value, 50.0f);
  EXPECT_FLOAT_EQ(RNA_property_float_get(&gamma_ptr, value), 10.0f);
  EXPECT_FLOAT_EQ(paint_layer_kind_param_get(*levels, PAINT_LAYER_LEVELS_GAMMA), 10.0f);

  /* A row with no parameters lists none. */
  PointerRNA row_ptr = RNA_pointer_create_discrete(&ma->id, RNA_MaterialPaintLayer, row);
  EXPECT_EQ(RNA_property_collection_length(&row_ptr, RNA_struct_find_property(&row_ptr, "kind_params")),
            0);
}

TEST_F(PaintLayersFilterKindsTest, rna_kind_params_carry_their_type)
{
  Material *ma = BKE_material_add(bmain, "RnaTypedParamMat");
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "A", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  MaterialPaintLayer *posterize = BKE_paint_layers_filter_add(
      *ma, row, MA_PAINT_LAYER_FILTER_POSTERIZE);
  ASSERT_NE(posterize, nullptr);
  MaterialPaintLayer *math = BKE_paint_layers_filter_add(*ma, row, MA_PAINT_LAYER_FILTER_MATH);
  ASSERT_NE(math, nullptr);

  /* An integer parameter: the type reads INT, the number field clamps to the schema's range, and
   * the float value property reads the same stored value. */
  PointerRNA posterize_ptr = RNA_pointer_create_discrete(&ma->id, RNA_MaterialPaintLayer, posterize);
  PropertyRNA *params = RNA_struct_find_property(&posterize_ptr, "kind_params");
  ASSERT_NE(params, nullptr);
  PointerRNA steps_ptr;
  ASSERT_TRUE(RNA_property_collection_lookup_string(&posterize_ptr, params, "steps", &steps_ptr));
  const char *type_identifier = nullptr;
  PropertyRNA *type = RNA_struct_find_property(&steps_ptr, "type");
  ASSERT_NE(type, nullptr);
  ASSERT_TRUE(RNA_property_enum_identifier(
      nullptr, &steps_ptr, type, RNA_property_enum_get(&steps_ptr, type), &type_identifier));
  EXPECT_STREQ(type_identifier, "INT");

  PropertyRNA *int_value = RNA_struct_find_property(&steps_ptr, "int_value");
  PropertyRNA *float_value = RNA_struct_find_property(&steps_ptr, "value");
  ASSERT_NE(int_value, nullptr);
  EXPECT_EQ(RNA_property_int_get(&steps_ptr, int_value), 4);
  RNA_property_int_set(&steps_ptr, int_value, 0);
  EXPECT_EQ(RNA_property_int_get(&steps_ptr, int_value), 2);
  RNA_property_int_set(&steps_ptr, int_value, 100);
  EXPECT_EQ(RNA_property_int_get(&steps_ptr, int_value), 64);
  EXPECT_FLOAT_EQ(RNA_property_float_get(&steps_ptr, float_value), 64.0f);
  EXPECT_EQ(paint_layer_kind_param_int_get(*posterize, PAINT_LAYER_POSTERIZE_STEPS), 64);

  /* An enum parameter: the type reads ENUM and the items come from the property's UI data. */
  PointerRNA math_ptr = RNA_pointer_create_discrete(&ma->id, RNA_MaterialPaintLayer, math);
  PropertyRNA *math_params = RNA_struct_find_property(&math_ptr, "kind_params");
  ASSERT_NE(math_params, nullptr);
  PointerRNA op_ptr;
  ASSERT_TRUE(
      RNA_property_collection_lookup_string(&math_ptr, math_params, "operation", &op_ptr));
  PropertyRNA *op_type = RNA_struct_find_property(&op_ptr, "type");
  ASSERT_NE(op_type, nullptr);
  ASSERT_TRUE(RNA_property_enum_identifier(
      nullptr, &op_ptr, op_type, RNA_property_enum_get(&op_ptr, op_type), &type_identifier));
  EXPECT_STREQ(type_identifier, "ENUM");

  PropertyRNA *enum_value = RNA_struct_find_property(&op_ptr, "enum_value");
  ASSERT_NE(enum_value, nullptr);
  const EnumPropertyItem *items = nullptr;
  int totitem = 0;
  bool items_free = false;
  RNA_property_enum_items(nullptr, &op_ptr, enum_value, &items, &totitem, &items_free);
  ASSERT_NE(items, nullptr);
  EXPECT_EQ(totitem, PAINT_LAYER_MATH_OP_LOGARITHM + 1);
  EXPECT_STREQ(items[PAINT_LAYER_MATH_OP_MULTIPLY].identifier, "MULTIPLY");
  if (items_free) {
    MEM_delete(const_cast<EnumPropertyItem *>(items));
  }

  RNA_property_enum_set(&op_ptr, enum_value, PAINT_LAYER_MATH_OP_DIVIDE);
  EXPECT_EQ(paint_layer_kind_param_int_get(*math, PAINT_LAYER_MATH_OPERATION),
            PAINT_LAYER_MATH_OP_DIVIDE);

  /* A boolean parameter: the type reads BOOL, the checkbox writes the property, and the float
   * value property reads the same stored value. */
  PointerRNA clamp_ptr;
  ASSERT_TRUE(RNA_property_collection_lookup_string(&math_ptr, math_params, "clamp", &clamp_ptr));
  PropertyRNA *clamp_type = RNA_struct_find_property(&clamp_ptr, "type");
  ASSERT_NE(clamp_type, nullptr);
  ASSERT_TRUE(RNA_property_enum_identifier(nullptr,
                                           &clamp_ptr,
                                           clamp_type,
                                           RNA_property_enum_get(&clamp_ptr, clamp_type),
                                           &type_identifier));
  EXPECT_STREQ(type_identifier, "BOOL");

  PropertyRNA *bool_value = RNA_struct_find_property(&clamp_ptr, "bool_value");
  PropertyRNA *clamp_float = RNA_struct_find_property(&clamp_ptr, "value");
  ASSERT_NE(bool_value, nullptr);
  EXPECT_FALSE(RNA_property_boolean_get(&clamp_ptr, bool_value));
  RNA_property_boolean_set(&clamp_ptr, bool_value, true);
  EXPECT_TRUE(RNA_property_boolean_get(&clamp_ptr, bool_value));
  EXPECT_TRUE(paint_layer_kind_param_bool_get(*math, PAINT_LAYER_MATH_CLAMP));
  ASSERT_NE(clamp_float, nullptr);
  EXPECT_FLOAT_EQ(RNA_property_float_get(&clamp_ptr, clamp_float), 1.0f);
}

/**
 * The shape of a ranged-float kind parameter as the generic RNA shows it: the `kind_params` item
 * \a identifier of \a layer_ptr reads \a default_value as a FLOAT in [0, 1], and a write past the
 * range clamps into it, in the property and in the row's own storage at \a index.
 */
static void rna_expect_ranged_float_param(PointerRNA &layer_ptr,
                                          const MaterialPaintLayer &layer,
                                          const char *identifier,
                                          const float default_value,
                                          const int index)
{
  PropertyRNA *params = RNA_struct_find_property(&layer_ptr, "kind_params");
  ASSERT_NE(params, nullptr) << identifier;
  PointerRNA value_ptr;
  ASSERT_TRUE(RNA_property_collection_lookup_string(&layer_ptr, params, identifier, &value_ptr));
  const char *type_identifier = nullptr;
  PropertyRNA *type = RNA_struct_find_property(&value_ptr, "type");
  ASSERT_NE(type, nullptr);
  ASSERT_TRUE(RNA_property_enum_identifier(
      nullptr, &value_ptr, type, RNA_property_enum_get(&value_ptr, type), &type_identifier));
  EXPECT_STREQ(type_identifier, "FLOAT");

  /* The schema's default and range, and a write past the range clamps to it. */
  PropertyRNA *float_value = RNA_struct_find_property(&value_ptr, "value");
  ASSERT_NE(float_value, nullptr);
  EXPECT_FLOAT_EQ(RNA_property_float_get(&value_ptr, float_value), default_value);
  float range_min = 0.0f, range_max = 0.0f;
  RNA_property_float_range(&value_ptr, float_value, &range_min, &range_max);
  EXPECT_FLOAT_EQ(range_min, 0.0f);
  EXPECT_FLOAT_EQ(range_max, 1.0f);
  RNA_property_float_set(&value_ptr, float_value, 5.0f);
  EXPECT_FLOAT_EQ(RNA_property_float_get(&value_ptr, float_value), 1.0f);
  EXPECT_FLOAT_EQ(paint_layer_kind_param_get(layer, index), 1.0f);
}

TEST_F(PaintLayersFilterKindsTest, rna_threshold_param_is_a_ranged_float)
{
  Material *ma = BKE_material_add(bmain, "RnaThresholdParamMat");
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "A", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  MaterialPaintLayer *threshold = BKE_paint_layers_filter_add(
      *ma, row, MA_PAINT_LAYER_FILTER_THRESHOLD);
  ASSERT_NE(threshold, nullptr);

  /* The kind's one parameter: the collection lists exactly it. */
  PointerRNA threshold_ptr = RNA_pointer_create_discrete(
      &ma->id, RNA_MaterialPaintLayer, threshold);
  PropertyRNA *params = RNA_struct_find_property(&threshold_ptr, "kind_params");
  ASSERT_NE(params, nullptr);
  EXPECT_EQ(RNA_property_collection_length(&threshold_ptr, params), 1);
  rna_expect_ranged_float_param(
      threshold_ptr, *threshold, "threshold", 0.5f, PAINT_LAYER_THRESHOLD_THRESHOLD);
}

TEST_F(PaintLayersFilterKindsTest, rna_clamp_params_are_ranged_floats)
{
  Material *ma = BKE_material_add(bmain, "RnaClampParamMat");
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "A", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  MaterialPaintLayer *clamp = BKE_paint_layers_filter_add(*ma, row, MA_PAINT_LAYER_FILTER_CLAMP);
  ASSERT_NE(clamp, nullptr);

  /* The kind's two bounds: the collection lists exactly them, in the schema's order. */
  PointerRNA clamp_ptr = RNA_pointer_create_discrete(&ma->id, RNA_MaterialPaintLayer, clamp);
  PropertyRNA *params = RNA_struct_find_property(&clamp_ptr, "kind_params");
  ASSERT_NE(params, nullptr);
  EXPECT_EQ(RNA_property_collection_length(&clamp_ptr, params), 2);
  rna_expect_ranged_float_param(clamp_ptr, *clamp, "min", 0.0f, PAINT_LAYER_CLAMP_MIN);
  rna_expect_ranged_float_param(clamp_ptr, *clamp, "max", 1.0f, PAINT_LAYER_CLAMP_MAX);
}

TEST_F(PaintLayersFilterKindsTest, exposure_kind_is_a_pointwise_filter_with_a_float_schema)
{
  Material *ma = BKE_material_add(bmain, "RnaExposureParamMat");
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "A", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  MaterialPaintLayer *exposure = BKE_paint_layers_filter_add(
      *ma, row, MA_PAINT_LAYER_FILTER_EXPOSURE);
  ASSERT_NE(exposure, nullptr);

  /* A pointwise filter takes a colour from below and reads no map: it is not CPU-computed, so it
   * carries the below flag alone -- no NEEDS_BAKE. */
  EXPECT_TRUE(paint_layer_kind_has_flag(*exposure, PAINT_LAYER_KIND_ACTS_ON_BELOW));
  EXPECT_FALSE(paint_layer_kind_has_flag(*exposure, PAINT_LAYER_KIND_NEEDS_BAKE));
  EXPECT_FALSE(paint_layer_kind_has_flag(*exposure, PAINT_LAYER_KIND_NO_GPU_BRANCH));

  /* Three float parameters: the exposure and the offset default to a no-op, the gamma to one. */
  EXPECT_EQ(paint_layer_kind_param_type(*exposure, PAINT_LAYER_EXPOSURE_EXPOSURE),
            PaintLayerKindParamType::Float);
  EXPECT_FLOAT_EQ(paint_layer_kind_param_get(*exposure, PAINT_LAYER_EXPOSURE_EXPOSURE), 0.0f);
  EXPECT_FLOAT_EQ(paint_layer_kind_param_get(*exposure, PAINT_LAYER_EXPOSURE_OFFSET), 0.0f);
  EXPECT_FLOAT_EQ(paint_layer_kind_param_get(*exposure, PAINT_LAYER_EXPOSURE_GAMMA), 1.0f);

  /* The RNA enum names the kind: a filter row reads back its EXPOSURE identifier. */
  PointerRNA exposure_ptr = RNA_pointer_create_discrete(&ma->id,
                                                        RNA_MaterialPaintLayer,
                                                        exposure);
  PropertyRNA *filter_prop = RNA_struct_find_property(&exposure_ptr, "filter_type");
  ASSERT_NE(filter_prop, nullptr);
  const int filter_value = RNA_property_enum_get(&exposure_ptr, filter_prop);
  EXPECT_EQ(filter_value, MA_PAINT_LAYER_FILTER_EXPOSURE);
  const char *identifier = nullptr;
  ASSERT_TRUE(RNA_property_enum_identifier(
      nullptr, &exposure_ptr, filter_prop, filter_value, &identifier));
  EXPECT_STREQ(identifier, "EXPOSURE");

  /* The collection lists exactly the schema's three parameters, labelled. */
  PropertyRNA *params = RNA_struct_find_property(&exposure_ptr, "kind_params");
  ASSERT_NE(params, nullptr);
  EXPECT_EQ(RNA_property_collection_length(&exposure_ptr, params), 3);
  const char *names[3] = {"exposure", "offset", "gamma"};
  const char *labels[3] = {"Exposure", "Offset", "Gamma"};
  for (const int i : IndexRange(3)) {
    PointerRNA param_ptr;
    ASSERT_TRUE(RNA_property_collection_lookup_string(&exposure_ptr, params, names[i], &param_ptr))
        << names[i];
    PropertyRNA *label = RNA_struct_find_property(&param_ptr, "label");
    ASSERT_NE(label, nullptr);
    EXPECT_EQ(RNA_property_string_get(&param_ptr, label), labels[i]);
  }
}

TEST_F(PaintLayersFilterKindsTest, histogram_scan_kind_is_a_pointwise_filter_with_a_float_schema)
{
  Material *ma = BKE_material_add(bmain, "RnaHistogramScanParamMat");
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "A", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  MaterialPaintLayer *scan = BKE_paint_layers_filter_add(
      *ma, row, MA_PAINT_LAYER_FILTER_HISTOGRAM_SCAN);
  ASSERT_NE(scan, nullptr);

  /* A pointwise filter takes a colour from below and reads no map: it is not CPU-computed, so it
   * carries the below flag alone -- no NEEDS_BAKE, and a shader branch exists. */
  EXPECT_TRUE(paint_layer_kind_has_flag(*scan, PAINT_LAYER_KIND_ACTS_ON_BELOW));
  EXPECT_FALSE(paint_layer_kind_has_flag(*scan, PAINT_LAYER_KIND_NEEDS_BAKE));
  EXPECT_FALSE(paint_layer_kind_has_flag(*scan, PAINT_LAYER_KIND_NO_GPU_BRANCH));

  /* The position defaults to the Threshold's own crossover, the contrast to the hard edge it
   * reads as, the invert to the two states as they are. */
  EXPECT_EQ(paint_layer_kind_param_type(*scan, PAINT_LAYER_HISTOGRAM_SCAN_POSITION),
            PaintLayerKindParamType::Float);
  EXPECT_FLOAT_EQ(paint_layer_kind_param_get(*scan, PAINT_LAYER_HISTOGRAM_SCAN_POSITION), 0.5f);
  EXPECT_FLOAT_EQ(paint_layer_kind_param_get(*scan, PAINT_LAYER_HISTOGRAM_SCAN_CONTRAST), 0.0f);
  EXPECT_FLOAT_EQ(paint_layer_kind_param_get(*scan, PAINT_LAYER_HISTOGRAM_SCAN_INVERT), 0.0f);

  /* The RNA enum names the kind, and the collection lists exactly the schema's three parameters,
   * labelled. */
  PointerRNA scan_ptr = RNA_pointer_create_discrete(&ma->id, RNA_MaterialPaintLayer, scan);
  PropertyRNA *filter_prop = RNA_struct_find_property(&scan_ptr, "filter_type");
  ASSERT_NE(filter_prop, nullptr);
  const char *identifier = nullptr;
  ASSERT_TRUE(RNA_property_enum_identifier(nullptr,
                                           &scan_ptr,
                                           filter_prop,
                                           MA_PAINT_LAYER_FILTER_HISTOGRAM_SCAN,
                                           &identifier));
  EXPECT_STREQ(identifier, "HISTOGRAM_SCAN");
  PropertyRNA *params = RNA_struct_find_property(&scan_ptr, "kind_params");
  ASSERT_NE(params, nullptr);
  EXPECT_EQ(RNA_property_collection_length(&scan_ptr, params), 3);
  const char *names[3] = {"position", "contrast", "invert_position"};
  const char *labels[3] = {"Position", "Contrast", "Invert Position"};
  for (const int i : IndexRange(3)) {
    PointerRNA param_ptr;
    ASSERT_TRUE(RNA_property_collection_lookup_string(&scan_ptr, params, names[i], &param_ptr))
        << names[i];
    PropertyRNA *label = RNA_struct_find_property(&param_ptr, "label");
    ASSERT_NE(label, nullptr);
    EXPECT_EQ(RNA_property_string_get(&param_ptr, label), labels[i]);
  }
}

TEST_F(PaintLayersFilterKindsTest, histogram_shift_kind_is_a_pointwise_filter_with_one_float)
{
  Material *ma = BKE_material_add(bmain, "RnaHistogramShiftParamMat");
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "A", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  MaterialPaintLayer *shift = BKE_paint_layers_filter_add(
      *ma, row, MA_PAINT_LAYER_FILTER_HISTOGRAM_SHIFT);
  ASSERT_NE(shift, nullptr);

  EXPECT_TRUE(paint_layer_kind_has_flag(*shift, PAINT_LAYER_KIND_ACTS_ON_BELOW));
  EXPECT_FALSE(paint_layer_kind_has_flag(*shift, PAINT_LAYER_KIND_NEEDS_BAKE));
  EXPECT_FALSE(paint_layer_kind_has_flag(*shift, PAINT_LAYER_KIND_NO_GPU_BRANCH));

  /* One float: the shift defaults to no shift at all. */
  EXPECT_EQ(paint_layer_kind_param_type(*shift, PAINT_LAYER_HISTOGRAM_SHIFT_POSITION),
            PaintLayerKindParamType::Float);
  EXPECT_FLOAT_EQ(paint_layer_kind_param_get(*shift, PAINT_LAYER_HISTOGRAM_SHIFT_POSITION), 0.0f);

  PointerRNA shift_ptr = RNA_pointer_create_discrete(&ma->id, RNA_MaterialPaintLayer, shift);
  PropertyRNA *filter_prop = RNA_struct_find_property(&shift_ptr, "filter_type");
  ASSERT_NE(filter_prop, nullptr);
  const char *identifier = nullptr;
  ASSERT_TRUE(RNA_property_enum_identifier(nullptr,
                                           &shift_ptr,
                                           filter_prop,
                                           MA_PAINT_LAYER_FILTER_HISTOGRAM_SHIFT,
                                           &identifier));
  EXPECT_STREQ(identifier, "HISTOGRAM_SHIFT");
  PropertyRNA *params = RNA_struct_find_property(&shift_ptr, "kind_params");
  ASSERT_NE(params, nullptr);
  EXPECT_EQ(RNA_property_collection_length(&shift_ptr, params), 1);
  PointerRNA param_ptr;
  ASSERT_TRUE(RNA_property_collection_lookup_string(
      &shift_ptr, params, "position", &param_ptr));
  PropertyRNA *label = RNA_struct_find_property(&param_ptr, "label");
  ASSERT_NE(label, nullptr);
  EXPECT_EQ(RNA_property_string_get(&param_ptr, label), "Position");
}

TEST_F(PaintLayersFilterKindsTest, blur_directional_kind_is_a_neighborhood_filter_with_a_schema)
{
  Material *ma = BKE_material_add(bmain, "RnaBlurDirectionalParamMat");
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "A", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  MaterialPaintLayer *blur = BKE_paint_layers_filter_add(
      *ma, row, MA_PAINT_LAYER_FILTER_BLUR_DIRECTIONAL);
  ASSERT_NE(blur, nullptr);

  /* A neighborhood filter reads a neighbourhood of the row's map: it is CPU-computed, and the
   * combined preview computes it on the GPU like the Blur. */
  EXPECT_TRUE(paint_layer_kind_has_flag(*blur, PAINT_LAYER_KIND_ACTS_ON_BELOW));
  EXPECT_TRUE(paint_layer_kind_has_flag(*blur, PAINT_LAYER_KIND_NEEDS_BAKE));
  EXPECT_TRUE(paint_layer_kind_has_flag(*blur, PAINT_LAYER_KIND_GPU_COMPUTED));
  EXPECT_FALSE(paint_layer_kind_has_flag(*blur, PAINT_LAYER_KIND_NO_GPU_BRANCH));

  /* The angle defaults to the map's +X, the radius to a visible streak, the intensity to all
   * average. */
  EXPECT_FLOAT_EQ(paint_layer_kind_param_get(*blur, PAINT_LAYER_BLUR_DIRECTIONAL_ANGLE), 0.0f);
  EXPECT_FLOAT_EQ(paint_layer_kind_param_get(*blur, PAINT_LAYER_BLUR_DIRECTIONAL_RADIUS), 8.0f);
  EXPECT_FLOAT_EQ(paint_layer_kind_param_get(*blur, PAINT_LAYER_BLUR_DIRECTIONAL_MIX), 1.0f);
  /* The radius is a whole texels parameter: a float write of a fractional value is refused. */
  EXPECT_FALSE(paint_layer_kind_param_set(*blur, PAINT_LAYER_BLUR_DIRECTIONAL_RADIUS, 4.5f));

  /* The RNA enum names the kind: a filter row reads back its BLUR_DIRECTIONAL identifier. */
  PointerRNA blur_ptr = RNA_pointer_create_discrete(&ma->id, RNA_MaterialPaintLayer, blur);
  PropertyRNA *filter_prop = RNA_struct_find_property(&blur_ptr, "filter_type");
  ASSERT_NE(filter_prop, nullptr);
  const char *identifier = nullptr;
  ASSERT_TRUE(RNA_property_enum_identifier(nullptr,
                                           &blur_ptr,
                                           filter_prop,
                                           MA_PAINT_LAYER_FILTER_BLUR_DIRECTIONAL,
                                           &identifier));
  EXPECT_STREQ(identifier, "BLUR_DIRECTIONAL");
  /* The collection lists exactly the schema's three parameters, labelled. */
  PropertyRNA *params = RNA_struct_find_property(&blur_ptr, "kind_params");
  ASSERT_NE(params, nullptr);
  EXPECT_EQ(RNA_property_collection_length(&blur_ptr, params), 3);
  const char *names[3] = {"angle", "radius", "mix"};
  const char *labels[3] = {"Angle", "Radius", "Intensity"};
  for (const int i : IndexRange(3)) {
    PointerRNA param_ptr;
    ASSERT_TRUE(RNA_property_collection_lookup_string(&blur_ptr, params, names[i], &param_ptr))
        << names[i];
    PropertyRNA *label = RNA_struct_find_property(&param_ptr, "label");
    ASSERT_NE(label, nullptr);
    EXPECT_EQ(RNA_property_string_get(&param_ptr, label), labels[i]);
  }
}

TEST_F(PaintLayersFilterKindsTest, drop_shadow_kind_is_a_cpu_computed_effect_with_a_schema)
{
  Material *ma = BKE_material_add(bmain, "RnaDropShadowParamMat");
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "A", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  MaterialPaintLayer *shadow = BKE_paint_layers_filter_add(
      *ma, row, MA_PAINT_LAYER_FILTER_DROP_SHADOW);
  ASSERT_NE(shadow, nullptr);

  /* A CPU-computed effect like the Bevel: the silhouette is spread, blurred and offset, a
   * sequence of passes no single shader kernel lays, so no GPU pass exists for it. */
  EXPECT_TRUE(paint_layer_kind_has_flag(*shadow, PAINT_LAYER_KIND_ACTS_ON_BELOW));
  EXPECT_TRUE(paint_layer_kind_has_flag(*shadow, PAINT_LAYER_KIND_NEEDS_BAKE));
  EXPECT_FALSE(paint_layer_kind_has_flag(*shadow, PAINT_LAYER_KIND_GPU_COMPUTED));
  EXPECT_TRUE(paint_layer_kind_has_flag(*shadow, PAINT_LAYER_KIND_MASK_NEEDS_LUMA));
  /* Its result is a layer with an alpha of its own, so the shadow shows on the empty map; no other
   * kind opts in, and every one of them keeps its colour-only result. */
  EXPECT_TRUE(paint_layer_kind_result_has_alpha(*shadow));
  MaterialPaintLayer *bevel = BKE_paint_layers_filter_add(*ma, row, MA_PAINT_LAYER_FILTER_BEVEL);
  ASSERT_NE(bevel, nullptr);
  EXPECT_FALSE(paint_layer_kind_result_has_alpha(*bevel));

  /* The source defaults to the alpha a stroke casts off, the cast to the map's lower left, the
   * blur and the offset to a visible shadow, the tint to black. */
  EXPECT_EQ(paint_layer_kind_param_int_get(*shadow, PAINT_LAYER_DROP_SHADOW_SOURCE),
            PAINT_LAYER_DROP_SHADOW_SOURCE_ALPHA);
  EXPECT_FLOAT_EQ(paint_layer_kind_param_get(*shadow, PAINT_LAYER_DROP_SHADOW_ANGLE), 135.0f);
  EXPECT_FLOAT_EQ(paint_layer_kind_param_get(*shadow, PAINT_LAYER_DROP_SHADOW_DISTANCE), 4.0f);
  EXPECT_FLOAT_EQ(paint_layer_kind_param_get(*shadow, PAINT_LAYER_DROP_SHADOW_SIZE), 8.0f);
  EXPECT_FLOAT_EQ(paint_layer_kind_param_get(*shadow, PAINT_LAYER_DROP_SHADOW_SPREAD), 0.0f);
  EXPECT_FLOAT_EQ(paint_layer_kind_param_get(*shadow, PAINT_LAYER_DROP_SHADOW_OPACITY), 1.0f);
  float color[4];
  paint_layer_kind_param_color_get(*shadow, PAINT_LAYER_DROP_SHADOW_COLOR, color);
  EXPECT_FLOAT_EQ(color[0], 0.0f);
  EXPECT_FLOAT_EQ(color[3], 1.0f);

  /* The RNA enum names the kind, and the collection lists exactly the schema's seven parameters,
   * labelled. */
  PointerRNA shadow_ptr = RNA_pointer_create_discrete(&ma->id, RNA_MaterialPaintLayer, shadow);
  PropertyRNA *filter_prop = RNA_struct_find_property(&shadow_ptr, "filter_type");
  ASSERT_NE(filter_prop, nullptr);
  const char *identifier = nullptr;
  ASSERT_TRUE(RNA_property_enum_identifier(nullptr,
                                           &shadow_ptr,
                                           filter_prop,
                                           MA_PAINT_LAYER_FILTER_DROP_SHADOW,
                                           &identifier));
  EXPECT_STREQ(identifier, "DROP_SHADOW");
  PropertyRNA *params = RNA_struct_find_property(&shadow_ptr, "kind_params");
  ASSERT_NE(params, nullptr);
  EXPECT_EQ(RNA_property_collection_length(&shadow_ptr, params), 7);
  const char *names[7] = {"source",
                          "angle",
                          "distance",
                          "size",
                          "spread",
                          "opacity",
                          "color"};
  for (const int i : IndexRange(7)) {
    PointerRNA param_ptr;
    ASSERT_TRUE(RNA_property_collection_lookup_string(&shadow_ptr, params, names[i], &param_ptr))
        << names[i];
    PropertyRNA *label = RNA_struct_find_property(&param_ptr, "label");
    ASSERT_NE(label, nullptr);
  }
}

TEST_F(PaintLayersFilterKindsTest, mask_outline_kind_is_a_cpu_computed_effect_with_a_schema)
{
  Material *ma = BKE_material_add(bmain, "RnaMaskOutlineParamMat");
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "A", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  MaterialPaintLayer *outline = BKE_paint_layers_filter_add(
      *ma, row, MA_PAINT_LAYER_FILTER_MASK_OUTLINE);
  ASSERT_NE(outline, nullptr);

  /* A CPU-computed effect like the Bevel: the band is a distance field the live graph cannot
   * express, so no GPU pass exists for it. */
  EXPECT_TRUE(paint_layer_kind_has_flag(*outline, PAINT_LAYER_KIND_ACTS_ON_BELOW));
  EXPECT_TRUE(paint_layer_kind_has_flag(*outline, PAINT_LAYER_KIND_NEEDS_BAKE));
  EXPECT_FALSE(paint_layer_kind_has_flag(*outline, PAINT_LAYER_KIND_GPU_COMPUTED));
  /* A content row's result is a ring layer with an alpha of its own, like the Drop Shadow's. */
  EXPECT_TRUE(paint_layer_kind_has_flag(*outline, PAINT_LAYER_KIND_RESULT_HAS_ALPHA));

  /* The band defaults to the outside of the border, a width of four and no blur, the falloff
   * halfway between the line and the smoothstep. */
  EXPECT_EQ(paint_layer_kind_param_int_get(*outline, PAINT_LAYER_MASK_OUTLINE_POSITION),
            PAINT_LAYER_MASK_OUTLINE_OUTSIDE);
  EXPECT_FLOAT_EQ(paint_layer_kind_param_get(*outline, PAINT_LAYER_MASK_OUTLINE_START_BLACK), 0.0f);
  EXPECT_FLOAT_EQ(paint_layer_kind_param_get(*outline, PAINT_LAYER_MASK_OUTLINE_THRESHOLD), 0.5f);
  EXPECT_FLOAT_EQ(paint_layer_kind_param_get(*outline, PAINT_LAYER_MASK_OUTLINE_WIDTH), 4.0f);
  EXPECT_FLOAT_EQ(paint_layer_kind_param_get(*outline, PAINT_LAYER_MASK_OUTLINE_BLUR), 0.0f);
  EXPECT_FLOAT_EQ(paint_layer_kind_param_get(*outline, PAINT_LAYER_MASK_OUTLINE_SHAPE), 0.5f);
  /* The shape is read from the alpha, and the ring is white. */
  EXPECT_EQ(paint_layer_kind_param_int_get(*outline, PAINT_LAYER_MASK_OUTLINE_SOURCE),
            PAINT_LAYER_DROP_SHADOW_SOURCE_ALPHA);
  float color[4];
  paint_layer_kind_param_color_get(*outline, PAINT_LAYER_MASK_OUTLINE_COLOR, color);
  for (const int c : IndexRange(4)) {
    EXPECT_FLOAT_EQ(color[c], 1.0f) << c;
  }

  /* The RNA enum names the kind, and the collection lists exactly the schema's eight parameters. */
  PointerRNA outline_ptr = RNA_pointer_create_discrete(&ma->id, RNA_MaterialPaintLayer, outline);
  PropertyRNA *filter_prop = RNA_struct_find_property(&outline_ptr, "filter_type");
  ASSERT_NE(filter_prop, nullptr);
  const char *identifier = nullptr;
  ASSERT_TRUE(RNA_property_enum_identifier(nullptr,
                                           &outline_ptr,
                                           filter_prop,
                                           MA_PAINT_LAYER_FILTER_MASK_OUTLINE,
                                           &identifier));
  EXPECT_STREQ(identifier, "MASK_OUTLINE");
  PropertyRNA *params = RNA_struct_find_property(&outline_ptr, "kind_params");
  ASSERT_NE(params, nullptr);
  EXPECT_EQ(RNA_property_collection_length(&outline_ptr, params), 8);
  const char *names[8] = {"position",
                          "start_from_black",
                          "threshold",
                          "width",
                          "blur",
                          "curve_shape",
                          "source",
                          "color"};
  for (const int i : IndexRange(8)) {
    PointerRNA param_ptr;
    ASSERT_TRUE(RNA_property_collection_lookup_string(&outline_ptr, params, names[i], &param_ptr))
        << names[i];
    PropertyRNA *label = RNA_struct_find_property(&param_ptr, "label");
    ASSERT_NE(label, nullptr);
  }
}

TEST_F(PaintLayersFilterKindsTest, fill_area_kind_is_a_cpu_computed_effect_with_a_schema)
{
  Material *ma = BKE_material_add(bmain, "RnaFillAreaParamMat");
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "A", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  MaterialPaintLayer *fill = BKE_paint_layers_filter_add(
      *ma, row, MA_PAINT_LAYER_FILTER_FILL_AREA);
  ASSERT_NE(fill, nullptr);

  /* A CPU-computed effect like the Bevel: the flood and the diffusion are whole-map passes, so no
   * GPU pass exists for it. */
  EXPECT_TRUE(paint_layer_kind_has_flag(*fill, PAINT_LAYER_KIND_ACTS_ON_BELOW));
  EXPECT_TRUE(paint_layer_kind_has_flag(*fill, PAINT_LAYER_KIND_NEEDS_BAKE));
  EXPECT_FALSE(paint_layer_kind_has_flag(*fill, PAINT_LAYER_KIND_GPU_COMPUTED));
  /* The result is a layer of its own: a hole between strokes has no coverage to fill into, and a
   * mask's fill is brought back to one grey. */
  EXPECT_TRUE(paint_layer_kind_has_flag(*fill, PAINT_LAYER_KIND_RESULT_HAS_ALPHA));
  EXPECT_TRUE(paint_layer_kind_has_flag(*fill, PAINT_LAYER_KIND_MASK_NEEDS_LUMA));

  /* The border defaults to the alpha a stroke closes with, a region open at the map's border
   * stays open, the fill diffuses the border's own colours over two dozen passes. */
  EXPECT_EQ(paint_layer_kind_param_int_get(*fill, PAINT_LAYER_FILL_AREA_SOURCE),
            PAINT_LAYER_FILL_AREA_SOURCE_ALPHA);
  EXPECT_FLOAT_EQ(paint_layer_kind_param_get(*fill, PAINT_LAYER_FILL_AREA_THRESHOLD), 0.5f);
  EXPECT_FLOAT_EQ(paint_layer_kind_param_get(*fill, PAINT_LAYER_FILL_AREA_DEBUG), 0.0f);
  EXPECT_EQ(paint_layer_kind_param_int_get(*fill, PAINT_LAYER_FILL_AREA_UV_BORDER),
            PAINT_LAYER_FILL_AREA_UV_BORDER_IGNORE);
  EXPECT_EQ(paint_layer_kind_param_int_get(*fill, PAINT_LAYER_FILL_AREA_COLOR_MODE),
            PAINT_LAYER_FILL_AREA_COLOR_MODE_SOURCE);
  EXPECT_FLOAT_EQ(paint_layer_kind_param_get(*fill, PAINT_LAYER_FILL_AREA_BLUR), 0.0f);
  EXPECT_FLOAT_EQ(paint_layer_kind_param_get(*fill, PAINT_LAYER_FILL_AREA_ITERATIONS), 24.0f);
  float color[4];
  paint_layer_kind_param_color_get(*fill, PAINT_LAYER_FILL_AREA_COLOR, color);
  EXPECT_FLOAT_EQ(color[0], 1.0f);

  /* The RNA enum names the kind, and the collection lists exactly the schema's eight parameters,
   * labelled. */
  PointerRNA fill_ptr = RNA_pointer_create_discrete(&ma->id, RNA_MaterialPaintLayer, fill);
  PropertyRNA *filter_prop = RNA_struct_find_property(&fill_ptr, "filter_type");
  ASSERT_NE(filter_prop, nullptr);
  const char *identifier = nullptr;
  ASSERT_TRUE(RNA_property_enum_identifier(nullptr,
                                           &fill_ptr,
                                           filter_prop,
                                           MA_PAINT_LAYER_FILTER_FILL_AREA,
                                           &identifier));
  EXPECT_STREQ(identifier, "FILL_AREA");
  PropertyRNA *params = RNA_struct_find_property(&fill_ptr, "kind_params");
  ASSERT_NE(params, nullptr);
  EXPECT_EQ(RNA_property_collection_length(&fill_ptr, params), 8);
  const char *names[8] = {"source",
                          "threshold",
                          "debug",
                          "uv_border",
                          "color_mode",
                          "color",
                          "blur",
                          "iterations"};
  const char *labels[8] = {"Area Detection",
                           "Area Detection Threshold",
                           "Debug Area Detection",
                           "UV Border Behavior",
                           "Color Mode",
                           "Fill Color",
                           "Blur Intensity",
                           "Diffusion Iterations"};
  for (const int i : IndexRange(8)) {
    PointerRNA param_ptr;
    ASSERT_TRUE(RNA_property_collection_lookup_string(&fill_ptr, params, names[i], &param_ptr))
        << names[i];
    PropertyRNA *label = RNA_struct_find_property(&param_ptr, "label");
    ASSERT_NE(label, nullptr);
    EXPECT_EQ(RNA_property_string_get(&param_ptr, label), labels[i]);
  }
}

TEST_F(PaintLayersFilterKindsTest, height_to_normal_kind_is_a_cross_channel_effect_with_a_schema)
{
  Material *ma = BKE_material_add(bmain, "RnaHeightToNormalParamMat");
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "A", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  MaterialPaintLayer *to_normal = BKE_paint_layers_filter_add(
      *ma, row, MA_PAINT_LAYER_FILTER_HEIGHT_TO_NORMAL);
  ASSERT_NE(to_normal, nullptr);

  /* A CPU-computed cross-channel effect: it reads the Height the row lays and lays into its own
   * channel, which is the Normal's -- the one filter a Normal may carry besides the Normal
   * Convert. No GPU pass exists for it, and a mask item has no channel to lay a normal into. */
  EXPECT_TRUE(paint_layer_kind_has_flag(*to_normal, PAINT_LAYER_KIND_ACTS_ON_BELOW));
  EXPECT_TRUE(paint_layer_kind_has_flag(*to_normal, PAINT_LAYER_KIND_NEEDS_BAKE));
  EXPECT_FALSE(paint_layer_kind_has_flag(*to_normal, PAINT_LAYER_KIND_GPU_COMPUTED));
  EXPECT_TRUE(paint_layer_kind_has_flag(*to_normal, PAINT_LAYER_KIND_NORMAL_SAFE));
  EXPECT_TRUE(paint_layer_kind_has_flag(*to_normal, PAINT_LAYER_KIND_READS_CHANNEL_HEIGHT));
  /* The gradients default to the intensity's own scale; the flip to the OpenGL encoding. */
  EXPECT_FLOAT_EQ(paint_layer_kind_param_get(*to_normal, PAINT_LAYER_HEIGHT_TO_NORMAL_STRENGTH),
                  1.0f);
  EXPECT_FLOAT_EQ(
      paint_layer_kind_param_get(*to_normal, PAINT_LAYER_HEIGHT_TO_NORMAL_WORLD_UNITS), 0.0f);
  EXPECT_FLOAT_EQ(
      paint_layer_kind_param_get(*to_normal, PAINT_LAYER_HEIGHT_TO_NORMAL_SURFACE_SIZE), 100.0f);
  EXPECT_FLOAT_EQ(
      paint_layer_kind_param_get(*to_normal, PAINT_LAYER_HEIGHT_TO_NORMAL_HEIGHT_DEPTH), 5.0f);
  EXPECT_FLOAT_EQ(
      paint_layer_kind_param_get(*to_normal, PAINT_LAYER_HEIGHT_TO_NORMAL_FLIP_GREEN), 0.0f);

  /* The RNA enum names the kind, and the collection lists exactly the schema's five parameters,
   * labelled. */
  PointerRNA normal_ptr = RNA_pointer_create_discrete(&ma->id, RNA_MaterialPaintLayer, to_normal);
  PropertyRNA *filter_prop = RNA_struct_find_property(&normal_ptr, "filter_type");
  ASSERT_NE(filter_prop, nullptr);
  const char *identifier = nullptr;
  ASSERT_TRUE(RNA_property_enum_identifier(nullptr,
                                           &normal_ptr,
                                           filter_prop,
                                           MA_PAINT_LAYER_FILTER_HEIGHT_TO_NORMAL,
                                           &identifier));
  EXPECT_STREQ(identifier, "HEIGHT_TO_NORMAL");
  PropertyRNA *params = RNA_struct_find_property(&normal_ptr, "kind_params");
  ASSERT_NE(params, nullptr);
  EXPECT_EQ(RNA_property_collection_length(&normal_ptr, params), 5);
  const char *names[5] = {"strength", "world_units", "surface_size", "height_depth", "flip_green"};
  const char *labels[5] = {"Normal Intensity",
                           "Use World Units",
                           "Surface Size (cm)",
                           "Height Depth (cm)",
                           "Flip Green"};
  for (const int i : IndexRange(5)) {
    PointerRNA param_ptr;
    ASSERT_TRUE(RNA_property_collection_lookup_string(&normal_ptr, params, names[i], &param_ptr))
        << names[i];
    PropertyRNA *label = RNA_struct_find_property(&param_ptr, "label");
    ASSERT_NE(label, nullptr);
    EXPECT_EQ(RNA_property_string_get(&param_ptr, label), labels[i]);
  }
}

TEST_F(PaintLayersFilterKindsTest, the_second_batch_pointwise_kinds_carry_their_schemas)
{
  /* The four pointwise kinds of the second batch: the flags the Threshold carries, the schemas
   * the specs name, and the RNA identifiers the dropdown reads. */
  const struct {
    eMaterialPaintLayerFilter filter;
    const char *identifier;
    int params_num;
  } pointwise[] = {
      {MA_PAINT_LAYER_FILTER_SMOOTHSTEP, "SMOOTHSTEP", 2},
      {MA_PAINT_LAYER_FILTER_CONTRAST_LUMINOSITY, "CONTRAST_LUMINOSITY", 2},
      {MA_PAINT_LAYER_FILTER_HEIGHT_ADJUST, "HEIGHT_ADJUST", 3},
  };
  for (const auto &expected : pointwise) {
    Material *ma = BKE_material_add(bmain, "RnaSecondBatchPointwiseMat");
    MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "A", nullptr, PaintLayerPlace::Above);
    ASSERT_NE(row, nullptr);
    MaterialPaintLayer *effect = BKE_paint_layers_filter_add(*ma, row, expected.filter);
    ASSERT_NE(effect, nullptr) << expected.identifier;

    EXPECT_TRUE(paint_layer_kind_has_flag(*effect, PAINT_LAYER_KIND_ACTS_ON_BELOW))
        << expected.identifier;
    EXPECT_FALSE(paint_layer_kind_has_flag(*effect, PAINT_LAYER_KIND_NEEDS_BAKE))
        << expected.identifier;
    EXPECT_FALSE(paint_layer_kind_has_flag(*effect, PAINT_LAYER_KIND_NO_GPU_BRANCH))
        << expected.identifier;
    EXPECT_EQ(paint_layer_kind_find(MA_PAINT_LAYER_SOURCE_FILTER, expected.filter)->params_num,
              expected.params_num)
        << expected.identifier;

    PointerRNA effect_ptr = RNA_pointer_create_discrete(&ma->id, RNA_MaterialPaintLayer, effect);
    PropertyRNA *filter_prop = RNA_struct_find_property(&effect_ptr, "filter_type");
    ASSERT_NE(filter_prop, nullptr);
    const char *identifier = nullptr;
    ASSERT_TRUE(RNA_property_enum_identifier(nullptr,
                                             &effect_ptr,
                                             filter_prop,
                                             expected.filter,
                                             &identifier));
    EXPECT_STREQ(identifier, expected.identifier);
  }
}

TEST_F(PaintLayersFilterKindsTest, deband_kind_is_a_pointwise_filter_with_a_mode_and_two_integers)
{
  Material *ma = BKE_material_add(bmain, "RnaDebandParamMat");
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "A", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  MaterialPaintLayer *deband = BKE_paint_layers_filter_add(
      *ma, row, MA_PAINT_LAYER_FILTER_DEBAND);
  ASSERT_NE(deband, nullptr);

  EXPECT_TRUE(paint_layer_kind_has_flag(*deband, PAINT_LAYER_KIND_ACTS_ON_BELOW));
  EXPECT_FALSE(paint_layer_kind_has_flag(*deband, PAINT_LAYER_KIND_NEEDS_BAKE));
  EXPECT_TRUE(paint_layer_kind_has_flag(*deband, PAINT_LAYER_KIND_NO_GPU_BRANCH));
  EXPECT_EQ(paint_layer_kind_param_type(*deband, PAINT_LAYER_DEBAND_MODE),
            PaintLayerKindParamType::Enum);
  EXPECT_EQ(paint_layer_kind_param_type(*deband, PAINT_LAYER_DEBAND_BITS),
            PaintLayerKindParamType::Int);
  EXPECT_EQ(paint_layer_kind_param_type(*deband, PAINT_LAYER_DEBAND_SEED),
            PaintLayerKindParamType::Int);
  EXPECT_EQ(paint_layer_kind_param_int_get(*deband, PAINT_LAYER_DEBAND_MODE),
            PAINT_LAYER_DEBAND_DEBAND);
  EXPECT_EQ(paint_layer_kind_param_int_get(*deband, PAINT_LAYER_DEBAND_BITS), 8);
  EXPECT_EQ(paint_layer_kind_param_int_get(*deband, PAINT_LAYER_DEBAND_SEED), 0);

  /* The RNA enum names the kind: a filter row reads back its DEBAND identifier. */
  PointerRNA deband_ptr = RNA_pointer_create_discrete(&ma->id, RNA_MaterialPaintLayer, deband);
  PropertyRNA *filter_prop = RNA_struct_find_property(&deband_ptr, "filter_type");
  ASSERT_NE(filter_prop, nullptr);
  const int filter_value = RNA_property_enum_get(&deband_ptr, filter_prop);
  EXPECT_EQ(filter_value, MA_PAINT_LAYER_FILTER_DEBAND);
  const char *identifier = nullptr;
  ASSERT_TRUE(RNA_property_enum_identifier(
      nullptr, &deband_ptr, filter_prop, filter_value, &identifier));
  EXPECT_STREQ(identifier, "DEBAND");
}

TEST_F(PaintLayersFilterKindsTest, normal_convert_kind_alone_carries_the_normal_safe_flag)
{
  Material *ma = BKE_material_add(bmain, "RnaNormalConvertParamMat");
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "A", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  MaterialPaintLayer *convert = BKE_paint_layers_filter_add(
      *ma, row, MA_PAINT_LAYER_FILTER_NORMAL_CONVERT);
  MaterialPaintLayer *invert = BKE_paint_layers_filter_add(*ma, row, MA_PAINT_LAYER_FILTER_INVERT);
  ASSERT_NE(convert, nullptr);
  ASSERT_NE(invert, nullptr);

  /* The flag sits past the eighth bit, and the Normal Convert and the Height To Normal alone carry it,
   * so every other filter keeps the Normal channel closed. */
  EXPECT_TRUE(paint_layer_kind_has_flag(*convert, PAINT_LAYER_KIND_NORMAL_SAFE));
  EXPECT_TRUE(paint_layer_kind_has_flag(*convert, PAINT_LAYER_KIND_ACTS_ON_BELOW));
  EXPECT_FALSE(paint_layer_kind_has_flag(*convert, PAINT_LAYER_KIND_NEEDS_BAKE));
  EXPECT_TRUE(paint_layer_kind_has_flag(*convert, PAINT_LAYER_KIND_NO_GPU_BRANCH));
  EXPECT_FALSE(paint_layer_kind_has_flag(*invert, PAINT_LAYER_KIND_NORMAL_SAFE));
  EXPECT_EQ(paint_layer_kind_param_type(*convert, PAINT_LAYER_NORMAL_CONVERT_MODE),
            PaintLayerKindParamType::Enum);
  EXPECT_EQ(paint_layer_kind_param_int_get(*convert, PAINT_LAYER_NORMAL_CONVERT_NORMALIZE), 1)
      << "the normalize starts on";
}

TEST_F(PaintLayersFilterKindsTest, rna_color_balance_reaches_the_payload)
{
  Material *ma = BKE_material_add(bmain, "RnaColorBalanceMat");
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "A", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  MaterialPaintLayer *plain = BKE_paint_layers_filter_add(*ma, row, MA_PAINT_LAYER_FILTER_CLAMP);
  MaterialPaintLayer *balance_row = BKE_paint_layers_filter_add(
      *ma, row, MA_PAINT_LAYER_FILTER_COLOR_BALANCE);
  ASSERT_NE(plain, nullptr);
  ASSERT_NE(balance_row, nullptr);

  PointerRNA plain_ptr = RNA_pointer_create_discrete(&ma->id, RNA_MaterialPaintLayer, plain);
  PropertyRNA *plain_supported = RNA_struct_find_property(&plain_ptr, "color_balance_supported");
  ASSERT_NE(plain_supported, nullptr);
  EXPECT_FALSE(RNA_property_boolean_get(&plain_ptr, plain_supported));
  /* A kind without the parameter keeps its payload null through the pointer's getter too. */
  EXPECT_EQ(RNA_pointer_get(&plain_ptr, "kind_color_balance").data, nullptr);

  PointerRNA balance_ptr = RNA_pointer_create_discrete(&ma->id, RNA_MaterialPaintLayer,
                                                       balance_row);
  PropertyRNA *supported = RNA_struct_find_property(&balance_ptr, "color_balance_supported");
  ASSERT_NE(supported, nullptr);
  EXPECT_TRUE(RNA_property_boolean_get(&balance_ptr, supported));

  /* The pointer getter ensures the payload: the read hands back the neutral balance. */
  PointerRNA payload = RNA_pointer_get(&balance_ptr, "kind_color_balance");
  ASSERT_NE(payload.data, nullptr);
  EXPECT_EQ(std::string(RNA_struct_identifier(payload.type)), "MaterialPaintLayerColorBalance");

  PropertyRNA *method = RNA_struct_find_property(&payload, "method");
  ASSERT_NE(method, nullptr);
  EXPECT_EQ(RNA_property_type(method), PROP_ENUM);
  EXPECT_EQ(RNA_property_enum_get(&payload, method), MA_PAINT_LAYER_COLOR_BALANCE_LGG);

  /* The values are plain linear floats a picker writes straight into the payload: a lift of a
   * quarter per channel lands in the row's own storage. */
  PropertyRNA *lift = RNA_struct_find_property(&payload, "lift");
  ASSERT_NE(lift, nullptr);
  EXPECT_EQ(RNA_property_type(lift), PROP_FLOAT);
  EXPECT_EQ(RNA_property_array_length(&payload, lift), 3);
  EXPECT_FLOAT_EQ(RNA_property_float_get_index(&payload, lift, 0), 0.0f);
  RNA_property_float_set_index(&payload, lift, 1, 0.25f);
  EXPECT_FLOAT_EQ(balance_row->color_balance->lift[1], 0.25f);
  /* The picker's range is the soft one; the hard range is wider so a value can be typed past it. */
  float range_min = 0.0f, range_max = 0.0f, step = 0.0f, precision = 0.0f;
  RNA_property_float_ui_range(&payload, lift, &range_min, &range_max, &step, &precision);
  EXPECT_FLOAT_EQ(range_min, -1.0f);
  EXPECT_FLOAT_EQ(range_max, 1.0f);
}

TEST_F(PaintLayersFilterKindsTest, rna_channel_mixer_reaches_the_payload)
{
  Material *ma = BKE_material_add(bmain, "RnaChannelMixerMat");
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "A", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  MaterialPaintLayer *plain = BKE_paint_layers_filter_add(*ma, row, MA_PAINT_LAYER_FILTER_CLAMP);
  MaterialPaintLayer *mixer_row = BKE_paint_layers_filter_add(
      *ma, row, MA_PAINT_LAYER_FILTER_CHANNEL_MIXER);
  ASSERT_NE(plain, nullptr);
  ASSERT_NE(mixer_row, nullptr);

  PointerRNA plain_ptr = RNA_pointer_create_discrete(&ma->id, RNA_MaterialPaintLayer, plain);
  PropertyRNA *plain_supported = RNA_struct_find_property(&plain_ptr, "channel_mixer_supported");
  ASSERT_NE(plain_supported, nullptr);
  EXPECT_FALSE(RNA_property_boolean_get(&plain_ptr, plain_supported));
  /* A kind without the parameter keeps its payload null through the pointer's getter too. */
  EXPECT_EQ(RNA_pointer_get(&plain_ptr, "kind_channel_mixer").data, nullptr);

  PointerRNA mixer_ptr = RNA_pointer_create_discrete(&ma->id, RNA_MaterialPaintLayer, mixer_row);
  PropertyRNA *supported = RNA_struct_find_property(&mixer_ptr, "channel_mixer_supported");
  ASSERT_NE(supported, nullptr);
  EXPECT_TRUE(RNA_property_boolean_get(&mixer_ptr, supported));
  EXPECT_EQ(RNA_enum_get(&mixer_ptr, "filter_type"), MA_PAINT_LAYER_FILTER_CHANNEL_MIXER);

  /* The pointer getter ensures the payload: the read hands back the identity matrix. */
  PointerRNA payload = RNA_pointer_get(&mixer_ptr, "kind_channel_mixer");
  ASSERT_NE(payload.data, nullptr);
  EXPECT_EQ(std::string(RNA_struct_identifier(payload.type)), "MaterialPaintLayerChannelMixer");

  PropertyRNA *green = RNA_struct_find_property(&payload, "green_row");
  ASSERT_NE(green, nullptr);
  EXPECT_EQ(RNA_property_type(green), PROP_FLOAT);
  EXPECT_EQ(RNA_property_array_length(&payload, green), 3);
  EXPECT_FLOAT_EQ(RNA_property_float_get_index(&payload, green, 1), 1.0f);
  RNA_property_float_set_index(&payload, green, 0, 0.5f);
  EXPECT_FLOAT_EQ(mixer_row->channel_mixer->green_row[0], 0.5f);
  float range_min = 0.0f, range_max = 0.0f;
  RNA_property_float_range(&payload, green, &range_min, &range_max);
  EXPECT_FLOAT_EQ(range_min, -2.0f);
  EXPECT_FLOAT_EQ(range_max, 2.0f);
}

}  // namespace blender::bke::tests
