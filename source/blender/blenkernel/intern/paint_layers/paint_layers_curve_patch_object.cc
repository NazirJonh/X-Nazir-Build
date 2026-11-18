/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Curve Patch rows with curves that name an external Curves object
 * (#PaintVectorItem::curve_object): the structural hash of those objects' curves and the refresh
 * that follows a change of them.
 *
 * The row's maps are stamped from the objects' curves (through a live session, or on the
 * next session entry), so a change of the curves invalidates the bake and -- while a session is
 * live -- reaches the tool through the editors hook. The change itself is detected here, by hash,
 * on every scene update: the depsgraph relation (see the relation builder's layered-material
 * block) only orders the curve object ahead of the material's update; what reacts is this scan,
 * the way the Text Decal refresh reacts to its Text object.
 */

#include <cstdint>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>

#include "BLI_array.hh"
#include "BLI_index_range.hh"
#include "BLI_map.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_set.hh"
#include "BLI_span.hh"
#include "BLI_string.h"
#include "BLI_threads.h"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "BKE_curves.hh"
#include "BKE_global.hh"
#include "BKE_lib_id.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_curve_patch_object.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "DNA_curves_types.h"
#include "DNA_material_types.h"
#include "DNA_object_types.h"
#include "DNA_paint_vector_types.h"

#include "paint_layers_curve_patch_object_intern.hh"
#include "paint_layers_generated_source.hh"
#include "paint_layers_hash_intern.hh"

namespace blender {

/** Whether a material still has an object-sourced Curve Patch row, cached between refreshes. */
enum class CurvePatchObjectPresent : int8_t {
  Unknown = -1,
  None = 0,
  Present = 1,
};

/** One write-back echo a row registered: the hash its session is about to put into the object. */
struct CurvePatchObjectExpectedHash {
  bUUID marker = {};
  uint64_t hash = 0;
};

/** The refresh state of one object-sourced Curve Patch row. */
struct CurvePatchObjectRow {
  bUUID marker = {};
  /** The last hash the refresh observed for the row. */
  uint64_t hash = 0;
};

/** The object-source refresh state of one material, keyed by #ID::session_uid. */
struct CurvePatchObjectRuntime {
  /**
   * The material that owns this entry. Used so the free path of a same-`session_uid` material
   * (the old ID during a memfile undo) does not delete the entry the re-read ID still uses.
   */
  const Material *owner = nullptr;
  /** The row states of this material's object-sourced Curve Patch rows, by row marker. */
  Vector<CurvePatchObjectRow> rows;
  /** The expected hashes rows' write-backs registered and no refresh consumed yet. */
  Vector<CurvePatchObjectExpectedHash> expected;
  /**
   * The row markers a refresh detected a change for but whose re-stamp could not run (or failed
   * on a temporary blocker): the marker asks every following refresh to run the editors hook
   * again, until a stamp lands or a permanent refusal clears it. Runtime-only state, the same
   * process memory the hashes above live in, so a blocked row survives its own refusal and the
   * next safe point (a later refresh, or the end of the session that occupied the stamp host)
   * picks the work up. A marker of a gone row is dropped with the rows
   * (#curve_patch_object_store_hashes).
   */
  Vector<bUUID> restamp_pending;
  /**
   * Whether the material has any object-sourced Curve Patch row: unknown until the stack is
   * scanned once, none, or present. A scene update reaches this before walking any layer tree, so
   * a material without one costs one cached read. Cleared by
   * #BKE_paint_layers_curve_patch_object_invalidate (a copy, an import or an undo can bring a
   * row in).
   */
  CurvePatchObjectPresent present = CurvePatchObjectPresent::Unknown;
};

/** Process globals of the refresh: the per-material state, the editors hook and the drag gate. */
struct CurvePatchObjectGlobals {
  GeneratedSourceRuntime<CurvePatchObjectRuntime> runtime;
  void (*changed_cb)(Main &bmain,
                     Material &ma,
                     MaterialPaintLayer &row,
                     const Depsgraph *depsgraph,
                     void *user_data) = nullptr;
  void *changed_cb_data = nullptr;
  /**
   * Depth of point drags in flight, not a bool: a drag begin that is not paired with its release
   * (a session torn down mid-drag) increments once and the teardown's release clamps at zero, so
   * the gate cannot stay up. The pair is owned by the editors (radius_begin/handle_event ->
   * drag_end, plus every session teardown), and only this counter decides whether the refresh
   * holds readers back.
   */
  int drag_in_flight = 0;
};

static CurvePatchObjectGlobals &curve_patch_object_globals()
{
  static CurvePatchObjectGlobals globals;
  return globals;
}

/* -------------------------------------------------------------------- */
/** \name Source hash
 *
 * The hash is the function the publish reads: the object's identity, the world transform the
 * mapping goes through, and the evaluated curves' shape -- the standard per-curve and per-point
 * data plus the nurbs fields a nurbs spline's shape depends on. The custom attributes are
 * deliberately left out: the patch reads none of them, so editing one must not re-stamp the row
 * (the `paintcurve_*` parameter attributes are the per-curve parameters the object mode does not
 * persist). The points are a handful, so the cost is noise next to a scene update.
 * \{ */

template<typename T>
static void curve_patch_hash_varray(uint64_t &hash, const VArray<T> &values, const int64_t size)
{
  if (size == 0) {
    return;
  }
  const auto hash_value = [&hash](const T &value) {
    if constexpr (std::is_same_v<T, float>) {
      hash = bake_hash_float(hash, value);
    }
    else {
      hash = bake_hash_mix(hash, uint64_t(value));
    }
  };
  /* Most attributes are a plain array; reading its span skips the materialize copy. A computed
   * attribute still needs the copy. */
  if (values.is_span()) {
    for (const T &value : values.get_internal_span()) {
      hash_value(value);
    }
    return;
  }
  Array<T> materialized(size);
  values.materialize(materialized.as_mutable_span());
  for (const T &value : materialized) {
    hash_value(value);
  }
}

uint64_t BKE_paint_layers_curve_patch_object_hash(const Object &curve_object)
{
  uint64_t hash = bake_hash_mix(uint64_t(0x811c9dc59e3779b9ull),
                                uint64_t(curve_object.id.session_uid));
  hash = bake_hash_mix(hash, uint64_t(curve_object.type));
  if (curve_object.type != OB_CURVES || curve_object.data == nullptr ||
      GS(static_cast<ID *>(curve_object.data)->name) != ID_CV)
  {
    /* Not a curves object: nothing more to read (the type byte above answers for it). */
    return hash;
  }
  const Curves *curves_id = id_cast<const Curves *>(curve_object.data);
  const bke::CurvesGeometry &geometry = curves_id->geometry.wrap();
  hash = bake_hash_mix(hash, uint64_t(geometry.points_num()));
  hash = bake_hash_mix(hash, uint64_t(geometry.curves_num()));
  /* The publish mapping goes through the object's world matrix, so a moved object re-maps its
   * splines: the TRANSFORM relation only orders the evaluation, this is what makes the refresh
   * react to the move. */
  const float4x4 object_to_world = curve_object.object_to_world();
  for (const int i : IndexRange(16)) {
    hash = bake_hash_float(hash, object_to_world.base_ptr()[i]);
  }
  if (geometry.curves_num() == 0) {
    return hash;
  }
  curve_patch_hash_varray(hash, geometry.curve_types(), geometry.curves_num());
  curve_patch_hash_varray(hash, geometry.cyclic(), geometry.curves_num());
  curve_patch_hash_varray(hash, geometry.resolution(), geometry.curves_num());
  /* The nurbs fields shape a nurbs spline's evaluation; nurbs objects are read-only sources, but
   * their publish still follows them, so a change must reach the row. */
  curve_patch_hash_varray(hash, geometry.nurbs_orders(), geometry.curves_num());
  curve_patch_hash_varray(hash, geometry.nurbs_knots_modes(), geometry.curves_num());

  const auto hash_positions = [&hash](const Span<float3> values) {
    for (const float3 &value : values) {
      hash = bake_hash_float(hash, value.x);
      hash = bake_hash_float(hash, value.y);
      hash = bake_hash_float(hash, value.z);
    }
  };
  hash_positions(geometry.positions());
  if (const std::optional<Span<float3>> left = geometry.handle_positions_left()) {
    hash_positions(*left);
  }
  if (const std::optional<Span<float3>> right = geometry.handle_positions_right()) {
    hash_positions(*right);
  }
  curve_patch_hash_varray(hash, geometry.handle_types_left(), geometry.points_num());
  curve_patch_hash_varray(hash, geometry.handle_types_right(), geometry.points_num());
  curve_patch_hash_varray(hash, geometry.radius(), geometry.points_num());
  if (const std::optional<Span<float>> weights = geometry.nurbs_weights()) {
    for (const float weight : *weights) {
      hash = bake_hash_float(hash, weight);
    }
  }
  return hash;
}

uint64_t BKE_paint_layers_curve_patch_row_hash(const MaterialPaintLayerCurvePatch &payload,
                                               const Depsgraph *depsgraph)
{
  uint64_t hash = uint64_t(0x9e3779b97f4a7c15ull);
  if (payload.vectors == nullptr) {
    return hash;
  }
  const PaintVector &pv = *payload.vectors;
  uint64_t items_sum = 0;
  for (const int item_i : IndexRange(pv.items_num)) {
    const PaintVectorItem &item = pv.items[item_i];
    if (item.type != PAINT_VECTOR_ITEM_CURVE_PATCH || item.curve_object == nullptr) {
      continue;
    }
    /* The evaluated copy carries modifiers, animation and shape keys: the effect reads its curves
     * through it, so a change behind a modifier re-stamps the row too. BKE cannot reach the
     * editors' resolver (#curve_patch_object_evaluated), so the same resolution happens here
     * inline. */
    const Object *object = item.curve_object;
    if (depsgraph != nullptr) {
      const Object *candidate = DEG_get_evaluated(depsgraph, item.curve_object);
      if (candidate != nullptr && candidate->type == OB_CURVES) {
        object = candidate;
      }
    }
    /* Summed, not chained: adding, removing or reordering the curves must not change the hash of
     * the curves that did not change, or every curve add in a live session would look like a
     * foreign change and re-publish (and reset) the session. The container index is deliberately
     * not part of it either. */
    uint64_t item_hash = bake_hash_mix(uint64_t(0x517cc1b727220a95ull),
                                       uint64_t(item.curve_object_spline));
    item_hash = bake_hash_mix(item_hash, BKE_paint_layers_curve_patch_object_hash(*object));
    items_sum += item_hash;
  }
  hash = bake_hash_mix(hash, items_sum);
  return hash;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Refresh runtime state
 *
 * The hash each row was last read with, per material and row marker, in process memory: the state
 * describes "what the row has already been brought up to date with", which no file carries. A row
 * observed for the first time only records its hash -- the setter that pointed the row at the
 * object invalidated the bake already. Rows are a handful per material, so the per-material state
 * is a linear list, like #PaintLayerMarkerMap. The state dies with its material
 * (#curve_patch_object_runtime_free, from the bake runtime free) and follows a same-uid
 * replacement (#curve_patch_object_runtime_owner_transfer, from the bake runtime owner transfer),
 * exactly like the Text Decal's state.
 * \{ */

void curve_patch_object_runtime_free(Material &ma)
{
  curve_patch_object_globals().runtime.runtime_free(ma.id.session_uid, ma);
}

void curve_patch_object_runtime_owner_transfer(Material &dst, Material &src)
{
  curve_patch_object_globals().runtime.owner_transfer(src.id.session_uid, dst, src);
}

/** The last hash observed for \a marker, whether it was observed at all, the write-back's pending
 * expected hash (consumed by this call), and whether the row still owes a re-stamp. All under one
 * lock: no reference into the map escapes it. */
static bool curve_patch_object_state_get(const uint32_t session_uid,
                                         const bUUID &marker,
                                         uint64_t &r_hash,
                                         bool &r_found,
                                         uint64_t &r_expected,
                                         bool &r_pending)
{
  std::lock_guard lock(curve_patch_object_globals().runtime.mutex());
  r_found = false;
  r_hash = 0;
  r_expected = 0;
  r_pending = false;
  bool has_expected = false;
  CurvePatchObjectRuntime *runtime = curve_patch_object_globals().runtime.map().lookup_ptr(
      session_uid);
  if (runtime == nullptr) {
    return false;
  }
  for (const CurvePatchObjectRow &entry : runtime->rows) {
    if (BLI_uuid_equal(entry.marker, marker)) {
      r_found = true;
      r_hash = entry.hash;
      break;
    }
  }
  for (const int i : runtime->expected.index_range()) {
    if (BLI_uuid_equal(runtime->expected[i].marker, marker)) {
      r_expected = runtime->expected[i].hash;
      has_expected = true;
      /* Consumed: a hash that does not come back cannot suppress a later real change. */
      runtime->expected.remove(i);
      break;
    }
  }
  for (const bUUID &pending_marker : runtime->restamp_pending) {
    if (BLI_uuid_equal(pending_marker, marker)) {
      r_pending = true;
      break;
    }
  }
  return has_expected;
}

/** Replace the row states of \a session_uid with \a rows; expected hashes and restamp-pending
 * markers of rows that are gone are dropped with them. */
static void curve_patch_object_store_hashes(const uint32_t session_uid,
                                            Vector<CurvePatchObjectRow> &&rows)
{
  std::lock_guard lock(curve_patch_object_globals().runtime.mutex());
  CurvePatchObjectRuntime *runtime = curve_patch_object_globals().runtime.map().lookup_ptr(
      session_uid);
  if (runtime == nullptr) {
    return;
  }
  runtime->rows = std::move(rows);
  runtime->expected.remove_if([&](const CurvePatchObjectExpectedHash &entry) {
    for (const CurvePatchObjectRow &row : runtime->rows) {
      if (BLI_uuid_equal(row.marker, entry.marker)) {
        return false;
      }
    }
    return true;
  });
  runtime->restamp_pending.remove_if([&](const bUUID &marker) {
    for (const CurvePatchObjectRow &row : runtime->rows) {
      if (BLI_uuid_equal(row.marker, marker)) {
        return false;
      }
    }
    return true;
  });
}

void BKE_paint_layers_curve_patch_object_restamp_pending_set(Material &ma,
                                                             const MaterialPaintLayer &row,
                                                             const bool pending)
{
  std::lock_guard lock(curve_patch_object_globals().runtime.mutex());
  CurvePatchObjectRuntime &runtime = curve_patch_object_globals().runtime.map().lookup_or_add(
      ma.id.session_uid, CurvePatchObjectRuntime{});
  if (runtime.owner == nullptr) {
    runtime.owner = &ma;
  }
  if (runtime.owner != &ma) {
    return;
  }
  int64_t current = -1;
  for (const int64_t i : runtime.restamp_pending.index_range()) {
    if (BLI_uuid_equal(runtime.restamp_pending[i], row.marker)) {
      current = i;
      break;
    }
  }
  if (pending && current == -1) {
    runtime.restamp_pending.append(row.marker);
  }
  else if (!pending && current != -1) {
    runtime.restamp_pending.remove(current);
  }
}

void BKE_paint_layers_curve_patch_object_expected_hash_set(Material &ma,
                                                           const MaterialPaintLayer &row,
                                                           const uint64_t hash)
{
  std::lock_guard lock(curve_patch_object_globals().runtime.mutex());
  CurvePatchObjectRuntime &runtime = curve_patch_object_globals().runtime.map().lookup_or_add(
      ma.id.session_uid, CurvePatchObjectRuntime{});
  if (runtime.owner == nullptr) {
    runtime.owner = &ma;
  }
  if (runtime.owner != &ma) {
    return;
  }
  for (CurvePatchObjectExpectedHash &entry : runtime.expected) {
    if (BLI_uuid_equal(entry.marker, row.marker)) {
      entry.hash = hash;
      return;
    }
  }
  runtime.expected.append({row.marker, hash});
}

void BKE_paint_layers_curve_patch_object_expected_hash_clear(Material &ma,
                                                             const MaterialPaintLayer &row)
{
  std::lock_guard lock(curve_patch_object_globals().runtime.mutex());
  CurvePatchObjectRuntime *runtime = curve_patch_object_globals().runtime.map().lookup_ptr(
      ma.id.session_uid);
  if (runtime == nullptr || runtime->owner != &ma) {
    return;
  }
  runtime->expected.remove_if([&](const CurvePatchObjectExpectedHash &entry) {
    return BLI_uuid_equal(entry.marker, row.marker);
  });
}

void BKE_paint_layers_curve_patch_object_invalidate(Material &ma)
{
  std::lock_guard lock(curve_patch_object_globals().runtime.mutex());
  CurvePatchObjectRuntime *runtime = curve_patch_object_globals().runtime.map().lookup_ptr(
      ma.id.session_uid);
  if (runtime != nullptr && runtime->owner == &ma) {
    /* Only the cached absence is re-asked; the per-row hashes stay (a re-read row observes
     * fresh, the way an add does). */
    runtime->present = CurvePatchObjectPresent::Unknown;
  }
}

void BKE_paint_layers_curve_patch_object_restamp_pending_session_end_notify(Main &bmain)
{
  for (Material &ma : bmain.materials) {
    int64_t pending_rows = 0;
    {
      std::lock_guard lock(curve_patch_object_globals().runtime.mutex());
      const CurvePatchObjectRuntime *runtime =
          curve_patch_object_globals().runtime.map().lookup_ptr(ma.id.session_uid);
      if (runtime != nullptr && runtime->owner == &ma) {
        pending_rows = runtime->restamp_pending.size();
      }
    }
    if (pending_rows == 0) {
      continue;
    }
    /* The session that held the stamp host is torn down by now (the callers run this after the
     * session's sculpt state is released), so the blocked re-stamps can go through on the next
     * update: the tag only schedules the refresh, exactly like the drag release does. */
    DEG_id_tag_update(&ma.id, ID_RECALC_SYNC_TO_EVAL | ID_RECALC_SHADING);
  }
}

static bool curve_patch_object_drag_in_flight()
{
  std::lock_guard lock(curve_patch_object_globals().runtime.mutex());
  return curve_patch_object_globals().drag_in_flight > 0;
}

static void curve_patch_object_drag_release_notify()
{
  if (G_MAIN == nullptr) {
    return;
  }
  /* The rows held back during the drag are rebuilt by the next scene update's refresh: nothing
   * else tags the graph after the last step, so the release asks for that update itself. */
  for (Material &ma : G_MAIN->materials) {
    if (BKE_paint_layers_is_layered(ma)) {
      DEG_id_tag_update(&ma.id, ID_RECALC_SYNC_TO_EVAL | ID_RECALC_SHADING);
    }
  }
}

void BKE_paint_layers_curve_patch_object_drag_in_flight_set(const bool drag_in_flight)
{
  bool released = false;
  {
    std::lock_guard lock(curve_patch_object_globals().runtime.mutex());
    int &depth = curve_patch_object_globals().drag_in_flight;
    if (drag_in_flight) {
      depth++;
    }
    else if (depth > 0) {
      depth--;
      released = depth == 0;
    }
  }
  if (released) {
    curve_patch_object_drag_release_notify();
  }
}

void BKE_paint_layers_curve_patch_object_drag_in_flight_reset()
{
  bool released = false;
  {
    std::lock_guard lock(curve_patch_object_globals().runtime.mutex());
    int &depth = curve_patch_object_globals().drag_in_flight;
    released = depth > 0;
    depth = 0;
  }
  if (released) {
    curve_patch_object_drag_release_notify();
  }
}

void BKE_paint_layers_curve_patch_object_changed_cb_set(void (*cb)(Main &bmain,
                                                                   Material &ma,
                                                                   MaterialPaintLayer &row,
                                                                   const Depsgraph *depsgraph,
                                                                   void *user_data),
                                                        void *user_data)
{
  std::lock_guard lock(curve_patch_object_globals().runtime.mutex());
  curve_patch_object_globals().changed_cb = cb;
  curve_patch_object_globals().changed_cb_data = user_data;
}

/** \} */

void BKE_paint_layers_curve_patch_object_refresh(Main &bmain,
                                                 Material &ma,
                                                 const Depsgraph *depsgraph)
{
  if (!BKE_paint_layers_is_layered(ma) || !ID_IS_EDITABLE(&ma.id) ||
      ID_IS_OVERRIDE_LIBRARY(&ma.id))
  {
    return;
  }
  /* The hook runs editor code (a live session's rebuild and re-stamp) and the state below is
   * written from here: only the main thread may run this (render workers reach the scene update
   * as well, and their next update picks the work up, exactly like the Text Decal refresh). */
  BLI_assert(BLI_thread_is_main());

  /* A material that answered "no object-sourced Curve Patch row" once costs one cached read on
   * every update; an add, a copy or an undo clears the answer
   * (#BKE_paint_layers_curve_patch_object_invalidate). The hook is read here too, so its whole
   * access is under the same lock the registration takes. */
  void (*changed_cb)(
      Main &, Material &, MaterialPaintLayer &, const Depsgraph *, void *) = nullptr;
  void *changed_cb_data = nullptr;
  {
    std::lock_guard lock(curve_patch_object_globals().runtime.mutex());
    CurvePatchObjectRuntime *runtime = curve_patch_object_globals().runtime.map().lookup_ptr(
        ma.id.session_uid);
    changed_cb = curve_patch_object_globals().changed_cb;
    changed_cb_data = curve_patch_object_globals().changed_cb_data;
    if (runtime != nullptr) {
      if (runtime->owner != &ma) {
        return;
      }
      if (runtime->present == CurvePatchObjectPresent::None) {
        return;
      }
    }
  }

  /* The rows of the object source mode: an effect of a row (a folder included) or an item of its
   * mask, collected before anything below touches a channel record. */
  Vector<MaterialPaintLayer *> rows;
  const auto collect = [&](MaterialPaintLayer &row) {
    if (row.source != MA_PAINT_LAYER_SOURCE_CURVE_PATCH || row.curve_patch == nullptr) {
      return;
    }
    if (!BKE_paint_layers_curve_patch_reads_object(row.curve_patch)) {
      return;
    }
    rows.append(&row);
  };
  BKE_paint_layers_foreach(ma, [&](MaterialPaintLayer &layer, MaterialPaintLayer * /*parent*/) {
    collect(layer);
    for (MaterialPaintLayer &effect : layer.effects) {
      collect(effect);
    }
    for (MaterialPaintLayer &item : layer.mask_stack) {
      collect(item);
    }
    return true;
  });

  {
    std::lock_guard lock(curve_patch_object_globals().runtime.mutex());
    CurvePatchObjectRuntime &runtime = curve_patch_object_globals().runtime.map().lookup_or_add(
        ma.id.session_uid, CurvePatchObjectRuntime{});
    if (runtime.owner == nullptr) {
      runtime.owner = &ma;
    }
    if (runtime.owner != &ma) {
      return;
    }
    runtime.present = rows.is_empty() ? CurvePatchObjectPresent::None :
                                        CurvePatchObjectPresent::Present;
  }
  if (rows.is_empty()) {
    /* Rows that stopped naming an object drop their state (a material that went away entirely
     * drops it with its session uid never observed again). */
    curve_patch_object_store_hashes(ma.id.session_uid, {});
    return;
  }

  /* The same evaluated-copy resolution as #BKE_paint_layers_curve_patch_row_hash, for the same
   * reason: a change behind a modifier re-stamps the row too. */
  const bool dragging = curve_patch_object_drag_in_flight();
  Vector<CurvePatchObjectRow> observed;
  observed.reserve(rows.size());
  bool any_changed = false;
  for (MaterialPaintLayer *row : rows) {
    const MaterialPaintLayerCurvePatch &payload = *row->curve_patch;
    const uint64_t hash = BKE_paint_layers_curve_patch_row_hash(payload, depsgraph);

    bool found = false;
    uint64_t previous = 0;
    uint64_t expected = 0;
    bool pending = false;
    const bool has_expected = curve_patch_object_state_get(
        ma.id.session_uid, row->marker, previous, found, expected, pending);
    /* The write-back's own echo: the session has already stamped the shape it wrote, so the
     * evaluated hash is the new baseline, not a change. With modifiers on the curve object the
     * evaluated geometry differs from the written original, so the original's hash answers the
     * match too. */
    if (has_expected &&
        (hash == expected || BKE_paint_layers_curve_patch_row_hash(payload, nullptr) == expected))
    {
      observed.append({row->marker, hash});
      continue;
    }

    /* A live session is dragging one of its points: the objects are rewritten on every step, and
     * re-stamping every row that merely reads them per step would make the drag pay for effects
     * it does not edit. The row's stored baseline is kept, so the shape the drag's release lands
     * is still a change when the next refresh runs -- that one rebuilds the row (the writing row
     * above is exempt: its expected hash keeps the echo suppressed). */
    if (dragging && !has_expected) {
      observed.append({row->marker, found ? previous : hash});
      continue;
    }

    /* The first observation only records: the setter that pointed the row at the object (or the
     * add that created it) invalidated the bake already, and re-invalidating here would re-stamp
     * a row nothing changed. */
    if (!found || previous == hash) {
      if (pending) {
        /* The retry of an earlier re-stamp that a busy stamp host blocked: the hook decides
         * again (its passive executor returns a status, and the pending marker survives until a
         * stamp lands or the refusal is permanent). No bake request and no tag here -- the
         * change that armed them was consumed when the row went pending, and a repeated tag
         * would just reschedule this same retry. An ECHO or a drag-held-back row skips it
         * above, so neither the write-back's own change nor a drag step pays for the attempt. */
        if (changed_cb != nullptr) {
          changed_cb(bmain, ma, *row, depsgraph, changed_cb_data);
        }
      }
      observed.append({row->marker, hash});
      continue;
    }
    any_changed = true;
    /* The row's maps were stamped from the old curves: the bake is due again, and a live session
     * re-reads the object and re-stamps through the editors hook. Marked pending BEFORE the hook
     * runs, so the change asks for a re-stamp even when the hook cannot deliver it right now
     * (no hook installed, or the stamp host busy): a pending row is retried on the following
     * refreshes and at the next session end, instead of waiting for another curve edit. */
    BKE_paint_layers_curve_patch_object_restamp_pending_set(ma, *row, true);
    BKE_paint_layers_bake_request(*row);
    if (changed_cb != nullptr) {
      changed_cb(bmain, ma, *row, depsgraph, changed_cb_data);
    }
    observed.append({row->marker, hash});
  }

  curve_patch_object_store_hashes(ma.id.session_uid, std::move(observed));
  if (any_changed) {
    /* The tag is safe mid-update: it only sets the stale flag and tags the material's ID, which
     * the NEXT update pass reads -- and a repeat refresh within this pass sees its own new hash
     * in the stored state above, so it does not fire again. */
    BKE_paint_layers_tag_value_edited(ma);
  }
}

void BKE_paint_layers_curve_patch_object_refresh_tagged(Main &bmain, const Depsgraph &depsgraph)
{
  /* The hook runs editor code (a live session's rebuild and re-stamp): only the main thread may
   * do that. Render workers reach the scene update as well, and their next update picks the work
   * up, exactly like the Text Decal refresh. */
  if (!BLI_thread_is_main()) {
    return;
  }
  for (Material &ma : bmain.materials) {
    if (BKE_paint_layers_is_layered(ma)) {
      BKE_paint_layers_curve_patch_object_refresh(bmain, ma, &depsgraph);
    }
  }
}

}  // namespace blender
