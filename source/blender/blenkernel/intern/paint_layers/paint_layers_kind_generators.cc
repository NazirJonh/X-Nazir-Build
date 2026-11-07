/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The kind schemas of the Paint Layers procedural generators: the registry entries whose source is
 * #MA_PAINT_LAYER_SOURCE_GENERATOR, kept beside the generator table they answer for. A new
 * generator is a schema here, an entry in that table (#paint_layers_generator_registry.cc), and the two
 * functions the entry names; the lookup in `paint_layers_kind.cc` walks these after its own, so a
 * generator kind is found exactly where every other kind is.
 */

#include "paint_layers_kind.hh"

#include "BLI_utildefines.h"

#include "BKE_paint_layers_generator.hh"
#include "BKE_paint_layers_generator_registry.hh"

#include "DNA_material_types.h"
#include "DNA_node_types.h"

/* The identifiers of the shared generator groups' interface sockets, which are the sockets a
 * compound generator's parameters drive. */
#include "paint_material_generator_group_ids.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Procedural generator schemas
 *
 * Every generator's schema starts with the same eight placement parameters, in the slots
 * #eMaterialPaintLayerGeneratorParam names, and ends with the Ramp parameter the colour channels
 * read from #MaterialPaintLayer::ramp. The placement is spelled once by
 * #GENERATOR_PLACEMENT_PARAMS, which the schemas expand at their start: the eight entries are
 * identical but for the pattern's own scale, which is the macro's one argument -- a Noise sits at
 * the Noise node's own scale, a Weave or a Grunge wants a coarser one.
 *
 * The values of an Enum parameter are the codes the shader node itself reads (#eNodeNoiseTexture_Type
 * and friends), so the number the CPU math and the generated node both see is one number with one
 * meaning. \{ */

/**
 * The eight placement entries every generator's schema starts with, in the slots
 * #eMaterialPaintLayerGeneratorParam lays out: the coordinate space, the X and Y scales, the
 * turn, the two offsets, the seed and the scale lock. The order here is the DNA's order -- the
 * slots must not move.
 */
#define GENERATOR_PLACEMENT_PARAMS(default_scale)                                              \
  {"space",                                                                                    \
   "Space",                                                                                    \
   float(MA_PAINT_LAYER_GRADIENT_SPACE_UV),                                                    \
   0.0f,                                                                                       \
   float(MA_PAINT_LAYER_GRADIENT_SPACE_GENERATED),                                             \
   PaintLayerKindParamType::Enum,                                                              \
   generator_space_items,                                                                      \
   int(std::size(generator_space_items))},                                                     \
      {"scale", "Scale X", default_scale, 0.01f, 1000.0f},                                     \
      /* An absolute scale like the X one; read as the X scale while `scale_lock` is on. */    \
      {"scale_y", "Scale Y", default_scale, 0.01f, 1000.0f},                                   \
      {"rotation", "Rotation", 0.0f, 0.0f, 360.0f},                                            \
      {"offset_x", "Offset X", 0.0f, -1000.0f, 1000.0f},                                       \
      {"offset_y", "Offset Y", 0.0f, -1000.0f, 1000.0f},                                       \
      /* The hash offset of the coordinate: the same pattern, moved to another place. */       \
      {"seed", "Seed", 0.0f, 0.0f, 10000.0f, PaintLayerKindParamType::Int},                    \
      /* On by default, as the mapping of a layer starts: one slider scales both axes. */      \
      {"scale_lock", "Lock Scale", 1.0f, 0.0f, 1.0f, PaintLayerKindParamType::Bool},

static const PaintLayerKindEnumItem generator_space_items[] = {
    {MA_PAINT_LAYER_GRADIENT_SPACE_UV, "UV", "UV"},
    {MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT, "OBJECT", "Object"},
    {MA_PAINT_LAYER_GRADIENT_SPACE_GENERATED, "GENERATED", "Generated"},
};
static_assert(int(std::size(generator_space_items)) ==
              MA_PAINT_LAYER_GRADIENT_SPACE_GENERATED + 1);

static const PaintLayerKindEnumItem generator_switch_items[] = {
    {PAINT_LAYER_GEN_OFF, "OFF", "Off"},
    {PAINT_LAYER_GEN_ON, "ON", "On"},
};
static_assert(int(std::size(generator_switch_items)) - 1 == PAINT_LAYER_GEN_ON);

/* The Noise node's own types, which the merged Musgrave node's types became. */
static const PaintLayerKindEnumItem generator_noise_type_items[] = {
    {SHD_NOISE_MULTIFRACTAL, "MULTIFRACTAL", "Multifractal"},
    {SHD_NOISE_FBM, "FBM", "fBm"},
    {SHD_NOISE_HYBRID_MULTIFRACTAL, "HYBRID_MULTIFRACTAL", "Hybrid Multifractal"},
    {SHD_NOISE_RIDGED_MULTIFRACTAL, "RIDGED_MULTIFRACTAL", "Ridged Multifractal"},
    {SHD_NOISE_HETERO_TERRAIN, "HETERO_TERRAIN", "Hetero Terrain"},
};
static_assert(int(std::size(generator_noise_type_items)) == 5);

static const PaintLayerKindParam noise_generator_params[] = {
    /* The Noise node's own default scale. */
    GENERATOR_PLACEMENT_PARAMS(5.0f)
    {"noise_type",
     "Noise Type",
     float(SHD_NOISE_FBM),
     0.0f,
     float(SHD_NOISE_HETERO_TERRAIN),
     PaintLayerKindParamType::Enum,
     generator_noise_type_items,
     int(std::size(generator_noise_type_items))},
    /* The node's own default: a normalized sum reads as a 0..1 factor. */
    {"normalize",
     "Normalize",
     1.0f,
     0.0f,
     1.0f,
     PaintLayerKindParamType::Enum,
     generator_switch_items,
     int(std::size(generator_switch_items))},
    {"detail", "Detail", 2.0f, 0.0f, 15.0f},
    {"roughness", "Roughness", 0.5f, 0.0f, 1.0f},
    {"lacunarity", "Lacunarity", 2.0f, 1.0f, 8.0f},
    {"distortion", "Distortion", 0.0f, 0.0f, 4.0f},
    /* Read by the three sums that take an offset; the others ignore it, as the node does. */
    {"offset", "Offset", 0.0f, -1000.0f, 1000.0f},
    {"gain", "Gain", 1.0f, 0.0f, 1000.0f},
    {"ramp", "Ramp", 0.0f, 0.0f, 1.0f, PaintLayerKindParamType::Ramp},
};
static_assert(std::size(noise_generator_params) == PAINT_LAYER_GEN_NOISE_PARAM_NUM);
static_assert(PAINT_LAYER_GEN_NOISE_RAMP == int(std::size(noise_generator_params)) - 1);

static const PaintLayerKindEnumItem generator_voronoi_feature_items[] = {
    {SHD_VORONOI_F1, "F1", "F1"},
    {SHD_VORONOI_F2, "F2", "F2"},
    {SHD_VORONOI_SMOOTH_F1, "SMOOTH_F1", "Smooth F1"},
    {SHD_VORONOI_DISTANCE_TO_EDGE, "DISTANCE_TO_EDGE", "Distance to Edge"},
    {SHD_VORONOI_N_SPHERE_RADIUS, "N_SPHERE_RADIUS", "N-Sphere Radius"},
};
static_assert(int(std::size(generator_voronoi_feature_items)) == 5);

static const PaintLayerKindEnumItem generator_voronoi_distance_items[] = {
    {SHD_VORONOI_EUCLIDEAN, "EUCLIDEAN", "Euclidean"},
    {SHD_VORONOI_MANHATTAN, "MANHATTAN", "Manhattan"},
    {SHD_VORONOI_CHEBYCHEV, "CHEBYSHEV", "Chebyshev"},
    {SHD_VORONOI_MINKOWSKI, "MINKOWSKI", "Minkowski"},
};
static_assert(int(std::size(generator_voronoi_distance_items)) == 4);

static const PaintLayerKindEnumItem generator_voronoi_output_items[] = {
    {PAINT_LAYER_GEN_VORONOI_OUTPUT_VALUE, "VALUE", "Value"},
    {PAINT_LAYER_GEN_VORONOI_OUTPUT_CELL_COLOR, "CELL_COLOR", "Cell Color"},
};
static_assert(int(std::size(generator_voronoi_output_items)) - 1 ==
              PAINT_LAYER_GEN_VORONOI_OUTPUT_CELL_COLOR);

static const PaintLayerKindParam voronoi_generator_params[] = {
    GENERATOR_PLACEMENT_PARAMS(5.0f)
    {"feature",
     "Feature",
     float(SHD_VORONOI_F1),
     0.0f,
     float(SHD_VORONOI_N_SPHERE_RADIUS),
     PaintLayerKindParamType::Enum,
     generator_voronoi_feature_items,
     int(std::size(generator_voronoi_feature_items))},
    {"distance",
     "Distance",
     float(SHD_VORONOI_EUCLIDEAN),
     0.0f,
     float(SHD_VORONOI_MINKOWSKI),
     PaintLayerKindParamType::Enum,
     generator_voronoi_distance_items,
     int(std::size(generator_voronoi_distance_items))},
    /* The Voronoi node's own default: fully jittered feature points. */
    {"randomness", "Randomness", 1.0f, 0.0f, 1.0f},
    {"output",
     "Output",
     float(PAINT_LAYER_GEN_VORONOI_OUTPUT_VALUE),
     0.0f,
     float(PAINT_LAYER_GEN_VORONOI_OUTPUT_CELL_COLOR),
     PaintLayerKindParamType::Enum,
     generator_voronoi_output_items,
     int(std::size(generator_voronoi_output_items))},
    /* A Voronoi detail of zero is the single-layer cells the node shows by default. */
    {"detail", "Detail", 0.0f, 0.0f, 15.0f},
    {"roughness", "Roughness", 0.5f, 0.0f, 1.0f},
    {"lacunarity", "Lacunarity", 2.0f, 1.0f, 8.0f},
    {"smoothness", "Smoothness", 1.0f, 0.0f, 1.0f},
    {"exponent", "Exponent", 0.5f, 0.0f, 32.0f},
    {"ramp", "Ramp", 0.0f, 0.0f, 1.0f, PaintLayerKindParamType::Ramp},
};
static_assert(std::size(voronoi_generator_params) == PAINT_LAYER_GEN_VORONOI_PARAM_NUM);
static_assert(PAINT_LAYER_GEN_VORONOI_RAMP == int(std::size(voronoi_generator_params)) - 1);

static const PaintLayerKindParam checker_generator_params[] = {
    GENERATOR_PLACEMENT_PARAMS(5.0f)
    /* The two colours of the board are the ramp's endpoints; a scalar channel reads its value ramp. */
    {"ramp", "Ramp", 0.0f, 0.0f, 1.0f, PaintLayerKindParamType::Ramp},
};
static_assert(std::size(checker_generator_params) == PAINT_LAYER_GEN_CHECKER_PARAM_NUM);

static const PaintLayerKindParam brick_generator_params[] = {
    GENERATOR_PLACEMENT_PARAMS(5.0f)
    {"offset", "Offset", 0.5f, 0.0f, 1.0f},
    {"offset_frequency", "Offset Frequency", 2.0f, 1.0f, 99.0f, PaintLayerKindParamType::Int},
    {"squash", "Squash", 1.0f, 0.0f, 1.0f},
    {"squash_frequency", "Squash Frequency", 1.0f, 1.0f, 99.0f, PaintLayerKindParamType::Int},
    {"mortar_size", "Mortar Size", 0.02f, 0.0f, 0.125f},
    {"mortar_smooth", "Mortar Smooth", 0.1f, 0.0f, 1.0f},
    /* The node's own range: -1 and 1 keep a single tint, zero spreads the two colours. */
    {"bias", "Bias", 0.0f, -1.0f, 1.0f},
    {"brick_width", "Brick Width", 0.5f, 0.01f, 100.0f},
    {"row_height", "Row Height", 0.25f, 0.01f, 100.0f},
    {"ramp", "Ramp", 0.0f, 0.0f, 1.0f, PaintLayerKindParamType::Ramp},
};
static_assert(std::size(brick_generator_params) == PAINT_LAYER_GEN_BRICK_PARAM_NUM);

static const PaintLayerKindEnumItem generator_wave_type_items[] = {
    {SHD_WAVE_BANDS, "BANDS", "Bands"},
    {SHD_WAVE_RINGS, "RINGS", "Rings"},
};
static_assert(int(std::size(generator_wave_type_items)) - 1 == SHD_WAVE_RINGS);

static const PaintLayerKindEnumItem generator_wave_bands_direction_items[] = {
    {SHD_WAVE_BANDS_DIRECTION_X, "X", "X"},
    {SHD_WAVE_BANDS_DIRECTION_Y, "Y", "Y"},
    {SHD_WAVE_BANDS_DIRECTION_Z, "Z", "Z"},
    {SHD_WAVE_BANDS_DIRECTION_DIAGONAL, "DIAGONAL", "Diagonal"},
};
static_assert(int(std::size(generator_wave_bands_direction_items)) - 1 ==
              SHD_WAVE_BANDS_DIRECTION_DIAGONAL);

static const PaintLayerKindEnumItem generator_wave_rings_direction_items[] = {
    {SHD_WAVE_RINGS_DIRECTION_X, "X", "X"},
    {SHD_WAVE_RINGS_DIRECTION_Y, "Y", "Y"},
    {SHD_WAVE_RINGS_DIRECTION_Z, "Z", "Z"},
    {SHD_WAVE_RINGS_DIRECTION_SPHERICAL, "SPHERICAL", "Spherical"},
};
static_assert(int(std::size(generator_wave_rings_direction_items)) - 1 ==
              SHD_WAVE_RINGS_DIRECTION_SPHERICAL);

static const PaintLayerKindEnumItem generator_wave_profile_items[] = {
    {SHD_WAVE_PROFILE_SIN, "SIN", "Sine"},
    {SHD_WAVE_PROFILE_SAW, "SAW", "Sawtooth"},
    {SHD_WAVE_PROFILE_TRI, "TRI", "Triangle"},
};
static_assert(int(std::size(generator_wave_profile_items)) - 1 == SHD_WAVE_PROFILE_TRI);

static const PaintLayerKindParam wave_generator_params[] = {
    GENERATOR_PLACEMENT_PARAMS(5.0f)
    {"wave_type",
     "Wave Type",
     float(SHD_WAVE_BANDS),
     0.0f,
     float(SHD_WAVE_RINGS),
     PaintLayerKindParamType::Enum,
     generator_wave_type_items,
     int(std::size(generator_wave_type_items))},
    {"bands_direction",
     "Bands Direction",
     float(SHD_WAVE_BANDS_DIRECTION_X),
     0.0f,
     float(SHD_WAVE_BANDS_DIRECTION_DIAGONAL),
     PaintLayerKindParamType::Enum,
     generator_wave_bands_direction_items,
     int(std::size(generator_wave_bands_direction_items))},
    {"rings_direction",
     "Rings Direction",
     float(SHD_WAVE_RINGS_DIRECTION_X),
     0.0f,
     float(SHD_WAVE_RINGS_DIRECTION_SPHERICAL),
     PaintLayerKindParamType::Enum,
     generator_wave_rings_direction_items,
     int(std::size(generator_wave_rings_direction_items))},
    {"profile",
     "Profile",
     float(SHD_WAVE_PROFILE_SIN),
     0.0f,
     float(SHD_WAVE_PROFILE_TRI),
     PaintLayerKindParamType::Enum,
     generator_wave_profile_items,
     int(std::size(generator_wave_profile_items))},
    {"distortion", "Distortion", 0.0f, 0.0f, 4.0f},
    {"detail", "Detail", 2.0f, 0.0f, 15.0f},
    {"detail_scale", "Detail Scale", 1.0f, 0.0f, 100.0f},
    {"detail_roughness", "Detail Roughness", 0.5f, 0.0f, 1.0f},
    {"phase_offset", "Phase Offset", 0.0f, -1000.0f, 1000.0f},
    {"ramp", "Ramp", 0.0f, 0.0f, 1.0f, PaintLayerKindParamType::Ramp},
};
static_assert(std::size(wave_generator_params) == PAINT_LAYER_GEN_WAVE_PARAM_NUM);

/* The scratches are a Voronoi edge, so their schema carries the cells' own parameters; the two
 * enums the compound never exposes (the feature, fixed to the distance-to-edge, and the metric,
 * fixed to Euclidean) are its group's constants rather than dead sliders. */
static const PaintLayerKindParam scratches_generator_params[] = {
    /* The Y ratio stays at one because the scratch's own direction comes from the stretch below. */
    GENERATOR_PLACEMENT_PARAMS(5.0f)
    /* Half the width of a line, in the cells' own distance units. */
    {"width", "Width", 0.02f, 0.0f, 0.5f},
    /* How far past the width the edge fades; zero is a hard line. */
    {"soften", "Soften", 0.02f, 0.0f, 0.25f},
    /* The Y axis shrinks by this factor, so an edge becomes a long scratch along X. */
    {"stretch", "Stretch", 3.0f, 1.0f, 20.0f},
    {"randomness", "Randomness", 1.0f, 0.0f, 1.0f},
    {"detail", "Detail", 0.0f, 0.0f, 15.0f},
    {"roughness", "Roughness", 0.5f, 0.0f, 1.0f},
    {"lacunarity", "Lacunarity", 2.0f, 1.0f, 8.0f},
    {"ramp", "Ramp", 0.0f, 0.0f, 1.0f, PaintLayerKindParamType::Ramp},
};
static_assert(std::size(scratches_generator_params) == PAINT_LAYER_GEN_SCRATCHES_PARAM_NUM);

static const PaintLayerKindEnumItem generator_weave_pattern_items[] = {
    {PAINT_LAYER_GEN_WEAVE_PLAIN, "PLAIN", "Plain"},
    {PAINT_LAYER_GEN_WEAVE_TWILL, "TWILL", "Twill"},
};
static_assert(int(std::size(generator_weave_pattern_items)) - 1 == PAINT_LAYER_GEN_WEAVE_TWILL);

static const PaintLayerKindParam weave_generator_params[] = {
    /* A weave reads as threads, which wants a coarser grid than a noise does. */
    GENERATOR_PLACEMENT_PARAMS(8.0f)
    {"pattern",
     "Pattern",
     float(PAINT_LAYER_GEN_WEAVE_PLAIN),
     0.0f,
     float(PAINT_LAYER_GEN_WEAVE_TWILL),
     PaintLayerKindParamType::Enum,
     generator_weave_pattern_items,
     int(std::size(generator_weave_pattern_items))},
    /* The thread fills this part of its cell; the rest is the gap the other direction shows. */
    {"thread_width", "Thread Width", 0.7f, 0.05f, 1.0f},
    {"gap", "Gap", 0.05f, 0.0f, 0.5f},
    {"soften", "Soften", 0.05f, 0.0f, 0.5f},
    /* How far a noise pushes the threads off their grid. */
    {"jitter", "Jitter", 0.0f, 0.0f, 1.0f},
    {"ramp", "Ramp", 0.0f, 0.0f, 1.0f, PaintLayerKindParamType::Ramp},
};
static_assert(std::size(weave_generator_params) == PAINT_LAYER_GEN_WEAVE_PARAM_NUM);

/* A grunge is a noise with a second noise on top, so its schema repeats the Noise's own slots and
 * adds the four knobs of the patch. The base noise's type and normalize, and the offset and gain a
 * different type would read, are the shared group's constants rather than sliders. */
static const PaintLayerKindParam grunge_generator_params[] = {
    /* Dirt sits at a coarse scale, where a single patch covers a hand's width. */
    GENERATOR_PLACEMENT_PARAMS(2.0f)
    {"detail", "Detail", 2.0f, 0.0f, 15.0f},
    {"roughness", "Roughness", 0.5f, 0.0f, 1.0f},
    {"lacunarity", "Lacunarity", 2.0f, 1.0f, 8.0f},
    {"distortion", "Distortion", 0.0f, 0.0f, 4.0f},
    {"variation_strength", "Variation Strength", 0.3f, 0.0f, 1.0f},
    {"variation_scale", "Variation Scale", 15.0f, 0.1f, 100.0f},
    {"threshold", "Threshold", 0.3f, 0.0f, 1.0f},
    {"soften", "Soften", 0.1f, 0.0f, 0.5f},
    {"ramp", "Ramp", 0.0f, 0.0f, 1.0f, PaintLayerKindParamType::Ramp},
};
static_assert(std::size(grunge_generator_params) == PAINT_LAYER_GEN_GRUNGE_PARAM_NUM);

/* -------------------------------------------------------------------- */
/** \name Generator parameters and the nodes they drive
 *
 * A generator's schema holds three sorts of parameter, and the graph treats them differently. The
 * ones listed here reach the pattern's node through a socket of its own, so the row group carries a
 * value input for each and an edit is a value edit. The placement -- the Y ratio, the turn and the
 * two offsets -- rides the mapping chain's three vector inputs instead, and the space, the seed and
 * the choices the node keeps in its storage decide which nodes are built at all. \{ */

/** One schema parameter and the socket of the pattern's node it drives. */
struct GeneratorSocketParam {
  int index;
  const char *socket;
};

/** A parameter the node reads only under a particular choice of another: the sockets the
 * multi-function's signature leaves out when the feature or the type says it does not use them. */
struct GeneratorConditionalSocketParam {
  int index;
  const char *socket;
  /** The parameter whose value decides, and the values that let this socket in. */
  int over_index;
  Span<const int> over_values;
  /** A second choice that must admit the socket too, or -1 when there is no second. The node's
   * declaration gates a socket on several of its switches at once -- the Voronoi Exponent is read
   * by the Minkowski metric of a feature that has one. */
  int also_index = -1;
  Span<const int> also_values;
};

static const GeneratorSocketParam noise_generator_sockets[] = {
    {PAINT_LAYER_GEN_SCALE, "Scale"},
    {PAINT_LAYER_GEN_NOISE_DETAIL, "Detail"},
    {PAINT_LAYER_GEN_NOISE_ROUGHNESS, "Roughness"},
    {PAINT_LAYER_GEN_NOISE_LACUNARITY, "Lacunarity"},
    {PAINT_LAYER_GEN_NOISE_DISTORTION, "Distortion"},
};
static const int noise_offset_types[] = {SHD_NOISE_RIDGED_MULTIFRACTAL,
                                         SHD_NOISE_HYBRID_MULTIFRACTAL,
                                         SHD_NOISE_HETERO_TERRAIN};
static const int noise_gain_types[] = {SHD_NOISE_RIDGED_MULTIFRACTAL,
                                        SHD_NOISE_HYBRID_MULTIFRACTAL};
static const GeneratorConditionalSocketParam noise_generator_conditional[] = {
    {PAINT_LAYER_GEN_NOISE_OFFSET,
     "Offset",
     PAINT_LAYER_GEN_NOISE_TYPE,
     Span<const int>(noise_offset_types, std::size(noise_offset_types))},
    {PAINT_LAYER_GEN_NOISE_GAIN,
     "Gain",
     PAINT_LAYER_GEN_NOISE_TYPE,
     Span<const int>(noise_gain_types, std::size(noise_gain_types))},
};

static const GeneratorSocketParam voronoi_generator_sockets[] = {
    {PAINT_LAYER_GEN_SCALE, "Scale"},
    {PAINT_LAYER_GEN_VORONOI_RANDOMNESS, "Randomness"},
};
/* The three octaving parameters are gone for N-Sphere Radius, which sums nothing; Smoothness is
 * only read by Smooth F1 and Exponent only by Minkowski, so their own conditionals say more than
 * this table. */
static const int voronoi_octave_features[] = {SHD_VORONOI_F1,
                                              SHD_VORONOI_F2,
                                              SHD_VORONOI_SMOOTH_F1,
                                              SHD_VORONOI_DISTANCE_TO_EDGE};
static const int voronoi_smooth_feature[] = {SHD_VORONOI_SMOOTH_F1};
static const int voronoi_minkowski_metric[] = {SHD_VORONOI_MINKOWSKI};
/* The declaration keeps the Exponent only for a feature whose metric is read at all; the edge and
 * the radius sum no distances, so they have no exponent to speak of. */
static const int voronoi_metric_features[] = {SHD_VORONOI_F1,
                                              SHD_VORONOI_F2,
                                              SHD_VORONOI_SMOOTH_F1};
static const GeneratorConditionalSocketParam voronoi_generator_conditional[] = {
    {PAINT_LAYER_GEN_VORONOI_DETAIL,
     "Detail",
     PAINT_LAYER_GEN_VORONOI_FEATURE,
     Span<const int>(voronoi_octave_features, std::size(voronoi_octave_features))},
    {PAINT_LAYER_GEN_VORONOI_ROUGHNESS,
     "Roughness",
     PAINT_LAYER_GEN_VORONOI_FEATURE,
     Span<const int>(voronoi_octave_features, std::size(voronoi_octave_features))},
    {PAINT_LAYER_GEN_VORONOI_LACUNARITY,
     "Lacunarity",
     PAINT_LAYER_GEN_VORONOI_FEATURE,
     Span<const int>(voronoi_octave_features, std::size(voronoi_octave_features))},
    {PAINT_LAYER_GEN_VORONOI_SMOOTHNESS,
     "Smoothness",
     PAINT_LAYER_GEN_VORONOI_FEATURE,
     Span<const int>(voronoi_smooth_feature, 1)},
    {PAINT_LAYER_GEN_VORONOI_EXPONENT,
     "Exponent",
     PAINT_LAYER_GEN_VORONOI_DISTANCE,
     Span<const int>(voronoi_minkowski_metric, 1),
     PAINT_LAYER_GEN_VORONOI_FEATURE,
     Span<const int>(voronoi_metric_features, std::size(voronoi_metric_features))},
};

static const GeneratorSocketParam checker_generator_sockets[] = {
    {PAINT_LAYER_GEN_SCALE, "Scale"},
};

static const GeneratorSocketParam brick_generator_sockets[] = {
    {PAINT_LAYER_GEN_SCALE, "Scale"},
    {PAINT_LAYER_GEN_BRICK_MORTAR_SIZE, "Mortar Size"},
    {PAINT_LAYER_GEN_BRICK_MORTAR_SMOOTH, "Mortar Smooth"},
    {PAINT_LAYER_GEN_BRICK_BIAS, "Bias"},
    {PAINT_LAYER_GEN_BRICK_WIDTH, "Brick Width"},
    {PAINT_LAYER_GEN_BRICK_ROW_HEIGHT, "Row Height"},
};

static const GeneratorSocketParam wave_generator_sockets[] = {
    {PAINT_LAYER_GEN_SCALE, "Scale"},
    {PAINT_LAYER_GEN_WAVE_DISTORTION, "Distortion"},
    {PAINT_LAYER_GEN_WAVE_DETAIL, "Detail"},
    {PAINT_LAYER_GEN_WAVE_DETAIL_SCALE, "Detail Scale"},
    {PAINT_LAYER_GEN_WAVE_DETAIL_ROUGHNESS, "Detail Roughness"},
    {PAINT_LAYER_GEN_WAVE_PHASE_OFFSET, "Phase Offset"},
};

/* A compound generator's pattern is a shared node group, and the group takes its numbers through
 * interface sockets whose identifiers its creation order fixed. The tables below say which schema
 * slot is which socket; what is not in them -- the space, the seed, the placement's three vectors --
 * the group reads nothing of, because the row's own mapping chain hands it the coordinate. */
static const GeneratorSocketParam scratches_generator_sockets[] = {
    {PAINT_LAYER_GEN_SCALE, SCRATCHES_GROUP_ID_SCALE},
    {PAINT_LAYER_GEN_SCRATCHES_DETAIL, SCRATCHES_GROUP_ID_DETAIL},
    {PAINT_LAYER_GEN_SCRATCHES_ROUGHNESS, SCRATCHES_GROUP_ID_ROUGHNESS},
    {PAINT_LAYER_GEN_SCRATCHES_LACUNARITY, SCRATCHES_GROUP_ID_LACUNARITY},
    {PAINT_LAYER_GEN_SCRATCHES_RANDOMNESS, SCRATCHES_GROUP_ID_RANDOMNESS},
    {PAINT_LAYER_GEN_SCRATCHES_WIDTH, SCRATCHES_GROUP_ID_WIDTH},
    {PAINT_LAYER_GEN_SCRATCHES_SOFTEN, SCRATCHES_GROUP_ID_SOFTEN},
    {PAINT_LAYER_GEN_SCRATCHES_STRETCH, SCRATCHES_GROUP_ID_STRETCH},
};

static const GeneratorSocketParam weave_generator_sockets[] = {
    {PAINT_LAYER_GEN_SCALE, WEAVE_GROUP_ID_SCALE},
    /* The over/under step is a value rather than a choice of nodes: the group switches between the
     * plain and the twill parity with a Mix, so an edit of it is not a rebuild. */
    {PAINT_LAYER_GEN_WEAVE_PATTERN, WEAVE_GROUP_ID_PATTERN},
    {PAINT_LAYER_GEN_WEAVE_THREAD_WIDTH, WEAVE_GROUP_ID_THREAD_WIDTH},
    {PAINT_LAYER_GEN_WEAVE_GAP, WEAVE_GROUP_ID_GAP},
    {PAINT_LAYER_GEN_WEAVE_SOFTEN, WEAVE_GROUP_ID_SOFTEN},
    {PAINT_LAYER_GEN_WEAVE_JITTER, WEAVE_GROUP_ID_JITTER},
};

static const GeneratorSocketParam grunge_generator_sockets[] = {
    {PAINT_LAYER_GEN_SCALE, GRUNGE_GROUP_ID_SCALE},
    {PAINT_LAYER_GEN_GRUNGE_DETAIL, GRUNGE_GROUP_ID_DETAIL},
    {PAINT_LAYER_GEN_GRUNGE_ROUGHNESS, GRUNGE_GROUP_ID_ROUGHNESS},
    {PAINT_LAYER_GEN_GRUNGE_LACUNARITY, GRUNGE_GROUP_ID_LACUNARITY},
    {PAINT_LAYER_GEN_GRUNGE_DISTORTION, GRUNGE_GROUP_ID_DISTORTION},
    {PAINT_LAYER_GEN_GRUNGE_VARIATION_STRENGTH, GRUNGE_GROUP_ID_VARIATION_STRENGTH},
    {PAINT_LAYER_GEN_GRUNGE_VARIATION_SCALE, GRUNGE_GROUP_ID_VARIATION_SCALE},
    {PAINT_LAYER_GEN_GRUNGE_THRESHOLD, GRUNGE_GROUP_ID_THRESHOLD},
    {PAINT_LAYER_GEN_GRUNGE_SOFTEN, GRUNGE_GROUP_ID_SOFTEN},
};

/** The sockets of the pattern's node that \a kind's parameters drive, and its conditional ones. */
static void generator_socket_tables(const int kind,
                                    Span<const GeneratorSocketParam> &r_sockets,
                                    Span<const GeneratorConditionalSocketParam> &r_conditional)
{
  switch (kind) {
    case MA_PAINT_LAYER_GENERATOR_NOISE:
      r_sockets = Span<const GeneratorSocketParam>(
          noise_generator_sockets, std::size(noise_generator_sockets));
      r_conditional = Span<const GeneratorConditionalSocketParam>(
          noise_generator_conditional, std::size(noise_generator_conditional));
      return;
    case MA_PAINT_LAYER_GENERATOR_VORONOI:
      r_sockets = Span<const GeneratorSocketParam>(
          voronoi_generator_sockets, std::size(voronoi_generator_sockets));
      r_conditional = Span<const GeneratorConditionalSocketParam>(
          voronoi_generator_conditional, std::size(voronoi_generator_conditional));
      return;
    case MA_PAINT_LAYER_GENERATOR_CHECKER:
      r_sockets = Span<const GeneratorSocketParam>(
          checker_generator_sockets, std::size(checker_generator_sockets));
      return;
    case MA_PAINT_LAYER_GENERATOR_BRICK:
      r_sockets = Span<const GeneratorSocketParam>(
          brick_generator_sockets, std::size(brick_generator_sockets));
      return;
    case MA_PAINT_LAYER_GENERATOR_WAVE:
      r_sockets = Span<const GeneratorSocketParam>(
          wave_generator_sockets, std::size(wave_generator_sockets));
      return;
    case MA_PAINT_LAYER_GENERATOR_SCRATCHES:
      r_sockets = Span<const GeneratorSocketParam>(
          scratches_generator_sockets, std::size(scratches_generator_sockets));
      return;
    case MA_PAINT_LAYER_GENERATOR_WEAVE:
      r_sockets = Span<const GeneratorSocketParam>(
          weave_generator_sockets, std::size(weave_generator_sockets));
      return;
    case MA_PAINT_LAYER_GENERATOR_GRUNGE:
      r_sockets = Span<const GeneratorSocketParam>(
          grunge_generator_sockets, std::size(grunge_generator_sockets));
      return;
    default:
      return;
  }
}

/** \} */


/* One entry per #eMaterialPaintLayerGenerator subtype, keyed like the kind registry's own. A
 * generator without a schema here is invisible to every consumer: the kind lookup answers null and
 * the row is passed by, which is what keeps a file saved by a newer build reading.
 *
 * The parametric generators register after the Gradient below, and
 * their parameter schemas land in this file as well: the kind registry in `paint_layers_kind.cc`
 * takes no generator kinds. */
static const PaintLayerKindInfo generator_kinds[] = {
    /* A procedural generator: its colour and coverage come from shader nodes reading the shading
     * coordinates, not from a map. As a row it lays its colour; as a content correction it lays
     * that colour over the row's; as a mask item its ramp's grey over the row's mask. */
    {MA_PAINT_LAYER_SOURCE_GENERATOR,
     MA_PAINT_LAYER_GENERATOR_GRADIENT,
     "Gradient",
     PaintLayerKindCategory::Source,
     paint_layer_all_roles,
     nullptr,
     0,
     PAINT_LAYER_KIND_GENERATES,
     /* The kind owns no map, so a stroke cannot paint into it. */
     false},
    /* The remaining generators share the Gradient's contract exactly: procedural colour, no map,
     * every role, and a schema whose first seven parameters are the placement. Only the emission
     * and the CPU math differ, kind by kind. */
    {MA_PAINT_LAYER_SOURCE_GENERATOR,
     MA_PAINT_LAYER_GENERATOR_NOISE,
     "Noise",
     PaintLayerKindCategory::Source,
     paint_layer_all_roles,
     noise_generator_params,
     int(std::size(noise_generator_params)),
     PAINT_LAYER_KIND_GENERATES,
     false},
    {MA_PAINT_LAYER_SOURCE_GENERATOR,
     MA_PAINT_LAYER_GENERATOR_VORONOI,
     "Voronoi",
     PaintLayerKindCategory::Source,
     paint_layer_all_roles,
     voronoi_generator_params,
     int(std::size(voronoi_generator_params)),
     PAINT_LAYER_KIND_GENERATES,
     false},
    {MA_PAINT_LAYER_SOURCE_GENERATOR,
     MA_PAINT_LAYER_GENERATOR_CHECKER,
     "Checker",
     PaintLayerKindCategory::Source,
     paint_layer_all_roles,
     checker_generator_params,
     int(std::size(checker_generator_params)),
     PAINT_LAYER_KIND_GENERATES,
     false},
    {MA_PAINT_LAYER_SOURCE_GENERATOR,
     MA_PAINT_LAYER_GENERATOR_BRICK,
     "Brick",
     PaintLayerKindCategory::Source,
     paint_layer_all_roles,
     brick_generator_params,
     int(std::size(brick_generator_params)),
     PAINT_LAYER_KIND_GENERATES,
     false},
    {MA_PAINT_LAYER_SOURCE_GENERATOR,
     MA_PAINT_LAYER_GENERATOR_WAVE,
     "Wave",
     PaintLayerKindCategory::Source,
     paint_layer_all_roles,
     wave_generator_params,
     int(std::size(wave_generator_params)),
     PAINT_LAYER_KIND_GENERATES,
     false},
    {MA_PAINT_LAYER_SOURCE_GENERATOR,
     MA_PAINT_LAYER_GENERATOR_SCRATCHES,
     "Scratches",
     PaintLayerKindCategory::Source,
     paint_layer_all_roles,
     scratches_generator_params,
     int(std::size(scratches_generator_params)),
     PAINT_LAYER_KIND_GENERATES,
     false},
    {MA_PAINT_LAYER_SOURCE_GENERATOR,
     MA_PAINT_LAYER_GENERATOR_WEAVE,
     "Weave",
     PaintLayerKindCategory::Source,
     paint_layer_all_roles,
     weave_generator_params,
     int(std::size(weave_generator_params)),
     PAINT_LAYER_KIND_GENERATES,
     false},
    {MA_PAINT_LAYER_SOURCE_GENERATOR,
     MA_PAINT_LAYER_GENERATOR_GRUNGE,
     "Grunge",
     PaintLayerKindCategory::Source,
     paint_layer_all_roles,
     grunge_generator_params,
     int(std::size(grunge_generator_params)),
     PAINT_LAYER_KIND_GENERATES,
     false},
};

int paint_layer_generator_kind_of(const MaterialPaintLayer &row)
{
  if (!paint_layer_kind_has_flag(row, PAINT_LAYER_KIND_GENERATES)) {
    return MA_PAINT_LAYER_GENERATOR_GRADIENT;
  }
  return int(row.kind_subtype);
}

bool paint_layer_generator_is_single_node(const int kind)
{
  /* The entry's node type is the one bit that tells a single-node pattern from the Gradient's
   * group and the compounds' shared ones: the registry is the one list that says which is which. */
  const PaintLayerGeneratorInfo *info = BKE_paint_layers_generator_find(int16_t(kind));
  return info != nullptr && info->shader_node_type != 0;
}

bool paint_layer_generator_is_compound(const int kind)
{
  const PaintLayerGeneratorInfo *info = BKE_paint_layers_generator_find(int16_t(kind));
  return info != nullptr && (info->flags & PAINT_LAYER_GENERATOR_COMPOUND) != 0;
}

int paint_layer_generator_space_of(const MaterialPaintLayer &row)
{
  const int kind = paint_layer_generator_kind_of(row);
  if (kind == MA_PAINT_LAYER_GENERATOR_GRADIENT) {
    return (row.gradient != nullptr) ? int(row.gradient->space) :
                                       MA_PAINT_LAYER_GRADIENT_SPACE_UV;
  }
  return int(paint_layer_kind_param_get(row, PAINT_LAYER_GEN_SPACE));
}

float paint_layer_generator_scale_y_ratio(const MaterialPaintLayer &row)
{
  return paint_layer_generator_scale_y_ratio(paint_layer_kind_param_get(row, PAINT_LAYER_GEN_SCALE),
                                             paint_layer_kind_param_get(row, PAINT_LAYER_GEN_SCALE_Y),
                                             paint_layer_kind_param_get(row, PAINT_LAYER_GEN_SCALE_LOCK) !=
                                                 0.0f);
}

bool paint_layer_generator_param_is_placement(const int index)
{
  /* The lock has no socket of its own: it only decides the Y ratio the Scale vector carries. */
  return ELEM(index,
              PAINT_LAYER_GEN_SCALE_Y,
              PAINT_LAYER_GEN_SCALE_LOCK,
              PAINT_LAYER_GEN_ROTATION,
              PAINT_LAYER_GEN_OFFSET_X,
              PAINT_LAYER_GEN_OFFSET_Y);
}

const char *paint_layer_generator_param_socket(const MaterialPaintLayer &row, const int index)
{
  Span<const GeneratorSocketParam> sockets;
  Span<const GeneratorConditionalSocketParam> conditional;
  generator_socket_tables(paint_layer_generator_kind_of(row), sockets, conditional);
  for (const GeneratorConditionalSocketParam &param : conditional) {
    if (param.index != index) {
      continue;
    }
    /* The node's multi-function leaves the socket out of its signature when the choice says it
     * reads nothing, so linking it would be a socket the node does not have. */
    if (!param.over_values.contains(
            int(paint_layer_kind_param_get(row, param.over_index))))
    {
      return nullptr;
    }
    if (param.also_index >= 0 &&
        !param.also_values.contains(int(paint_layer_kind_param_get(row, param.also_index))))
    {
      return nullptr;
    }
    return param.socket;
  }
  for (const GeneratorSocketParam &param : sockets) {
    if (param.index == index) {
      return param.socket;
    }
  }
  return nullptr;
}

bool paint_layer_generator_param_is_active(const MaterialPaintLayer &row, const int index)
{
  if (index < 0) {
    return false;
  }
  /* The Wave keeps both direction choices in its storage and reads only the one its type asks:
   * the other is dead until the type comes back. */
  const int kind = paint_layer_generator_kind_of(row);
  if (kind == MA_PAINT_LAYER_GENERATOR_WAVE) {
    const int wave_type = int(paint_layer_kind_param_get(row, PAINT_LAYER_GEN_WAVE_TYPE));
    if (index == PAINT_LAYER_GEN_WAVE_BANDS_DIRECTION) {
      return wave_type == SHD_WAVE_BANDS;
    }
    if (index == PAINT_LAYER_GEN_WAVE_RINGS_DIRECTION) {
      return wave_type == SHD_WAVE_RINGS;
    }
  }
  Span<const GeneratorConditionalSocketParam> conditional;
  Span<const GeneratorSocketParam> sockets;
  generator_socket_tables(kind, sockets, conditional);
  for (const GeneratorConditionalSocketParam &param : conditional) {
    if (param.index != index) {
      continue;
    }
    /* The same question the socket answers: the node's multi-function leaves the socket out of
     * its signature when the choice says it reads nothing, so the parameter would edit nothing.
     * A choice that gates no socket, a placement and the seed are read whatever the state. */
    if (!param.over_values.contains(
            int(paint_layer_kind_param_get(row, param.over_index))))
    {
      return false;
    }
    if (param.also_index >= 0 &&
        !param.also_values.contains(int(paint_layer_kind_param_get(row, param.also_index))))
    {
      return false;
    }
    return true;
  }
  return true;
}

bool BKE_paint_layers_generator_param_is_active(const MaterialPaintLayer &row, const char *name)
{
  const PaintLayerKindInfo *kind = paint_layer_kind_of(row);
  if (kind == nullptr || name == nullptr) {
    return true;
  }
  for (int index = 0; index < kind->params_num; index++) {
    if (STREQ(kind->params[index].name, name)) {
      return paint_layer_generator_param_is_active(row, index);
    }
  }
  /* A property the current schema no longer holds draws as an ordinary parameter. */
  return true;
}

bool paint_layer_generator_param_is_value(const MaterialPaintLayer &row, const int index)
{
  /* A placement parameter rides the mapping chain's vector inputs, a socket parameter the socket of
   * the pattern's own node; both are values an edit syncs in place. Everything else -- the space,
   * the seed, a choice the node keeps in its storage, or a socket its signature leaves out for the
   * feature or type chosen now -- decides which nodes the build makes. */
  if (paint_layer_generator_param_is_placement(index)) {
    return true;
  }
  return paint_layer_generator_param_socket(row, index) != nullptr;
}

bool paint_layer_generator_has_own_color(const MaterialPaintLayer &row)
{
  if (paint_layer_generator_kind_of(row) != MA_PAINT_LAYER_GENERATOR_VORONOI) {
    return false;
  }
  /* The cell colour is the one setting under which the pattern lays a colour of its own instead of
   * an index into the row's band; the two features without a Color socket cannot ask for it. */
  const int feature = int(paint_layer_kind_param_get(row, PAINT_LAYER_GEN_VORONOI_FEATURE));
  if (ELEM(feature, SHD_VORONOI_DISTANCE_TO_EDGE, SHD_VORONOI_N_SPHERE_RADIUS)) {
    return false;
  }
  return int(paint_layer_kind_param_get(row, PAINT_LAYER_GEN_VORONOI_OUTPUT)) ==
         PAINT_LAYER_GEN_VORONOI_OUTPUT_CELL_COLOR;
}


Span<const PaintLayerKindInfo> paint_layer_kind_generator_kinds()
{
  /* #Span of this module has no array constructor: the pointer-and-size form is the one the
   * registry's own readers use (see #paint_layers_generate_common.cc). */
  return Span<const PaintLayerKindInfo>(generator_kinds, ARRAY_SIZE(generator_kinds));
}

}  // namespace blender
