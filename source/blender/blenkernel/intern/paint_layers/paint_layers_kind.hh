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

#include "BLI_function_ref.hh"

#include "DNA_ID.h"
#include "DNA_material_types.h"

#include "BKE_lib_query.hh"

namespace blender {

enum class PaintLayerDependencyKind : uint8_t {
  /* A channel's paint map. */
  ImageMap = 0,
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

/**
 * Report the IDs \a row itself holds, not those of its mask items, effects or children: the
 * consumers already recurse over those lists in their own order.
 */
inline void paint_layer_dependencies(MaterialPaintLayer &row,
                                     const FunctionRef<void(PaintLayerDependency &)> fn)
{
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

struct PaintLayerKindInfo {
  /** Registry key: #MaterialPaintLayer::source and #MaterialPaintLayer::kind_subtype. */
  int8_t source;
  int16_t subtype;
  const char *name;
  PaintLayerKindCategory category;
  /** The roles the kind may take, from #paint_layer_role_bit. */
  uint8_t allowed_roles;
};

/** The kind of \a row, or null when the file names one this build does not know. */
const PaintLayerKindInfo *paint_layer_kind_find(int8_t source, int16_t subtype);

inline const PaintLayerKindInfo *paint_layer_kind_of(const MaterialPaintLayer &row)
{
  return paint_layer_kind_find(row.source, row.kind_subtype);
}

/** Whether \a row's kind may take the role the row has; false for an unknown kind. */
bool paint_layer_kind_role_allowed(const MaterialPaintLayer &row);

/** \} */

/** The ID walk flags a dependency is reported with. */
inline LibraryForeachIDCallbackFlag paint_layer_dependency_foreach_flag(const PaintLayerDependency &dep)
{
  switch (dep.kind) {
    case PaintLayerDependencyKind::ImageMap:
      return IDWALK_CB_USER;
  }
  return IDWALK_CB_NOP;
}

}  // namespace blender
