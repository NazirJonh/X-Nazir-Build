/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * The CPU/GPU parity of the procedural generators, checked as the graph-eval fixture checks the
 * filters: the generated tree is evaluated at a pixel by the tiny interpreter -- which computes the
 * texture nodes through the same #BLI_noise.hh and #BLI_procedural_texture.hh entries the real
 * multi-functions call -- and compared with the CPU composite's own evaluation of the row at the
 * same coordinate. A mis-wired socket, a placement applied on one side only, or a parameter read at
 * the wrong schema index shows up as a texel that disagrees.
 *
 * The coordinate both sides read is the texel's own UV: the graph's Texture Coordinate node gives
 * the reference texel's centre for every one of its outputs, and the CPU payload of a row with no
 * surface (`approximate`) reads the texel's UV too, which is the contract of the approximate
 * preview a UV, an Object and a Generated row share in these tests. What differs per space is the
 * output socket the graph wires, and that wiring is what a table row per space exercises.
 *
 * The factor tables run the parameter extremes of each kind -- detail 0 and 15, randomness 0 and
 * 1, distortion 0 and its practical maximum, seed shifts -- at several texels, over a colour and a
 * scalar channel, at opacity 1, 0.5 and 0.
 */

#include "BKE_paint_layers_generator_registry.hh"

#include "BLI_utildefines.h"

#include "paint_layers_graph_eval_test_fixture.hh"

