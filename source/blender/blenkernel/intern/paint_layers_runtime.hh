/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * Runtime-only per-material state of the Paint Layers generator, owned by
 * #Material::paint_layers_runtime. It is never saved: a file load starts clean and the next
 * regeneration rebuilds exactly what the live material needs.
 */

#include <memory>

#include "BLI_assert.h"
#include "BLI_map.hh"
#include "BLI_vector.hh"

#include "MEM_guardedalloc.h"

#include "DNA_ID.h"
#include "DNA_material_types.h"
#include "DNA_uuid_types.h"

namespace blender::bke {

/**
 * A row's virtual (warm) items and the bookkeeping that decides whether they are in the graph.
 *
 * The items are never linked into the DNA lists: they exist only so the generator builds a neutral
 * chain that the real item of the same kind later takes the place of, without new shader code.
 */
struct WarmRow {
  bUUID marker = {};
  bool mask_present = false;
  bool effect_present = false;
  /** Compatible real items seen at the last reconcile; growth means the slot was taken. */
  int mask_real_seen = 0;
  int effect_real_seen = 0;
  /** Derived at each reconcile: present while the row has no real base mask. */
  bool base_present = false;
  /**
   * Per slot (0 base mask, 1 mask, 2 effect): the real item that took it, and the marker the spare
   * had at that moment. The real item builds into the spare's interface sockets (found by that slot
   * marker), so taking a slot leaves the group's interface and the root's mirrors as they were.
   * The spare that replaces a taken one gets a new marker (see the generation counters), or it
   * would share the sockets the real item now owns.
   */
  bUUID taker[3] = {};
  bUUID taker_slot[3] = {};
  uint8_t mask_gen = 0;
  uint8_t effect_gen = 0;
  MaterialPaintLayer base_item;
  MaterialPaintLayer mask_item;
  MaterialPaintLayer effect_item;

  /* The items' `channels` arrays are owned raw pointers: a copy would free them twice, and none is
   * ever wanted (the rows live in unique pointers below). Declaring them keeps the default
   * constructor from being suppressed. */
  WarmRow() = default;
  WarmRow(const WarmRow &) = delete;
  WarmRow &operator=(const WarmRow &) = delete;

  ~WarmRow()
  {
    MEM_delete(base_item.channels);
    MEM_delete(mask_item.channels);
    MEM_delete(effect_item.channels);
  }
};

/**
 * Derived state of one owning material's generated Paint Layers graph.
 *
 * Only state that is written and read on the main thread for the material that owns the description
 * lives here. State that a depsgraph evaluation thread or an evaluated copy reads stays in a
 * process-global map instead, so this runtime is never asked on a copy.
 */
struct MaterialPaintLayersRuntime {
  /**
   * Markers of the disabled rows the current generated graph was built without. A stale marker is
   * ignored while the row is re-enabled, and the moved topology hash forces the one rebuild that
   * brings it back, so an undo cannot leave a row permanently missing.
   */
  Vector<bUUID> removed_rows;
  /**
   * Markers of Material rows whose live source wrapper could not be built in the last
   * regeneration. The Main-free status and the Outliner read it; a successful rebuild, a different
   * source or the owner's free clears the marker.
   */
  Vector<bUUID> source_group_build_failed;
  /**
   * The warm items the generated graph carries for the rows entitled to one. An entry is created
   * present when the row first appears, marked consumed when the row gains a compatible real item,
   * and set present again by the idle replenishment. Never saved.
   *
   * The items' channel records point at the shared `.PL Warm` image. #paint_layers_warm_reconcile
   * re-binds them on every regeneration, and the image carries a fake user, so the stored pointers
   * stay valid between two passes; a pure build without a reconcile on this material reads the
   * last reconciled binding.
   */
  Vector<std::unique_ptr<WarmRow>> warm_rows;
  /**
   * The bake size of a row whose bake structure the AUTO rules released: the structure is the only
   * copy of the size, but it is the user's choice rather than part of the cache, so the row's next
   * bake reads it from here instead of falling back to the content dimensions. Never saved.
   */
  Map<UUID, int> dropped_bake_size;
};

/** The runtime of \a ma, or null when it was never needed. */
inline const MaterialPaintLayersRuntime *paint_layers_runtime_get(const Material &ma)
{
  return ma.paint_layers_runtime;
}

/** The runtime of \a ma, or null when it was never needed; for in-place edits. */
inline MaterialPaintLayersRuntime *paint_layers_runtime_mutable(Material &ma)
{
  return ma.paint_layers_runtime;
}

/**
 * The runtime of \a ma, created on first use.
 *
 * The runtime belongs to the material that owns its stack: a localized or evaluated copy shares the
 * description with the original, so it must never be asked to create its own.
 */
inline MaterialPaintLayersRuntime &paint_layers_runtime_ensure(Material &ma)
{
  BLI_assert((ma.id.tag & (ID_TAG_LOCALIZED | ID_TAG_COPIED_ON_EVAL | ID_TAG_NO_MAIN)) == 0);
  if (ma.paint_layers_runtime == nullptr) {
    ma.paint_layers_runtime = MEM_new<MaterialPaintLayersRuntime>(__func__);
  }
  return *ma.paint_layers_runtime;
}

/** Drop \a ma's runtime. Called from the ID free path and to model a file load. */
inline void paint_layers_runtime_free(Material &ma)
{
  MEM_delete(ma.paint_layers_runtime);
  ma.paint_layers_runtime = nullptr;
}

/**
 * Move \a src's runtime to \a dst when \a dst has none, leaving \a src empty. Used by
 * #material_undo_preserve so a memfile undo does not drop state the undo snapshot still describes
 * (the global maps this runtime replaces survived undo through an unchanged `session_uid`).
 */
inline void paint_layers_runtime_transfer(Material &dst, Material &src)
{
  if (dst.paint_layers_runtime == nullptr && src.paint_layers_runtime != nullptr) {
    dst.paint_layers_runtime = src.paint_layers_runtime;
    src.paint_layers_runtime = nullptr;
  }
}

}  // namespace blender::bke
