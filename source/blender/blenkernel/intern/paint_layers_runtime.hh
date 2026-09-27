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

#include "BLI_assert.h"
#include "BLI_vector.hh"

#include "MEM_guardedalloc.h"

#include "DNA_ID.h"
#include "DNA_material_types.h"
#include "DNA_uuid_types.h"

namespace blender::bke {

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