namespace blender::bke::tests {

namespace {

/** The texels of the 4x4 reference grid the tables read: two corners and two mid-grid points. */
constexpr int kParityTexels[][2] = {{0, 0}, {1, 1}, {3, 0}, {2, 3}};
constexpr int kParityTexelsNum = 4;

}  // namespace

/** The one check every table row lands on: the CPU sample and the graph agree at every texel. */
static void expect_parity(PaintLayersGraphEvalTest &fixture,
                          const eMaterialPaintChannel channel,
                          const float tolerance = 1e-6f)
{
  Vector<PaintMaterialCompositeImageLayer> layers;
  EXPECT_TRUE(BKE_paint_layers_composite_image_layers(*fixture.ma, channel, layers));
  ASSERT_FALSE(layers.is_empty());
  ASSERT_TRUE(layers[0].gradient.enabled)
      << "channel " << int(channel) << ": the row does not generate";
  const PaintMaterialCompositeGradient generator = layers[0].gradient;
  for (const int t : IndexRange(kParityTexelsNum)) {
    const int x = kParityTexels[t][0];
    const int y = kParityTexels[t][1];
    float rgba[4];
    ASSERT_TRUE(BKE_paint_material_generator_sample(generator, x, y, 4, 4, rgba));
    const RGBA graph = fixture.graph_pixel(channel, x, y);
    EXPECT_NEAR(graph.r, rgba[0], tolerance) << "texel " << x << "," << y;
    EXPECT_NEAR(graph.g, rgba[1], tolerance) << "texel " << x << "," << y;
    EXPECT_NEAR(graph.b, rgba[2], tolerance) << "texel " << x << "," << y;
  }
}

/* -------------------------------------------------------------------- */
/** \name Noise
 * \{ */

TEST_F(PaintLayersGraphEvalTest, noise_parity_over_the_parameter_extremes)
{
  MaterialPaintLayer *row = add_generator_layer("Noise", MA_PAINT_LAYER_GENERATOR_NOISE);
  ASSERT_NE(row, nullptr);

  struct ParamVector {
    const char *what;
    int param;
    float value;
  };
  /* The slider extremes, one at a time over the defaults: a detail the node clamps at
   * both ends, a distortion the node adds unclamped, the two ends of the roughness, and the scale's
   * practical ends. */
  const ParamVector vectors[] = {
      {"defaults", -1, 0.0f},
      {"detail 0", PAINT_LAYER_GEN_NOISE_DETAIL, 0.0f},
      {"detail 15", PAINT_LAYER_GEN_NOISE_DETAIL, 15.0f},
      {"roughness 0", PAINT_LAYER_GEN_NOISE_ROUGHNESS, 0.0f},
      {"roughness 1", PAINT_LAYER_GEN_NOISE_ROUGHNESS, 1.0f},
      {"lacunarity 1", PAINT_LAYER_GEN_NOISE_LACUNARITY, 1.0f},
      {"lacunarity 8", PAINT_LAYER_GEN_NOISE_LACUNARITY, 8.0f},
      {"distortion 0", PAINT_LAYER_GEN_NOISE_DISTORTION, 0.0f},
      {"distortion 4", PAINT_LAYER_GEN_NOISE_DISTORTION, 4.0f},
      {"scale 0.01", PAINT_LAYER_GEN_SCALE, 0.01f},
      {"scale 1000", PAINT_LAYER_GEN_SCALE, 1000.0f},
  };
  for (const ParamVector &vector : vectors) {
    SCOPED_TRACE(vector.what);
    if (vector.param >= 0) {
      set_param(*row, vector.param, vector.value);
    }
    expect_parity(*this, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  }
  /* The normalize switch is an Enum parameter of the schema, not a slider. */
  set_param_int(*row, PAINT_LAYER_GEN_NOISE_NORMALIZE, 0);
  expect_parity(*this, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
}

TEST_F(PaintLayersGraphEvalTest, noise_parity_over_the_types_and_seeds)
{
  MaterialPaintLayer *row = add_generator_layer("Noise", MA_PAINT_LAYER_GENERATOR_NOISE);
  ASSERT_NE(row, nullptr);

  /* The five families of the summed noise: the two multifractals read Offset, the ridged and the
   * hybrid read Gain, and the hetero terrain reads the offset alone. */
  const int types[] = {SHD_NOISE_FBM,
                       SHD_NOISE_MULTIFRACTAL,
                       SHD_NOISE_RIDGED_MULTIFRACTAL,
                       SHD_NOISE_HYBRID_MULTIFRACTAL,
                       SHD_NOISE_HETERO_TERRAIN};
  for (const int type : types) {
    SCOPED_TRACE(testing::Message() << "type " << type);
    set_param_int(*row, PAINT_LAYER_GEN_NOISE_TYPE, type);
    set_param(*row, PAINT_LAYER_GEN_NOISE_DETAIL, 6.0f);
    set_param(*row, PAINT_LAYER_GEN_NOISE_OFFSET, 0.5f);
    set_param(*row, PAINT_LAYER_GEN_NOISE_GAIN, 0.5f);
    expect_parity(*this, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  }

  /* The seeds move the pattern: every seed lands the noise somewhere else, but the graph and the
   * CPU land it in the same place. */
  for (const int seed : {0, 7, 10000}) {
    SCOPED_TRACE(testing::Message() << "seed " << seed);
    set_param_int(*row, PAINT_LAYER_GEN_SEED, seed);
    expect_parity(*this, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  }
}

TEST_F(PaintLayersGraphEvalTest, noise_parity_over_the_spaces_and_the_placement)
{
  MaterialPaintLayer *row = add_generator_layer("Noise", MA_PAINT_LAYER_GENERATOR_NOISE);
  ASSERT_NE(row, nullptr);

  /* The three spaces wire three outputs of one Texture Coordinate node; the approximate CPU payload
   * of each reads the same texel UV, so the wiring is what the table checks. */
  const int spaces[] = {PAINT_LAYER_GEN_SPACE_UV,
                        PAINT_LAYER_GEN_SPACE_OBJECT,
                        PAINT_LAYER_GEN_SPACE_GENERATED};
  for (const int space : spaces) {
    SCOPED_TRACE(testing::Message() << "space " << space);
    set_param_int(*row, PAINT_LAYER_GEN_SPACE, space);
    expect_parity(*this, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  }

  /* The placement: a Y stretch, a quarter turn, a move, and the 3D counterpart of each -- the
   * rotate-then-scale-then-move order is what the chain and the CPU must share. */
  set_param_int(*row, PAINT_LAYER_GEN_SPACE, PAINT_LAYER_GEN_SPACE_UV);
  set_param(*row, PAINT_LAYER_GEN_SCALE_Y, 2.0f);
  expect_parity(*this, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  set_param(*row, PAINT_LAYER_GEN_ROTATION, 90.0f);
  expect_parity(*this, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  set_param(*row, PAINT_LAYER_GEN_OFFSET_X, 0.25f);
  set_param(*row, PAINT_LAYER_GEN_OFFSET_Y, -0.75f);
  expect_parity(*this, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  set_param_int(*row, PAINT_LAYER_GEN_SPACE, PAINT_LAYER_GEN_SPACE_OBJECT);
  expect_parity(*this, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
}

TEST_F(PaintLayersGraphEvalTest, noise_parity_on_a_scalar_channel_and_over_opacity)
{
  MaterialPaintLayer *row = add_generator_layer("Noise",
                                                MA_PAINT_LAYER_GENERATOR_NOISE,
                                                PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  ASSERT_NE(row, nullptr);
  set_param(*row, PAINT_LAYER_GEN_NOISE_DETAIL, 8.0f);
  /* The scalar channel reads its own value ramp, which the Add seeded with the channel's ends. */
  expect_parity(*this, PAINT_MATERIAL_CHANNEL_ROUGHNESS);

  /* The factor table: the graph blends the ramp's answer over what is below by the row's opacity,
   * exactly what the composite does, so the lerp of the two agrees at every step. */
  float bottom[4];
  BKE_paint_layers_channel_bottom_color(PAINT_MATERIAL_CHANNEL_ROUGHNESS, bottom);
  Vector<PaintMaterialCompositeImageLayer> layers;
  EXPECT_TRUE(
      BKE_paint_layers_composite_image_layers(*ma, PAINT_MATERIAL_CHANNEL_ROUGHNESS, layers));
  ASSERT_FALSE(layers.is_empty());
  ASSERT_TRUE(layers[0].gradient.enabled);
  const PaintMaterialCompositeGradient generator = layers[0].gradient;
  for (const float opacity : {0.0f, 0.5f, 1.0f}) {
    SCOPED_TRACE(testing::Message() << "opacity " << opacity);
    ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, row, opacity));
    for (const int t : IndexRange(kParityTexelsNum)) {
      const int x = kParityTexels[t][0];
      const int y = kParityTexels[t][1];
      float rgba[4];
      ASSERT_TRUE(BKE_paint_material_generator_sample(generator, x, y, 4, 4, rgba));
      const float expected = bottom[0] + (rgba[0] - bottom[0]) * opacity;
      const RGBA graph = graph_pixel(PAINT_MATERIAL_CHANNEL_ROUGHNESS, x, y);
      EXPECT_NEAR(graph.r, expected, 1e-6f);
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Voronoi
 * \{ */

TEST_F(PaintLayersGraphEvalTest, voronoi_parity_over_the_features_and_metrics)
{
  MaterialPaintLayer *row = add_generator_layer("Voronoi", MA_PAINT_LAYER_GENERATOR_VORONOI);
  ASSERT_NE(row, nullptr);

  const int features[] = {SHD_VORONOI_F1,
                          SHD_VORONOI_F2,
                          SHD_VORONOI_SMOOTH_F1,
                          SHD_VORONOI_DISTANCE_TO_EDGE,
                          SHD_VORONOI_N_SPHERE_RADIUS};
  const int metrics[] = {SHD_VORONOI_EUCLIDEAN,
                         SHD_VORONOI_MANHATTAN,
                         SHD_VORONOI_CHEBYCHEV,
                         SHD_VORONOI_MINKOWSKI};
  for (const int feature : features) {
    for (const int metric : metrics) {
      SCOPED_TRACE(testing::Message() << "feature " << feature << " metric " << metric);
      set_param_int(*row, PAINT_LAYER_GEN_VORONOI_FEATURE, feature);
      set_param_int(*row, PAINT_LAYER_GEN_VORONOI_DISTANCE, metric);
      if (metric == SHD_VORONOI_MINKOWSKI) {
        set_param(*row, PAINT_LAYER_GEN_VORONOI_EXPONENT, 1.5f);
      }
      expect_parity(*this, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
    }
  }
}

TEST_F(PaintLayersGraphEvalTest, voronoi_parity_over_randomness_detail_and_cell_color)
{
  MaterialPaintLayer *row = add_generator_layer("Voronoi", MA_PAINT_LAYER_GENERATOR_VORONOI);
  ASSERT_NE(row, nullptr);

  set_param(*row, PAINT_LAYER_GEN_VORONOI_RANDOMNESS, 0.0f);
  expect_parity(*this, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  set_param(*row, PAINT_LAYER_GEN_VORONOI_RANDOMNESS, 1.0f);
  expect_parity(*this, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  set_param(*row, PAINT_LAYER_GEN_VORONOI_DETAIL, 4.0f);
  set_param(*row, PAINT_LAYER_GEN_VORONOI_ROUGHNESS, 0.7f);
  set_param(*row, PAINT_LAYER_GEN_VORONOI_LACUNARITY, 3.0f);
  expect_parity(*this, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  set_param(*row, PAINT_LAYER_GEN_VORONOI_SMOOTHNESS, 1.0f);
  set_param_int(*row, PAINT_LAYER_GEN_VORONOI_FEATURE, SHD_VORONOI_SMOOTH_F1);
  expect_parity(*this, PAINT_MATERIAL_CHANNEL_BASE_COLOR);

  /* The cell colour bypasses the band: the graph wires the node's Color socket and a scalar index
   * of its luminance, the CPU the same hash through the same function. */
  set_param_int(*row, PAINT_LAYER_GEN_VORONOI_OUTPUT, PAINT_LAYER_GEN_VORONOI_OUTPUT_CELL_COLOR);
  set_param_int(*row, PAINT_LAYER_GEN_VORONOI_FEATURE, SHD_VORONOI_F1);
  expect_parity(*this, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  set_param_int(*row, PAINT_LAYER_GEN_VORONOI_FEATURE, SHD_VORONOI_F2);
  expect_parity(*this, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Checker, Brick, Wave
 * \{ */

TEST_F(PaintLayersGraphEvalTest, checker_parity_over_scale_placement_and_seed)
{
  MaterialPaintLayer *row = add_generator_layer("Checker", MA_PAINT_LAYER_GENERATOR_CHECKER);
  ASSERT_NE(row, nullptr);
  expect_parity(*this, PAINT_MATERIAL_CHANNEL_BASE_COLOR);

  set_param(*row, PAINT_LAYER_GEN_SCALE, 0.5f);
  expect_parity(*this, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  set_param(*row, PAINT_LAYER_GEN_SCALE, 8.0f);
  expect_parity(*this, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  /* A diagonal checker: the placement turn is what makes it one. */
  set_param(*row, PAINT_LAYER_GEN_ROTATION, 45.0f);
  expect_parity(*this, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  set_param_int(*row, PAINT_LAYER_GEN_SEED, 5);
  set_param(*row, PAINT_LAYER_GEN_OFFSET_X, 0.5f);
  expect_parity(*this, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
}

TEST_F(PaintLayersGraphEvalTest, brick_parity_over_the_seam_and_the_tint)
{
  MaterialPaintLayer *row = add_generator_layer("Brick", MA_PAINT_LAYER_GENERATOR_BRICK);
  ASSERT_NE(row, nullptr);

  struct ParamVector {
    const char *what;
    int param;
    float value;
  };
  const ParamVector vectors[] = {
      {"offset 0", PAINT_LAYER_GEN_BRICK_OFFSET, 0.0f},
      {"offset 1", PAINT_LAYER_GEN_BRICK_OFFSET, 1.0f},
      {"squash 0.5", PAINT_LAYER_GEN_BRICK_SQUASH, 0.5f},
      {"mortar 0", PAINT_LAYER_GEN_BRICK_MORTAR_SIZE, 0.0f},
      {"mortar 0.125", PAINT_LAYER_GEN_BRICK_MORTAR_SIZE, 0.125f},
      {"mortar smooth 0", PAINT_LAYER_GEN_BRICK_MORTAR_SMOOTH, 0.0f},
      {"mortar smooth 1", PAINT_LAYER_GEN_BRICK_MORTAR_SMOOTH, 1.0f},
      {"bias 0", PAINT_LAYER_GEN_BRICK_BIAS, 0.0f},
      {"bias 1", PAINT_LAYER_GEN_BRICK_BIAS, 1.0f},
      {"brick width 1", PAINT_LAYER_GEN_BRICK_WIDTH, 1.0f},
      {"row height 0.5", PAINT_LAYER_GEN_BRICK_ROW_HEIGHT, 0.5f},
      {"scale 3", PAINT_LAYER_GEN_SCALE, 3.0f},
  };
  for (const ParamVector &vector : vectors) {
    SCOPED_TRACE(vector.what);
    set_param(*row, vector.param, vector.value);
    expect_parity(*this, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  }
  /* The two frequencies are whole rows: they are Int parameters of the schema. */
  set_param_int(*row, PAINT_LAYER_GEN_BRICK_OFFSET_FREQUENCY, 4);
  expect_parity(*this, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  set_param_int(*row, PAINT_LAYER_GEN_BRICK_SQUASH_FREQUENCY, 3);
  expect_parity(*this, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
}

TEST_F(PaintLayersGraphEvalTest, wave_parity_over_types_directions_and_profiles)
{
  MaterialPaintLayer *row = add_generator_layer("Wave", MA_PAINT_LAYER_GENERATOR_WAVE);
  ASSERT_NE(row, nullptr);

  for (const int wave_type : {SHD_WAVE_BANDS, SHD_WAVE_RINGS}) {
    for (const int profile : {SHD_WAVE_PROFILE_SIN, SHD_WAVE_PROFILE_SAW, SHD_WAVE_PROFILE_TRI}) {
      SCOPED_TRACE(testing::Message() << "type " << wave_type << " profile " << profile);
      set_param_int(*row, PAINT_LAYER_GEN_WAVE_TYPE, wave_type);
      set_param_int(*row, PAINT_LAYER_GEN_WAVE_PROFILE, profile);
      expect_parity(*this, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
    }
  }
  for (const int direction : {SHD_WAVE_BANDS_DIRECTION_X,
                              SHD_WAVE_BANDS_DIRECTION_Y,
                              SHD_WAVE_BANDS_DIRECTION_Z,
                              SHD_WAVE_BANDS_DIRECTION_DIAGONAL})
  {
    SCOPED_TRACE(testing::Message() << "bands direction " << direction);
    set_param_int(*row, PAINT_LAYER_GEN_WAVE_TYPE, SHD_WAVE_BANDS);
    set_param_int(*row, PAINT_LAYER_GEN_WAVE_BANDS_DIRECTION, direction);
    expect_parity(*this, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  }
  for (const int direction : {SHD_WAVE_RINGS_DIRECTION_X,
                              SHD_WAVE_RINGS_DIRECTION_Y,
                              SHD_WAVE_RINGS_DIRECTION_Z,
                              SHD_WAVE_RINGS_DIRECTION_SPHERICAL})
  {
    SCOPED_TRACE(testing::Message() << "rings direction " << direction);
    set_param_int(*row, PAINT_LAYER_GEN_WAVE_TYPE, SHD_WAVE_RINGS);
    set_param_int(*row, PAINT_LAYER_GEN_WAVE_RINGS_DIRECTION, direction);
    expect_parity(*this, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  }

  /* The noise that rides the distortion: its detail, scale and roughness are sockets the graph
   * wires from the schema, so the extremes cross the fractual noise too. */
  const struct {
    const char *what;
    int param;
    float value;
  } vectors[] = {
      {"distortion 4", PAINT_LAYER_GEN_WAVE_DISTORTION, 4.0f},
      {"detail 0", PAINT_LAYER_GEN_WAVE_DETAIL, 0.0f},
      {"detail 15", PAINT_LAYER_GEN_WAVE_DETAIL, 15.0f},
      {"detail scale 4", PAINT_LAYER_GEN_WAVE_DETAIL_SCALE, 4.0f},
      {"detail roughness 1", PAINT_LAYER_GEN_WAVE_DETAIL_ROUGHNESS, 1.0f},
      {"phase offset 2.5", PAINT_LAYER_GEN_WAVE_PHASE_OFFSET, 2.5f},
  };
  for (const auto &vector : vectors) {
    SCOPED_TRACE(vector.what);
    set_param(*row, vector.param, vector.value);
    expect_parity(*this, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name The compound generators: Scratches, Weave, Grunge
 * \{ */

TEST_F(PaintLayersGraphEvalTest, scratches_parity_over_the_line_parameters)
{
  MaterialPaintLayer *row = add_generator_layer("Scratches", MA_PAINT_LAYER_GENERATOR_SCRATCHES);
  ASSERT_NE(row, nullptr);
  expect_parity(*this, PAINT_MATERIAL_CHANNEL_BASE_COLOR);

  const struct {
    const char *what;
    int param;
    float value;
  } vectors[] = {
      {"width 0", PAINT_LAYER_GEN_SCRATCHES_WIDTH, 0.0f},
      {"width 0.5", PAINT_LAYER_GEN_SCRATCHES_WIDTH, 0.5f},
      {"soften 0", PAINT_LAYER_GEN_SCRATCHES_SOFTEN, 0.0f},
      {"soften 0.25", PAINT_LAYER_GEN_SCRATCHES_SOFTEN, 0.25f},
      {"stretch 1", PAINT_LAYER_GEN_SCRATCHES_STRETCH, 1.0f},
      {"stretch 20", PAINT_LAYER_GEN_SCRATCHES_STRETCH, 20.0f},
      {"randomness 0", PAINT_LAYER_GEN_SCRATCHES_RANDOMNESS, 0.0f},
      {"randomness 1", PAINT_LAYER_GEN_SCRATCHES_RANDOMNESS, 1.0f},
      {"detail 15", PAINT_LAYER_GEN_SCRATCHES_DETAIL, 15.0f},
      {"roughness 1", PAINT_LAYER_GEN_SCRATCHES_ROUGHNESS, 1.0f},
      {"lacunarity 8", PAINT_LAYER_GEN_SCRATCHES_LACUNARITY, 8.0f},
      {"scale 0.01", PAINT_LAYER_GEN_SCALE, 0.01f},
      {"scale 1000", PAINT_LAYER_GEN_SCALE, 1000.0f},
      {"stretch along y", PAINT_LAYER_GEN_SCALE_Y, 2.0f},
      {"turned", PAINT_LAYER_GEN_ROTATION, 30.0f},
  };
  for (const auto &vector : vectors) {
    SCOPED_TRACE(vector.what);
    set_param(*row, vector.param, vector.value);
    expect_parity(*this, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  }
  /* The 3D group is another group of its own, wired the same way. */
  set_param_int(*row, PAINT_LAYER_GEN_SPACE, PAINT_LAYER_GEN_SPACE_OBJECT);
  expect_parity(*this, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
}

TEST_F(PaintLayersGraphEvalTest, weave_parity_over_the_threads)
{
  MaterialPaintLayer *row = add_generator_layer("Weave", MA_PAINT_LAYER_GENERATOR_WEAVE);
  ASSERT_NE(row, nullptr);
  expect_parity(*this, PAINT_MATERIAL_CHANNEL_BASE_COLOR);

  for (const int pattern : {PAINT_LAYER_GEN_WEAVE_PLAIN, PAINT_LAYER_GEN_WEAVE_TWILL}) {
    SCOPED_TRACE(testing::Message() << "pattern " << pattern);
    set_param_int(*row, PAINT_LAYER_GEN_WEAVE_PATTERN, pattern);
    expect_parity(*this, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  }
  const struct {
    const char *what;
    int param;
    float value;
  } vectors[] = {
      {"thread 0.2", PAINT_LAYER_GEN_WEAVE_THREAD_WIDTH, 0.2f},
      {"thread 0.9", PAINT_LAYER_GEN_WEAVE_THREAD_WIDTH, 0.9f},
      {"gap 0.3", PAINT_LAYER_GEN_WEAVE_GAP, 0.3f},
      {"soften 0", PAINT_LAYER_GEN_WEAVE_SOFTEN, 0.0f},
      {"soften 0.4", PAINT_LAYER_GEN_WEAVE_SOFTEN, 0.4f},
      {"jitter 0.5", PAINT_LAYER_GEN_WEAVE_JITTER, 0.5f},
  };
  for (const auto &vector : vectors) {
    SCOPED_TRACE(vector.what);
    set_param(*row, vector.param, vector.value);
    expect_parity(*this, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  }
  set_param_int(*row, PAINT_LAYER_GEN_SEED, 9);
  expect_parity(*this, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  set_param_int(*row, PAINT_LAYER_GEN_SPACE, PAINT_LAYER_GEN_SPACE_GENERATED);
  expect_parity(*this, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
}

TEST_F(PaintLayersGraphEvalTest, grunge_parity_over_the_dirt)
{
  MaterialPaintLayer *row = add_generator_layer("Grunge", MA_PAINT_LAYER_GENERATOR_GRUNGE);
  ASSERT_NE(row, nullptr);
  expect_parity(*this, PAINT_MATERIAL_CHANNEL_BASE_COLOR);

  const struct {
    const char *what;
    int param;
    float value;
  } vectors[] = {
      {"detail 0", PAINT_LAYER_GEN_GRUNGE_DETAIL, 0.0f},
      {"detail 15", PAINT_LAYER_GEN_GRUNGE_DETAIL, 15.0f},
      {"roughness 0", PAINT_LAYER_GEN_GRUNGE_ROUGHNESS, 0.0f},
      {"roughness 1", PAINT_LAYER_GEN_GRUNGE_ROUGHNESS, 1.0f},
      {"lacunarity 8", PAINT_LAYER_GEN_GRUNGE_LACUNARITY, 8.0f},
      {"distortion 4", PAINT_LAYER_GEN_GRUNGE_DISTORTION, 4.0f},
      {"variation 0", PAINT_LAYER_GEN_GRUNGE_VARIATION_STRENGTH, 0.0f},
      {"variation 1", PAINT_LAYER_GEN_GRUNGE_VARIATION_STRENGTH, 1.0f},
      {"variation scale 10", PAINT_LAYER_GEN_GRUNGE_VARIATION_SCALE, 10.0f},
      {"threshold 0", PAINT_LAYER_GEN_GRUNGE_THRESHOLD, 0.0f},
      {"threshold 1", PAINT_LAYER_GEN_GRUNGE_THRESHOLD, 1.0f},
      {"soften 0", PAINT_LAYER_GEN_GRUNGE_SOFTEN, 0.0f},
      {"soften 0.5", PAINT_LAYER_GEN_GRUNGE_SOFTEN, 0.5f},
      {"scale 0.5", PAINT_LAYER_GEN_SCALE, 0.5f},
  };
  for (const auto &vector : vectors) {
    SCOPED_TRACE(vector.what);
    set_param(*row, vector.param, vector.value);
    expect_parity(*this, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  }
  set_param_int(*row, PAINT_LAYER_GEN_SEED, 1234);
  expect_parity(*this, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  set_param_int(*row, PAINT_LAYER_GEN_SPACE, PAINT_LAYER_GEN_SPACE_OBJECT);
  expect_parity(*this, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name The three roles and the kind switch
 *
 * The same pattern as a correction and as a mask item, and the switch that turns one kind into
 * another without disturbing the role. The composite contract is the mask item's coverage laid by
 * the item's blend and opacity, which the gradient's mask tests already fixed; here a Checker
 * correction and a Noise mask item over one painted row are the check, against the graph.
 * \{ */

TEST_F(PaintLayersGraphEvalTest, generator_as_correction_and_mask_item)
{
  Image *image = add_solid_image("Map", 255, 255, 255, 255);
  make_image_data(image);
  MaterialPaintLayer *painted = add_layer("Row", image, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(painted, nullptr);

  /* The correction: a Checker laid over the row's colour. The CPU lays its ramp at the checker's
   * factor over the row's colour by the correction's opacity; the graph wires the same three
   * nodes. One texel is enough: the checkerboard is exactly 0 or 1 there, whichever side of the
   * cell border the placement falls. */
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, painted, "Checker Correction", MA_PAINT_LAYER_SOURCE_GENERATOR);
  ASSERT_NE(correction, nullptr);
  ASSERT_TRUE(BKE_paint_layers_generator_kind_set(*ma,
                                                 *correction,
                                                 MA_PAINT_LAYER_GENERATOR_CHECKER));

  Vector<PaintMaterialCompositeImageLayer> layers;
  EXPECT_TRUE(BKE_paint_layers_composite_image_layers(*ma, PAINT_MATERIAL_CHANNEL_BASE_COLOR, layers));
  ASSERT_EQ(layers.size(), 1);
  bool found = false;
  RGBA cpu;
  for (const PaintMaterialCompositeCorrection &c : layers[0].content_corrections) {
    if (!c.gradient.enabled) {
      continue;
    }
    float rgba[4];
    ASSERT_TRUE(BKE_paint_material_generator_sample(c.gradient, 1, 1, 4, 4, rgba));
    cpu = {rgba[0], rgba[1], rgba[2], rgba[3]};
    found = true;
  }
  ASSERT_TRUE(found);
  const RGBA graph = graph_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR, 1, 1);
  /* The painted map is white, so the composite reads the checker directly: white laid at the
   * correction's factor. */
  EXPECT_NEAR(graph.r, cpu.r, 1e-6f);
  EXPECT_NEAR(graph.g, cpu.g, 1e-6f);
  EXPECT_NEAR(graph.b, cpu.b, 1e-6f);

  /* The correction leaves, so the mask item below covers the white row itself. */
  ASSERT_TRUE(BKE_paint_layers_correction_remove(*ma, correction));

  /* The mask item: the same generator as the row's mask coverage. */
  MaterialPaintLayer *item = BKE_paint_layers_mask_item_add(
      *ma, painted, MA_PAINT_LAYER_SOURCE_GENERATOR, "Noise Mask");
  ASSERT_NE(item, nullptr);
  ASSERT_TRUE(BKE_paint_layers_generator_kind_set(*ma, *item, MA_PAINT_LAYER_GENERATOR_NOISE));
  set_param(*item, PAINT_LAYER_GEN_NOISE_DETAIL, 4.0f);

  Vector<PaintMaterialCompositeImageLayer> mask_layers;
  EXPECT_TRUE(BKE_paint_layers_composite_image_layers(*ma,
                                                      PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                                      mask_layers));
  ASSERT_EQ(mask_layers.size(), 1);
  found = false;
  for (const PaintMaterialCompositeMaskItem &mask_item : mask_layers[0].mask_items) {
    if (!mask_item.gradient.enabled) {
      continue;
    }
    float rgba[4];
    ASSERT_TRUE(BKE_paint_material_generator_sample(mask_item.gradient, 1, 1, 4, 4, rgba));
    /* The item's grey is the coverage the white row is laid by: the composite reads the same grey
     * and lays the same white over what is below by it, so the lerp of the bottom and the row's
     * white is the answer both give. */
    float bottom[4];
    BKE_paint_layers_channel_bottom_color(PAINT_MATERIAL_CHANNEL_BASE_COLOR, bottom);
    const float coverage = rgba[0];
    const RGBA graph = graph_pixel(PAINT_MATERIAL_CHANNEL_BASE_COLOR, 1, 1);
    EXPECT_NEAR(graph.r, bottom[0] + (1.0f - bottom[0]) * coverage, 1e-6f)
        << "the mask item's grey covers the white row";
    EXPECT_NEAR(graph.g, bottom[1] + (1.0f - bottom[1]) * coverage, 1e-6f);
    EXPECT_NEAR(graph.b, bottom[2] + (1.0f - bottom[2]) * coverage, 1e-6f);
    found = true;
  }
  ASSERT_TRUE(found);
}

TEST_F(PaintLayersGraphEvalTest, kind_switch_keeps_the_row_and_changes_the_pattern)
{
  MaterialPaintLayer *row = add_generator_layer("Pattern", MA_PAINT_LAYER_GENERATOR_CHECKER);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_remove(*ma, row, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, row, PAINT_MATERIAL_CHANNEL_ROUGHNESS), nullptr);

  /* The switch keeps the row's identity and its record: only the pattern and its parameters
   * change. */
  ASSERT_TRUE(
      BKE_paint_layers_generator_kind_set(*ma, *row, MA_PAINT_LAYER_GENERATOR_BRICK));
  EXPECT_EQ(row->source, MA_PAINT_LAYER_SOURCE_GENERATOR);
  EXPECT_EQ(row->kind_subtype, MA_PAINT_LAYER_GENERATOR_BRICK);
  expect_parity(*this, PAINT_MATERIAL_CHANNEL_ROUGHNESS);

  ASSERT_TRUE(
      BKE_paint_layers_generator_kind_set(*ma, *row, MA_PAINT_LAYER_GENERATOR_GRUNGE));
  EXPECT_EQ(row->kind_subtype, MA_PAINT_LAYER_GENERATOR_GRUNGE);
  expect_parity(*this, PAINT_MATERIAL_CHANNEL_ROUGHNESS);

  /* A refused switch leaves the row alone: a kind this build does not know, and a second pass
   * with the same kind. */
  EXPECT_FALSE(BKE_paint_layers_generator_kind_set(*ma,
                                                  *row,
                                                  eMaterialPaintLayerGenerator(99)));
  EXPECT_EQ(row->kind_subtype, MA_PAINT_LAYER_GENERATOR_GRUNGE);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name The bake anchors
 *
 * The space decides what a generator's bake can be cached as: a UV row is one map for every object
 * of the material, an Object or Generated row is one per object. The predicates are what the bake,
 * the planner and the preview read, so the table walks the kinds and the spaces once.
 * \{ */

TEST_F(PaintLayersGraphEvalTest, bake_anchor_predicates_follow_the_space)
{
  for (const eMaterialPaintLayerGenerator generator :
       {MA_PAINT_LAYER_GENERATOR_NOISE,
        MA_PAINT_LAYER_GENERATOR_VORONOI,
        MA_PAINT_LAYER_GENERATOR_CHECKER,
        MA_PAINT_LAYER_GENERATOR_BRICK,
        MA_PAINT_LAYER_GENERATOR_WAVE,
        MA_PAINT_LAYER_GENERATOR_SCRATCHES,
        MA_PAINT_LAYER_GENERATOR_WEAVE,
        MA_PAINT_LAYER_GENERATOR_GRUNGE})
  {
    SCOPED_TRACE(testing::Message() << "generator " << int(generator));
    MaterialPaintLayer *row = add_generator_layer("Pattern", generator);
    ASSERT_NE(row, nullptr);

    EXPECT_TRUE(BKE_paint_layers_generator_supported(*row));
    EXPECT_TRUE(BKE_paint_layers_generator_is_bakeable(*row));
    EXPECT_TRUE(BKE_paint_layers_generator_subtree_is_bakeable(*row));
    /* A fresh row reads the UV: one map for every object. */
    EXPECT_TRUE(BKE_paint_layers_generator_is_shared_bakeable(*row));
    EXPECT_TRUE(BKE_paint_layers_generator_subtree_is_shared_bakeable(*row));

    set_param_int(*row, PAINT_LAYER_GEN_SPACE, PAINT_LAYER_GEN_SPACE_OBJECT);
    EXPECT_FALSE(BKE_paint_layers_generator_is_shared_bakeable(*row));
    EXPECT_FALSE(BKE_paint_layers_generator_subtree_is_shared_bakeable(*row));
    EXPECT_TRUE(BKE_paint_layers_generator_is_bakeable(*row));

    set_param_int(*row, PAINT_LAYER_GEN_SPACE, PAINT_LAYER_GEN_SPACE_GENERATED);
    EXPECT_FALSE(BKE_paint_layers_generator_is_shared_bakeable(*row));
    /* The Generated space reads the orco grid, which the surface of a bake has to carry. */
    EXPECT_TRUE(BKE_paint_layers_generator_subtree_holds_generated(*row));

    set_param_int(*row, PAINT_LAYER_GEN_SPACE, PAINT_LAYER_GEN_SPACE_UV);
    EXPECT_TRUE(BKE_paint_layers_generator_is_shared_bakeable(*row));
    EXPECT_FALSE(BKE_paint_layers_generator_subtree_holds_generated(*row));
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Registry, kind schema and RNA parity
 *
 * The Generator picker's list (`rna_material.cc`) is a static table the makesrna binary compiles,
 * so it cannot be generated from the registry at compile time. The test holds the three lists
 * together instead, one item per subtype: every registry entry carries the subtype, the
 * identifier and the name the RNA enum spells, and the kind schema whose parameters the row's
 * properties read. A generator registered on one side only -- or spelled differently in two --
 * fails here before it can disagree at runtime.
 * \{ */

TEST(generator_registry, matches_the_kind_schemas_and_the_rna_enum)
{
  /* The RNA enum's items, in its own order: the subtype the DNA stores, the identifier and the
   * name the picker shows. */
  static const struct RnaItem {
    int16_t subtype;
    const char *identifier;
    const char *name;
  } rna_items[] = {
      {MA_PAINT_LAYER_GENERATOR_GRADIENT, "GRADIENT", "Gradient"},
      {MA_PAINT_LAYER_GENERATOR_NOISE, "NOISE", "Noise"},
      {MA_PAINT_LAYER_GENERATOR_VORONOI, "VORONOI", "Voronoi"},
      {MA_PAINT_LAYER_GENERATOR_CHECKER, "CHECKER", "Checker"},
      {MA_PAINT_LAYER_GENERATOR_BRICK, "BRICK", "Brick"},
      {MA_PAINT_LAYER_GENERATOR_WAVE, "WAVE", "Wave"},
      {MA_PAINT_LAYER_GENERATOR_SCRATCHES, "SCRATCHES", "Scratches"},
      {MA_PAINT_LAYER_GENERATOR_WEAVE, "WEAVE", "Weave"},
      {MA_PAINT_LAYER_GENERATOR_GRUNGE, "GRUNGE", "Grunge"},
  };

  const Span<const PaintLayerGeneratorInfo> generators = BKE_paint_layers_generator_all();
  ASSERT_EQ(generators.size(), ARRAY_SIZE(rna_items));

  for (const int i : generators.index_range()) {
    const PaintLayerGeneratorInfo &info = generators[i];
    EXPECT_EQ(info.subtype, rna_items[i].subtype) << "entry " << i;
    EXPECT_STREQ(info.identifier, rna_items[i].identifier) << "entry " << i;
    EXPECT_STREQ(info.name, rna_items[i].name) << "entry " << i;

    /* Every entry has the kind schema its parameters are read through, spelled the same and
     * carrying the generating flag the row's participation rides on. */
    const PaintLayerKindInfo *schema = paint_layer_kind_find(MA_PAINT_LAYER_SOURCE_GENERATOR,
                                                            info.subtype);
    ASSERT_NE(schema, nullptr) << "generator " << info.name;
    EXPECT_STREQ(schema->name, info.name) << "generator " << info.name;
    EXPECT_TRUE((schema->flags & PAINT_LAYER_KIND_GENERATES) != 0) << "generator " << info.name;
  }
}

/** \} */

}  // namespace blender::bke::tests
