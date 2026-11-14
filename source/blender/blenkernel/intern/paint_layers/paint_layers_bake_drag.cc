/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The bake-drag state: a Transform or Mirror handle drag makes the bake of a row with a Decal
 * effect cheaper at every step, and the full bake follows once. The state is process-global but
 * runtime only: it is keyed by session uid, dropped with the material, and cleared with the file.
 */

#include <cstdint>
#include <mutex>

#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_paint_layers.hh"

#include "BLI_map.hh"
#include "BLI_time.h"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "DNA_ID.h"
#include "DNA_material_types.h"
#include "DNA_uuid_types.h"

#include "DEG_depsgraph.hh"

#include "paint_layers_bake_intern.hh"
#include "paint_layers_intern.hh"

namespace blender {

namespace {

/* A row baked below its size while a handle drag lasts: the size it owes once the drag ends. */
struct BakeDragReduced {
  uint32_t material_uid;
  bUUID marker;
  int full_size;
};

/* A drag whose end never arrived (cancelled gizmo, closed file) stops counting after this long. */
constexpr double BAKE_DRAG_TIMEOUT_SECONDS = 60.0;

/**
 * The one drag state: the live marks by material uid, and the rows they baked below their size.
 * Guarded by #bake_runtime_mutex, since #BKE_paint_layers_bake_is_valid is also asked off the main
 * thread. Keeping both under one accessor means a cancelled drag cannot leave one half behind.
 */
struct BakeDragState {
  Map<uint32_t, double> marks;
  Vector<BakeDragReduced> reduced;
};

BakeDragState &bake_drag_state()
{
  static BakeDragState state;
  return state;
}

bool bake_drag_active_locked(const uint32_t material_uid)
{
  const double *stamp = bake_drag_state().marks.lookup_ptr(material_uid);
  return stamp != nullptr && BLI_time_now_seconds() - *stamp <= BAKE_DRAG_TIMEOUT_SECONDS;
}

}  // namespace

bool bake_drag_active(const Material &ma)
{
  std::lock_guard lock(bake_runtime_mutex());
  return bake_drag_active_locked(ma.id.session_uid);
}

int bake_drag_full_size(const Material &ma, const MaterialPaintLayer &layer)
{
  std::lock_guard lock(bake_runtime_mutex());
  for (const BakeDragReduced &entry : bake_drag_state().reduced) {
    if (entry.material_uid == ma.id.session_uid && BLI_uuid_equal(entry.marker, layer.marker)) {
      return entry.full_size;
    }
  }
  return 0;
}

bool bake_drag_reduced_owes(const Material &ma, const MaterialPaintLayer &layer)
{
  std::lock_guard lock(bake_runtime_mutex());
  const BakeDragState &state = bake_drag_state();
  if (state.reduced.is_empty() || bake_drag_active_locked(ma.id.session_uid)) {
    return false;
  }
  for (const BakeDragReduced &entry : state.reduced) {
    if (entry.material_uid == ma.id.session_uid && BLI_uuid_equal(entry.marker, layer.marker)) {
      return true;
    }
  }
  return false;
}

void bake_drag_reduced_remember(const Material &ma, const MaterialPaintLayer &layer, const int size)
{
  std::lock_guard lock(bake_runtime_mutex());
  for (BakeDragReduced &entry : bake_drag_state().reduced) {
    if (entry.material_uid == ma.id.session_uid && BLI_uuid_equal(entry.marker, layer.marker)) {
      entry.full_size = size;
      return;
    }
  }
  bake_drag_state().reduced.append({ma.id.session_uid, layer.marker, size});
}

bool bake_drag_reduced_forget(const Material &ma, const MaterialPaintLayer &layer)
{
  std::lock_guard lock(bake_runtime_mutex());
  Vector<BakeDragReduced> &rows = bake_drag_state().reduced;
  for (const int64_t i : rows.index_range()) {
    if (rows[i].material_uid == ma.id.session_uid && BLI_uuid_equal(rows[i].marker, layer.marker)) {
      rows.remove_and_reorder(i);
      return true;
    }
  }
  return false;
}

void bake_drag_forget_material(const uint32_t material_uid)
{
  std::lock_guard lock(bake_runtime_mutex());
  BakeDragState &state = bake_drag_state();
  state.marks.remove(material_uid);
  for (int64_t i = int64_t(state.reduced.size()) - 1; i >= 0; i--) {
    if (state.reduced[i].material_uid == material_uid) {
      state.reduced.remove_and_reorder(i);
    }
  }
}

void BKE_paint_layers_bake_drag_set(Main &bmain, const uint32_t material_uid, const bool active)
{
  bool owes = false;
  {
    std::lock_guard lock(bake_runtime_mutex());
    BakeDragState &state = bake_drag_state();
    if (active) {
      state.marks.add_overwrite(material_uid, BLI_time_now_seconds());
      return;
    }
    if (!state.marks.remove(material_uid)) {
      return;
    }
    for (const BakeDragReduced &entry : state.reduced) {
      owes |= entry.material_uid == material_uid;
    }
  }
  if (!owes) {
    return;
  }
  for (Material &ma : bmain.materials) {
    if (ma.id.session_uid == material_uid) {
      /* The reduced rows read as invalid from here (#bake_drag_reduced_owes); the planner only runs
       * on a stale material, and a depsgraph pass is what reaches it. */
      ma.paint_layers_flag |= MA_PAINT_LAYERS_BAKE_STALE;
      DEG_id_tag_update(&ma.id, ID_RECALC_SHADING);
      break;
    }
  }
}

bool BKE_paint_layers_bake_drag_is_active(const uint32_t material_uid)
{
  std::lock_guard lock(bake_runtime_mutex());
  return bake_drag_active_locked(material_uid);
}

void BKE_paint_layers_bake_drag_clear_all()
{
  std::lock_guard lock(bake_runtime_mutex());
  BakeDragState &state = bake_drag_state();
  /* Both release their buffers: the state is a function-local static, whose buffers a static
   * destructor would free after the allocator's exit check has run. */
  state.marks.clear();
  state.reduced.clear_and_shrink();
}

}  // namespace blender
