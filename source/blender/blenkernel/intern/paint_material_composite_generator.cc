/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The procedural generator as the CPU composite reads it: the per-texel sample of a generator's
 * payload, the pattern factors the generator table names, the count of the payloads an evaluation
 * lays, and the hash that moves the composite cache when one of them changes. The factor goes
 * through the generator table (#BKE_paint_layers_generator_find), so the composite never re-derives
 * one; the GPU counterpart of the sample is the emission in
 * `paint_layers_generate_build_generator.cc`.
 */

#include "BKE_paint_material_composite.hh"

#include "BKE_colorband.hh"
#include "BKE_paint_layers_generator.hh"
#include "BKE_paint_layers_generator_registry.hh"
#include "BKE_paint_layers_gradient.hh"

#include "BLI_hash.hh"
#include "BLI_math_base.h"
#include "BLI_math_base.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_noise.hh"
#include "BLI_procedural_texture.hh"
#include "BLI_span.hh"
#include "BLI_utildefines.h"

#include "DNA_material_types.h"
#include "DNA_node_types.h"

#include "paint_layers/paint_layers_kind.hh"
#include "paint_layers/paint_layers_generator_registry_intern.hh"
#include "paint_material_composite_internal.hh"

#include <algorithm>
#include <array>

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Procedural generator factors
 *
 * The factor a generator gives at a placed coordinate, and the colour it lays where no ramp could
 * express it. Each is the shader node's own multi-function body called through the same
 * #BLI_noise.hh and #BLI_procedural_texture.hh functions, with the node's own clamps; the
 * generated tree puts the same node in the same place, so the composite and the render cannot
 * fall out of step. A formula written here that the node does not have would break exactly that,
 * which is why the Checker, Brick and Wave bodies live in `BLI_procedural_texture.hh` and the
 * node files call them too. \{ */

/** A generator item's parameters, spelled short for the readers below. */
using GeneratorParams = std::array<float, PAINT_MATERIAL_COMPOSITE_GENERATOR_PARAM_MAX>;

/**
 * Whether the generator samples two-dimensionally: a UV row's coordinate carries the texel's Z,
 * which a node built with `dimensions == 2` ignores. An Object or Generated row reads the whole
 * vector, so the pattern sits in the mesh's volume.
 */
static bool generator_dimension_2d(const int16_t space)
{
  return space == MA_PAINT_LAYER_GRADIENT_SPACE_UV;
}

static float generator_factor_noise(const GeneratorParams &params,
                                    const float3 &coordinate,
                                    const bool is_2d)
{
  const float scale = params[PAINT_LAYER_GEN_SCALE];
  const float detail = clamp_f(params[PAINT_LAYER_GEN_NOISE_DETAIL], 0.0f, 15.0f);
  const float roughness = std::max(params[PAINT_LAYER_GEN_NOISE_ROUGHNESS], 0.0f);
  const float lacunarity = params[PAINT_LAYER_GEN_NOISE_LACUNARITY];
  const float offset = params[PAINT_LAYER_GEN_NOISE_OFFSET];
  const float gain = params[PAINT_LAYER_GEN_NOISE_GAIN];
  const float distortion = params[PAINT_LAYER_GEN_NOISE_DISTORTION];
  const int type = int(params[PAINT_LAYER_GEN_NOISE_TYPE]);
  const bool normalize = params[PAINT_LAYER_GEN_NOISE_NORMALIZE] != 0.0f;
  /* The node evaluates the same sum in two and in three dimensions, and only the vector's rank
   * differs: the one call, spelled once, over the placed vector the dimension asks for. */
  const auto eval = [&](const auto &scaled) {
    return noise::perlin_fractal_distorted(
        scaled, detail, roughness, lacunarity, offset, gain, distortion, type, normalize);
  };
  if (is_2d) {
    return eval(float2(coordinate * scale));
  }
  return eval(coordinate * scale);
}

/** The Voronoi node's parameter block at the item's values, filled the way its multi-function
 * fills it per texel: the smoothness halved and clamped, the randomness clamped, the exponent read
 * only under the Minkowski metric. */
static noise::VoronoiParams generator_voronoi_params(const GeneratorParams &params)
{
  noise::VoronoiParams voronoi;
  voronoi.feature = int(params[PAINT_LAYER_GEN_VORONOI_FEATURE]);
  voronoi.metric = int(params[PAINT_LAYER_GEN_VORONOI_DISTANCE]);
  /* The node's own default: a switch of its own would only let the composite and the graph pick
   * different ranges for the distance. */
  voronoi.normalize = false;
  voronoi.scale = params[PAINT_LAYER_GEN_SCALE];
  voronoi.detail = params[PAINT_LAYER_GEN_VORONOI_DETAIL];
  voronoi.roughness = params[PAINT_LAYER_GEN_VORONOI_ROUGHNESS];
  voronoi.lacunarity = params[PAINT_LAYER_GEN_VORONOI_LACUNARITY];
  voronoi.smoothness = (voronoi.feature == SHD_VORONOI_SMOOTH_F1) ?
                           math::clamp(params[PAINT_LAYER_GEN_VORONOI_SMOOTHNESS] / 2.0f,
                                       0.0f,
                                       0.5f) :
                           0.0f;
  voronoi.exponent = (voronoi.metric == SHD_VORONOI_MINKOWSKI) ?
                         params[PAINT_LAYER_GEN_VORONOI_EXPONENT] :
                         0.0f;
  voronoi.randomness = math::clamp(params[PAINT_LAYER_GEN_VORONOI_RANDOMNESS], 0.0f, 1.0f);
  return voronoi;
}

/**
 * The F1/F2/Smooth-F1 answer of the Voronoi pattern at the placed \a coordinate: the output the
 * node's Color and Distance sockets carry, with \a cell_color choosing which of the two the
 * multi-function fills. The one block the factor and the own-colour sample share -- the reach,
 * the F2 scale and the max distance the two branches of the dimension spell out -- so the CPU
 * composite and the colour it lays cannot disagree about it. The Distance-to-Edge and N-Sphere
 * features answer outside this, as the factor's own branches below.
 */
static noise::VoronoiOutput generator_voronoi_eval(const GeneratorParams &params,
                                                   const float3 &coordinate,
                                                   const bool is_2d,
                                                   const bool cell_color)
{
  noise::VoronoiParams voronoi = generator_voronoi_params(params);
  const float3 scaled = coordinate * voronoi.scale;
  const float f2_scale = (voronoi.feature == SHD_VORONOI_F2) ? 2.0f : 1.0f;
  const float reach = 0.5f + 0.5f * voronoi.randomness;
  noise::VoronoiOutput output;
  if (is_2d) {
    voronoi.max_distance = noise::voronoi_distance(float2{0.0f, 0.0f},
                                                   float2(reach, reach),
                                                   voronoi) *
                           f2_scale;
    output = noise::fractal_voronoi_x_fx<float2>(voronoi, float2{scaled.x, scaled.y}, cell_color);
  }
  else {
    voronoi.max_distance = noise::voronoi_distance(float3{0.0f, 0.0f, 0.0f},
                                                   float3(reach, reach, reach),
                                                   voronoi) *
                           f2_scale;
    output = noise::fractal_voronoi_x_fx<float3>(voronoi, scaled, cell_color);
  }
  return output;
}

static float generator_factor_voronoi(const GeneratorParams &params,
                                      const float3 &coordinate,
                                      const bool is_2d)
{
  noise::VoronoiParams voronoi = generator_voronoi_params(params);

  if (voronoi.feature == SHD_VORONOI_N_SPHERE_RADIUS) {
    /* The Radius output is the whole of this feature: the tree wires the same socket. */
    return is_2d ? noise::voronoi_n_sphere_radius(voronoi, float2(coordinate * voronoi.scale)) :
                   noise::voronoi_n_sphere_radius(voronoi, coordinate * voronoi.scale);
  }
  if (voronoi.feature == SHD_VORONOI_DISTANCE_TO_EDGE) {
    voronoi.max_distance = 0.5f + 0.5f * voronoi.randomness;
    const float3 scaled = coordinate * voronoi.scale;
    return is_2d ?
               noise::fractal_voronoi_distance_to_edge<float2>(
                   voronoi, float2{scaled.x, scaled.y}) :
               noise::fractal_voronoi_distance_to_edge<float3>(voronoi, scaled);
  }
  const bool cell_color = int(params[PAINT_LAYER_GEN_VORONOI_OUTPUT]) ==
                          PAINT_LAYER_GEN_VORONOI_OUTPUT_CELL_COLOR;
  const noise::VoronoiOutput output = generator_voronoi_eval(params, coordinate, is_2d, cell_color);
  if (!cell_color) {
    return output.distance;
  }
  /* The cell's colour, read as the RGB to BW node reads it: the factor the ramp of a scalar
   * channel indexes. */
  return clamp_f(0.2126f * output.color.x + 0.7152f * output.color.y + 0.0722f * output.color.z,
                 0.0f,
                 1.0f);
}

/** The cell colour the Voronoi generator lays where a ramp would otherwise index, in straight
 * scene-linear RGBA. False for every other setting, which leaves the ramp's answer to the
 * caller. */
static bool generator_own_color(const GeneratorParams &params,
                                const float3 &coordinate,
                                const bool is_2d,
                                float r_rgba[4])
{
  if (int(params[PAINT_LAYER_GEN_VORONOI_OUTPUT]) != PAINT_LAYER_GEN_VORONOI_OUTPUT_CELL_COLOR) {
    return false;
  }
  noise::VoronoiParams voronoi = generator_voronoi_params(params);
  if (voronoi.feature == SHD_VORONOI_N_SPHERE_RADIUS ||
      voronoi.feature == SHD_VORONOI_DISTANCE_TO_EDGE)
  {
    /* Neither has a Color socket, so the switch cannot be on for them. */
    return false;
  }
  const noise::VoronoiOutput output = generator_voronoi_eval(params, coordinate, is_2d, true);
  r_rgba[0] = output.color.x;
  r_rgba[1] = output.color.y;
  r_rgba[2] = output.color.z;
  r_rgba[3] = 1.0f;
  return true;
}

/** The registry's #PaintLayerGeneratorInfo::own_color for the Voronoi: the same answer under the
 * item's own dimension, which the entry's caller carries in the payload. */
bool paint_layer_generator_voronoi_own_color(const PaintMaterialCompositeGradient &gradient,
                                             const float3 &coordinate,
                                             float r_rgba[4])
{
  return generator_own_color(
      gradient.params, coordinate, generator_dimension_2d(gradient.space), r_rgba);
}

static float generator_factor_checker(const GeneratorParams &params, const float3 &coordinate)
{
  return procedural_checker_factor(coordinate, params[PAINT_LAYER_GEN_SCALE]);
}

static float generator_factor_brick(const GeneratorParams &params, const float3 &coordinate)
{
  /* The node reads its Vector three-dimensionally whatever the size; a UV row leaves Z at zero,
   * which is the same answer the 2D form of every other generator gives. */
  return procedural_brick_factor(coordinate,
                                 params[PAINT_LAYER_GEN_SCALE],
                                 params[PAINT_LAYER_GEN_BRICK_MORTAR_SIZE],
                                 params[PAINT_LAYER_GEN_BRICK_MORTAR_SMOOTH],
                                 params[PAINT_LAYER_GEN_BRICK_BIAS],
                                 params[PAINT_LAYER_GEN_BRICK_WIDTH],
                                 params[PAINT_LAYER_GEN_BRICK_ROW_HEIGHT],
                                 params[PAINT_LAYER_GEN_BRICK_OFFSET],
                                 int(params[PAINT_LAYER_GEN_BRICK_OFFSET_FREQUENCY]),
                                 params[PAINT_LAYER_GEN_BRICK_SQUASH],
                                 int(params[PAINT_LAYER_GEN_BRICK_SQUASH_FREQUENCY]));
}

static float generator_factor_wave(const GeneratorParams &params,
                                   const float3 &coordinate,
                                   const bool is_2d)
{
  return procedural_wave_factor(is_2d ? float3(coordinate.x, coordinate.y, 0.0f) : coordinate,
                                params[PAINT_LAYER_GEN_SCALE],
                                params[PAINT_LAYER_GEN_WAVE_DISTORTION],
                                params[PAINT_LAYER_GEN_WAVE_DETAIL],
                                params[PAINT_LAYER_GEN_WAVE_DETAIL_SCALE],
                                params[PAINT_LAYER_GEN_WAVE_DETAIL_ROUGHNESS],
                                params[PAINT_LAYER_GEN_WAVE_PHASE_OFFSET],
                                eProceduralWaveType(int(params[PAINT_LAYER_GEN_WAVE_TYPE])),
                                int(params[PAINT_LAYER_GEN_WAVE_BANDS_DIRECTION]),
                                int(params[PAINT_LAYER_GEN_WAVE_RINGS_DIRECTION]),
                                eProceduralWaveProfile(int(params[PAINT_LAYER_GEN_WAVE_PROFILE])));
}

/** The CPU factors the generator table names: the pattern's ramp position at \a coordinate, each
 * reading its kind's parameters out of the composite's payload. The registry's entries point here,
 * so a new parametric generator is one of these plus its table row. */
float paint_layer_generator_noise_cpu_factor(const PaintMaterialCompositeGradient &gradient,
                                             const float3 &coordinate)
{
  return generator_factor_noise(
      gradient.params, coordinate, generator_dimension_2d(gradient.space));
}

float paint_layer_generator_voronoi_cpu_factor(const PaintMaterialCompositeGradient &gradient,
                                               const float3 &coordinate)
{
  return generator_factor_voronoi(
      gradient.params, coordinate, generator_dimension_2d(gradient.space));
}

float paint_layer_generator_checker_cpu_factor(const PaintMaterialCompositeGradient &gradient,
                                               const float3 &coordinate)
{
  return generator_factor_checker(gradient.params, coordinate);
}

float paint_layer_generator_brick_cpu_factor(const PaintMaterialCompositeGradient &gradient,
                                             const float3 &coordinate)
{
  return generator_factor_brick(gradient.params, coordinate);
}

float paint_layer_generator_wave_cpu_factor(const PaintMaterialCompositeGradient &gradient,
                                            const float3 &coordinate)
{
  return generator_factor_wave(
      gradient.params, coordinate, generator_dimension_2d(gradient.space));
}

float paint_layer_generator_scratches_cpu_factor(const PaintMaterialCompositeGradient &gradient,
                                                  const float3 &coordinate)
{
  return BKE_paint_material_scratches_factor(
      coordinate, gradient.params, generator_dimension_2d(gradient.space));
}

float paint_layer_generator_weave_cpu_factor(const PaintMaterialCompositeGradient &gradient,
                                              const float3 &coordinate)
{
  return BKE_paint_material_weave_factor(
      coordinate, gradient.params, generator_dimension_2d(gradient.space));
}

float paint_layer_generator_grunge_cpu_factor(const PaintMaterialCompositeGradient &gradient,
                                               const float3 &coordinate)
{
  return BKE_paint_material_grunge_factor(
      coordinate, gradient.params, generator_dimension_2d(gradient.space));
}

/** \} */

bool BKE_paint_material_generator_sample(const PaintMaterialCompositeGradient &generator,
                                         const int x,
                                         const int y,
                                         float r_rgba[4])
{
  return BKE_paint_material_generator_sample(
      generator, x, y, generator.width, generator.height, r_rgba);
}

bool BKE_paint_material_generator_sample(const PaintMaterialCompositeGradient &generator,
                                         const int x,
                                         const int y,
                                         const int grid_width,
                                         const int grid_height,
                                         float r_rgba[4])
{
  const int width = std::max(grid_width, 1);
  const int height = std::max(grid_height, 1);
  const float u = (float(x) + 0.5f) / float(width);
  const float v = (float(y) + 0.5f) / float(height);
  float3 coordinate;
  if (generator.approximate || generator.surface == nullptr) {
    /* No surface: the texel's own UV is the coordinate. A UV row always reads this way. */
    coordinate = float3(u, v, 0.0f);
  }
  else if (generator.space == MA_PAINT_LAYER_GRADIENT_SPACE_GENERATED) {
    /* Generated reads the texel's orco, which is not the object-space position once a deformer
     * moved it; #sample_generated prefers the stored CD_ORCO and only falls back to the bbox
     * normalization. */
    if (!generator.surface->sample_generated(float2(u, v), coordinate)) {
      zero_v4(r_rgba);
      return false;
    }
  }
  else if (!generator.surface->sample(float2(u, v), coordinate)) {
    zero_v4(r_rgba);
    return false;
  }
  const bool is_gradient = generator.subtype == MA_PAINT_LAYER_GENERATOR_GRADIENT;
  const float3 placed = is_gradient ?
                            coordinate :
                            paint_layer_generator_coordinate(
                                coordinate,
                                paint_layer_generator_scale_y_ratio(
                                    generator.params[PAINT_LAYER_GEN_SCALE],
                                    generator.params[PAINT_LAYER_GEN_SCALE_Y],
                                    generator.params[PAINT_LAYER_GEN_SCALE_LOCK] != 0.0f),
                                generator.params[PAINT_LAYER_GEN_ROTATION],
                                generator.params[PAINT_LAYER_GEN_OFFSET_X],
                                generator.params[PAINT_LAYER_GEN_OFFSET_Y],
                                int(generator.params[PAINT_LAYER_GEN_SEED]));
  /* The ramp position comes through the generator table: the subtype names the CPU factor, as the
   * generated graph reads its own through the shared group. The explicit Gradient call is the way
   * out for a subtype this build does not know -- a file from a newer one -- where the payload is
   * still a Gradient's and reading it as one is the closest answer; the describe refuses such a
   * row before it gets here, so the path only guards a hand-built item. */
  const PaintLayerGeneratorInfo *entry = BKE_paint_layers_generator_find(generator.subtype);
  float t;
  if (entry != nullptr) {
    t = entry->cpu_factor(generator, placed);
    if (!is_gradient) {
      /* A pattern's factor is a ramp position, and the ramp reads 0..1; the Gradient's line is
       * left alone, its own ramp lays the whole of it. */
      t = clamp_f(t, 0.0f, 1.0f);
    }
  }
  else {
    BLI_assert_msg(false, "Unknown paint layer generator subtype");
    t = BKE_paint_layers_gradient_factor(generator.payload, coordinate);
  }
  if (generator.color_channel) {
    if (is_gradient) {
      BKE_paint_layers_gradient_color(generator.payload, t, r_rgba);
      return true;
    }
    /* The colour the entry's own_color answers before any ramp is asked -- the Voronoi cell
     * colour, under the parameter state the entry's own math checks. */
    if (entry != nullptr && entry->own_color != nullptr &&
        entry->own_color(generator, placed, r_rgba))
    {
      return true;
    }
    if (generator.color_ramp_valid && generator.color_ramp.tot > 0) {
      BKE_colorband_evaluate(&generator.color_ramp, t, r_rgba);
      return true;
    }
    /* A row with no band of its own lays the factor as a grey, which is what a graph with no
     * Color Ramp node reads: the factor into all three of an RGB. */
    r_rgba[0] = r_rgba[1] = r_rgba[2] = t;
    r_rgba[3] = 1.0f;
    return true;
  }
  if (generator.scalar_ramp_valid) {
    /* The channel's grey value ramp: red carries the value, the alpha stays one. */
    float rgba[4];
    BKE_colorband_evaluate(&generator.scalar_ramp, t, rgba);
    r_rgba[0] = r_rgba[1] = r_rgba[2] = rgba[0];
    r_rgba[3] = 1.0f;
    return true;
  }
  const float value = generator.scalar_start + (generator.scalar_end - generator.scalar_start) * t;
  r_rgba[0] = r_rgba[1] = r_rgba[2] = value;
  r_rgba[3] = 1.0f;
  return true;
}

/** How many generator payloads \a image_layers lays: a row, a correction and a mask item each
 * sample its line and ramp per texel, so their count is the part of an evaluation a line drag pays
 * for again on every step. */
int composite_stack_generator_count(Span<PaintMaterialCompositeImageLayer> image_layers)
{
  int count = 0;
  for (const PaintMaterialCompositeImageLayer &layer : image_layers) {
    if (layer.gradient.enabled) {
      count++;
    }
    for (const PaintMaterialCompositeCorrection &correction : layer.content_corrections) {
      if (correction.gradient.enabled) {
        count++;
      }
    }
    for (const PaintMaterialCompositeMaskItem &item : layer.mask_items) {
      if (item.gradient.enabled) {
        count++;
      }
    }
    if (layer.is_folder) {
      count += composite_stack_generator_count(
          Span<PaintMaterialCompositeImageLayer>(layer.children.data(),
                                                 int64_t(layer.children.size())));
    }
  }
  return count;
}

/**
 * Fold a generator into \a hash: every parameter moves the composited pixels. The payload, the
 * surface and the scalar ends belong to the Gradient; the subtype, the space and the schema's
 * parameters belong to every generator, and a colour ramp of its own to all but the Gradient.
 */
uint64_t composite_generator_hash(uint64_t hash, const PaintMaterialCompositeGradient &gradient)
{
  if (!gradient.enabled) {
    return hash;
  }
  hash = get_default_hash(hash,
                          gradient.color_channel,
                          int(gradient.subtype),
                          int(gradient.space),
                          int(gradient.payload.space));
  hash = get_default_hash(hash,
                          int(gradient.payload.type),
                          int(gradient.payload.repeat),
                          gradient.payload.start[0],
                          gradient.payload.start[1]);
  hash = get_default_hash(hash,
                          gradient.payload.start[2],
                          gradient.payload.end[0],
                          gradient.payload.end[1],
                          gradient.payload.end[2]);
  hash = get_default_hash(hash, BKE_paint_layers_gradient_midpoint_effective(gradient.payload));
  /* The approximate and exact variants must not share a cache entry. The content revision, unlike
   * the identity key, changes when a notifier forces the same object/UV/size surface to be
   * rebuilt. */
  hash = get_default_hash(hash,
                          gradient.surface_key,
                          gradient.approximate,
                          gradient.surface ? gradient.surface->revision : 0);
  hash = get_default_hash(hash,
                          gradient.scalar_start,
                          gradient.scalar_end,
                          gradient.width,
                          gradient.height);
  hash = get_default_hash(hash, gradient.scalar_ramp_valid);
  if (gradient.scalar_ramp_valid) {
    hash = get_default_hash(hash, BKE_paint_layers_gradient_ramp_hash(gradient.scalar_ramp));
  }
  /* Only the Gradient reads a band out of its payload; the other generators use the item's own
   * \a color_ramp, which a row's Color Ramp editor writes. */
  hash = get_default_hash(hash, gradient.color_ramp_valid);
  if (gradient.color_ramp_valid) {
    hash = get_default_hash(hash, paint_layer_ramp_hash(gradient.color_ramp));
  }
  hash = get_default_hash(hash, BKE_paint_layers_gradient_ramp_hash(gradient.payload.ramp));
  if (gradient.subtype != MA_PAINT_LAYER_GENERATOR_GRADIENT) {
    /* Every schema parameter reaches a factor of the composite, so every one has to move the
     * hash. Trailing slots stay zero for the kinds with fewer parameters, which costs a few
     * multiplications and saves the hash from having to know each schema's length. */
    for (const float param : gradient.params) {
      hash = get_default_hash(hash, param);
    }
  }
  return hash;
}

}  // namespace blender
