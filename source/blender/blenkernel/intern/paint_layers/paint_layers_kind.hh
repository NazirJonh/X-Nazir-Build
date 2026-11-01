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
#include <cstdint>

#include "BLI_function_ref.hh"
#include "BLI_math_base_safe.h"

#include "DNA_ID.h"
#include "DNA_material_types.h"
#include "DNA_scene_types.h"

#include "BKE_lib_query.hh"

namespace blender {

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
enum PaintLayerKindFlag : uint8_t {
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
  uint8_t flags = 0;
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
  PAINT_LAYER_HSL_PARAM_NUM = 3,
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

/** The positions of a map-coverage operator's parameters in its schema. */
enum PaintLayerCoverageParam : int {
  /** How far a sample may differ from a stored colour and still match (max-channel). */
  PAINT_LAYER_COVERAGE_TOLERANCE = 0,
  /** The width of the falloff past the tolerance; zero reads as a hard edge. */
  PAINT_LAYER_COVERAGE_SOFTNESS = 1,
  PAINT_LAYER_COVERAGE_PARAM_NUM = 2,
};

/**
 * The HSL filter applied to \a rgb in place, at full strength: the Hue/Saturation/Value node's
 * function (hue shifted by `hue - 0.5` turns, saturation clamped to one, value scaled, result not
 * below zero), so the CPU composite and the generated node agree.
 */
void paint_layer_hsl_apply(float rgb[3], const float params[PAINT_LAYER_HSL_PARAM_NUM]);

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
