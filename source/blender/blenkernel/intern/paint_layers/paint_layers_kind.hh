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

#include <cmath>

#include "BLI_function_ref.hh"

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

/**
 * One float parameter of a kind. The schema lives with the kind; the value lives in the row's
 * #MaterialPaintLayer::system_properties under #name, and reads as #default_value while unset, so
 * a row never has to carry parameters it did not change.
 */
struct PaintLayerKindParam {
  const char *name;
  const char *label;
  float default_value;
  float min;
  float max;
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
      return IDWALK_CB_USER;
  }
  return IDWALK_CB_NOP;
}

}  // namespace blender
