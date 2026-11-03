/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * What a Paint Layers row depends on. #paint_layer_dependencies is the single place that knows
 * which ID pointers a row holds, so the ID walk, the map collection and the map copy read one
 * list instead of each repeating the row's layout. A new kind registers its IDs here once.
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "BLI_function_ref.hh"
#include "BLI_math_base_safe.h"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "DNA_ID.h"
#include "DNA_material_types.h"
#include "DNA_scene_types.h"

#include "BKE_lib_query.hh"
#include "BKE_paint_layers.hh"

namespace blender {

struct BlendDataReader;
struct BlendWriter;

enum class PaintLayerDependencyKind : uint8_t {
  /* A channel's paint map. */
  ImageMap = 0,
  /**
   * A map of the row's bake cache: generated from the description, so it is never packed on save
   * and a copy of the material gets its own.
   */
  BakeCache = 1,
  /** The material a Material row reads: a user of that material, never a map to paint. */
  SourceMaterial = 2,
  /**
   * The PaintVector container a Curve Patch row owns: its curves are the row's content, so it is
   * a dependency the way a source is, and the row's bake digests it.
   */
  CurvePatchVectors = 3,
  /** The brush snapshot a Curve Patch row owns (the row's tool state, digested by the bake). */
  BrushPreset = 4,
};

/** What a change to the dependency invalidates. */
enum PaintLayerDependencyInfluence : uint8_t {
  PAINT_LAYER_DEP_TOPOLOGY = 1 << 0,
  PAINT_LAYER_DEP_PIXELS = 1 << 1,
  PAINT_LAYER_DEP_OWNERSHIP = 1 << 2,
};

struct PaintLayerDependency {
  /** Address of the pointer: the ID walk and the copy remap rewrite it in place. */
  ID **slot;
  PaintLayerDependencyKind kind;
  uint8_t influence;
  /** The channel the ID belongs to, or -1. */
  int8_t channel;
};

/* -------------------------------------------------------------------- */
/** \name Kind registry
 *
 * A row's kind is the pair (#MaterialPaintLayer::source, #MaterialPaintLayer::kind_subtype). The
 * table answers what a kind is and where it may sit; the behavior the generator and the CPU stack
 * share moves in here kind by kind, so a new kind is one entry rather than a branch per consumer.
 * \{ */

enum class PaintLayerKindCategory : uint8_t {
  /** Produces its own colour and coverage. */
  Source,
  /** Reads what is below it and changes it; produces no colour of its own. */
  Operator,
  /** Holds a stack of rows and lays their isolated result. */
  Container,
};

/** Bits of #PaintLayerKindInfo::allowed_roles, one per #eMaterialPaintLayerRole. */
inline constexpr uint8_t paint_layer_role_bit(const int role)
{
  return uint8_t(1u << role);
}

enum class PaintLayerKindParamType : uint8_t {
  /** A float slider, the original and still most common kind. */
  Float = 0,
  /** An integer, stored in the row's system properties. */
  Int,
  /** An integer chosen from a fixed list; #PaintLayerKindParam::enum_items names the choices. */
  Enum,
  /** Four floats (RGB + alpha), stored as an array property. */
  Color,
  /** A ColorBand, kept in #MaterialPaintLayer::ramp rather than in the properties. */
  Ramp,
  /** An on/off checkbox, stored as an #IDP_BOOLEAN in the row's system properties. */
  Bool,
  /** A CurveMapping, kept in #MaterialPaintLayer::curves rather than in the properties. */
  Curves,
  /** A #MaterialPaintLayerColorBalance, kept in #MaterialPaintLayer::color_balance. */
  ColorBalance,
  /** A #MaterialPaintLayerChannelMixer, kept in #MaterialPaintLayer::channel_mixer. */
  ChannelMixer,
};

/**
 * The lifecycle of one kind of row payload (a color balance, a channel mixer): where the row's
 * pointer lives, and how to create, copy, hash, write and read the value behind it. The registry in
 * `paint_layers_kind.cc` is the one list a consumer walks, so a new payload is an entry there and a
 * kind's schema entry, not a branch in every copy, file and bake path.
 */
struct PaintLayerPayloadInfo {
  PaintLayerKindParamType type;
  /** Offset of the payload pointer in #MaterialPaintLayer, from `offsetof`. */
  size_t offset;
  void *(*create)();
  void (*destroy)(void *payload);
  void *(*copy)(const void *payload);
  uint64_t (*hash)(const void *payload);
  void (*write)(BlendWriter *writer, const void *payload);
  void (*read)(BlendDataReader *reader, void **slot);
  /** How an edit of the payload tags the material; null for a payload with no value edit. */
  void (*edited)(Material &ma);
};

/** One choice of an #PaintLayerKindParamType::Enum parameter. */
struct PaintLayerKindEnumItem {
  int value;
  const char *identifier;
  const char *label;
};

/**
 * One parameter of a kind. The schema lives with the kind; the value lives in the row's
 * #MaterialPaintLayer::system_properties under #name, and reads as the default while unset, so a
 * row never has to carry parameters it did not change.
 *
 * The fields after #max are type-specific and default to a plain float, so a kind that has only
 * float parameters writes the same five-field initializer it always did. A ramp has no value in
 * the properties: #MaterialPaintLayer::ramp holds it.
 */
struct PaintLayerKindParam {
  const char *name;
  const char *label;
  float default_value;
  float min;
  float max;
  PaintLayerKindParamType type = PaintLayerKindParamType::Float;
  /** The choices of an #PaintLayerKindParamType::Enum parameter, or null. */
  const PaintLayerKindEnumItem *enum_items = nullptr;
  int enum_items_num = 0;
  /** The default of a Color parameter. */
  float default_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
};

/** What a kind is, as far as the rules shared by the generator and the CPU stack care. */
enum PaintLayerKindFlag : uint16_t {
  /**
   * The kind lays the value it stores while it has no map (a Fill): a live record alone makes it
   * take part, as a correction it lays its flat colour, as a mask item its flat grey.
   */
  PAINT_LAYER_KIND_FLAT_VALUE = 1 << 0,
  /**
   * The kind changes the colour it reads instead of laying one of its own (a filter): as a
   * correction it works on its row's colour, as a layer on the stack below it, as a mask item on
   * the coverage so far. It owns no map and takes part through its channel records only.
   */
  PAINT_LAYER_KIND_ACTS_ON_BELOW = 1 << 1,
  /**
   * A map-driven mask coverage operator (Color ID, UV Island): it lays `match` from the map of its
   * Base Color record over the coverage below it, `cov = lerp(cov_below, cov_below * match,
   * opacity)`. Unlike #PAINT_LAYER_KIND_ACTS_ON_BELOW it owns a map, and unlike a laid mask item it
   * takes part whenever its element is enabled: a missing map samples as `match = 0` instead of
   * dropping the operator out. Its Base Color record is structural -- no user channel switch -- so
   * the element's visibility is the only thing that turns it on or off.
   */
  PAINT_LAYER_KIND_MAP_COVERAGE = 1 << 2,
  /**
   * The kind reads another ID instead of owning a map (a Material row): it has no channel records
   * of its own, so a stroke has nothing to write into and a new record would be one the kind never
   * reads. What it shows is decided from that ID, not from the row.
   */
  PAINT_LAYER_KIND_READS_SOURCE = 1 << 3,
  /**
   * An Anchor Point: an inert marker inside a row's effects or mask stack. It produces nothing of
   * its own; it publishes the slice of the row it sits in to every reader that names its marker.
   * The list it sits in decides whether it publishes content or the mask.
   */
  PAINT_LAYER_KIND_ANCHOR_POINT_MARK = 1 << 4,
  /**
   * An Anchor Point Reader: it lays the slice a marker publishes instead of owning a map. Like a
   * row that reads another material it has no channel records of its own; unlike one it reads the
   * same material, so it is not #PAINT_LAYER_KIND_READS_SOURCE (which is tied to
   * #MaterialPaintLayer::material).
   */
  PAINT_LAYER_KIND_READS_ANCHOR_POINT = 1 << 5,
  /**
   * The kind produces its colour and coverage procedurally (a Gradient): a live channel record
   * makes it take part, even without a map or a flat value. It owns no map, so a stroke has nothing
   * to write into; its values are its kind's payload and its channel records' endpoints.
   */
  PAINT_LAYER_KIND_GENERATES = 1 << 6,
  /**
   * A CPU-computed effect: the live graph cannot reproduce its result, so the effect cache computes
   * it for the CPU composite and the bake, and a row that carries one is baked for the live graph,
   * whatever its bake mode. A Blur is one because it reads a neighbourhood the graph has no node
   * for; a Noise is another because the graph's White Noise and the CPU hash are different
   * sequences. Such a kind exists only as a correction of a row's colour. The kernel reads no ID,
   * so the flag changes no dependency walk.
   */
  PAINT_LAYER_KIND_NEEDS_BAKE = 1 << 7,
  /**
   * A filter that reads the encoding of a Normal channel as numbers, not as a direction (a Normal
   * Convert): the one filter such a channel may carry, where every other filter would corrupt the
   * vector. Past the eighth bit, which is why the flags are 16 bits wide.
   */
  PAINT_LAYER_KIND_NORMAL_SAFE = 1 << 8,
  /**
   * A pointwise filter the combined preview shader has no branch for (a Deband, which needs the
   * texel coordinates, or a Normal Convert). The gather refuses the GPU path and falls back to the
   * CPU, so a new such kind cannot be silently shaded as a no-op.
   */
  PAINT_LAYER_KIND_NO_GPU_BRANCH = 1 << 9,
};

struct PaintLayerKindInfo {
  /** Registry key: #MaterialPaintLayer::source and #MaterialPaintLayer::kind_subtype. */
  int8_t source;
  int16_t subtype;
  const char *name;
  PaintLayerKindCategory category;
  /** The roles the kind may take, from #paint_layer_role_bit. */
  uint8_t allowed_roles;
  /** The kind's parameters in a fixed order, or null for a kind with none. */
  const PaintLayerKindParam *params = nullptr;
  int params_num = 0;
  /** #PaintLayerKindFlag bits. */
  uint16_t flags = 0;
  /**
   * Whether the kind's map may be a stroke target. A #PAINT_LAYER_KIND_MAP_COVERAGE kind whose map
   * is reference-only (Color ID: an external ID Map) reports false, so the target resolve and the UI
   * refuse to select it for painting. Meaningless for a kind that owns no map.
   */
  bool map_paintable = false;
};

/** The positions of the Levels filter's parameters in its schema. */
enum PaintLayerLevelsParam : int {
  PAINT_LAYER_LEVELS_IN_BLACK = 0,
  PAINT_LAYER_LEVELS_IN_WHITE = 1,
  PAINT_LAYER_LEVELS_GAMMA = 2,
  PAINT_LAYER_LEVELS_OUT_BLACK = 3,
  PAINT_LAYER_LEVELS_OUT_WHITE = 4,
  PAINT_LAYER_LEVELS_PARAM_NUM = 5,
};

/** The positions of the HSL filter's parameters in its schema. */
enum PaintLayerHslParam : int {
  PAINT_LAYER_HSL_HUE = 0,
  PAINT_LAYER_HSL_SATURATION = 1,
  PAINT_LAYER_HSL_VALUE = 2,
  /** The lift the muted colours get, in the addon's -100..100 scaled to -1..1: zero is no-op. */
  PAINT_LAYER_HSL_VIBRANCE = 3,
  PAINT_LAYER_HSL_PARAM_NUM = 4,
};

/** The positions of the Brightness/Contrast filter's parameters in its schema. */
enum PaintLayerBrightnessContrastParam : int {
  PAINT_LAYER_BRIGHTNESS_CONTRAST_BRIGHTNESS = 0,
  PAINT_LAYER_BRIGHTNESS_CONTRAST_CONTRAST = 1,
  PAINT_LAYER_BRIGHTNESS_CONTRAST_PARAM_NUM = 2,
};

/** The positions of the Color Ramp filter's parameters in its schema; the ramp payload is last. */
enum PaintLayerColorRampParam : int {
  /** 0 ignores the alpha of the ramp stop, 1 multiplies the filter's strength by it. */
  PAINT_LAYER_COLOR_RAMP_AFFECT_ALPHA = 0,
  PAINT_LAYER_COLOR_RAMP_PARAM_NUM = 2,
};

/** The positions of the Posterize filter's parameters in its schema. */
enum PaintLayerPosterizeParam : int {
  PAINT_LAYER_POSTERIZE_STEPS = 0,
  PAINT_LAYER_POSTERIZE_PARAM_NUM = 1,
};

/** The operation of the Math filter, the value of its #PAINT_LAYER_MATH_OPERATION parameter. */
enum PaintLayerMathOperation : int {
  PAINT_LAYER_MATH_OP_ADD = 0,
  PAINT_LAYER_MATH_OP_SUBTRACT = 1,
  PAINT_LAYER_MATH_OP_MULTIPLY = 2,
  PAINT_LAYER_MATH_OP_DIVIDE = 3,
  PAINT_LAYER_MATH_OP_MINIMUM = 4,
  PAINT_LAYER_MATH_OP_MAXIMUM = 5,
  PAINT_LAYER_MATH_OP_POWER = 6,
  PAINT_LAYER_MATH_OP_LOGARITHM = 7,
};

/** The positions of the Math filter's parameters in its schema. */
enum PaintLayerMathParam : int {
  PAINT_LAYER_MATH_OPERATION = 0,
  /** The node's own Use Clamp: the result folds into [0, 1]. */
  PAINT_LAYER_MATH_CLAMP = 1,
  /** One operand per colour channel: the value the channel is combined with. */
  PAINT_LAYER_MATH_OPERAND_R = 2,
  PAINT_LAYER_MATH_OPERAND_G = 3,
  PAINT_LAYER_MATH_OPERAND_B = 4,
  PAINT_LAYER_MATH_PARAM_NUM = 5,
};

/** The positions of the Threshold filter's parameters in its schema. */
enum PaintLayerThresholdParam : int {
  PAINT_LAYER_THRESHOLD_THRESHOLD = 0,
  PAINT_LAYER_THRESHOLD_PARAM_NUM = 1,
};

/** The positions of the Clamp filter's parameters in its schema. */
enum PaintLayerClampParam : int {
  PAINT_LAYER_CLAMP_MIN = 0,
  PAINT_LAYER_CLAMP_MAX = 1,
  PAINT_LAYER_CLAMP_PARAM_NUM = 2,
};

/**
 * The positions of the Color Balance filter's folded triples in the 12-float layout the CPU
 * composite, the generated graph and the GPU push constants share: A is the slope of the linear
 * part, B its offset and E the power's exponent; each lands in the `.xyz` of its own float4.
 */
enum PaintLayerColorBalanceParam : int {
  PAINT_LAYER_COLOR_BALANCE_A = 0,
  PAINT_LAYER_COLOR_BALANCE_B = 4,
  PAINT_LAYER_COLOR_BALANCE_E = 8,
  /** The length the folded array needs; the triples' fourths ([3], [7], [11]) stay unused. */
  PAINT_LAYER_COLOR_BALANCE_FOLDED_NUM = 12,
};

/**
 * The three folded triples the Color Balance filter reads (A: slope/gain, B: offset/lift,
 * E: power/gamma).
 */
struct PaintLayerColorBalanceTriple {
  const char *suffix;
  int position;
};
inline constexpr int PAINT_LAYER_COLOR_BALANCE_TRIPLE_NUM = 3;
inline constexpr PaintLayerColorBalanceTriple
    PAINT_LAYER_COLOR_BALANCE_TRIPLES[PAINT_LAYER_COLOR_BALANCE_TRIPLE_NUM] = {
        {"Balance A", PAINT_LAYER_COLOR_BALANCE_A},
        {"Balance B", PAINT_LAYER_COLOR_BALANCE_B},
        {"Balance E", PAINT_LAYER_COLOR_BALANCE_E},
};

/** The positions of the Channel Mixer filter's parameters in its schema. */
enum PaintLayerChannelMixerParam : int {
  /** The payload: #MaterialPaintLayerChannelMixer, behind the row's own pointer. */
  PAINT_LAYER_CHANNEL_MIXER_MIXER = 0,
  /** On: a grey from the red row and the constants' mean. */
  PAINT_LAYER_CHANNEL_MIXER_MONOCHROME = 1,
  PAINT_LAYER_CHANNEL_MIXER_PARAM_NUM = 2,
};

/**
 * The layout of the Channel Mixer's folded 12 floats, shared by the CPU composite, the generated
 * graph and the GPU push constants: the three rows in the `.xyz` of the three float4s (positions
 * 0, 4, 8) and the three constants in their `.w` (3, 7, 11), the output channel k reading row k.
 */
enum PaintLayerChannelMixerFolded : int {
  PAINT_LAYER_CHANNEL_MIXER_ROW_R = 0,
  PAINT_LAYER_CHANNEL_MIXER_ROW_G = 4,
  PAINT_LAYER_CHANNEL_MIXER_ROW_B = 8,
  /** The position of the first constant: the `.w` of the red row's float4. */
  PAINT_LAYER_CHANNEL_MIXER_CONSTANTS = 3,
  /** The length the folded array needs; the constant of row k sits at `3 + 4 * k`. */
  PAINT_LAYER_CHANNEL_MIXER_FOLDED_NUM = 12,
};

/** One vector a Channel Mixer input carries: a row, or the three constants. */
struct PaintLayerChannelMixerVector {
  const char *suffix;
  int position;
};
inline constexpr int PAINT_LAYER_CHANNEL_MIXER_VECTOR_NUM = 4;
inline constexpr PaintLayerChannelMixerVector
    PAINT_LAYER_CHANNEL_MIXER_VECTORS[PAINT_LAYER_CHANNEL_MIXER_VECTOR_NUM] = {
        {"Mixer R", PAINT_LAYER_CHANNEL_MIXER_ROW_R},
        {"Mixer G", PAINT_LAYER_CHANNEL_MIXER_ROW_G},
        {"Mixer B", PAINT_LAYER_CHANNEL_MIXER_ROW_B},
        {"Mixer Constants", PAINT_LAYER_CHANNEL_MIXER_CONSTANTS},
};

/** The positions of a map-coverage operator's parameters in its schema. */
enum PaintLayerCoverageParam : int {
  /** How far a sample may differ from a stored colour and still match (max-channel). */
  PAINT_LAYER_COVERAGE_TOLERANCE = 0,
  /** The width of the falloff past the tolerance; zero reads as a hard edge. */
  PAINT_LAYER_COVERAGE_SOFTNESS = 1,
  PAINT_LAYER_COVERAGE_PARAM_NUM = 2,
};

/** The shape of the Blur filter's kernel, the value of its #PAINT_LAYER_BLUR_SHAPE parameter. */
enum PaintLayerBlurShape : int {
  PAINT_LAYER_BLUR_BOX = 0,
  PAINT_LAYER_BLUR_GAUSSIAN = 1,
};

/** The positions of the Blur filter's parameters in its schema. */
enum PaintLayerBlurParam : int {
  PAINT_LAYER_BLUR_SHAPE = 0,
  PAINT_LAYER_BLUR_RADIUS = 1,
  PAINT_LAYER_BLUR_PARAM_NUM = 2,
};

/** The positions of the Sharpen filter's parameters in its schema. */
enum PaintLayerSharpenParam : int {
  PAINT_LAYER_SHARPEN_RADIUS = 0,
  PAINT_LAYER_SHARPEN_AMOUNT = 1,
  PAINT_LAYER_SHARPEN_PARAM_NUM = 2,
};

/** The value of the High Pass filter's #PAINT_LAYER_HIGH_PASS_OUTPUT. */
enum PaintLayerHighPassOutput : int {
  /** The detail of each colour channel. */
  PAINT_LAYER_HIGH_PASS_OUTPUT_COLOR = 0,
  /** The detail of the luminance, the same grey in all three channels. */
  PAINT_LAYER_HIGH_PASS_OUTPUT_GRAYSCALE = 1,
};

/** The positions of the High Pass filter's parameters in its schema. */
enum PaintLayerHighPassParam : int {
  PAINT_LAYER_HIGH_PASS_RADIUS = 0,
  PAINT_LAYER_HIGH_PASS_CONTRAST = 1,
  PAINT_LAYER_HIGH_PASS_OUTPUT = 2,
  PAINT_LAYER_HIGH_PASS_PARAM_NUM = 3,
};

/** The direction of the Erode/Dilate kernel, the value of its #PAINT_LAYER_ERODE_DILATE_MODE. */
enum PaintLayerErodeDilateMode : int {
  /** The minimum of the neighbourhood: the bright regions shrink. */
  PAINT_LAYER_ERODE_DILATE_ERODE = 0,
  /** The maximum of the neighbourhood: the bright regions grow. */
  PAINT_LAYER_ERODE_DILATE_DILATE = 1,
};

/** The positions of the Erode/Dilate filter's parameters in its schema. */
enum PaintLayerErodeDilateParam : int {
  PAINT_LAYER_ERODE_DILATE_MODE = 0,
  PAINT_LAYER_ERODE_DILATE_RADIUS = 1,
  PAINT_LAYER_ERODE_DILATE_PARAM_NUM = 2,
};

/** The kernel of the Glow filter, the value of its #PAINT_LAYER_GLOW_MODE parameter. */
enum PaintLayerGlowMode : int {
  /** Three box passes: a soft, gaussian-like falloff. */
  PAINT_LAYER_GLOW_SOFT = 0,
  /** One box pass: a hard-edged falloff. */
  PAINT_LAYER_GLOW_PRECISE = 1,
};

/** The positions of the Glow filter's parameters in its schema. */
enum PaintLayerGlowParam : int {
  PAINT_LAYER_GLOW_MODE = 0,
  /** Whole texels: the kernel's half reach. */
  PAINT_LAYER_GLOW_WIDTH = 1,
  /** The luminance the glow starts at: a higher value chokes the source to its bright core. */
  PAINT_LAYER_GLOW_SPREAD = 2,
  PAINT_LAYER_GLOW_OPACITY = 3,
  PAINT_LAYER_GLOW_COLOR = 4,
  PAINT_LAYER_GLOW_PARAM_NUM = 5,
};

/** What the Bevel filter reads its border from, the value of its #PAINT_LAYER_BEVEL_SOURCE
 * parameter. */
enum PaintLayerBevelSource : int {
  /** The content's own alpha: a row painted edge to edge has no border and is one everywhere. */
  PAINT_LAYER_BEVEL_SOURCE_ALPHA = 0,
  /** The scene-linear luminance, the one the Glow and the Noise read: the dark side of the content
   * is the border the ramp rises from. */
  PAINT_LAYER_BEVEL_SOURCE_LUMINANCE = 1,
};

/** The corner metric of the Bevel filter, the value of its #PAINT_LAYER_BEVEL_CORNER parameter. */
enum PaintLayerBevelCorner : int {
  /** The chamfer 3-4 walk: an approximation of the Euclidean distance that rounds the corners. */
  PAINT_LAYER_BEVEL_ROUND = 0,
  /** The chessboard walk: the corners stay square. */
  PAINT_LAYER_BEVEL_ANGULAR = 1,
};

/** The positions of the Bevel filter's parameters in its schema. */
enum PaintLayerBevelParam : int {
  PAINT_LAYER_BEVEL_SOURCE = 0,
  /** Whole texels the ramp takes to rise from the border to one. */
  PAINT_LAYER_BEVEL_DISTANCE = 1,
  PAINT_LAYER_BEVEL_CORNER = 2,
  /** Whole texels of the box blur laid over the finished ramp; zero leaves it as it is. */
  PAINT_LAYER_BEVEL_SMOOTH = 3,
  PAINT_LAYER_BEVEL_PARAM_NUM = 4,
};

/** The positions of the Color Match filter's parameters in its schema. */
enum PaintLayerColorMatchParam : int {
  /** On: the source colour is the picked one; off: the average of the input itself. */
  PAINT_LAYER_COLOR_MATCH_SOURCE_PICKED = 0,
  PAINT_LAYER_COLOR_MATCH_SOURCE_COLOR = 1,
  PAINT_LAYER_COLOR_MATCH_TARGET_COLOR = 2,
  /** On: a per-texel jitter of the three amounts below is laid over the grade. */
  PAINT_LAYER_COLOR_MATCH_VARIATION = 3,
  /** The jitter's reach in turns of hue. */
  PAINT_LAYER_COLOR_MATCH_VARIATION_HUE = 4,
  PAINT_LAYER_COLOR_MATCH_VARIATION_CHROMA = 5,
  PAINT_LAYER_COLOR_MATCH_VARIATION_LUMA = 6,
  PAINT_LAYER_COLOR_MATCH_PARAM_NUM = 7,
};

/** The per-texel jitter a Color Match lays over the grade: the reach in turns of hue and in shares
 * of chroma and luma. Zero amounts leave the jitter out. */
struct PaintLayerColorMatchVariation {
  float hue = 0.0f;
  float chroma = 0.0f;
  float luma = 0.0f;
};

/**
 * The Color Match grade applied to one texel \a rgb in place: each HSL component moves by the
 * target's difference from the source's, then the variation jitters it from a hash of the texel
 * (\a x, \a y). Zero amounts leave the jitter out.
 */
void paint_layer_color_match_apply(const float source_color[3],
                                   const float target_color[3],
                                   const PaintLayerColorMatchVariation &variation,
                                   int x,
                                   int y,
                                   float rgb[3]);

/** The positions of the Exposure filter's parameters in its schema. */
enum PaintLayerExposureParam : int {
  /** The power of two the colour is scaled by: one stop per unit. */
  PAINT_LAYER_EXPOSURE_EXPOSURE = 0,
  /** What the scaled colour rises or falls by, in scene linear. */
  PAINT_LAYER_EXPOSURE_OFFSET = 1,
  /** The power every positive value is raised to, read as one over it. */
  PAINT_LAYER_EXPOSURE_GAMMA = 2,
  PAINT_LAYER_EXPOSURE_PARAM_NUM = 3,
};

/** The positions of the Invert filter's parameters in its schema. */
enum PaintLayerInvertParam : int {
  /** How far the red channel turns to its complement: one is the whole way, zero none of it. */
  PAINT_LAYER_INVERT_AMOUNT_R = 0,
  /** The same for green. */
  PAINT_LAYER_INVERT_AMOUNT_G = 1,
  /** The same for blue. */
  PAINT_LAYER_INVERT_AMOUNT_B = 2,
  PAINT_LAYER_INVERT_PARAM_NUM = 3,
};

/** The two modes of the Deband filter. */
enum PaintLayerDebandMode : int {
  /** A triangular noise under the quantization step, breaking the bands up. */
  PAINT_LAYER_DEBAND_DEBAND = 0,
  /** The colour rounded to fewer bits, the same triangular noise on top. */
  PAINT_LAYER_DEBAND_DITHER = 1,
};

/** The positions of the Deband filter's parameters in its schema. */
enum PaintLayerDebandParam : int {
  PAINT_LAYER_DEBAND_MODE = 0,
  /** Whole bits the colour keeps: 16 is the identity's neighbourhood, 4 the coarsest. */
  PAINT_LAYER_DEBAND_BITS = 1,
  /** Which pattern of the hash family the texels read. */
  PAINT_LAYER_DEBAND_SEED = 2,
  PAINT_LAYER_DEBAND_PARAM_NUM = 3,
};

/**
 * The HSL filter applied to \a rgb in place, at full strength: the Hue/Saturation/Value node's
 * function (hue shifted by `hue - 0.5` turns, saturation clamped to one, value scaled, result not
 * below zero), so the CPU composite and the generated node agree.
 */
void paint_layer_hsl_apply(float rgb[3], const float params[PAINT_LAYER_HSL_PARAM_NUM]);

/**
 * One uniform in [0, 1) of a texel hash family: \a salt separates the passes and the channels, the
 * seed moves the pattern, and the texel's coordinates are where the noise lives. The one generator
 * the Deband and Noise filters both read.
 */
float paint_layer_texel_hash_uniform(int x, int y, int seed, uint32_t salt);

/**
 * One uniform of the Deband filter's triangular pair, from #paint_layer_texel_hash_uniform: the
 * salt keeps the two passes and the three channels apart.
 */
float paint_layer_deband_noise(int x, int y, int seed, int channel, int pass);

/**
 * The Deband/Dither filter applied to \a rgb in place at the texel \a texel_xy: a triangular
 * (difference of two uniforms) noise of one quantization step, either under the colour itself or
 * under a rounding to `bits` levels, clamped to [0, 1] as the addon's clip does. A texel pointer of
 * null reads the origin, the stand-in for the callers that have no position.
 */
void paint_layer_deband_apply(float rgb[3],
                              const float params[PAINT_LAYER_DEBAND_PARAM_NUM],
                              const int texel_xy[2]);

/** The Noise filter's families. */
enum PaintLayerNoiseType : int {
  /** A sum of three uniforms, scaled so the spread reads as the intensity. */
  PAINT_LAYER_NOISE_GAUSSIAN = 0,
  /** One uniform, signed and scaled. */
  PAINT_LAYER_NOISE_UNIFORM = 1,
  /** The gaussian noise as a factor of the colour itself. */
  PAINT_LAYER_NOISE_SPECKLE = 2,
  /** The channels the uniform puts under the intensity's half read as black, the ones over the
   * other end as white, and the rest stay as they are. */
  PAINT_LAYER_NOISE_SALT_PEPPER = 3,
};

/** The positions of the Noise filter's parameters in its schema. */
enum PaintLayerNoiseParam : int {
  PAINT_LAYER_NOISE_TYPE = 0,
  /** The spread of the noise, in colour: the sigma of the gaussian family, the reach of the rest. */
  PAINT_LAYER_NOISE_INTENSITY = 1,
  /** On: the noise moves the scene-linear luminance and the colour follows it. */
  PAINT_LAYER_NOISE_MONOCHROME = 2,
  PAINT_LAYER_NOISE_SEED = 3,
  PAINT_LAYER_NOISE_PARAM_NUM = 4,
};

/**
 * One uniform in [0, 1) of the Noise filter, from the same hash family the Deband pair reads: the
 * salt keeps the three passes and the channels apart, the seed moves the pattern, and the texel is
 * where the noise lives.
 */
float paint_layer_noise_uniform(int x, int y, int seed, int channel, int pass);

/**
 * The gaussian noise of one texel channel: three uniforms of #paint_layer_noise_uniform summed,
 * centred and scaled, so the spread of the answer reads as \a intensity. The one function the
 * effect kernel and the tests both call, so they cannot disagree.
 */
float paint_layer_noise_gaussian(int x, int y, int seed, int channel, float intensity);

/** The Normal Convert filter's modes, over the encoded [0, 1] channels. */
enum PaintLayerNormalConvertMode : int {
  /** The DirectX green flipped into the OpenGL one: `g = 1 - g`. */
  PAINT_LAYER_NORMAL_CONVERT_DX_TO_GL = 0,
  /** The same flip the other way. */
  PAINT_LAYER_NORMAL_CONVERT_GL_TO_DX = 1,
  /** A two-channel map's blue rebuilt from the other two: `z = sqrt(max(1 - x² - y², 0))`. */
  PAINT_LAYER_NORMAL_CONVERT_REBUILD_Z = 2,
  /** The blue channel dropped, leaving the two-channel form. */
  PAINT_LAYER_NORMAL_CONVERT_CLEAR_Z = 3,
  /** The scene-linear values encoded as sRGB, the IEC 61966-2.1 broken line. */
  PAINT_LAYER_NORMAL_CONVERT_LINEAR_TO_SRGB = 4,
  /** The sRGB values decoded to scene linear, the same broken line backwards. */
  PAINT_LAYER_NORMAL_CONVERT_SRGB_TO_LINEAR = 5,
};

/** The positions of the Normal Convert filter's parameters in its schema. */
enum PaintLayerNormalConvertParam : int {
  PAINT_LAYER_NORMAL_CONVERT_MODE = 0,
  /** On: the result decoded, brought back to unit length and encoded again. */
  PAINT_LAYER_NORMAL_CONVERT_NORMALIZE = 1,
  PAINT_LAYER_NORMAL_CONVERT_PARAM_NUM = 2,
};

/**
 * The Normal Convert filter applied to \a rgb in place, over the encoded channels: the mode's
 * conversion, then the optional normalize, which decodes, brings the vector back to unit length
 * (a length under 1e-10 stands for it) and encodes again. Alpha is not part of \a rgb and so
 * stays untouched.
 */
void paint_layer_normal_convert_apply(float rgb[3],
                                      const float params[PAINT_LAYER_NORMAL_CONVERT_PARAM_NUM]);

/** The Levels of a filter value: the same function for every consumer that applies it on the CPU. */
inline float paint_layer_levels_apply(const float value, const float params[PAINT_LAYER_LEVELS_PARAM_NUM])
{
  /* A zero span or gamma reads as zero, which is what the Math Divide node gives the graph. */
  const float span = params[PAINT_LAYER_LEVELS_IN_WHITE] - params[PAINT_LAYER_LEVELS_IN_BLACK];
  const float k = (span != 0.0f) ? 1.0f / span : 0.0f;
  const float m = -(params[PAINT_LAYER_LEVELS_IN_BLACK] * k);
  const float inv_gamma = (params[PAINT_LAYER_LEVELS_GAMMA] != 0.0f) ?
                              1.0f / params[PAINT_LAYER_LEVELS_GAMMA] :
                              0.0f;
  float t = value * k + m;
  t = (t < 0.0f) ? 0.0f : ((t > 1.0f) ? 1.0f : t);
  /* The Gamma node leaves a non-positive value as it is. */
  if (t > 0.0f) {
    t = powf(t, inv_gamma);
  }
  return t * (params[PAINT_LAYER_LEVELS_OUT_WHITE] - params[PAINT_LAYER_LEVELS_OUT_BLACK]) +
         params[PAINT_LAYER_LEVELS_OUT_BLACK];
}

/**
 * The Invert filter applied to \a rgb in place, each channel by its own strength: the channel moves
 * from its colour to `|c - 1|` -- the complement on [0, 1], the same distance past it, which is what
 * the Difference mix against white and the CPU composite both read. The default of every amount is
 * one, so a row without stored parameters inverts the whole way as the unparameterised filter always
 * did. Alpha is not part of \a rgb and so stays untouched.
 */
inline void paint_layer_invert_apply(float rgb[3],
                                     const float params[PAINT_LAYER_INVERT_PARAM_NUM])
{
  for (int k = 0; k < 3; k++) {
    rgb[k] += (std::fabs(rgb[k] - 1.0f) - rgb[k]) * params[k];
  }
}

/**
 * The Exposure filter applied to \a rgb in place: the colour scaled by `2^exposure`, shifted by the
 * offset, then raised to one over the gamma. The power is the Gamma node's own function -- a
 * non-positive value is left as it is -- so a default gamma leaves a colour above one above one:
 * the filter brightens an HDR map instead of clipping what it already holds. Alpha is not part of
 * \a rgb and so stays untouched.
 */
inline void paint_layer_exposure_apply(
    float rgb[3], const float params[PAINT_LAYER_EXPOSURE_PARAM_NUM])
{
  const float scale = powf(2.0f, params[PAINT_LAYER_EXPOSURE_EXPOSURE]);
  const float exponent = 1.0f / params[PAINT_LAYER_EXPOSURE_GAMMA];
  for (int i = 0; i < 3; i++) {
    const float scaled = rgb[i] * scale + params[PAINT_LAYER_EXPOSURE_OFFSET];
    rgb[i] = (scaled > 0.0f) ? powf(scaled, exponent) : scaled;
  }
}

/**
 * The Brightness/Contrast filter applied to \a rgb in place, at full strength: the
 * Brightness/Contrast shader node's function, `out = max(a * in + b, 0)` with `a = 1 + contrast`
 * and `b = brightness - contrast * 0.5`, so the CPU composite and the generated node agree. Alpha is
 * not part of \a rgb and so stays untouched.
 */
inline void paint_layer_brightness_contrast_apply(
    float rgb[3], const float params[PAINT_LAYER_BRIGHTNESS_CONTRAST_PARAM_NUM])
{
  const float contrast = params[PAINT_LAYER_BRIGHTNESS_CONTRAST_CONTRAST];
  const float a = 1.0f + contrast;
  const float b = params[PAINT_LAYER_BRIGHTNESS_CONTRAST_BRIGHTNESS] - contrast * 0.5f;
  for (int i = 0; i < 3; i++) {
    rgb[i] = std::max(a * rgb[i] + b, 0.0f);
  }
}

/**
 * The Color Ramp filter applied to \a rgb in place: the colour's luminance indexes \a band, and the
 * ramp's colour replaces the colour. Returns the multiplier of the filter's strength: 1, or the
 * alpha of the ramp colour found when \a use_alpha is set.
 */
float paint_layer_color_ramp_apply(const ColorBand &band, bool use_alpha, float rgb[3]);

/**
 * The RGB Curves filter applied to \a rgb in place, at full strength: the RGB Curves shader node's
 * own function, each channel through its curve with no white balancing (the node's GPU and
 * multi-function evaluations ignore the levels). The mapping's tables must be built
 * (#BKE_curvemapping_init) when this runs. Alpha is untouched.
 */
void paint_layer_curves_apply(const CurveMapping &curve_mapping, float rgb[3]);

/**
 * The shader's own lookup in the table #paint_layer_curves_lut_build (BKE_paint_layers_composite.hh)
 * made, as a CPU reference.
 */
void paint_layer_curves_lut_eval(Span<float4> texels, int width, float rgb[3]);

/**
 * The shader's own lookup in the table #paint_layer_color_ramp_lut_build made, as a CPU reference.
 * \a constant selects the nearest texel, which is what a CONSTANT band lays; otherwise the two
 * neighbours are linearly interpolated. Returns the filter's strength multiplier: the stop's alpha
 * when \a use_alpha is set, one otherwise.
 */
float paint_layer_color_ramp_lut_eval(Span<float4> texels,
                                      int width,
                                      bool constant,
                                      bool use_alpha,
                                      float rgb[3]);

/**
 * The Posterize filter applied to one channel: the value quantized to \a steps evenly spaced levels
 * over [0, 1], `floor(clamp(value, 0, 1) * (steps - 1) + 0.5) / (steps - 1)`, so both endpoints are
 * preserved and a value at a level's midpoint rounds to the nearer one. A step count below two
 * reads as two, the smallest count that shows a step.
 */
inline float paint_layer_posterize_apply(const float value, const float steps)
{
  const float k = std::max(steps - 1.0f, 1.0f);
  const float clamped = std::clamp(value, 0.0f, 1.0f);
  return std::floor(clamped * k + 0.5f) / k;
}

/**
 * One channel of the Math filter: \a value combined with the constant \a operand by \a operation
 * (a #PaintLayerMathOperation). POWER and LOGARITHM read the Math node's safe rules through
 * #safe_powf and #safe_logf, so a negative base under a fractional exponent and a non-positive
 * logarithm base or argument stay defined; a division by zero reads zero. With \a clamp the result
 * folds into [0, 1], the node's own Use Clamp, so the CPU composite and the generated node agree.
 */
inline float paint_layer_math_apply(const float value,
                                    const int operation,
                                    const float operand,
                                    const bool clamp)
{
  float result = value;
  switch (operation) {
    case PAINT_LAYER_MATH_OP_ADD:
      result = value + operand;
      break;
    case PAINT_LAYER_MATH_OP_SUBTRACT:
      result = value - operand;
      break;
    case PAINT_LAYER_MATH_OP_MULTIPLY:
      result = value * operand;
      break;
    case PAINT_LAYER_MATH_OP_DIVIDE:
      result = (operand != 0.0f) ? value / operand : 0.0f;
      break;
    case PAINT_LAYER_MATH_OP_MINIMUM:
      result = std::min(value, operand);
      break;
    case PAINT_LAYER_MATH_OP_MAXIMUM:
      result = std::max(value, operand);
      break;
    case PAINT_LAYER_MATH_OP_POWER:
      result = safe_powf(value, operand);
      break;
    case PAINT_LAYER_MATH_OP_LOGARITHM:
      result = safe_logf(value, operand);
      break;
    default:
      break;
  }
  return clamp ? std::clamp(result, 0.0f, 1.0f) : result;
}

/**
 * One channel of the Threshold filter: the two states of the Math node's Greater Than, `1.0` when
 * the value is strictly above \a threshold and `0.0` otherwise, so the CPU composite and the
 * generated node agree on the comparison's edge.
 */
inline float paint_layer_threshold_apply(const float value, const float threshold)
{
  return (value > threshold) ? 1.0f : 0.0f;
}

/**
 * One channel of the Clamp filter: `min(max(value, min), max)`, the Math node's own Clamp and the
 * order that keeps a well-defined result when \a min is above \a max.
 */
inline float paint_layer_clamp_apply(const float value, const float min, const float max)
{
  return std::min(std::max(value, min), max);
}

/**
 * The value of the parameter \a index of \a row's kind: the stored one, or the schema's default.
 * A row whose kind has no such parameter reads 0. The storage is the row's system properties, which
 * only this and #paint_layer_kind_param_set touch.
 */
float paint_layer_kind_param_get(const MaterialPaintLayer &row, int index);

/**
 * Store every parameter of \a row's kind at its default, with the schema's range and label as UI
 * data, so the UI can list them before any has been edited. Parameters already stored are kept.
 */
void paint_layer_kind_params_init(MaterialPaintLayer &row);

/** Store the parameter \a index, clamped to the schema's range. False for an unknown index. */
bool paint_layer_kind_param_set(MaterialPaintLayer &row, int index, float value);

/** The type of parameter \a index, or Float for an unknown one. */
PaintLayerKindParamType paint_layer_kind_param_type(const MaterialPaintLayer &row, int index);

/** The integer value of parameter \a index, clamped to the schema's range. */
int paint_layer_kind_param_int_get(const MaterialPaintLayer &row, int index);
bool paint_layer_kind_param_int_set(MaterialPaintLayer &row, int index, int value);

/** The boolean value of parameter \a index, or the schema default for an unknown one. */
bool paint_layer_kind_param_bool_get(const MaterialPaintLayer &row, int index);
bool paint_layer_kind_param_bool_set(MaterialPaintLayer &row, int index, bool value);

/** The RGBA value of a Color parameter \a index, or the schema default for an unknown one. */
void paint_layer_kind_param_color_get(const MaterialPaintLayer &row,
                                      int index,
                                      float r_color[4]);
bool paint_layer_kind_param_color_set(MaterialPaintLayer &row,
                                      int index,
                                      const float color[4]);

/** Whether \a row's kind has a Ramp parameter (the Color Ramp filter). */
bool paint_layer_kind_has_ramp_param(const MaterialPaintLayer &row);

/**
 * The row's ramp payload, allocated with the default black-to-white band when it has none. Only a
 * kind with a Ramp parameter gets one; returns null for every other kind.
 */
struct MaterialPaintLayerRamp *BKE_paint_layers_ramp_ensure(MaterialPaintLayer &row);

/** The row's ramp band, or null when it has no ramp payload. */
const struct ColorBand *BKE_paint_layers_ramp_get(const MaterialPaintLayer &row);

/** A hash of every ramp stop, for the bake and composite caches. */
uint64_t paint_layer_ramp_hash(const struct ColorBand &band);

/** Whether \a row's kind has a Curves parameter (the RGB Curves filter). */
bool paint_layer_kind_has_curves_param(const MaterialPaintLayer &row);

/**
 * The row's curves payload, allocated with a flat 0..1 four-curve mapping when it has none. Only a
 * kind with a Curves parameter gets one; returns null for every other kind.
 */
struct MaterialPaintLayerCurves *BKE_paint_layers_curves_ensure(MaterialPaintLayer &row);

/** The row's curve mapping, or null when it has no curves payload. */
const struct CurveMapping *BKE_paint_layers_curves_get(const MaterialPaintLayer &row);

/**
 * Sync \a ma's generated curves nodes with the row's curve mapping, in place, and mark the bake
 * accordingly. \a bmain is the main the tree updates run against. Called from the RNA update of
 * the mapping pointer, which the curve widget fires after every edit of it; during a point drag
 * (inside #BKE_paint_layers_curves_live_begin) the bake flags are left alone.
 * \return true when \a data is a row's curve mapping, whether or not the sync succeeded.
 */
bool BKE_paint_layers_curves_edited(Main &bmain, Material &ma, const void *data);

/** A hash of the curves' shape, levels and tone, for the bake and composite caches. */
uint64_t paint_layer_curves_hash(const struct CurveMapping &curve_mapping);

/* -------------------------------------------------------------------- */
/** \name Kind payload registry
 *
 * One list owns every pointer payload a kind keeps outside #MaterialPaintLayer::system_properties
 * (a color balance, a channel mixer). The consumers walk it instead of naming each payload, so a
 * third payload is a #PaintLayerPayloadInfo entry and a schema entry of its kind.
 * \{ */

/** Whether \a row's kind has a parameter of \a type (a payload kind has exactly one). */
bool paint_layer_kind_has_payload_param(const MaterialPaintLayer &row,
                                        PaintLayerKindParamType type);

/** The registry entry of \a type, or null when no payload of that type is registered. */
const PaintLayerPayloadInfo *paint_layer_payload_info(PaintLayerKindParamType type);

/** The row's payload of \a type, created with its neutral defaults when it has none. */
void *paint_layer_payload_ensure(MaterialPaintLayer &row, PaintLayerKindParamType type);

/** Free every registered payload \a row holds and clear its pointer. */
void paint_layer_payload_free(MaterialPaintLayer &row);

/** Copy every registered payload of \a src into \a dst, allocating a duplicate of each. */
void paint_layer_payload_copy(const MaterialPaintLayer &src, MaterialPaintLayer &dst);

/** A hash of every payload \a row holds, for the bake and composite caches. */
uint64_t paint_layer_payload_hash(const MaterialPaintLayer &row);

/** Write every payload \a row holds, in registry order, each as its own DNA struct. */
void paint_layer_payload_write(BlendWriter &writer, const MaterialPaintLayer &layer);

/** Read every payload of \a layer, giving a kind that supports one its defaults when absent. */
void paint_layer_payload_read(BlendDataReader &reader, MaterialPaintLayer &layer);

/**
 * Tag \a ma when \a data is the payload of \a type of any row, its mask items, effects or children,
 * through the registry entry's edit rule. Returns whether the payload was found.
 */
bool paint_layer_payload_edited(Material &ma, const void *data, PaintLayerKindParamType type);

/** \} */

/** Whether \a row's kind has a Color Balance parameter (the Color Balance filter). */
bool paint_layer_kind_has_color_balance_param(const MaterialPaintLayer &row);

/**
 * The row's color balance payload, or the neutral one when the row has none.
 * The neutral payload is owned by this function, so callers never manufacture their own.
 */
const MaterialPaintLayerColorBalance &paint_layer_color_balance_or_neutral(
    const MaterialPaintLayer &row);

/**
 * Fold \a balance into the 12 numbers both the CPU composite and the generated graph read: the
 * per-channel triples of `out_i = pow(max(c_i * A_i + B_i, 0), E_i)` laid at
 * #PaintLayerColorBalanceParam's positions. The one place that knows the formulas — its callers
 * are the CPU description, the graph builder and the GPU gather, none of which re-derives them.
 */
void paint_layer_color_balance_fold(const MaterialPaintLayerColorBalance &balance,
                                    float r_params[PAINT_LAYER_COLOR_BALANCE_FOLDED_NUM]);

/**
 * The Color Balance filter applied to \a rgb in place, at full strength, through the same fold the
 * CPU composite and the graph read. The C++ reference the parity tests compare against.
 */
void paint_layer_color_balance_apply(const MaterialPaintLayerColorBalance &balance, float rgb[3]);

/** A hash of the balance's method and every stored value, for the bake cache. */
uint64_t paint_layer_color_balance_hash(const MaterialPaintLayerColorBalance &balance);

/** Whether \a row's kind has a Channel Mixer parameter (the Channel Mixer filter). */
bool paint_layer_kind_has_channel_mixer_param(const MaterialPaintLayer &row);

/** The row's matrix, or null when it has no channel-mixer payload. */
const struct MaterialPaintLayerChannelMixer *BKE_paint_layers_channel_mixer_get(
    const MaterialPaintLayer &row);

/**
 * Fold \a mixer into the 12 numbers the CPU composite, the generated graph and the GPU read, laid
 * at #PaintLayerChannelMixerFolded's positions: `out_k = dot(row_k, c) + constant_k`. With
 * \a monochrome all three rows are the red row and all three constants their mean, so the three
 * outputs are one grey -- the addon's monochrome, `gray = red_row . c + mean(constants)`. The one
 * place that knows the formulas; its callers never re-derive them.
 */
void paint_layer_channel_mixer_fold(const MaterialPaintLayerChannelMixer &mixer,
                                    bool monochrome,
                                    float r_params[PAINT_LAYER_CHANNEL_MIXER_FOLDED_NUM]);

/**
 * The three numbers of the \a index-th vector of #PAINT_LAYER_CHANNEL_MIXER_VECTORS out of the
 * folded layout: a row's three neighbours, or the three constants, which sit in the `.w` of the
 * three rows four floats apart. The one place the layout's stride is written for its readers.
 */
void paint_layer_channel_mixer_vector_get(
    const float folded[PAINT_LAYER_CHANNEL_MIXER_FOLDED_NUM], int index, float r_vec[3]);

/**
 * The folded matrix applied to \a rgb in place and clamped to [0, 1], as the addon's result is
 * (`np.clip(result, 0, 1)`, in both of its branches). The C++ reference the parity tests compare
 * against.
 */
void paint_layer_channel_mixer_apply(const float params[PAINT_LAYER_CHANNEL_MIXER_FOLDED_NUM],
                                     float rgb[3]);

/** A hash of every stored value of the matrix, for the bake cache. */
uint64_t paint_layer_channel_mixer_hash(const MaterialPaintLayerChannelMixer &mixer);

/**
 * The position of the parameter \a name in \a row's kind's schema, or -1 when the kind has no
 * parameter of that name. The exposed-parameters list stores the position, so a rebuild of the
 * list names the same parameter the row's own schema has.
 */
int paint_layer_kind_param_index(const MaterialPaintLayer &row, const char *name);

/** The kind of \a row, or null when the file names one this build does not know. */
const PaintLayerKindInfo *paint_layer_kind_find(int8_t source, int16_t subtype);

inline const PaintLayerKindInfo *paint_layer_kind_of(const MaterialPaintLayer &row)
{
  return paint_layer_kind_find(row.source, row.kind_subtype);
}

/** Whether \a row's kind may take the role the row has; false for an unknown kind. */
bool paint_layer_kind_role_allowed(const MaterialPaintLayer &row);

/** Whether \a row's kind has \a flag; an unknown kind has none. */
inline bool paint_layer_kind_has_flag(const MaterialPaintLayer &row, const PaintLayerKindFlag flag)
{
  const PaintLayerKindInfo *info = paint_layer_kind_of(row);
  return info != nullptr && (info->flags & flag) != 0;
}

/** Whether \a row is a map-driven mask coverage operator (Color ID or UV Island). */
inline bool paint_layer_kind_is_map_coverage(const MaterialPaintLayer &row)
{
  return paint_layer_kind_has_flag(row, PAINT_LAYER_KIND_MAP_COVERAGE);
}

/** Whether \a row is the inert Anchor Point marker. */
inline bool paint_layer_kind_is_anchor_point_marker(const MaterialPaintLayer &row)
{
  return paint_layer_kind_has_flag(row, PAINT_LAYER_KIND_ANCHOR_POINT_MARK);
}

/** Whether \a row reads a slice an Anchor Point publishes. */
inline bool paint_layer_kind_reads_anchor_point(const MaterialPaintLayer &row)
{
  return paint_layer_kind_has_flag(row, PAINT_LAYER_KIND_READS_ANCHOR_POINT);
}

/** Whether \a row's kind allows its map to be a stroke target; false for an unknown kind. */
inline bool paint_layer_kind_map_paintable(const MaterialPaintLayer &row)
{
  const PaintLayerKindInfo *info = paint_layer_kind_of(row);
  return info != nullptr && info->map_paintable;
}

/**
 * Report the IDs \a row itself holds, not those of its mask items, effects or children: the
 * consumers already recurse over those lists in their own order.
 *
 * A map-coverage operator reads one structural map, in its Base Color record: reporting that record
 * alone (rather than walking every record) keeps the ID walk, the map collection and the map copy
 * in step with what the kind actually samples.
 */
inline void paint_layer_dependencies(MaterialPaintLayer &row,
                                     const FunctionRef<void(PaintLayerDependency &)> fn)
{
  /* The source first: it is what a Material row is, and the bake below is built from it. */
  if (row.material != nullptr || row.source == MA_PAINT_LAYER_SOURCE_MATERIAL) {
    PaintLayerDependency dep{reinterpret_cast<ID **>(&row.material),
                             PaintLayerDependencyKind::SourceMaterial,
                             PAINT_LAYER_DEP_TOPOLOGY | PAINT_LAYER_DEP_PIXELS |
                                 PAINT_LAYER_DEP_OWNERSHIP,
                             -1};
    fn(dep);
  }
  /* A Curve Patch row owns its curve container and brush snapshot outright: both are reported
   * with owner semantics, like the material of a Material row. */
  if (row.curve_patch != nullptr) {
    if (row.curve_patch->vectors != nullptr) {
      PaintLayerDependency dep{reinterpret_cast<ID **>(&row.curve_patch->vectors),
                               PaintLayerDependencyKind::CurvePatchVectors,
                               PAINT_LAYER_DEP_TOPOLOGY | PAINT_LAYER_DEP_PIXELS |
                                   PAINT_LAYER_DEP_OWNERSHIP,
                               -1};
      fn(dep);
    }
    if (row.curve_patch->brush != nullptr) {
      PaintLayerDependency dep{reinterpret_cast<ID **>(&row.curve_patch->brush),
                               PaintLayerDependencyKind::BrushPreset,
                               PAINT_LAYER_DEP_TOPOLOGY | PAINT_LAYER_DEP_PIXELS |
                                   PAINT_LAYER_DEP_OWNERSHIP,
                               -1};
      fn(dep);
    }
  }
  /* The bake cache: the colour maps by channel, then the shared coverage map. */
  if (row.bake != nullptr) {
    for (int i = 0; i < PAINT_MATERIAL_CHANNEL_NUM; i++) {
      PaintLayerDependency dep{reinterpret_cast<ID **>(&row.bake->images[i]),
                               PaintLayerDependencyKind::BakeCache,
                               PAINT_LAYER_DEP_PIXELS | PAINT_LAYER_DEP_OWNERSHIP,
                               int8_t(i)};
      fn(dep);
    }
    PaintLayerDependency coverage_dep{reinterpret_cast<ID **>(&row.bake->coverage),
                                      PaintLayerDependencyKind::BakeCache,
                                      PAINT_LAYER_DEP_PIXELS | PAINT_LAYER_DEP_OWNERSHIP,
                                      -1};
    fn(coverage_dep);
  }
  if (paint_layer_kind_is_map_coverage(row)) {
    for (int i = 0; i < row.channels_num; i++) {
      MaterialPaintLayerChannel &record = row.channels[i];
      if (record.channel != PAINT_MATERIAL_CHANNEL_BASE_COLOR) {
        continue;
      }
      /* The slot is the stored `Image *`, which shares its address with the `ID *` it starts with. */
      PaintLayerDependency dep{reinterpret_cast<ID **>(&record.image),
                               PaintLayerDependencyKind::ImageMap,
                               PAINT_LAYER_DEP_PIXELS | PAINT_LAYER_DEP_OWNERSHIP,
                               int8_t(record.channel)};
      fn(dep);
      return;
    }
    return;
  }
  for (int i = 0; i < row.channels_num; i++) {
    MaterialPaintLayerChannel &record = row.channels[i];
    /* The slot is the stored `Image *`, which shares its address with the `ID *` it starts with. */
    PaintLayerDependency dep{reinterpret_cast<ID **>(&record.image),
                             PaintLayerDependencyKind::ImageMap,
                             PAINT_LAYER_DEP_PIXELS | PAINT_LAYER_DEP_OWNERSHIP,
                             int8_t(record.channel)};
    fn(dep);
  }
}

/** Read-only counterpart: the callee must not write through the slot. */
inline void paint_layer_dependencies(const MaterialPaintLayer &row,
                                     const FunctionRef<void(const PaintLayerDependency &)> fn)
{
  paint_layer_dependencies(const_cast<MaterialPaintLayer &>(row),
                           [&](PaintLayerDependency &dep) { fn(dep); });
}

/** \} */

/** The ID walk flags a dependency is reported with. */
inline LibraryForeachIDCallbackFlag paint_layer_dependency_foreach_flag(const PaintLayerDependency &dep)
{
  switch (dep.kind) {
    case PaintLayerDependencyKind::ImageMap:
    case PaintLayerDependencyKind::BakeCache:
    case PaintLayerDependencyKind::SourceMaterial:
    case PaintLayerDependencyKind::CurvePatchVectors:
    case PaintLayerDependencyKind::BrushPreset:
      return IDWALK_CB_USER;
  }
  return IDWALK_CB_NOP;
}

}  // namespace blender
