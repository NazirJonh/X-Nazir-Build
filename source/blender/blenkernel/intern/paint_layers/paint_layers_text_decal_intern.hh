/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Runtime state of the Text Decal rasters (`paint_layers_text_decal.cc`). It is parallel to the
 * bake planner rather than part of it: the planner bakes rows, while a Text Decal is a generated
 * source whose raster the depsgraph update refreshes. Keeping the state in its own map makes that
 * distinction explicit and leaves room for the next generated source.
 */

#pragma once

#include <cstdint>
#include <mutex>

#include "BLI_map.hh"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

namespace blender {

struct Material;
struct Object;

/**
 * The raster state of one Text Decal row. The hash is runtime only, deliberately: a file that comes
 * back with no entry re-bakes the text once from the object, instead of trusting a stale hash
 * against the map that traveled with it.
 */
struct TextDecalEntry {
  bUUID row = {};
  /** Everything the raster depends on: the object's content, its formatting, the bake options. */
  uint64_t hash = 0;
  /** Whether the last state was rendered at all (an empty text or a missing font was not). */
  bool has_raster = false;
  /**
   * The last hash seen while the Text object was in Edit Mode, and when it last changed
   * (#BLI_time_now_seconds). The raster waits until the edit buffer has stayed put for
   * #TEXT_DECAL_EDIT_DELAY_SECONDS, so typing does not re-rasterize per keystroke.
   */
  uint64_t observed_hash = 0;
  double observed_time = 0.0;
};

/**
 * Seconds a Text object's edit buffer must stay unchanged before its Text Decal is re-rasterized.
 * Short, unlike #MAP_ONLY_EDIT_DELAY_SECONDS: the text is the thing being looked at while typing,
 * so the pause that ends it should show the result almost at once.
 */
constexpr double TEXT_DECAL_EDIT_DELAY_SECONDS = 0.4;

/** The Text Decal state of one material, keyed by #ID::session_uid. */
struct TextDecalRuntime {
  /**
   * The material that owns this entry. Used so the free path of a same-`session_uid` material (the
   * old ID during a memfile undo) does not delete the entry the re-read ID still uses.
   */
  const Material *owner = nullptr;
  /** The raster states of this material's Text Decal rows, by row marker. */
  Vector<TextDecalEntry> entries;
  /**
   * Whether the material has any Text Decal effect: -1 unknown (scan the stack once), 0 none,
   * 1 present. A scene update reaches this before walking any layer tree, so a material without a
   * Text Decal costs one cached read.
   */
  int8_t present = -1;
  /** The original Text objects whose live edit holds a raster back; the wake timer tags them. */
  Vector<Object *> editing_sources;
};

/** The Text Decal runtime of every material, by #ID::session_uid. Hold the mutex around it. */
Map<uint32_t, TextDecalRuntime> &text_decal_runtime();
std::mutex &text_decal_runtime_mutex();

/** Drop the entry of \a ma when it owns it (the material is being freed). */
void text_decal_runtime_free(Material &ma);
/** Move the owned entry to \a dst (the material was replaced by a new ID instance). */
void text_decal_runtime_owner_transfer(Material &dst, Material &src);

}  // namespace blender
