/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Sculpt-mode integration for non-destructive sculpt layers (see #BKE_sculpt_layers.hh).
 *
 * This module records brush strokes into the active layer, applies per-layer influence and
 * visibility changes to the live sculpted geometry (regular mesh and multires), and implements
 * the layer management operators.
 *
 * Combination model: the final sculpted position of an element is
 * `base + sum_over_enabled_layers(layer.data[i] * effective(layer))`.
 *
 * Mesh (vertex domain): the un-layered base is kept as a runtime array
 * (#SculptSession::layers::mesh_base, derived on sculpt-mode enter and updated by non-REC
 * strokes). Interactive edits may update the live positions incrementally, but every operator
 * commit recomputes them canonically from the base, so float drift cannot accumulate.
 *
 * Multires (grid domain): layers store *tangent-space* displacement in the MDisps layout at the
 * top level. The composed surface is produced by the subdivision displacement evaluator
 * (`subdiv_displacement_multires.cc`) as `MDisps + sum(enabled layers)` — there is no runtime
 * base and CCG positions of any level are a deterministic function of the stored data. Recording
 * a stroke converts the sculpted CCG back to tangent space via the reshape machinery and stores
 * the difference in the layer, leaving #CD_MDISPS untouched
 * (see #multiresModifier_reshapeFromCCG_into_sculpt_layer).
 */

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cstdio>
#include <limits>
#include <optional>
#include <string>

#include "CLG_log.h"

#include "MEM_guardedalloc.h"

#include "DNA_key_types.h"
#include "DNA_mesh_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BLI_array.hh"
#include "BLI_bit_vector.hh"
#include "BLI_index_mask.hh"
#include "BLI_listbase.h"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_set.hh"
#include "BLI_span.hh"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_task.hh"
#include "BLI_vector.hh"

#include "BLT_translation.hh"

#include "BKE_attribute.hh"
#include "BKE_context.hh"
#include "BKE_key.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_mesh.hh"
#include "BKE_multires.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_report.hh"
#include "BKE_multires_grid_resample.hh"
#include "BKE_sculpt_layers.hh"
#include "BKE_subdiv_ccg.hh"

#include "DEG_depsgraph.hh"

#include "ED_object.hh"
#include "ED_screen.hh"
#include "ED_sculpt.hh"
#include "ED_select_utils.hh"
#include "ED_undo.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"

#include "UI_interface.hh"
#include "UI_interface_layout.hh"

#include "WM_api.hh"
#include "WM_toolsystem.hh"
#include "WM_types.hh"

#include "sculpt_intern.hh"
#include "sculpt_undo.hh"

static CLG_LogRef LOG = {"ed.sculpt_layers"};

namespace blender::ed::sculpt_paint::layers {

/* Master switch for the [DEBUG-perf] stroke instrumentation. Set to 0 to compile out the
 * stroke-end timing probes, the single-node micro-profile and the ns/vertex window so the hot path
 * runs at full speed; 1 keeps the diagnostics. When 0, #SLP_PERF forwards its arguments to an
 * inlined no-op, so the values that exist only to be printed do not trigger "unused variable"
 * warnings. Tied to the module-wide #SCULPT_LAYERS_DEBUG_LOG switch (see `BKE_sculpt_layers.hh`).
 */
#define SCULPT_LAYERS_DEBUG_PERF SCULPT_LAYERS_DEBUG_LOG
#if SCULPT_LAYERS_DEBUG_PERF
#  define SLP_PERF(...) printf(__VA_ARGS__)

using SlpPerfTimePoint = std::chrono::high_resolution_clock::time_point;

inline SlpPerfTimePoint slp_perf_now()
{
  return std::chrono::high_resolution_clock::now();
}

inline long long slp_perf_us(const SlpPerfTimePoint a, const SlpPerfTimePoint b)
{
  return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count();
}
#else
template<typename... Args> inline void slp_perf_discard(const Args &.../*args*/) {}
#  define SLP_PERF(...) slp_perf_discard(__VA_ARGS__)

/* #SLP_PERF forwards to a *function* in the disabled build, so its arguments are still evaluated:
 * writing the clock reads inline would leave every `now()` call and every elapsed-time computation
 * in the hot path with only the `printf` compiled out. Routing the probes through these two helpers
 * instead collapses them to an empty struct and a constant, which is what actually makes the
 * disabled build free. Timing probes must go through #slp_perf_now / #slp_perf_us, never
 * `std::chrono` directly. */
struct SlpPerfTimePoint {};

inline SlpPerfTimePoint slp_perf_now()
{
  return {};
}

inline long long slp_perf_us(const SlpPerfTimePoint /*a*/, const SlpPerfTimePoint /*b*/)
{
  return 0;
}
#endif

/* The mesh-invariant validator (#debug_validate_mesh_invariant) is a correctness check, not a perf
 * probe: it rebuilds and compares the whole composed surface, an O(verts) serial pass that on a
 * multi-million vertex mesh costs tens of milliseconds *inside* the stroke-end timing region and so
 * pollutes every measurement. Keep it on its own switch, off by default, so enabling the perf logs
 * does not tax the very path being measured. Flip to 1 only when hunting a canonical-invariant
 * corruption. */
#ifndef SCULPT_LAYERS_DEBUG_INVARIANT
#  define SCULPT_LAYERS_DEBUG_INVARIANT 0
#endif

/* -------------------------------------------------------------------------------------------------
 * Helpers
 */

static Mesh &mesh_of(Object &object)
{
  return *id_cast<Mesh *>(object.data);
}

/* The containing folder's uid, or 0 for a node sitting at the root (the root group holds uid 0, so
 * this needs no special case — see #bke::sculpt_layers::node_find_by_uid). */
static int node_parent_uid(const SculptLayerTreeNode &node)
{
  return node.parent ? node.parent->base.uid : 0;
}

/* The sibling \a node follows, as a uid, or 0 when it is the head of its folder. */
static int node_prev_uid(const SculptLayerTreeNode &node)
{
  return node.prev ? node.prev->uid : 0;
}

/* The single authority for a layer's effective influence (see #BKE_sculpt_layers.hh). Pulled into
 * this namespace so unqualified `effective(layer)` calls resolve to it — do not re-define a local
 * copy, so a future change to the blend/mute semantics stays a one-line edit. */
using bke::sculpt_layers::effective;

static SculptSession *session_of(Object &object)
{
  return object.runtime->sculpt_session;
}

void tag_layer_overlays_dirty(Object &object)
{
  bke::pbvh::Tree *pbvh = bke::object::pbvh_get(object);
  if (pbvh == nullptr) {
    return;
  }
  IndexMaskMemory memory;
  const IndexMask nodes = bke::pbvh::all_leaf_nodes(*pbvh, memory);
  pbvh->tag_layer_masks_changed(nodes);
  pbvh->tag_layer_previews_changed(nodes);
}

void invalidate_runtime(Object &object)
{
  SculptSession *ss = session_of(object);
  if (!ss) {
    return;
  }
  /* Drop the cached mesh base so the next #session_state_ensure re-derives it. Multires keeps no
   * runtime layer state (the composed surface is re-evaluated from stored data). */
  ss->layers.state_valid = false;
  ss->layers.mesh_base = {};
  ss->layers.base_view = {};
  ss->layers.base_view_dc = float3(0.0f);
  ss->layers.base_view_node_offset_bounds = {};
}

bool is_supported(const Object &object)
{
  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(object);
  return pbvh && pbvh->type() != bke::pbvh::Type::BMesh;
}

/* Sync-group tree fan-out (add layer / add folder / move / drag-reorder): prefer a live sculpt
 * session, but still allow mutating the layer tree on mesh data for other group members that are
 * not in sculpt mode yet. Those members are recorded in \a r_tree_only and skip surface recompose
 * / mask-edit gates. */
static bool sync_group_tree_create_member_prepare(Depsgraph *depsgraph,
                                                  const Object &active_ob,
                                                  Object *member,
                                                  wmOperator *op,
                                                  const char *warn_prefix,
                                                  bool &r_tree_only)
{
  r_tree_only = false;
  if (member->type != OB_MESH || member->data == nullptr) {
    return false;
  }
  if (active_ob.sculpt_layer_sync_group == 0 ||
      !bke::sculpt_layers::object_in_same_sync_group(active_ob, *member))
  {
    return false;
  }

  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(*member);
  if (pbvh && pbvh->type() == bke::pbvh::Type::BMesh) {
    BKE_reportf(op->reports,
                RPT_WARNING,
                "%s: skipping \"%s\" (dynamic topology)",
                warn_prefix,
                member->id.name + 2);
    return false;
  }

  if (member->runtime->sculpt_session != nullptr) {
    BKE_sculpt_update_object_for_edit(depsgraph, member, false);
    if (!is_supported(*member)) {
      bke::object::pbvh_ensure(*depsgraph, *member);
    }
  }
  else if (member->mode & OB_MODE_SCULPT) {
    BKE_object_sculpt_data_create(member);
    bke::object::pbvh_ensure(*depsgraph, *member);
  }

  if (is_supported(*member)) {
    return true;
  }

  r_tree_only = true;
  return true;
}

bool in_use(const Object &object)
{
  if (!is_supported(object)) {
    return false;
  }
  const SculptSession *ss = object.runtime->sculpt_session;
  if (ss && ss->layers.rec_active) {
    return true;
  }
  const Mesh &mesh = *id_cast<const Mesh *>(object.data);
  for (const SculptLayer *layer : bke::sculpt_layers::layers(mesh)) {
    /* "Armed", not "contributing" — a deliberately weaker question than #effective answers,
     * and the reason this tests flags instead of calling it. A layer dialled to influence 0 is
     * still armed (the slider is live, the next dab records into it), so it keeps the layer
     * machinery on; that is pre-existing behavior and not this task's to change. Folder-hidden
     * is different in kind: such a layer cannot be dialled up at all without first re-enabling
     * its folder, so it is no more armed than an explicitly disabled one. */
    if ((layer->base.flag & SCULPT_LAYER_ENABLED) &&
        !(layer->base.flag & SCULPT_LAYER_GROUP_HIDDEN))
    {
      return true;
    }
  }
  return false;
}

bool destructive_edit_check(const Mesh &mesh, ReportList *reports)
{
  /* Layers are not the only per-element carrier the tree holds: a *folder* has its own weight mask
   * (#SculptLayerTreeNode::mask lives on the shared base), and it can outlive every layer under it —
   * paint a folder mask, then remove or move out its layers. Counting layers alone let such a mesh
   * through, and since #Type::Geometry undo neither captures nor restores #Mesh::sculpt_layer_root,
   * the mask kept a `totelem` describing the pre-edit topology. #is_stale_mask then fails open, so
   * it stopped contributing silently — no crash, no report, just a weight map that quietly does
   * nothing. */
  bool has_element_data = !bke::sculpt_layers::layers(mesh).is_empty();
  if (!has_element_data) {
    for (const SculptLayerGroup *group : bke::sculpt_layers::groups(mesh)) {
      if (group->base.mask != nullptr) {
        has_element_data = true;
        break;
      }
    }
  }
  if (!has_element_data) {
    return true;
  }
  if (reports) {
    BKE_report(reports,
               RPT_INFO,
               "Sculpt layers are not baked; bake them before performing a destructive mesh edit");
  }
  return false;
}

short domain_for(const Object &object)
{
  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(object);
  return (pbvh && pbvh->type() == bke::pbvh::Type::Grids) ? SCULPT_LAYER_DOMAIN_GRID :
                                                            SCULPT_LAYER_DOMAIN_VERT;
}

int64_t element_count(const Object &object)
{
  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(object);
  if (pbvh && pbvh->type() == bke::pbvh::Type::Grids) {
    const SculptSession *ss = object.runtime->sculpt_session;
    return (ss && ss->subdiv_ccg) ? ss->subdiv_ccg->positions.size() : 0;
  }
  return id_cast<const Mesh *>(object.data)->verts_num;
}

/* -------------------------------------------------------------------------------------------------
 * Layer validation and session state
 */

/* Grid-domain layers are only usable when their data matches the MDisps layout at the current top
 * level (I3). A mismatch means the base topology changed or the file predates the resampling
 * hooks; the data cannot be mapped, so wipe it (with a warning) instead of producing garbage. */
static void validate_grid_layers(Object &object)
{
  SculptSession *ss = session_of(object);
  Mesh &mesh = mesh_of(object);
  if (!ss || !ss->multires_modifier) {
    return;
  }
  const int totlvl = ss->multires_modifier->totlvl;
  const int64_t expected = bke::grid_totelem(mesh.corners_num, totlvl);

  const auto needs_repair = [&](const SculptLayer &layer) {
    if (layer.domain != SCULPT_LAYER_DOMAIN_GRID || layer.data == nullptr) {
      return false;
    }
    return bke::sculpt_layers::is_stale(layer, expected) || layer.level != totlvl;
  };

  bool any_repair = false;
  for (const SculptLayer *layer : bke::sculpt_layers::layers(mesh)) {
    if (needs_repair(*layer)) {
      any_repair = true;
      break;
    }
  }
  if (!any_repair) {
    return;
  }

  /* The weight masks are the second per-element carrier on this domain and have to move with the
   * data, the same pairing #multires_set_tot_level makes between #resample_grid_layers and
   * #resample_grid_masks. Run first, and over the whole tree so folder masks come along: it is what
   * rescues the common case where only the level moved, and it is the only chance to do so — after
   * the wipe below a mask left at the old level would read as stale forever, and a stale mask is
   * unreachable rather than merely inert (#SCULPT_OT_layer_validate never offers a layer that is no
   * longer stale, while #mask_button_draw still draws its mask button). */
  bke::sculpt_layers::resample_grid_masks(mesh, mesh.corners_num, totlvl);

  bool warned = false;
  bool mask_warned = false;
  for (SculptLayer *layer : bke::sculpt_layers::layers(mesh)) {
    if (!needs_repair(*layer)) {
      continue;
    }
    MEM_delete_void(layer->data);
    layer->data = nullptr;
    layer->totelem = 0;
    layer->level = short(totlvl);
    /* Whatever the resample above could not carry across — a mask whose geometry no longer names a
     * level on this topology, which is exactly the case the resampler leaves alone — is dropped
     * rather than left in place, for the unreachability reason given above. Weights that select
     * among deltas cannot outlive the deltas: this layer's data was just wiped. */
    if (layer->base.mask != nullptr &&
        bke::sculpt_layers::is_stale_mask(*layer->base.mask, expected))
    {
      bke::sculpt_layers::mask_free(layer->base.mask);
      layer->base.mask = nullptr;
      if (!mask_warned) {
        /* Logged rather than reported: this runs from the session-state refresh, which has no
         * #ReportList to speak through. #layer_validate_exec, the operator counterpart, reports. */
        CLOG_WARN(&LOG,
                  "Grid sculpt layer weight mask dropped: it could not be mapped onto the current "
                  "topology, so the layer now contributes in full.");
        mask_warned = true;
      }
    }
    if (!warned) {
      CLOG_WARN(&LOG,
                "Grid sculpt layer data reset: stored level/topology no longer matches the mesh.");
      warned = true;
    }
  }
}

/* Vertex-domain counterpart of #validate_grid_layers, and the "layer validator" that
 * #OBJECT_OT_editmode_toggle's warning defers to (see `object_edit.cc`). Unlike the grid one it
 * only reports: a grid layer's data is wiped because it cannot even be indexed against the MDisps
 * layout the evaluation walks, whereas a stale vertex layer is simply skipped by every consumer, so
 * there is no need to destroy the user's data behind their back. The layer stays visibly broken
 * (marked in the list, its value controls refused by the RNA setters) until the user picks a repair
 * through #SCULPT_OT_layer_validate. */
static void warn_stale_vert_layers(Object &object)
{
  const Mesh &mesh = mesh_of(object);
  for (const SculptLayer *layer : bke::sculpt_layers::layers(mesh)) {
    if (layer->domain != SCULPT_LAYER_DOMAIN_VERT || !bke::sculpt_layers::is_stale(mesh, *layer)) {
      continue;
    }
    CLOG_WARN(&LOG,
              "Sculpt layer '%s' is stale: it stores %d elements but the mesh has %d vertices. "
              "Its displacement is ignored until the layer is reset or removed.",
              layer->base.name,
              layer->totelem,
              mesh.verts_num);
    /* One line per session-state refresh is enough to flag the condition; the list marks every
     * affected layer. */
    break;
  }
}

void session_state_ensure(Object &object)
{
  SculptSession *ss = session_of(object);
  if (!ss) {
    return;
  }
  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(object);
  if (!pbvh) {
    return;
  }
  ss->layers.state_valid = true;
  if (pbvh->type() == bke::pbvh::Type::Grids) {
    /* Multires keeps no runtime layer state: the composed surface is a deterministic function of
     * `MDisps + sum(enabled layers)` evaluated by the depsgraph. Only validate the stored data. */
    ss->layers.mesh_base = {};
    validate_grid_layers(object);
    return;
  }
  warn_stale_vert_layers(object);
  if (ss->deform_modifiers_active || ss->shapekey_active) {
    /* Deform modifier / shape key: like the grids path, keep no runtime mesh base. The composed
     * surface is re-derived from `base + sum(enabled layers)` by the depsgraph at evaluation
     * (see #apply_vert_layers_eval), and #mesh.vert_positions holds the untouched basis — deriving
     * a base by subtracting the layers from it would mix spaces (basis vs. evaluated) and must not
     * feed the canonical recompute (see #recompute_mesh_canonical). */
    ss->layers.mesh_base = {};
    return;
  }
  /* Mesh (vertex) domain: capture the un-layered base from the live combined positions. */
  Mesh &mesh = mesh_of(object);
  const Span<float3> positions = mesh.vert_positions();
  ss->layers.mesh_base.reinitialize(positions.size());
  bke::sculpt_layers::derive_base_mesh(
      positions, bke::sculpt_layers::layers(mesh), ss->layers.mesh_base);
  SLP_PERF("[DEBUG-perf] session_state_ensure: captured mesh_base for %zu verts\n",
           size_t(positions.size()));
}

/* -------------------------------------------------------------------------------------------------
 * Canonical position update after a layer change
 */

/* Recompute the live mesh positions canonically from `mesh_base + layers` and refresh the PBVH
 * without a full geometry re-evaluation (the re-eval would rebuild the whole PBVH — the
 * per-stroke freeze this system explicitly avoids). Falls back to an honest re-eval for the
 * complex paths (deform modifiers / shape keys) whose live positions are not a direct copy of the
 * evaluated surface. */
static void recompute_mesh_canonical(Object &object)
{
  SculptSession *ss = session_of(object);
  if (!ss) {
    return;
  }
  Mesh &mesh = mesh_of(object);

  if (ss->deform_modifiers_active || ss->shapekey_active) {
    /* Under a shape key / deform modifier the composed surface is re-derived from `base + layers`
     * at evaluation (see #apply_vert_layers_eval); #mesh.vert_positions holds the untouched basis
     * and must NOT be overwritten here. Writing `mesh_base + layers` into the basis corrupts it and
     * the change leaks into the surface as an inverted layer (disabling a layer subtracts it from
     * the whole shape) — mirrors #restore_active_sculpt_layer's #skip_positions. A plain geometry
     * re-evaluation reflects the metadata change without touching the basis. */
    mesh.tag_positions_changed();
    DEG_id_tag_update(&mesh.id, ID_RECALC_GEOMETRY);
    return;
  }

  MutableSpan<float3> positions = mesh.vert_positions_for_write();
  if (!ss->layers.state_valid || ss->layers.mesh_base.size() != positions.size()) {
    /* Base missing or stale (topology change): re-capture before recomputing. The capture derives
     * the base from the current (layer-combined) positions and must run before any recompute that
     * would fold the layers in twice. */
    session_state_ensure(object);
  }
  if (ss->layers.mesh_base.size() != positions.size()) {
    /* Capture still could not size the base (no valid pbvh/mesh): nothing safe to do. */
    return;
  }
  bke::sculpt_layers::combine_layers_mesh(
      ss->layers.mesh_base, bke::sculpt_layers::layers(mesh), positions);
  mesh.tag_positions_changed();

  bke::pbvh::Tree *pbvh = bke::object::pbvh_get(object);
  if (!pbvh || pbvh->type() != bke::pbvh::Type::Mesh) {
    DEG_id_tag_update(&mesh.id, ID_RECALC_GEOMETRY);
    return;
  }
  IndexMaskMemory memory;
  const IndexMask node_mask = bke::pbvh::all_leaf_nodes(*pbvh, memory);
  pbvh->tag_positions_changed(node_mask);
  pbvh->update_bounds_mesh(mesh.vert_positions());
  /* The positions just moved outside of a stroke, so the "original" bounds must follow. They are
   * otherwise only resynced at stroke end (#store_bounds_orig_for_dirty_leaves), which is too late:
   * the next stroke gathers its nodes against #Node::bounds_orig_ (every brush with accumulate off
   * passes `use_original`), and stale boxes drop nodes whose geometry the layer change moved under
   * the cursor. Those nodes then keep their vertices while their neighbors deform — the mesh tears
   * along leaf-node borders. */
  bke::pbvh::store_bounds_orig(*pbvh);
  mesh.bounds_set_eager(bke::pbvh::bounds_get(*pbvh));
  if (object.runtime->bounds_eval) {
    object.runtime->bounds_eval = mesh.bounds_min_max();
  }
  DEG_id_tag_update(&object.id, ID_RECALC_SHADING);
}

#if SCULPT_LAYERS_DEBUG_INVARIANT
/* Debug validator for the mesh-domain canonical invariant `positions == mesh_base + sum(enabled
 * vert layers * influence)`. Prints a report when the live positions diverge, tagged with the
 * calling site, so a corruption can be attributed to the operation that introduced it. Only
 * meaningful for the common mesh path (no deform modifiers / shape keys).
 *
 * The invariant now presumes *masked* composition: #derive_base_mesh and #combine_layers_mesh both
 * attenuate a layer by its own mask and by the folder chain above it, and they do it through one
 * shared routine, so the round trip through #session_state_ensure stays exact. The consequence is
 * that a position written by any path that still adds a layer unmasked no longer disappears into
 * the base — it is reported here as a divergence instead of being silently absorbed. That is the
 * intended behavior: such a write is the defect, and this is the check that names it. */
static void debug_validate_mesh_invariant(Object &object, const char *where)
{
  SculptSession *ss = session_of(object);
  if (!ss || !ss->layers.state_valid) {
    return;
  }
  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(object);
  if (!pbvh || pbvh->type() != bke::pbvh::Type::Mesh) {
    return;
  }
  if (ss->deform_modifiers_active || ss->shapekey_active) {
    return;
  }
  Mesh &mesh = mesh_of(object);
  const Span<float3> positions = mesh.vert_positions();
  if (ss->layers.mesh_base.size() != positions.size()) {
    printf("[sculpt-layers][invariant] %s: mesh_base size %zu != positions %zu\n",
           where,
           size_t(ss->layers.mesh_base.size()),
           size_t(positions.size()));
    return;
  }
  Array<float3> expected(positions.size());
  bke::sculpt_layers::combine_layers_mesh(
      ss->layers.mesh_base.as_span(), bke::sculpt_layers::layers(mesh), expected);
  float max_dev = 0.0f;
  int64_t over = 0;
  int64_t first_bad = -1;
  for (const int64_t i : positions.index_range()) {
    const float dev = math::length(positions[i] - expected[i]);
    if (dev > max_dev) {
      max_dev = dev;
    }
    if (dev > 1e-4f) {
      over++;
      if (first_bad < 0) {
        first_bad = i;
      }
    }
  }
  if (over > 0) {
    printf(
        "[sculpt-layers][invariant] BROKEN at %s: max_dev=%.6f verts_over=%lld/%lld "
        "first_bad=%lld\n",
        where,
        double(max_dev),
        static_cast<long long>(over),
        static_cast<long long>(positions.size()),
        static_cast<long long>(first_bad));
    int layer_index = 0;
    for (const SculptLayer *layer : bke::sculpt_layers::layers(mesh)) {
      printf("  layer[%d] uid=%d domain=%d enabled=%d influence=%.4f totelem=%d\n",
             layer_index++,
             layer->base.uid,
             int(layer->domain),
             int((layer->base.flag & SCULPT_LAYER_ENABLED) != 0),
             double(layer->influence),
             layer->totelem);
    }
  }
  else {
    printf("[sculpt-layers][invariant] ok at %s (max_dev=%.7f)\n", where, double(max_dev));
  }
}
#else
static void debug_validate_mesh_invariant(Object & /*object*/, const char * /*where*/) {}
#endif

void commit_layers_change(const Depsgraph & /*depsgraph*/, Object &object)
{
  commit_layers_change(object);
}

void commit_layers_change(Object &object)
{
  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(object);
  const bool mesh_path = pbvh && pbvh->type() == bke::pbvh::Type::Mesh;
  if (mesh_path) {
    /* The base is captured here, before the refresh below, and not left to the lazy capture inside
     * #recompute_mesh_canonical. #derive_base_mesh recovers the base by subtracting each layer's
     * contribution at the weights in force when it runs, so deriving it after the exemption moved
     * would subtract an unmasked contribution from positions that were composed masked and fold the
     * difference into the base permanently — the dent #bke::sculpt_layers::rec_exempt_set warns
     * about. #layer_toggle_rec_exec settles the base before its own flip for the same reason.
     *
     * Guarded exactly as #recompute_mesh_canonical guards its own lazy capture, so the capture is
     * still paid at most once and is not paid at all on the multires and deform / shape-key paths,
     * which keep no runtime mesh base and re-evaluate instead. */
    const SculptSession *ss = session_of(object);
    if (ss && !ss->deform_modifiers_active && !ss->shapekey_active &&
        (!ss->layers.state_valid || ss->layers.mesh_base.size() != mesh_of(object).verts_num))
    {
      session_state_ensure(object);
    }
    /* Pins the ordering the comment above argues for, so a future reshuffle fails loudly instead of
     * silently denting the base. Moving #rec_exemption_refresh above the capture would leave an
     * invalid state here on exactly the runs where the capture was needed; when the state was
     * already valid the capture is skipped and there is nothing for the exemption to corrupt, which
     * is why the relaxed cases are permitted. */
    BLI_assert_msg(!ss || ss->deform_modifiers_active || ss->shapekey_active ||
                       (ss->layers.state_valid &&
                        ss->layers.mesh_base.size() == mesh_of(object).verts_num),
                   "The mesh base must be captured before the REC exemption is refreshed");
  }
  /* Before the recompose below, not after: the exemption decides how the recording layer's mask is
   * weighed, so a resync afterwards would leave the freshly composed positions one step stale. Every
   * operation that can move a layer, reparent it or restore an undo step passes through here, which
   * is what keeps the exemption tracking the active layer without each of those paths having to know
   * that REC exists. Idempotent and one uid lookup, so calling it on every commit is free.
   *
   * The result is deliberately not consulted: this function recomposes unconditionally on both
   * paths below, so there is no stale surface left for a caller to act on. */
  rec_exemption_refresh(object);
  /* Placed here, ahead of the branch, because the two paths below both return: the sculpt layer mask
   * overlay draws one node's weights, and a node can have been added, removed, reparented or had its
   * mask replaced by whatever called this. The comment above already establishes that every such
   * operation passes through here, which makes this the one place that does not have to be
   * remembered at each call site — the overlay stayed stale after a layer delete for precisely that
   * reason. The paths that deliberately skip recomposition (the bakes, which leave the surface
   * unchanged on purpose) do not reach this and tag for themselves. */
  tag_layer_overlays_dirty(object);
  if (mesh_path) {
    recompute_mesh_canonical(object);
    debug_validate_mesh_invariant(object, "commit_layers_change");
    return;
  }
  /* Multires (and any other target): honest re-evaluation. The depsgraph rebuilds the CCG from
   * `MDisps + sum(enabled layers)`, which is the single point of truth for the composed surface. */
  DEG_id_tag_update(&mesh_of(object).id, ID_RECALC_GEOMETRY);
}

void flush_pending_multires_base(Object &object)
{
  SculptSession *ss = session_of(object);
  if (!ss || !ss->subdiv_ccg) {
    return;
  }
  if (!ss->subdiv_ccg->dirty.coords && !ss->subdiv_ccg->dirty.hidden) {
    return;
  }
  SLP_PERF("[sculpt-layers][flush] flush_pending_multires_base: consuming pending base edits\n");
  /* Reshape the pending (lazily flushed) base sculpt edits from the live CCG into the base MDisps
   * NOW, while the CCG and the stored layer set are still consistent. The layer-aware reshape
   * subtracts the *current* layer contributions from the composed surface; if the layer set or an
   * influence changed first, a later flush (e.g. #object_update_from_subsurf_ccg during the
   * re-evaluation that the change itself triggers) would subtract the new set from a CCG built
   * with the old one and leak the difference into the base MDisps. */
  multires_flush_sculpt_updates(&object);
}

bool flush_pending_multires_base_for_mesh(Main &bmain, Mesh &mesh)
{
  for (Object &ob : bmain.objects) {
    if (ob.data != &mesh.id || !(ob.mode & OB_MODE_SCULPT) || !ob.runtime->sculpt_session) {
      continue;
    }
    const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(ob);
    if (pbvh && pbvh->type() == bke::pbvh::Type::Grids) {
      flush_pending_multires_base(ob);
      return true;
    }
  }
  return false;
}

bool finish_mask_edit(Object &object)
{
  const SculptSession *ss = session_of(object);
  if (ss == nullptr || ss->layers.mask_edit.node_uid == 0) {
    return false;
  }
  /* Stored rather than discarded, and rather than refusing the level change: the weights are the
   * user's hand work, and #mask_edit_end is the very same close the Finish operator performs — it
   * consumes the pending base edits, puts the user's own sculpt mask back and recomposes. What it
   * deliberately does not do is push an undo step of its own; the callers here are all global-undo
   * operators or RNA setters whose own step captures the mesh, mask included. */
  mask_edit_end(object);
  return true;
}

bool finish_mask_edit_for_mesh(Main &bmain, Mesh &mesh)
{
  /* Sculpt mode owns a single active object, so the first object using this mesh in sculpt mode
   * with a live session is the relevant one — as in #flush_pending_multires_base_for_mesh. */
  for (Object &ob : bmain.objects) {
    if (ob.data != &mesh.id || !(ob.mode & OB_MODE_SCULPT) || !ob.runtime->sculpt_session) {
      continue;
    }
    return finish_mask_edit(ob);
  }
  return false;
}

bool flush_interactive_update(Main &bmain, Mesh &mesh)
{
  const auto t_start = slp_perf_now();

  /* Sculpt mode owns a single active object, so the first object using this mesh in sculpt mode
   * with a live session is the relevant one. */
  const auto t_search0 = slp_perf_now();
  Object *object = nullptr;
  for (Object &ob : bmain.objects) {
    if (ob.data != &mesh.id || !(ob.mode & OB_MODE_SCULPT) || !ob.runtime->sculpt_session) {
      continue;
    }
    object = &ob;
    break;
  }
  const auto t_search1 = slp_perf_now();
  if (!object) {
    return false;
  }

  const SculptSession *ss = object->runtime->sculpt_session;
  bke::pbvh::Tree *pbvh = bke::object::pbvh_get(*object);
  /* Only the vertex-domain mesh path keeps the live positions in sync directly (the RNA setter
   * already wrote them). Multires is driven from the #SubdivCCG and shape-key / deform-modifier
   * sessions do not draw the mesh positions verbatim, so those need the full re-evaluation. */
  if (!pbvh || pbvh->type() != bke::pbvh::Type::Mesh || ss->deform_modifiers_active ||
      ss->shapekey_active)
  {
    return false;
  }

  debug_validate_mesh_invariant(*object, "rna_flush");

  /* Invalidate the PBVH nodes, refresh the bounds from the new positions and request a viewport
   * redraw. This avoids the per-tick #ID_RECALC_GEOMETRY that would rebuild the evaluated mesh and
   * the whole PBVH, which is what makes the influence slider lag on dense meshes. */
  IndexMaskMemory memory;
  const auto t_leaf0 = slp_perf_now();
  const IndexMask node_mask = bke::pbvh::all_leaf_nodes(*pbvh, memory);
  const auto t_leaf1 = slp_perf_now();
  pbvh->tag_positions_changed(node_mask);
  const auto t_tag1 = slp_perf_now();
  pbvh->update_bounds_mesh(mesh.vert_positions());
  /* Keep the "original" bounds in step with the moved positions; see #recompute_mesh_canonical for
   * why a stale copy tears the next stroke along node borders. Cheap next to the bounds update
   * itself: one copy per node, not per vertex. */
  bke::pbvh::store_bounds_orig(*pbvh);
  const auto t_bounds_update1 = slp_perf_now();
  mesh.bounds_set_eager(bke::pbvh::bounds_get(*pbvh));
  if (object->runtime->bounds_eval) {
    object->runtime->bounds_eval = mesh.bounds_min_max();
  }
  const auto t_bounds_block1 = slp_perf_now();

  DEG_id_tag_update(&object->id, ID_RECALC_SHADING);
  WM_main_add_notifier(NC_OBJECT | ND_DRAW, &object->id);

  const auto t_end = slp_perf_now();
  SLP_PERF("[DEBUG-perf] flush_interactive_update: search=%lldus all_leaf_nodes=%lldus "
           "tag_positions_changed=%lldus update_bounds_mesh=%lldus bounds_block=%lldus "
           "total=%lldus leaf_nodes=%lld verts=%d\n",
           slp_perf_us(t_search0, t_search1),
           slp_perf_us(t_leaf0, t_leaf1),
           slp_perf_us(t_leaf1, t_tag1),
           slp_perf_us(t_tag1, t_bounds_update1),
           slp_perf_us(t_bounds_update1, t_bounds_block1),
           slp_perf_us(t_start, t_end),
           static_cast<long long>(node_mask.size()),
           mesh.verts_num);
  return true;
}

bool sync_multires_for_rna(Main &bmain, Scene * /*scene*/, Mesh &mesh, SculptLayer & /*changed_layer*/)
{
  /* Locate the first object in sculpt mode that uses this mesh and has a live grids PBVH. */
  Object *object = nullptr;
  for (Object &ob : bmain.objects) {
    if (ob.data != &mesh.id || !(ob.mode & OB_MODE_SCULPT) || !ob.runtime->sculpt_session) {
      continue;
    }
    const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(ob);
    if (pbvh && pbvh->type() == bke::pbvh::Type::Grids) {
      object = &ob;
      break;
    }
  }
  if (!object) {
    return false; /* No live grid session; caller falls back to ID_RECALC_GEOMETRY. */
  }

  /* Settle an open weight-mask session before the rebuild below discards it. The layer operators
   * refuse outright instead (#mask_edit_refuse_ccg_rebuild), but an RNA update callback runs *after*
   * the value has already changed, so there is nothing left to refuse — closing keeps the painted
   * weights (compressed onto the node) instead of letting the rebuilt CCG drop them. The closed
   * session is visible in the UI: the row's mask icon reverts. */
  mask_edit_end(*object);

  /* Honest re-evaluation: the depsgraph rebuilds the CCG from `MDisps + sum(enabled layers)`,
   * which already reflects the just-changed influence/visibility. MDisps are not touched (the
   * base never contains layer contributions). Undo is handled by the layer operators — an RNA
   * update callback runs after the value changed, so it cannot capture a pre-change snapshot. */
  DEG_id_tag_update(&mesh.id, ID_RECALC_GEOMETRY);

  WM_main_add_notifier(NC_OBJECT | ND_DRAW, &object->id);
  WM_main_add_notifier(NC_GEOM | ND_DATA, &mesh.id);
  return true;
}

static void apply_layer_enabled_value_to_mesh(Main &bmain,
                                              Mesh &mesh,
                                              SculptLayer &layer,
                                              const bool enable)
{
  const bool was_enabled = (layer.base.flag & SCULPT_LAYER_ENABLED) != 0;
  if (was_enabled == enable) {
    return;
  }
  if (layer.domain == SCULPT_LAYER_DOMAIN_VERT) {
    if (mesh.runtime->edit_mesh != nullptr || mesh.verts_num == 0) {
      return;
    }
    if (bke::sculpt_layers::is_stale(layer, mesh.verts_num)) {
      return;
    }
  }
  if (layer.domain == SCULPT_LAYER_DOMAIN_GRID) {
    flush_pending_multires_base_for_mesh(bmain, mesh);
  }
  const float old_effective = effective(layer);
  SET_FLAG_FROM_TEST(layer.base.flag, enable, SCULPT_LAYER_ENABLED);
  if (layer.domain == SCULPT_LAYER_DOMAIN_VERT && mesh.key == nullptr) {
    MutableSpan<float3> positions = mesh.vert_positions_for_write();
    bke::sculpt_layers::apply_delta_mesh(layer, effective(layer) - old_effective, positions);
    mesh.tag_positions_changed();
  }
}

static void apply_layer_influence_value_to_mesh(Main &bmain,
                                                Mesh &mesh,
                                                SculptLayer &layer,
                                                const float value)
{
  if (layer.domain == SCULPT_LAYER_DOMAIN_VERT) {
    if (mesh.runtime->edit_mesh != nullptr || mesh.verts_num == 0) {
      return;
    }
    if (bke::sculpt_layers::is_stale(layer, mesh.verts_num)) {
      return;
    }
  }
  if (layer.domain == SCULPT_LAYER_DOMAIN_GRID) {
    flush_pending_multires_base_for_mesh(bmain, mesh);
  }
  const float old_effective = effective(layer);
  layer.influence = value;
  if (layer.domain == SCULPT_LAYER_DOMAIN_VERT && mesh.key == nullptr) {
    MutableSpan<float3> positions = mesh.vert_positions_for_write();
    bke::sculpt_layers::apply_delta_mesh(layer, effective(layer) - old_effective, positions);
    mesh.tag_positions_changed();
  }
}

static void apply_group_influence_value_to_mesh(Main &bmain,
                                                Mesh &mesh,
                                                SculptLayerGroup &group,
                                                const float value)
{
  const Span<SculptLayer *> descendants = bke::sculpt_layers::layers(group);
  for (SculptLayer *layer : descendants) {
    if (layer->domain == SCULPT_LAYER_DOMAIN_GRID) {
      flush_pending_multires_base_for_mesh(bmain, mesh);
      break;
    }
  }
  Vector<float> old_effective(descendants.size());
  for (const int64_t i : descendants.index_range()) {
    old_effective[i] = effective(*descendants[i]);
  }
  group.influence = value;
  bke::sculpt_layers::resync_group_state(mesh);
  bool tagged_positions = false;
  for (const int64_t i : descendants.index_range()) {
    SculptLayer &layer = *descendants[i];
    if (layer.domain != SCULPT_LAYER_DOMAIN_VERT || mesh.key != nullptr ||
        mesh.runtime->edit_mesh != nullptr || mesh.verts_num == 0 ||
        bke::sculpt_layers::is_stale(layer, mesh.verts_num))
    {
      continue;
    }
    MutableSpan<float3> positions = mesh.vert_positions_for_write();
    bke::sculpt_layers::apply_delta_mesh(
        layer, effective(layer) - old_effective[i], positions);
    tagged_positions = true;
  }
  if (tagged_positions) {
    mesh.tag_positions_changed();
  }
}

static void refresh_mesh_after_interactive_layer_change(Main &bmain, Mesh &mesh)
{
  if (flush_interactive_update(bmain, mesh)) {
    return;
  }
  for (SculptLayer *layer : bke::sculpt_layers::layers(mesh)) {
    if (layer->domain == SCULPT_LAYER_DOMAIN_GRID) {
      if (sync_multires_for_rna(bmain, nullptr, mesh, *layer)) {
        return;
      }
      break;
    }
  }
  DEG_id_tag_update(&mesh.id, ID_RECALC_GEOMETRY);
  WM_main_add_notifier(NC_GEOM | ND_DATA, &mesh.id);
  WM_main_add_notifier(NC_OBJECT | ND_MODIFIER, nullptr);
}

void sync_group_propagate_layer_influence(Main &bmain,
                                          Object &source_ob,
                                          const SculptLayer &source_layer,
                                          Depsgraph *depsgraph)
{
  if (source_layer.base.sync_uid == 0 || source_ob.sculpt_layer_sync_group == 0) {
    return;
  }
  const float value = source_layer.influence;
  const int sync_uid = source_layer.base.sync_uid;

  for (Object *member_ob : sync_group_members(bmain, source_ob)) {
    if (!bke::sculpt_layers::sync_group_is_valid_mesh_member(*member_ob)) {
      continue;
    }
    Mesh &member_mesh = mesh_of(*member_ob);
    SculptLayerTreeNode *member_node = bke::sculpt_layers::node_find_by_sync_uid(member_mesh,
                                                                                  sync_uid);
    SculptLayer *member_layer = bke::sculpt_layers::node_as_layer(member_node);
    if (member_layer == nullptr) {
      continue;
    }
    if (depsgraph != nullptr && member_ob->runtime->sculpt_session != nullptr &&
        is_supported(*member_ob))
    {
      if (bke::object::pbvh_get(*member_ob)->type() == bke::pbvh::Type::Grids) {
        flush_pending_multires_base(*member_ob);
      }
      session_state_ensure(*member_ob);
      member_layer->influence = value;
      commit_layers_change(*depsgraph, *member_ob);
    }
    else {
      apply_layer_influence_value_to_mesh(bmain, member_mesh, *member_layer, value);
      refresh_mesh_after_interactive_layer_change(bmain, member_mesh);
    }
    WM_main_add_notifier(NC_OBJECT | ND_DRAW, member_ob);
  }
}

void sync_group_propagate_group_influence(Main &bmain,
                                          Object &source_ob,
                                          const SculptLayerGroup &source_group)
{
  if (source_group.base.sync_uid == 0 || source_ob.sculpt_layer_sync_group == 0) {
    return;
  }
  const float value = source_group.influence;
  const int sync_uid = source_group.base.sync_uid;

  for (Object *member_ob : sync_group_members(bmain, source_ob)) {
    if (!bke::sculpt_layers::sync_group_is_valid_mesh_member(*member_ob)) {
      continue;
    }
    Mesh &member_mesh = mesh_of(*member_ob);
    SculptLayerTreeNode *member_node = bke::sculpt_layers::node_find_by_sync_uid(member_mesh,
                                                                                  sync_uid);
    SculptLayerGroup *member_group = bke::sculpt_layers::node_as_group(member_node);
    if (member_group == nullptr) {
      continue;
    }
    apply_group_influence_value_to_mesh(bmain, member_mesh, *member_group, value);
    refresh_mesh_after_interactive_layer_change(bmain, member_mesh);
    WM_main_add_notifier(NC_OBJECT | ND_DRAW, member_ob);
  }
}

void sync_group_propagate_node_name(Main &bmain,
                                    Object &source_ob,
                                    const SculptLayerTreeNode &source_node)
{
  if (source_node.sync_uid == 0 || source_ob.sculpt_layer_sync_group == 0) {
    return;
  }
  const int sync_uid = source_node.sync_uid;

  for (Object *member_ob : sync_group_members(bmain, source_ob)) {
    if (!bke::sculpt_layers::sync_group_is_valid_mesh_member(*member_ob)) {
      continue;
    }
    Mesh &member_mesh = mesh_of(*member_ob);
    SculptLayerTreeNode *member_node = bke::sculpt_layers::node_find_by_sync_uid(member_mesh,
                                                                                  sync_uid);
    if (member_node == nullptr) {
      continue;
    }
    STRNCPY_UTF8(member_node->name, source_node.name);
    bke::sculpt_layers::node_name_ensure_unique(*member_node);
    DEG_id_tag_update(&member_mesh.id, ID_RECALC_GEOMETRY);
    WM_main_add_notifier(NC_GEOM | ND_DATA, &member_mesh.id);
    WM_main_add_notifier(NC_OBJECT | ND_DRAW, member_ob);
  }
}

/* -------------------------------------------------------------------------------------------------
 * Base view (base edits only — never while recording into a layer, see #stroke_record_begin)
 *
 * When the base is sculpted with layers visible, any brush input computed from the composed
 * surface embeds the layer pattern: smoothing/plane targets absorb it directly, and even
 * offset-style brushes (Draw, Grab) leak it through pattern-modulated falloff distances and
 * normals. The flush then has to store `surface - layers` and the residual bakes into the base.
 * The base view is the per-element layer contribution `O = combined - base`, computed once per
 * stroke; brushes subtract it from the live positions for their *computations* while still
 * writing to the live (composed) positions, so the base receives a pattern-free delta and the
 * user keeps seeing base + layers.
 *
 * The offset is always taken *relative to the brush contact point* (`O[i] - O_dc`, see
 * #base_view_dc_update): the brush reference point stays on the composed surface, so removing the
 * raw offset would push the sampled positions out of the brush radius by the layer height. Only the
 * pattern must be stripped from the brush inputs, not the height under the cursor.
 *
 * Brush texture coordinates are deliberately NOT part of this: they are sampled on the composed
 * surface (see #sculpt_apply_texture). The texture only modulates the amplitude, so it is not a
 * channel through which a lower layer's shape gets copied into the recorded one, while the user
 * sees and aims at the composed surface — a stencil or tiled stamp evaluated on the base view would
 * land next to the cursor, off by the screen parallax of the layer height.
 */

/* Sample the base view at the current brush contact point. Every consumer subtracts this constant
 * from the per-element offset, which anchors the sampled surface under the cursor again (see
 * #SculptSession::layers::base_view_dc). Refreshed once per brush action, since #location_symm
 * changes with the symmetry and tile pass. */
void base_view_dc_update(const Depsgraph &depsgraph, Object &object)
{
  SculptSession *ss = session_of(object);
  if (!ss) {
    return;
  }
  ss->layers.base_view_dc = float3(0.0f);

  const Span<float3> base_view = ss->layers.base_view;
  if (base_view.is_empty() || !ss->cache) {
    return;
  }
  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(object);
  if (!pbvh) {
    return;
  }
  /* The search runs on the composed positions, the same space #location_symm lives in, so the
   * contact point is genuinely on the sampled surface and the nearest element is found within a
   * fraction of the radius. */
  const float3 &location = ss->cache->location_symm;
  const float radius = ss->cache->radius;
  if (pbvh->type() == bke::pbvh::Type::Mesh) {
    const Span<float3> positions = bke::pbvh::vert_positions_eval(depsgraph, object);
    const std::optional<int> vert = nearest_vert_calc_mesh(
        *pbvh, positions, {}, location, radius, false);
    if (vert && *vert >= 0 && int64_t(*vert) < base_view.size()) {
      ss->layers.base_view_dc = base_view[*vert];
    }
  }
  else if (pbvh->type() == bke::pbvh::Type::Grids && ss->subdiv_ccg) {
    const SubdivCCG &subdiv_ccg = *ss->subdiv_ccg;
    const std::optional<SubdivCCGCoord> coord = nearest_vert_calc_grids(
        *pbvh, subdiv_ccg, location, radius, false);
    if (coord) {
      const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
      const int index = coord->to_index(key);
      if (index >= 0 && int64_t(index) < base_view.size()) {
        ss->layers.base_view_dc = base_view[index];
      }
    }
  }
}

/** Create invalid bounds for use with #math::min_max (see the same helper in `pbvh.cc`). */
static Bounds<float3> negative_bounds()
{
  return {float3(std::numeric_limits<float>::max()), float3(std::numeric_limits<float>::lowest())};
}

/* Bounds of the base-view offset (`base_view[i]`) over each leaf node's own elements. Computed once
 * per stroke, right after the base view itself; non-leaf entries stay empty and never intersect
 * anything. The offset is what is stored rather than the base-view positions, so the box can follow
 * the live positions as the stroke deforms them — see
 * #SculptSession::layers::base_view_node_offset_bounds and #base_view_extend_node_mask. */
static void base_view_node_offset_bounds_ensure(const Depsgraph &depsgraph, Object &object)
{
  SculptSession *ss = session_of(object);
  ss->layers.base_view_node_offset_bounds = {};

  const Span<float3> base_view = ss->layers.base_view;
  if (base_view.is_empty()) {
    return;
  }
  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(object);
  if (!pbvh) {
    return;
  }

  Array<Bounds<float3>> node_bounds(pbvh->nodes_num(), negative_bounds());

  IndexMaskMemory memory;
  const IndexMask leaves = bke::pbvh::all_leaf_nodes(*pbvh, memory);
  const Vector<int> leaf_indices = leaves.to_indices<int>();

  if (pbvh->type() == bke::pbvh::Type::Mesh) {
    /* Sized against the evaluated positions rather than #Mesh::vert_positions: those are what the
     * brush — and therefore the mask this feeds — works on, and with a shape key or a deform
     * modifier the mesh holds the untouched basis instead. A mismatch means the base view cannot be
     * indexed by the vertices the node bounds cover, so leave the bounds empty. */
    const Span<float3> positions = bke::pbvh::vert_positions_eval(depsgraph, object);
    if (positions.size() != base_view.size()) {
      return;
    }
    const Span<bke::pbvh::MeshNode> nodes = pbvh->nodes<bke::pbvh::MeshNode>();
    threading::parallel_for(leaf_indices.index_range(), 8, [&](const IndexRange range) {
      for (const int index : range) {
        const int node_index = leaf_indices[index];
        Bounds<float3> bounds = negative_bounds();
        /* #all_verts(), matching #update_node_bounds_mesh: the two are subtracted from each other
         * at test time, so they must cover the same elements. Using #verts() also dropped the
         * boundary neighbors, which own no vertex inside the footprint but whose shared vertices
         * still move — that carved a leaf-node grid into the stroke. */
        for (const int vert : nodes[node_index].all_verts()) {
          math::min_max(base_view[vert], bounds.min, bounds.max);
        }
        node_bounds[node_index] = bounds;
      }
    });
  }
  else if (pbvh->type() == bke::pbvh::Type::Grids && ss->subdiv_ccg) {
    const SubdivCCG &subdiv_ccg = *ss->subdiv_ccg;
    const Span<float3> positions = subdiv_ccg.positions;
    if (positions.size() != base_view.size()) {
      return;
    }
    const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
    const Span<bke::pbvh::GridsNode> nodes = pbvh->nodes<bke::pbvh::GridsNode>();
    threading::parallel_for(leaf_indices.index_range(), 8, [&](const IndexRange range) {
      for (const int index : range) {
        const int node_index = leaf_indices[index];
        Bounds<float3> bounds = negative_bounds();
        for (const int grid : nodes[node_index].grids()) {
          for (const int i : bke::ccg::grid_range(key, grid)) {
            math::min_max(base_view[i], bounds.min, bounds.max);
          }
        }
        node_bounds[node_index] = bounds;
      }
    });
  }
  else {
    return;
  }

  ss->layers.base_view_node_offset_bounds = std::move(node_bounds);
}

IndexMask base_view_extend_node_mask(const Object &object,
                                     const IndexMask &node_mask,
                                     const float radius,
                                     IndexMaskMemory &memory)
{
  const SculptSession *ss = object.runtime->sculpt_session;
  if (!ss || !ss->cache) {
    return node_mask;
  }
  const Span<Bounds<float3>> offset_bounds = ss->layers.base_view_node_offset_bounds;
  if (offset_bounds.is_empty()) {
    return node_mask;
  }
  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(object);
  if (!pbvh || offset_bounds.size() != pbvh->nodes_num()) {
    return node_mask;
  }

  /* The brush measures its falloff on the base view, `position[i] - (base_view[i] - dc)`, so an
   * element is reached when its base-view position lies within the radius of the cursor — that is,
   * when `position[i] - base_view[i]` lies within the radius of `location - dc`. Per node
   * `position` is bounded by #Node::bounds_ and `base_view` by the stored offset bounds, so their
   * difference bounds `position - base_view`: a conservative box for the node in base-view space.
   * Deriving it here rather than storing it keeps it in step with the live positions, which the
   * stroke moves dab by dab. #Node::bounds_orig_ is merged in because the "use original" brushes
   * (every brush with accumulate off) measure their falloff on the pre-stroke positions instead.
   *
   * Gather every node that box brings within the radius, on top of the nodes the composed-space
   * search already found. The result is a superset, so the elements the gather adds simply receive
   * their (possibly zero) factor like any other; what matters is that no element with a NON-zero
   * factor is left in an ungathered node, which is what carved the stroke along node borders. */
  const float3 center = ss->cache->location_symm - ss->layers.base_view_dc;
  const float radius_sq = radius * radius;
  const auto reached_nodes = [&](const auto nodes) {
    return IndexMask::from_predicate(
        IndexMask(offset_bounds.index_range()), memory, [&](const int64_t i) {
          const Bounds<float3> &offset = offset_bounds[i];
          if (offset.min.x > offset.max.x) {
            /* Empty: a non-leaf node. */
            return false;
          }
          const Bounds<float3> &live = nodes[i].bounds();
          const Bounds<float3> &orig = nodes[i].bounds_orig();
          const float3 box_min = math::min(live.min, orig.min) - offset.max;
          const float3 box_max = math::max(live.max, orig.max) - offset.min;
          const float3 nearest = math::clamp(center, box_min, box_max);
          return math::distance_squared(nearest, center) < radius_sq;
        });
  };
  if (pbvh->type() == bke::pbvh::Type::Mesh) {
    return IndexMask::from_union(
        node_mask, reached_nodes(pbvh->nodes<bke::pbvh::MeshNode>()), memory);
  }
  if (pbvh->type() == bke::pbvh::Type::Grids) {
    return IndexMask::from_union(
        node_mask, reached_nodes(pbvh->nodes<bke::pbvh::GridsNode>()), memory);
  }
  return node_mask;
}

/* The base view of a shape-key session, summed straight from the layer data:
 * `sum_over_enabled_vert_layers(data[i] * effective(layer))`.
 *
 * There is no runtime mesh base to subtract from here (#session_state_ensure keeps none) and
 * #Mesh::vert_positions holds the untouched basis, so the `positions - base` derivation of the plain
 * path does not apply. It also is not needed: the vertex layers are composed as a plain object-space
 * offset on top of whatever the deform produced (#bke::sculpt_layers::apply_vert_layers on
 * #SculptSession::deform_cos, #apply_vert_layers_eval on the evaluated mesh), so their sum *is* the
 * layer contribution, expressed in the evaluated space the brushes read and write. */
static Array<float3> base_view_from_layer_data(const Mesh &mesh, const int64_t verts_num)
{
  Array<float3> view(verts_num, float3(0.0f));
  bke::sculpt_layers::apply_vert_layers(bke::sculpt_layers::layers(mesh), view);
  return view;
}

/* Only base edits build a base view; a recorded stroke works on the composed surface (see
 * #stroke_record_begin), so there is no layer to exclude from the sum. */
static void base_view_ensure(const Depsgraph &depsgraph, Object &object)
{
  SculptSession *ss = session_of(object);
  ss->layers.base_view = {};
  ss->layers.base_view_dc = float3(0.0f);
  ss->layers.base_view_node_offset_bounds = {};

  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(object);
  if (!pbvh || pbvh->type() == bke::pbvh::Type::BMesh) {
    return;
  }
  /* A deforming modifier without a shape key does not support layer recording at all (see
   * #stroke_record_begin), so leave that session with the plain behavior. A shape key is handled:
   * its base view comes from the layer data directly (see #base_view_from_layer_data). */
  if (ss->deform_modifiers_active && !ss->shapekey_active) {
    return;
  }
  Mesh &mesh = mesh_of(object);
  const short domain = domain_for(object);
  bool any_enabled = false;
  for (const SculptLayer *layer : bke::sculpt_layers::layers(mesh)) {
    if (layer->domain == domain && effective(*layer) != 0.0f && layer->data != nullptr) {
      any_enabled = true;
      break;
    }
  }
  if (!any_enabled) {
    return;
  }

  /* Which composition path built the base view, and how many layers it actually summed. For the
   * mesh paths the sum is already baked into the maintained invariant (a single subtraction), so
   * this is 0; only the multires path pays a per-layer cost and is the candidate for a folder-level
   * composite cache. */
  const char *path_name = "none";
  int grid_layers_summed = 0;

  const auto t0 = slp_perf_now();
  if (pbvh->type() == bke::pbvh::Type::Mesh && ss->shapekey_active) {
    path_name = "mesh-shapekey";
    ss->layers.base_view = base_view_from_layer_data(mesh, mesh.verts_num);
  }
  else if (pbvh->type() == bke::pbvh::Type::Mesh) {
    path_name = "mesh-invariant";
    if (!ss->layers.state_valid) {
      session_state_ensure(object);
    }
    const Span<float3> positions = mesh.vert_positions();
    if (ss->layers.mesh_base.size() != positions.size()) {
      return;
    }
    const Span<float3> base = ss->layers.mesh_base;
    /* `positions - base` is the sum of all enabled layer contributions (the maintained mesh
     * invariant). */
    Array<float3> view(positions.size());
    threading::parallel_for(positions.index_range(), 8192, [&](const IndexRange range) {
      for (const int64_t i : range) {
        view[i] = positions[i] - base[i];
      }
    });
    ss->layers.base_view = std::move(view);
  }
  else if (ss->subdiv_ccg) {
    path_name = "multires";
    SubdivCCG &subdiv_ccg = *ss->subdiv_ccg;
    Array<float3> total(subdiv_ccg.positions.size(), float3(0.0f));
    Array<float3> contrib(subdiv_ccg.positions.size());
    for (const SculptLayer *layer : bke::sculpt_layers::layers(mesh)) {
      /* The weight must be the one the composed surface was built with: #base_view is subtracted
       * from it, so a layer weighted differently here than by #effective leaves a residual the
       * brush then reads as real geometry. */
      const float influence = effective(*layer);
      if (layer->domain != SCULPT_LAYER_DOMAIN_GRID || influence == 0.0f ||
          layer->data == nullptr) {
        continue;
      }
      if (!BKE_multires_sculpt_layer_object_contribution(mesh, subdiv_ccg, *layer, contrib)) {
        /* A partial sum would make the brush inputs inconsistent; fall back to plain behavior. */
        return;
      }
      threading::parallel_for(total.index_range(), 8192, [&](const IndexRange range) {
        for (const int64_t i : range) {
          total[i] += contrib[i] * influence;
        }
      });
      grid_layers_summed++;
    }
    ss->layers.base_view = std::move(total);
  }
  base_view_node_offset_bounds_ensure(depsgraph, object);
  const auto t1 = slp_perf_now();
  SLP_PERF("[DEBUG-perf] base_view_ensure: path=%s %zu elems grid_layers_summed=%d in %lld us\n",
           path_name,
           size_t(ss->layers.base_view.size()),
           grid_layers_summed,
           slp_perf_us(t0, t1));

#if SCULPT_LAYERS_DEBUG_PERF
  /* One-shot census of the tree the composite is built from: how many layers in total, and the
   * largest single folder's layer count. A folder-level composite cache only pays off when some
   * folder holds several layers that stay untouched while the stroke edits elsewhere. */
  {
    int folder_count = 0;
    int max_layers_in_folder = 0;
    for (SculptLayerGroup *group : bke::sculpt_layers::groups(mesh)) {
      folder_count++;
      const int n = int(bke::sculpt_layers::layers(*group).size());
      if (n > max_layers_in_folder) {
        max_layers_in_folder = n;
      }
    }
    SLP_PERF("[DEBUG-perf] base_view census: layers=%d folders=%d max_layers_in_folder=%d\n",
             int(bke::sculpt_layers::layers(mesh).size()),
             folder_count,
             max_layers_in_folder);
  }
#endif
}

Span<float3> stroke_base_view(const Object &object)
{
  const SculptSession *ss = object.runtime->sculpt_session;
  if (!ss) {
    return {};
  }
  return ss->layers.base_view;
}

float3 stroke_base_view_dc(const Object &object)
{
  const SculptSession *ss = object.runtime->sculpt_session;
  if (!ss) {
    return float3(0.0f);
  }
  return ss->layers.base_view_dc;
}

Span<float3> base_view_adjust_compact_mesh(const Object &object,
                                           const Span<int> verts,
                                           const Span<float3> positions,
                                           Vector<float3> &r_storage)
{
  const Span<float3> base_view = stroke_base_view(object);
  if (base_view.is_empty()) {
    return positions;
  }
  const float3 dc = stroke_base_view_dc(object);
  r_storage.resize(verts.size());
  for (const int i : verts.index_range()) {
    r_storage[i] = positions[i] - (base_view[verts[i]] - dc);
  }
  return r_storage;
}

Span<float3> base_view_gather_mesh(const Object &object,
                                   const Span<int> verts,
                                   const Span<float3> vert_positions,
                                   Vector<float3> &r_storage)
{
  const Span<float3> base_view = stroke_base_view(object);
  if (base_view.is_empty()) {
    return {};
  }
  const float3 dc = stroke_base_view_dc(object);
  r_storage.resize(verts.size());
  for (const int i : verts.index_range()) {
    const int vert = verts[i];
    r_storage[i] = vert_positions[vert] - (base_view[vert] - dc);
  }
  return r_storage;
}

Span<float3> base_view_adjust_compact_grids(const Object &object,
                                            const SubdivCCG &subdiv_ccg,
                                            const Span<int> grids,
                                            const Span<float3> positions,
                                            Vector<float3> &r_storage)
{
  const Span<float3> base_view = stroke_base_view(object);
  if (base_view.is_empty()) {
    return positions;
  }
  const float3 dc = stroke_base_view_dc(object);
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  r_storage.resize(positions.size());
  for (const int i : grids.index_range()) {
    const IndexRange node_range = bke::ccg::grid_range(key, i);
    const IndexRange grid_range = bke::ccg::grid_range(key, grids[i]);
    for (const int offset : IndexRange(key.grid_area)) {
      r_storage[node_range[offset]] = positions[node_range[offset]] -
                                      (base_view[grid_range[offset]] - dc);
    }
  }
  return r_storage;
}

void base_view_compose_mesh(const Object &object,
                            const Span<int> verts,
                            const MutableSpan<float3> positions)
{
  const Span<float3> base_view = stroke_base_view(object);
  if (base_view.is_empty()) {
    return;
  }
  const float3 dc = stroke_base_view_dc(object);
  for (const int i : verts.index_range()) {
    positions[i] += base_view[verts[i]] - dc;
  }
}

void base_view_compose_grids(const Object &object,
                             const SubdivCCG &subdiv_ccg,
                             const Span<int> grids,
                             const MutableSpan<float3> positions)
{
  const Span<float3> base_view = stroke_base_view(object);
  if (base_view.is_empty()) {
    return;
  }
  const float3 dc = stroke_base_view_dc(object);
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  for (const int i : grids.index_range()) {
    const IndexRange node_range = bke::ccg::grid_range(key, i);
    const IndexRange grid_range = bke::ccg::grid_range(key, grids[i]);
    for (const int offset : IndexRange(key.grid_area)) {
      positions[node_range[offset]] += base_view[grid_range[offset]] - dc;
    }
  }
}

/* -------------------------------------------------------------------------------------------------
 * Stroke recording (active layer)
 */

bool rec_exemption_refresh(Object &object)
{
  /* Tested rather than asserted, because this is called from the sculpt-mode entry and exit paths,
   * which also run for the vertex- and weight-paint sessions and could be reached for a non-mesh
   * object by a script assigning `object.mode`. Sculpt layers only ever exist on a #Mesh, so there
   * is nothing to repair on any other object type. */
  if (object.type != OB_MESH || object.data == nullptr) {
    return false;
  }
  const Mesh &mesh = *id_cast<const Mesh *>(object.data);
  const SculptSession *ss = object.runtime->sculpt_session;
  if (ss == nullptr) {
    /* No session to mirror, which for the sculpt-mode exit is not "REC is off" but "REC has nowhere
     * left to live". #SCULPT_LAYER_REC_ARMED is deliberately left exactly as it stands: it is the
     * only record that REC was armed, and reading it back is what makes the next entry restore the
     * mode the user left. #SCULPT_LAYER_REC_EXEMPT still has to go — see #object_sculpt_mode_exit
     * for why a composite outside sculpt mode may never drop a layer's weight map. */
    return bke::sculpt_layers::rec_exempt_set(mesh, nullptr);
  }
  const SculptLayer *armed_layer = ss->layers.rec_active ? bke::sculpt_layers::active_get(mesh) :
                                                           nullptr;
  /* Both bits mirror the same answer while a session exists. Ordered exemption-last only so that the
   * returned value keeps naming the bit callers recompose on; neither call reads the other's bit. */
  bke::sculpt_layers::rec_armed_set(mesh, armed_layer);
  return bke::sculpt_layers::rec_exempt_set(mesh, armed_layer);
}

void rec_active_set(Object &object, const bool armed)
{
  SculptSession *ss = session_of(object);
  if (ss == nullptr || ss->layers.rec_active == armed) {
    return;
  }
  /* Tested rather than asserted, for the reason #rec_exemption_refresh gives: the session-bearing
   * exit paths this can be reached from also run for objects that carry no sculpt layer tree. */
  if (object.type != OB_MESH || object.data == nullptr) {
    return;
  }
  SculptLayer *layer = bke::sculpt_layers::active_get(mesh_of(object));

  /* Does this layer carry any weight map at all — its own or a folder's? Asked through the one
   * resolver rather than by reading #SculptLayerTreeNode::mask here, so "masked" means exactly what
   * the composite means by it (staleness, block-size mismatch and all).
   *
   * The exemption has to be lifted for the duration of the question: while REC is armed it is
   * precisely what makes #node_mask_for_composite answer "unmasked", so the disarming half would
   * always answer no. Restored immediately, and before anything that composes: the call below only
   * resolves pointers, this runs on the main thread with no evaluation in flight, and both
   * #session_state_ensure and the multires flush further down measure the layer against the surface
   * as it stands *now* — either of them seeing a different exemption than the one the live positions
   * were composed with would dent the base by the difference. */
  bool layer_masked = false;
  if (layer != nullptr) {
    const int flag_before = layer->base.flag;
    layer->base.flag &= ~SCULPT_LAYER_REC_EXEMPT;
    layer_masked =
        bke::sculpt_layers::node_mask_for_composite(*layer, element_count(object)).primary !=
        nullptr;
    layer->base.flag = flag_before;
  }

  /* Multires, and before the exemption flips. The CCG can be holding base edits whose lazy flush
   * subtracts each layer's contribution with the weights in force at flush time; letting that flush
   * land after the flip would subtract an unmasked contribution the evaluator had composed masked
   * and dent the base by the difference. Only reachable for a masked layer, which is why it is not
   * paid on the (overwhelmingly common) unmasked toggle. */
  if (layer_masked) {
    flush_pending_multires_base(object);
  }

  /* Derive the runtime base from the still-consistent pre-change state. */
  session_state_ensure(object);
  /* Captured before the flip rather than after it, so that it measures the pre-toggle contribution
   * by construction. Reading it afterwards happens to give the same answer only because #effective
   * consults the layer and never the session, which is not a property this code should depend on. */
  const float old_eff = layer ? effective(*layer) : 0.0f;
  ss->layers.rec_active = armed;
  if (armed && layer) {
    layer->base.flag |= SCULPT_LAYER_ENABLED;
    layer->influence = 1.0f;
  }
  /* The result is not consulted here, and the condition below is used instead. This is the one site
   * that flips the bit on purpose, so it nearly always moves — but it only moves the *surface* when
   * the layer carries a weight map, which #layer_masked answers exactly. Recomposing on the return
   * value would recompute the whole mesh on every unmasked toggle, which is the common case. */
  rec_exemption_refresh(object);
  /* REC pins the layer to enabled + influence 1.0 and, for a masked layer, drops its weight map as
   * well; either way the composed surface moves, so the positions are brought in sync and the first
   * recorded stroke starts from the surface the user actually sees. Both halves of the toggle
   * recompose when a mask is involved — putting the mask back on disarm changes the surface exactly
   * as much as dropping it did. */
  if (layer && (effective(*layer) != old_eff || layer_masked)) {
    commit_layers_change(object);
  }
}

/* Create the layer REC needs when the mesh has none, and make it active.
 *
 * REC can be armed on a mesh with an empty layer tree — nothing refuses it — and every stroke then
 * silently edits the base while the button says otherwise. Rather than refuse the arming (which
 * would make the user's first act "press +, then press REC" every time), the layer is created for
 * them. Named apart from the "+" button's "Layer" so the tree shows at a glance which layers the
 * user made and which appeared on their behalf.
 *
 * Only creates and names. The caller owns the undo bracket and is responsible for recording the
 * creation into it, because the two call sites bracket their work very differently: the REC toggle
 * opens its own sculpt undo step, while the stroke start is already inside one it does not own.
 *
 * Requires no PBVH of its own — #bke::sculpt_layers::add is pure tree work — but the grid branch
 * reads the subdivision level off the session, so a session must exist. The new layer carries zero
 * displacement, so no recompose is owed: the composed surface is unchanged by construction. */
static SculptLayer *auto_layer_create(Object &object)
{
  Mesh &mesh = mesh_of(object);
  const SculptSession *ss = session_of(object);
  BLI_assert(ss != nullptr);
  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(object);
  if (pbvh != nullptr && pbvh->type() == bke::pbvh::Type::Grids) {
    /* Grid layers store data canonically at the max subdivision level, and are sized lazily by
     * #data_ensure at first use — the same arguments #layer_add_exec passes. */
    const short totlvl = ss->multires_modifier ? ss->multires_modifier->totlvl :
                                                 short(ss->subdiv_ccg ? ss->subdiv_ccg->level : 0);
    return bke::sculpt_layers::add(
        mesh, DATA_("Auto Layer"), SCULPT_LAYER_DOMAIN_GRID, 0, totlvl);
  }
  return bke::sculpt_layers::add(
      mesh, DATA_("Auto Layer"), domain_for(object), element_count(object));
}

void stroke_ensure_rec_layer(const Scene &scene, Object &object)
{
  SculptSession *ss = session_of(object);
  if (ss == nullptr || !ss->layers.rec_active) {
    return;
  }
  if (object.type != OB_MESH || object.data == nullptr) {
    return;
  }
  Mesh &mesh = mesh_of(object);
  if (bke::sculpt_layers::active_get(mesh) != nullptr) {
    return;
  }
  /* The armed-but-empty state the REC toggle now prevents at the source (#layer_toggle_rec_exec
   * creates the layer as it arms). What is left for this fallback is everything that empties the
   * tree afterwards without disarming REC — removing the last layer, an undo that restores a
   * layer-less tree — where the alternative is a stroke that silently edits the base under a lit
   * REC button.
   *
   * Deliberately a *separate* undo step, pushed before the stroke's own step opens rather than
   * inside it. #push_sculpt_layer_list_change sets #StepData::type to #Type::SculptLayer, while the
   * stroke's first dab sets it to #Type::Position; the field is one scalar, so sharing a step would
   * trip `BLI_assert(ELEM(step_data->type, Type::None, type))` in #push_node and, in a release
   * build, silently drop one of the two restores. Two steps cost the user a second Ctrl+Z and keep
   * both restores honest.
   *
   * #session_state_ensure before the bracket, mirroring #layer_add_exec: it inverts the composite to
   * recover the runtime base and has to measure the pre-change state. */
  session_state_ensure(object);
  undo::push_begin_ex(scene, object, "Auto Layer");
  SculptLayer *layer = auto_layer_create(object);
  undo::push_sculpt_layer_list_change(object, {}, Vector<int>({layer->base.uid}), false);
  /* The new layer is active now, so the exemption has to follow it — the same reason
   * #layer_add_exec refreshes. No recompose is owed: the layer carries zero displacement, and the
   * refresh only moves a bit that composes nothing while REC is armed on it. */
  rec_exemption_refresh(object);
  undo::push_end(object);
}

Vector<Object *> sync_group_members(const Main &bmain, const Object &active_ob)
{
  Vector<Object *> result;
  if (active_ob.sculpt_layer_sync_group == 0) {
    return result;
  }
  /* #ListBaseT::begin()/end() always yield a mutable element reference regardless of the
   * container's own constness (see the TODO on #ListBaseT in DNA_listBase.h), and this file
   * already relies on that to collect `Object *` from `bmain.objects` -- see
   * #flush_pending_multires_base_for_mesh above. */
  /* #layer_sync_group_create_exec refuses to *create* a group containing two objects that share
   * one #Mesh, but nothing stops a member from being linked-duplicated (Alt+D) afterwards: the
   * duplicate inherits #Object::sculpt_layer_sync_group and shares `data` with the original. Both
   * would then resolve to the SAME #SculptLayerTreeNode under the same sync_uid, and
   * #node_find_by_sync_uid's first-match resolution could no longer tell them apart -- silently
   * turning an involutive fan-out (invert, toggle) into a no-op on the duplicate. Deduplicated by
   * data pointer here, the single choke point every fan-out operator and the stroke-recording
   * extension go through, rather than guarded at each of their call sites. Seeded with \a
   * active_ob's own data so a member sharing data with the ACTIVE object is excluded too, not just
   * members sharing data with each other. Same idiom as #layer_sync_group_create_exec's
   * `object_data` set: #Set::add returns false when the key was already present. This function has
   * no #wmOperator to report through, so a duplicate is skipped silently rather than reported. */
  Set<const ID *> seen_data;
  seen_data.add(static_cast<const ID *>(active_ob.data));
  for (Object &ob : bmain.objects) {
    if (&ob == &active_ob || !bke::sculpt_layers::object_in_same_sync_group(active_ob, ob)) {
      continue;
    }
    if (!bke::sculpt_layers::sync_group_is_valid_mesh_member(ob)) {
      continue;
    }
    if (seen_data.add(static_cast<const ID *>(ob.data))) {
      result.append(&ob);
    }
  }
  return result;
}

Vector<Object *> sync_group_raw_members(const Main &bmain, const Object &active_ob)
{
  Vector<Object *> result;
  if (active_ob.sculpt_layer_sync_group == 0) {
    return result;
  }
  for (Object &ob : bmain.objects) {
    if (&ob == &active_ob || !bke::sculpt_layers::object_in_same_sync_group(active_ob, ob)) {
      continue;
    }
    result.append(&ob);
  }
  return result;
}

Vector<Object *> fanout_targets(const Main &bmain, Object &active_ob)
{
  Vector<Object *> result;
  result.append(&active_ob);
  result.extend(sync_group_members(bmain, active_ob));
  return result;
}

bool sync_group_all_targets_selected(bContext *C, Object &active_ob)
{
  Main *bmain = CTX_data_main(C);
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const View3D *v3d = CTX_wm_view3d(C);
  BKE_view_layer_synced_ensure(*bmain, scene, view_layer);

  for (Object *ob : fanout_targets(*bmain, active_ob)) {
    Base *base = BKE_view_layer_base_find(view_layer, ob);
    if (base == nullptr) {
      continue;
    }
    if (!BASE_SELECTABLE(v3d, base)) {
      continue;
    }
    if ((base->flag & BASE_SELECTED) == 0) {
      return false;
    }
  }
  return true;
}

void sync_group_unlink_object(Main &bmain, Object &ob)
{
  bke::sculpt_layers::sync_group_detach_object(bmain, ob);
}

/* Sync-group remove scope: #AllSyncGroup removes matching layers on every member and keeps sync;
 * #ActiveOnly removes only on the active object and unlinks it from the group afterward.
 * Unsynced layers (#sync_uid == 0) always take the #ActiveOnly + unlink path. */
enum class SyncScope : int {
  AllSyncGroup = 0,
  ActiveOnly = 1,
};

static const EnumPropertyItem sync_scope_items[] = {
    {int(SyncScope::AllSyncGroup),
     "ALL_SYNC_GROUP",
     0,
     "All Sync Group",
     "Apply to every object in the sync group, including unselected members"},
    {int(SyncScope::ActiveOnly),
     "ACTIVE_ONLY",
     0,
     "Active Object Only",
     "Apply only to the active object and remove it from the sync group"},
    {0, nullptr, 0, nullptr, nullptr},
};

static void sync_scope_rna_def(wmOperatorType *ot)
{
  PropertyRNA *prop = RNA_def_enum(ot->srna,
                                   "sync_scope",
                                   sync_scope_items,
                                   int(SyncScope::AllSyncGroup),
                                   "Sync Scope",
                                   "Which sync-group members this operator affects");
  RNA_def_property_flag(prop, PROP_HIDDEN);
}

static SyncScope sync_scope_get(const wmOperator *op)
{
  PropertyRNA *prop = RNA_struct_find_property(op->ptr, "sync_scope");
  if (prop == nullptr) {
    return SyncScope::AllSyncGroup;
  }
  return SyncScope(RNA_property_enum_get(op->ptr, prop));
}

static Vector<Object *> sync_scope_targets(Main &bmain, Object &active_ob, const SyncScope scope)
{
  if (scope == SyncScope::ActiveOnly) {
    Vector<Object *> result;
    result.append(&active_ob);
    return result;
  }
  return fanout_targets(bmain, active_ob);
}

static void sync_scope_unlink_after_success(Main &bmain, Object &active_ob, const SyncScope scope)
{
  if (scope == SyncScope::ActiveOnly && active_ob.sculpt_layer_sync_group != 0) {
    sync_group_unlink_object(bmain, active_ob);
  }
}

struct SyncScopeChoiceDialog {
  wmOperator *op;
  std::string title;
  std::string message;
  bool enable_active_only = true;
};

static void sync_scope_choice_popup_cancel(bContext *C, void *user_data)
{
  SyncScopeChoiceDialog *data = static_cast<SyncScopeChoiceDialog *>(user_data);
  if (data->op != nullptr) {
    if (data->op->type->cancel != nullptr) {
      data->op->type->cancel(C, data->op);
    }
    WM_operator_free(data->op);
  }
  MEM_delete(data);
}

static void sync_scope_choice_apply(bContext *C,
                                    SyncScopeChoiceDialog *data,
                                    ui::Block *block,
                                    const SyncScope scope)
{
  RNA_enum_set(data->op->ptr, "sync_scope", int(scope));
  wmOperator *op = data->op;
  data->op = nullptr;
  MEM_delete(data);

  ui::popup_menu_retval_set(block, ui::RETURN_OK, true);
  ui::popup_block_close(C, CTX_wm_window(C), block);
  WM_operator_call_ex(C, op, true);
}

static void sync_scope_choice_all_cb(bContext *C, void *arg1, void *arg2)
{
  sync_scope_choice_apply(
      C, static_cast<SyncScopeChoiceDialog *>(arg1), static_cast<ui::Block *>(arg2), SyncScope::AllSyncGroup);
}

static void sync_scope_choice_active_cb(bContext *C, void *arg1, void *arg2)
{
  sync_scope_choice_apply(
      C, static_cast<SyncScopeChoiceDialog *>(arg1), static_cast<ui::Block *>(arg2), SyncScope::ActiveOnly);
}

static void sync_scope_choice_cancel_cb(bContext *C, void *arg1, void *arg2)
{
  SyncScopeChoiceDialog *data = static_cast<SyncScopeChoiceDialog *>(arg1);
  ui::Block *block = static_cast<ui::Block *>(arg2);
  if (data->op != nullptr) {
    if (data->op->type->cancel != nullptr) {
      data->op->type->cancel(C, data->op);
    }
    WM_operator_free(data->op);
    data->op = nullptr;
  }
  MEM_delete(data);
  ui::popup_menu_retval_set(block, ui::RETURN_CANCEL, true);
  ui::popup_block_close(C, CTX_wm_window(C), block);
}

static ui::Block *sync_scope_choice_block_create(bContext *C, ARegion *region, void *user_data)
{
  SyncScopeChoiceDialog *data = static_cast<SyncScopeChoiceDialog *>(user_data);
  const uiStyle *style = ui::style_get_dpi();
  const short icon_size = 40 * UI_SCALE_FAC;
  const int dialog_width = int(350.0f * UI_SCALE_FAC);

  ui::Block *block = ui::block_begin(C, region, __func__, ui::EmbossType::Emboss);
  ui::block_theme_style_set(block, ui::BLOCK_THEME_STYLE_POPUP);
  ui::block_flag_disable(block, ui::BLOCK_LOOP);
  ui::popup_dummy_panel_set(region, block, data->op->type->idname);
  ui::block_flag_enable(block, ui::BLOCK_KEEP_OPEN | ui::BLOCK_NUMSELECT | ui::BLOCK_POPUP);

  ui::Layout &layout = *ui::uiItemsAlertBox(
      block, style, dialog_width + icon_size, ui::AlertIcon::Warning, icon_size);

  ui::Layout &content = layout.column(false);
  content.scale_y_set(0.75f);
  ui::uiItemL_ex(&content, data->title, ICON_NONE, true, false);
  content.separator(1.0f);
  content.label(data->message, ICON_NONE);

  layout.separator(1.5f);
  ui::block_func_set(block, nullptr, nullptr, nullptr);

  ui::Layout &buttons = layout.column(false);
  buttons.scale_y_set(1.2f);
  ui::Block *buttons_block = buttons.block();

  ui::Button *all_but = ui::uiDefBut(buttons_block,
                                     ui::ButtonType::But,
                                     IFACE_("All Sync Group"),
                                     0,
                                     0,
                                     0,
                                     UI_UNIT_Y,
                                     nullptr,
                                     0,
                                     0,
                                     "");
  ui::button_func_set(all_but, sync_scope_choice_all_cb, data, block);

  ui::Button *active_but = ui::uiDefBut(buttons_block,
                                        ui::ButtonType::But,
                                        IFACE_("Active Object Only"),
                                        0,
                                        0,
                                        0,
                                        UI_UNIT_Y,
                                        nullptr,
                                        0,
                                        0,
                                        "");
  ui::button_func_set(active_but, sync_scope_choice_active_cb, data, block);
  if (!data->enable_active_only) {
    ui::button_flag_enable(active_but, ui::BUT_DISABLED);
  }

  ui::Button *cancel_but = ui::uiDefBut(buttons_block,
                                        ui::ButtonType::But,
                                        IFACE_("Cancel"),
                                        0,
                                        0,
                                        0,
                                        UI_UNIT_Y,
                                        nullptr,
                                        0,
                                        0,
                                        "");
  ui::button_func_set(cancel_but, sync_scope_choice_cancel_cb, data, block);
  ui::button_flag_enable(cancel_but, ui::BUT_ACTIVE_DEFAULT);

  ui::block_bounds_set_centered(block, 14 * UI_SCALE_FAC);
  return block;
}

static wmOperatorStatus sync_scope_choice_dialog(bContext *C,
                                                 wmOperator *op,
                                                 const char *title,
                                                 const char *message,
                                                 const bool enable_active_only = true)
{
  SyncScopeChoiceDialog *data = MEM_new<SyncScopeChoiceDialog>(__func__);
  data->op = op;
  data->title = title;
  data->message = message;
  data->enable_active_only = enable_active_only;
  ui::popup_block_ex(
      C, sync_scope_choice_block_create, nullptr, sync_scope_choice_popup_cancel, data, op);
  return OPERATOR_RUNNING_MODAL;
}

/* Which surface the stroke that is starting will actually land on, said once rather than on every
 * dab. #WM_global_reportf rather than #BKE_report because a stroke start has no #bContext and no
 * #wmOperator in scope; the status bar and the info log are reached all the same.
 *
 * Both halves exist because REC now outlives sculpt mode (see #SCULPT_LAYER_REC_ARMED). Before that,
 * a user who was recording had pressed the button moments earlier and in the same mode; now they can
 * re-enter a mode that records without having touched anything, so the state has to announce itself.
 */
static void rec_report_recording(SculptSession &ss, const SculptLayer &layer)
{
  /* Keyed on the layer rather than on a bare "already reported" bool, so that changing the active
   * layer mid-session reports the new target — the one change of target the user can make without
   * touching REC at all. */
  if (ss.layers.rec_notified_uid == layer.base.uid) {
    return;
  }
  ss.layers.rec_notified_uid = layer.base.uid;
  /* Cleared here rather than where the warning is given: a cause the user fixes, and then
   * re-introduces, is worth reporting a second time. */
  ss.layers.rec_notified_blocked = false;
  WM_global_reportf(RPT_INFO, "Sculpting into layer \"%s\"", layer.base.name);
}

static void rec_report_blocked(SculptSession &ss, const char *reason)
{
  if (ss.layers.rec_notified_blocked) {
    return;
  }
  ss.layers.rec_notified_blocked = true;
  WM_global_reportf(RPT_INFO, "REC is on, but the stroke edits the base shape: %s", reason);
}

void stroke_record_begin(const Depsgraph &depsgraph, Object &object)
{
  SculptSession *ss = session_of(object);
  if (!ss) {
    return;
  }
  ss->layers.recording = false;

  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(object);
  if (!pbvh || pbvh->type() == bke::pbvh::Type::BMesh) {
    return;
  }
  const bool grids = pbvh->type() == bke::pbvh::Type::Grids;

  /* Ensure the runtime mesh base is captured for this session (needed for both the REC recording
   * and the non-REC base update on the mesh path). */
  if (!ss->layers.state_valid) {
    session_state_ensure(object);
  }

  /* Resynced here as well as at every state change, because a stroke is the one point where the
   * exemption being wrong is destructive rather than cosmetic: the delta this stroke stores is
   * measured against the surface the composite is showing right now.
   *
   * After the base capture above and never before it. #session_state_ensure inverts the composite to
   * recover the base, so a flip in between would have the base absorb the difference between the
   * masked and the unmasked contribution — permanent, and not something a later commit can undo (see
   * #bke::sculpt_layers::rec_exempt_set). Nothing between the two reads the exemption: the
   * statements skipped over are a #bke::object::pbvh_get, a null / BMesh test and one bool.
   *
   * Reordering alone would only downgrade the failure to a surface one composite stale, so the
   * repair is completed here: when the bit actually moved, the positions were composed under the old
   * answer and are recomposed now, against the base just settled. #commit_layers_change re-runs the
   * refresh, which is idempotent and answers false the second time, so this cannot recurse. Only
   * reachable when an undo restore moved the bit, so the ordinary stroke start pays one flag test
   * per layer and nothing else. */
  if (rec_exemption_refresh(object)) {
    commit_layers_change(depsgraph, object);
  }

  Mesh &mesh = mesh_of(object);
  SculptLayer *layer = bke::sculpt_layers::active_get(mesh);
  /* #effective is 0 for a layer a disabled folder hides, whatever REC pins. A dab recorded there
   * would still write `layer->data` and move the positions while the layer contributes nothing,
   * breaking `positions == base + sum(data * effective)`, and the stroke would land a second time
   * when the folder is re-enabled. #layer_toggle_rec_exec refuses to arm REC on such a layer, but
   * the folder can be disabled, or the active layer changed, after arming (see #layer_move_to,
   * #layer_select). Treated like Solo Base: there is nothing to record into. */
  const bool rec_blocked = layer && (layer->base.flag & SCULPT_LAYER_GROUP_HIDDEN);
  if (!ss->layers.rec_active || rec_blocked || bke::sculpt_layers::solo_active(mesh)) {
    /* REC off, blocked, or Solo Base is isolating the base for direct sculpting (nothing to
     * record into while layers are hidden): the stroke edits the base. Compute the base view so
     * brush inputs (falloff, area normal, smoothing / plane targets) can be evaluated against the
     * un-layered base; see the Base view section above. */
    base_view_ensure(depsgraph, object);
    /* Only when REC is armed: an ordinary non-REC stroke editing the base is the plain case and has
     * nothing to announce. Reported after the base view is built, so a report can never change what
     * the stroke does. */
    if (ss->layers.rec_active) {
      rec_report_blocked(*ss,
                         rec_blocked ? "the layer's group is disabled" :
                                       "Solo Base is isolating the base shape");
    }
    return;
  }

  /* Note: an active shape key sets #deform_modifiers_active too (see #sculpt_modifiers_active), so a
   * plain "deform active -> skip" would also disable the shape-key case we do support. Recording is
   * routed into the layer as an object-space offset composed on top of every deform at evaluation
   * (see #PositionDeformData::deform, which captures the object-space displacement before the
   * crazyspace correction). That works whenever the shape key is what drives the deform. Only a real
   * deforming modifier *without* a shape key is left unsupported for now: there the composed surface
   * is not re-derived from base + layers at evaluation, so the recorded delta would have nowhere to
   * be reproduced. */
  if (ss->deform_modifiers_active && !ss->shapekey_active) {
    rec_report_blocked(*ss, "a deforming modifier is active");
    return;
  }

  if (!layer || layer->domain != domain_for(object) || (layer->base.flag & SCULPT_LAYER_LOCKED)) {
    rec_report_blocked(*ss,
                       layer && (layer->base.flag & SCULPT_LAYER_LOCKED) ?
                           "the layer is locked" :
                           "no sculpt layer can receive this stroke");
    return;
  }
  if (grids) {
    /* Grid layers are written at stroke end via the reshape machinery
     * (#multiresModifier_reshapeFromCCG_into_sculpt_layer); make sure the stored data still
     * matches the current top level and consume any pending base edits first — otherwise the
     * stroke-end delta (surface minus saved base) would absorb the un-flushed base stroke into
     * the layer and drop it from the base. */
    flush_pending_multires_base(object);
    validate_grid_layers(object);
  }
  else {
    /* REC enable already set enabled + influence 1.0 (see #layer_toggle_rec_exec). Just ensure
     * the data buffer exists so deform can accumulate per dab. */
    bke::sculpt_layers::data_ensure(*layer, element_count(object));
  }

  /* No base view while recording: a stroke that writes into a layer is evaluated against the
   * composed surface, exactly like a stroke without layers.
   *
   * This used to populate the base view with the OTHER enabled layers, so the recorded layer would
   * not absorb their shape. That independence has a price the user pays on every stroke: the brush
   * footprint is then measured on a surface the user cannot see, and once a lower layer carries a
   * large form (not just fine detail) the affected region no longer matches the cursor at all — a
   * circle under the cursor lands as a big distorted patch, because the vertices within the radius
   * in base space are spread far apart on the composed surface.
   *
   * The un-layered base is required only for strokes that write INTO THE BASE: there a composed
   * falloff distance carries the layer pattern into the basis and accumulates across strokes (see
   * the Base view section above). A stroke recorded into a layer above writes nothing to the base,
   * so nothing can be baked in; the only effect of the base view there was the independence above.
   * We choose WYSIWYG: what is drawn is what was seen. The consequence is the usual layer-stack
   * one — smoothing over a lower layer records a delta that cancels its detail, and hiding that
   * layer later reveals the ghost of what was smoothed away. */
  ss->layers.base_view = {};
  ss->layers.base_view_dc = float3(0.0f);
  ss->layers.base_view_node_offset_bounds = {};

  ss->layers.recording = true;
  rec_report_recording(*ss, *layer);

  if (!grids) {
    debug_validate_mesh_invariant(object, "stroke_begin");
  }
  SLP_PERF("[DEBUG-perf] stroke_record_begin: grids=%d, rec_active=true, RECORDING\n", grids);
}

void stroke_record_end(const Depsgraph &depsgraph, Object &object)
{
  SculptSession *ss = session_of(object);
  if (!ss) {
    return;
  }
  /* Non-const: the grids branch below tags the layer-preview overlay on the draw cache at stroke
   * end, which is a non-const #bke::pbvh::Tree method. Every other use here only reads. */
  bke::pbvh::Tree *pbvh = bke::object::pbvh_get(object);
  Mesh &mesh = mesh_of(object);

  const auto t_rec_end_start = slp_perf_now();

  /* The base view is only valid for the duration of one stroke (the stroke changes the base, and
   * for multires the tangent frames with it). */
  ss->layers.base_view = {};
  ss->layers.base_view_dc = float3(0.0f);
  ss->layers.base_view_node_offset_bounds = {};

  if (!ss->layers.recording) {
    /* REC off: the stroke edits the base. Mesh folds the stroke into the runtime base using the
     * per-node undo data (O(brushed)). Multires needs nothing here: the brush marked the CCG
     * dirty, and the lazy (layer-aware) flush reshapes the stroke into the base MDisps while
     * keeping the layer contributions out (see #multiresModifier_reshapeFromCCG). */
    if (pbvh && pbvh->type() == bke::pbvh::Type::Mesh &&
        ss->layers.mesh_base.size() == int64_t(mesh.vert_positions().size())) {
      const Span<float3> positions = mesh.vert_positions();
      MutableSpan<float3> base = ss->layers.mesh_base;
      undo::foreach_recorded_position_mesh(
          object, [&](const Span<int> verts, const Span<float3> orig) {
            for (const int64_t j : verts.index_range()) {
              const int v = verts[j];
              if (v >= 0 && v < int(base.size()) && v < int(positions.size())) {
                base[v] += positions[v] - orig[j];
              }
            }
          });
      debug_validate_mesh_invariant(object, "stroke_end_base");
    }
    SLP_PERF("[DEBUG-perf] stroke_record_end: REC=off mesh base-fold in %lld us\n",
             slp_perf_us(t_rec_end_start, slp_perf_now()));
    return;
  }

  /* REC on path. */
  ss->layers.recording = false;
  SculptLayer *layer = bke::sculpt_layers::active_get(mesh);

  if (layer && pbvh && layer->domain == domain_for(object)) {
    if (pbvh->type() == bke::pbvh::Type::Grids && ss->subdiv_ccg) {
      /* Multires: convert the sculpted (composed) CCG surface back to tangent displacement at
       * the top level via the reshape machinery and accumulate the difference against the
       * pre-stroke composed surface into the layer. The base MDisps are left unchanged (I1). */
      SubdivCCG &subdiv_ccg = *ss->subdiv_ccg;
      const int totlvl = ss->multires_modifier ? ss->multires_modifier->totlvl : subdiv_ccg.level;

      /* Collect the grids touched by this stroke from the in-progress undo step (must run before
       * #undo::push_end). */
      BitVector<> touched_bits(subdiv_ccg.grids_num, false);
      undo::foreach_recorded_grids(object, [&](const Span<int> grids) {
        for (const int grid : grids) {
          if (grid >= 0 && grid < touched_bits.size()) {
            touched_bits[grid].set();
          }
        }
      });
      Vector<int> touched;
      for (const int64_t i : touched_bits.index_range()) {
        if (touched_bits[i]) {
          touched.append(int(i));
        }
      }
      /* Captured before the #std::move below empties #touched (the probe printed 0 otherwise). */
      const int touched_num = int(touched.size());

      if (!touched.is_empty()) {
        Vector<float3> undo_delta;
        if (multiresModifier_reshapeFromCCG_into_sculpt_layer(
                totlvl, &mesh, &subdiv_ccg, touched, *layer, undo_delta))
        {
          undo::store_active_sculpt_layer_grids(object, std::move(touched), std::move(undo_delta));

          /* The reshape wrote the stroke into the layer only now, at stroke end. The mesh branch
           * needs no such tag: its #data grows per dab, so the per-dab #tag_positions_changed
           * already refreshes the preview live. On grids the coefficients cannot exist until the
           * whole composed surface is reshaped back, so the layer-preview overlay must be tagged
           * here or it stays neutral until the next full retag (e.g. a layer switch). Only the
           * preview channel is tagged — the brush does not touch the layer mask. Tagged per touched
           * node, not the whole tree, because this system deliberately avoids per-stroke whole-mesh
           * work; and this is a draw-cache dirty flag, not a depsgraph tag, so it does not trigger
           * the PBVH rebuild the block below avoids. */
          const Span<bke::pbvh::GridsNode> grid_nodes = pbvh->nodes<bke::pbvh::GridsNode>();
          IndexMaskMemory preview_memory;
          const IndexMask preview_nodes = IndexMask::from_predicate(
              IndexMask(grid_nodes.index_range()), preview_memory, [&](const int64_t i) {
                for (const int grid : grid_nodes[i].grids()) {
                  if (touched_bits[grid]) {
                    return true;
                  }
                }
                return false;
              });
          pbvh->tag_layer_previews_changed(preview_nodes);
        }
      }
      /* The stroke is now fully captured in the layer and the base MDisps are unchanged. Clear
       * the dirty flag so a later flush does not bake the composed surface into the base, and
       * skip any depsgraph tag: the live CCG already equals base + layers, so a re-evaluation
       * would only rebuild the PBVH (the per-stroke freeze this system explicitly avoids).
       *
       * This assumes `subdiv_ccg` (from `ss->subdiv_ccg`) is the same #SubdivCCG that a later
       * depsgraph-driven flush reads off `mesh_eval`, which only holds while
       * #BKE_mesh_wrapper_ensure_subdivision resolves to a no-op (see the load-bearing-identity
       * comment on that assumption in object.cc, object_update_from_subsurf_ccg). If the two ever
       * diverge, this clear is silently lost and the next flush bakes the composed surface into
       * the base MDisps. */
      subdiv_ccg.dirty.coords = false;
      SLP_PERF("[DEBUG-perf] stroke_record_end: REC=on multires reshape %d touched grids in %lld us\n",
               touched_num,
               slp_perf_us(t_rec_end_start, slp_perf_now()));
    }
    else if (pbvh->type() == bke::pbvh::Type::Mesh) {
      /* Mesh: #data is accumulated per dab by #PositionDeformData::deform. Record the per-vertex
       * undo deltas for the touched vertices (work proportional to the brushed area). Must run
       * before #undo::push_end. */
      MutableSpan<float3> data = bke::sculpt_layers::data_ensure(*layer, element_count(object));
      /* Under a shape key the basis positions (#mesh.vert_positions) are untouched by the stroke —
       * the brush edited the evaluated/display positions. Diff the live post-stroke evaluated
       * positions against the pre-stroke ones recorded by undo to recover the per-vertex layer delta
       * (which equals the per-dab accumulation done by #PositionDeformData::record_layer_offsets).
       * #ss->deform_cos cannot be used here: it is a stable pre-stroke snapshot, not updated mid
       * stroke (see #BKE_sculpt_update_object). */
      const Span<float3> positions = ss->shapekey_active ?
                                         bke::pbvh::vert_positions_eval(depsgraph, object) :
                                         mesh.vert_positions();

      struct NodeInfo {
        Span<int> vert_indices;
        Span<float3> orig_positions;
      };
      Vector<NodeInfo> active_nodes;
      const auto collect = [&](const Span<int> verts, const Span<float3> orig) {
        active_nodes.append({verts, orig});
      };
      /* Under a shape key #foreach_recorded_position_mesh would hand back the base-space
       * #orig_position (the active key block); the stroke lives in evaluated space, so gather the
       * pre-stroke evaluated positions instead to match #positions (#deform_cos) above. */
      if (ss->shapekey_active) {
        undo::foreach_recorded_eval_position_mesh(object, collect);
      }
      else {
        undo::foreach_recorded_position_mesh(object, collect);
      }

      Array<int> node_offsets(active_nodes.size() + 1);
      {
        int acc = 0;
        for (const int i : active_nodes.index_range()) {
          node_offsets[i] = acc;
          acc += int(active_nodes[i].vert_indices.size());
        }
        node_offsets[active_nodes.size()] = acc;
      }
      const int max_total = node_offsets[active_nodes.size()];

      Vector<int> changed_verts;
      Vector<float3> deltas;
      changed_verts.resize(max_total);
      deltas.resize(max_total);
      Array<int> actual_counts(active_nodes.size(), 0);

      threading::parallel_for(active_nodes.index_range(), 1, [&](const IndexRange range) {
        for (const int i : range) {
          const NodeInfo &ni = active_nodes[i];
          int count = 0;
          const int base_off = node_offsets[i];
          for (const int64_t j : ni.vert_indices.index_range()) {
            const int v = ni.vert_indices[j];
            if (v < 0 || v >= int(data.size()) || v >= int(positions.size())) {
              continue;
            }
            const float3 delta = positions[v] - ni.orig_positions[j];
            if (delta == float3(0.0f)) {
              continue;
            }
            changed_verts[base_off + count] = v;
            deltas[base_off + count] = delta;
            count++;
          }
          actual_counts[i] = count;
        }
      });

      Vector<int> seg_start;
      Vector<int> seg_count;
      int out = 0;
      for (const int i : active_nodes.index_range()) {
        const int count = actual_counts[i];
        if (count == 0) {
          continue;
        }
        seg_start.append(node_offsets[i]);
        seg_count.append(count);
        out += count;
      }

      if (out > 0) {
        undo::store_active_sculpt_layer_verts(object,
                                              std::move(changed_verts),
                                              std::move(deltas),
                                              std::move(seg_start),
                                              std::move(seg_count));
      }
      debug_validate_mesh_invariant(object, "stroke_end_rec");
      SLP_PERF("[DEBUG-perf] stroke_record_end: REC=on mesh recorded %d changed verts in %lld us\n",
               out,
               slp_perf_us(t_rec_end_start, slp_perf_now()));
    }
  }

  /* Skip the DEG re-eval that would free and rebuild the whole PBVH from scratch every stroke — on
   * dense meshes that rebuild is the dominant per-stroke freeze (hundreds of ms on multi-million
   * vertex meshes), and the stroke topology never changes so the rebuilt tree is identical.
   *
   * It is safe to skip whenever the live display already reflects the finished stroke:
   * - Plain mesh: the live positions already equal base + layers (the brush wrote them directly).
   * - Multires: the layer capture above leaves the live CCG untouched.
   * - Shape key: the brush updated the PBVH's own coordinates per dab (see #PositionDeformData) and
   *   the stroke was fully recorded into the layer as an object-space offset, so the persistent data
   *   (basis key + key blocks + layer data) reproduces the same surface on the next natural
   *   evaluation. File-modified marking is covered by the undo push that follows this call.
   *
   * Only a real deforming modifier *without* a shape key keeps the re-eval: that path does not
   * re-derive the composed surface from base + layers at evaluation, so the live positions are not
   * reproducible and must be committed through a full re-evaluation. */
  const bool needs_reeval = pbvh && pbvh->type() == bke::pbvh::Type::Mesh &&
                            ss->deform_modifiers_active && !ss->shapekey_active;
  if (needs_reeval) {
    DEG_id_tag_update(&mesh.id, 0);
  }
}

void stroke_record_cancel(const Depsgraph &depsgraph, Object &object)
{
  SculptSession *ss = session_of(object);
  if (!ss) {
    return;
  }
  const bool was_recording = ss->layers.recording;
  ss->layers.base_view = {};
  ss->layers.base_view_dc = float3(0.0f);
  ss->layers.base_view_node_offset_bounds = {};

  if (!was_recording) {
    /* REC off: the regular sculpt undo restores positions; the base was not updated yet. */
    ss->layers.recording = false;
    return;
  }

  /* REC on, mesh: subtract the per-dab accumulation from the layer data. Call
   * #cancel_recorded_offsets while recording is still true (it checks the flag).
   *
   * REC on, grids: nothing to do — the layer data is only written at stroke end (which does not
   * run for a cancelled stroke) and the regular undo restore brings the CCG positions back. */
  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(object);
  if (pbvh && pbvh->type() == bke::pbvh::Type::Mesh) {
    cancel_recorded_offsets(depsgraph, object);
  }
  ss->layers.recording = false;
}

MutableSpan<float3> active_record_data(Object &object)
{
  const SculptSession *ss = session_of(object);
  if (!ss || !ss->layers.recording) {
    return {};
  }
  /* Only the mesh (vertex) path accumulates per dab. Multires captures the stroke at stroke end
   * via the reshape machinery, and #PositionDeformData is not used by the grids brushes anyway. */
  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(object);
  if (!pbvh || pbvh->type() != bke::pbvh::Type::Mesh) {
    return {};
  }
  SculptLayer *layer = bke::sculpt_layers::active_get(mesh_of(object));
  if (!layer || !layer->data || layer->domain != domain_for(object)) {
    return {};
  }
  return bke::sculpt_layers::data_get(*layer);
}

void cancel_recorded_offsets(const Depsgraph &depsgraph, Object &object)
{
  SculptSession *ss = session_of(object);
  if (!ss || !ss->layers.recording) {
    return;
  }
  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(object);
  if (!pbvh || pbvh->type() != bke::pbvh::Type::Mesh) {
    return;
  }
  SculptLayer *layer = bke::sculpt_layers::active_get(mesh_of(object));
  if (!layer || !layer->data || layer->domain != domain_for(object)) {
    return;
  }
  /* The stroke was accumulated into the layer per dab (#PositionDeformData::deform). Undo that by
   * subtracting the net offset, recomputed from the still post-stroke positions and the pre-stroke
   * positions kept in the in-progress per-node undo data. This must run before the undo system
   * restores the positions and before #push_end clears that per-node data.
   *
   * Under a shape key the basis (#mesh.vert_positions) is untouched by the stroke — the brush edited
   * the evaluated/display positions — so diff those against the pre-stroke evaluated positions
   * instead, mirroring the REC-on capture in #stroke_record_end. Using the basis here would compute
   * a zero offset and leave the cancelled stroke baked into the layer. */
  Mesh &mesh = mesh_of(object);
  MutableSpan<float3> data = bke::sculpt_layers::data_get(*layer);
  const Span<float3> positions = ss->shapekey_active ?
                                     bke::pbvh::vert_positions_eval(depsgraph, object) :
                                     mesh.vert_positions();
  const auto revert = [&](const Span<int> verts, const Span<float3> orig) {
    for (const int64_t j : verts.index_range()) {
      const int v = verts[j];
      if (v >= 0 && v < data.size() && v < positions.size()) {
        data[v] -= positions[v] - orig[j];
      }
    }
  };
  if (ss->shapekey_active) {
    undo::foreach_recorded_eval_position_mesh(object, revert);
  }
  else {
    undo::foreach_recorded_position_mesh(object, revert);
  }
}

/* -------------------------------------------------------------------------------------------------
 * Operators
 */

static bool layers_poll(bContext *C)
{
  const Object *ob = CTX_data_active_object(C);
  return ob && (ob->mode & OB_MODE_SCULPT);
}

/* The layer operators the mesh properties panel must also offer outside sculpt mode: selecting the
 * active layer, and toggling a folder's visibility. Both act on state the panel shows in object mode
 * and neither needs anything sculpt mode provides — one writes a uid, the other flips a flag and
 * re-bakes the cascade. Each carries a separate object-mode exec; see #layer_select_object_mode_exec
 * for why the sculpt-mode path cannot simply be run with a null session.
 *
 * Everything else stays sculpt-mode only (#layers_poll): it changes the tree, or moves the surface
 * through the runtime base, and both need the session and the PBVH that only sculpt mode has.
 *
 * Edit mode is deliberately not included. The tree is drawn there too, but the live positions cannot
 * be updated (see the influence slider's note in `properties_data_mesh.py`), and nothing has asked
 * for it. */
static bool layers_object_mode_poll(bContext *C)
{
  const Object *ob = CTX_data_active_object(C);
  if (ob == nullptr || ob->type != OB_MESH) {
    return false;
  }
  Main *bmain = CTX_data_main(C);
  if (!BKE_id_is_editable(bmain, &ob->id) || ID_IS_LINKED(&ob->id)) {
    return false;
  }
  if (ob->data != nullptr) {
    ID *mesh_id = static_cast<ID *>(ob->data);
    if (!BKE_id_is_editable(bmain, mesh_id) || ID_IS_LINKED(mesh_id) ||
        ID_IS_OVERRIDE_LIBRARY(mesh_id))
    {
      return false;
    }
  }
  return (ob->mode & OB_MODE_SCULPT) || ob->mode == OB_MODE_OBJECT;
}

/* UI-only refresh for operations that do not change the combined surface (add / move / select).
 * Operations that do change positions call #commit_layers_change instead, which picks the right
 * update path per PBVH type without an unconditional geometry re-evaluation. */
static void layers_ui_notify(bContext *C, Object &object)
{
  Mesh &mesh = mesh_of(object);
  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, &object);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, &mesh.id);
}

/* Recompute the folder visibility cascade and record only what it actually changed into the current
 * sculpt undo step. Call after the tree mutation is complete, between #undo::push_begin and
 * #undo::push_end.
 *
 * The snapshot is taken here rather than by the caller because the diff is what makes the undo step
 * cheap and exact: a folder toggle high in the tree may leave most layers untouched, and swapping a
 * flag that did not change would be a no-op that still has to be stored. */
static void group_cascade_resync_with_undo(Object &object, Mesh &mesh)
{
  Vector<int> uids;
  Vector<int> flags_before;
  {
    const Span<SculptLayer *> before = bke::sculpt_layers::layers(mesh);
    uids.reserve(before.size());
    flags_before.reserve(before.size());
    for (const SculptLayer *layer : before) {
      uids.append(layer->base.uid);
      flags_before.append(layer->base.flag);
    }
  }

  bke::sculpt_layers::resync_group_state(mesh);

  /* The span is re-read rather than held across the resync. #resync_group_state adds, removes and
   * reorders nothing — it only rewrites per-layer derived state — which is what makes the second walk
   * line up with the first, index for index. Its float #group_influence_cached write is deliberately
   * outside the diff below, which compares only the int #flag; the float is non-undoable derived
   * state (folder influence is a live slider), so it is never part of a flags_batch payload. */
  Vector<int> changed_uids;
  Vector<int> changed_flags;
  int i = 0;
  for (const SculptLayer *layer : bke::sculpt_layers::layers(mesh)) {
    if (layer->base.flag != flags_before[i]) {
      changed_uids.append(uids[i]);
      changed_flags.append(flags_before[i]);
    }
    i++;
  }
  if (!changed_uids.is_empty()) {
    undo::push_sculpt_layer_flags_batch(object, std::move(changed_uids), std::move(changed_flags));
  }
}

struct OpContext {
  Object *object;
  Depsgraph *depsgraph;
  Mesh *mesh;
  bool grids;
};

static bool op_context_get(bContext *C, wmOperator *op, OpContext &r_ctx)
{
  Object &object = *CTX_data_active_object(C);
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  BKE_sculpt_update_object_for_edit(depsgraph, &object, false);
  if (!is_supported(object)) {
    BKE_report(op->reports, RPT_ERROR, "Sculpt layers are not available for this object");
    return false;
  }
  r_ctx.object = &object;
  r_ctx.depsgraph = depsgraph;
  r_ctx.mesh = &mesh_of(object);
  r_ctx.grids = bke::object::pbvh_get(object)->type() == bke::pbvh::Type::Grids;
  if (r_ctx.grids) {
    /* Every layer operator may change the layer set or influences; consume any pending base
     * sculpt edits first, while the live CCG still matches the stored layers. */
    flush_pending_multires_base(object);
  }
  else {
    debug_validate_mesh_invariant(object, "op_enter");
  }
  return true;
}

static wmOperatorStatus layer_add_exec(bContext *C, wmOperator *op)
{
  OpContext ctx;
  if (!op_context_get(C, op, ctx)) {
    return OPERATOR_CANCELLED;
  }
  /* Commits below without closing an open session first, so the multires rebuild would discard it.
   * See #mask_edit_refuse_ccg_rebuild; the operators that instead *close* the session (removal,
   * validate, select, the group removals, the bakes) call #mask_edit_end and are not refused. */
  if (mask_edit_refuse_ccg_rebuild(op, *ctx.object)) {
    return OPERATOR_CANCELLED;
  }
  Main *bmain = CTX_data_main(C);

  /* Gather the eligible fan-out set BEFORE opening any undo bracket: #push_begin_multi_object and
   * #finish_multi_object both dereference every listed object's #SculptSession unconditionally
   * (session-state save, lazy #bke::object::pbvh_ensure) -- a crash for a sync-group member that
   * was never taken into sculpt mode, which is an entirely ordinary state, since
   * #sync_group_members applies no mode/session filter and #SCULPT_OT_layer_sync_group_create
   * groups objects from OBJECT mode. Mirrors #edit_op_exec's own gather-then-bracket structure
   * (`sculpt_face_set.cc`): filter into a local list first, brace only the survivors. The active
   * object was already validated by #op_context_get above and is included unconditionally; each
   * other member is brought up to date and gated exactly as #op_context_get gates the active
   * object. */
  Vector<Object *> eligible;
  Set<Object *> tree_only_members;
  eligible.append(ctx.object);
  for (Object *member : fanout_targets(*bmain, *ctx.object)) {
    if (member == ctx.object) {
      continue;
    }
    bool tree_only = false;
    if (!sync_group_tree_create_member_prepare(ctx.depsgraph,
                                               *ctx.object,
                                               member,
                                               op,
                                               "Add Sculpt Layer",
                                               tree_only))
    {
      continue;
    }
    if (tree_only) {
      tree_only_members.add(member);
      eligible.append(member);
      continue;
    }
    if (bke::object::pbvh_get(*member)->type() == bke::pbvh::Type::Grids) {
      flush_pending_multires_base(*member);
    }
    if (mask_edit_refuse_ccg_rebuild(op, *member)) {
      continue;
    }
    eligible.append(member);
  }

  session_state_ensure(*ctx.object);
  undo::push_begin_multi_object(*CTX_data_scene(C), op, eligible.as_span());
  /* Active object: unchanged from the single-object path. */
  SculptLayer *layer = nullptr;
  if (ctx.grids) {
    /* Grid layers store data canonically at the max subdivision level. Pass totelem = 0 so no
     * buffer is pre-allocated (data_ensure will size it at first use), and pass the current totlvl
     * so the level field is set correctly from the start. */
    const SculptSession *ss = ctx.object->runtime->sculpt_session;
    const short totlvl = ss->multires_modifier ? ss->multires_modifier->totlvl :
                                                 short(ss->subdiv_ccg ? ss->subdiv_ccg->level : 0);
    layer = bke::sculpt_layers::add(*ctx.mesh, DATA_("Layer"), SCULPT_LAYER_DOMAIN_GRID, 0, totlvl);
  }
  else {
    layer = bke::sculpt_layers::add(
        *ctx.mesh, DATA_("Layer"), domain_for(*ctx.object), element_count(*ctx.object));
  }
  undo::push_sculpt_layer_list_change(*ctx.object, {}, Vector<int>({layer->base.uid}), false);
  /* Auto-enable REC so sculpting immediately records into the freshly added layer; the layer
   * itself already starts enabled at influence 1.0 (see #bke::sculpt_layers::add). */
  if (SculptSession *ss = session_of(*ctx.object)) {
    ss->layers.rec_active = true;
  }
  /* The new layer starts with zero displacement, so *it* adds nothing to the surface — but this
   * operator also moves the active layer, and the REC exemption has to follow it. Left behind,
   * #SCULPT_LAYER_REC_EXEMPT would keep a previously-active masked layer composing unmasked while it
   * is no longer the recording target, since #node_mask_for_composite short-circuits on that bit and
   * #bke::sculpt_layers::rec_exempt_set is the only thing that moves it. That is a real surface
   * change, so it is recomposed here, and inside this step rather than after #push_end so the
   * recompose rides the same undo step as the add (mirrors #layer_select_exec). #session_state_ensure
   * already ran above, against the pre-change exemption, so no dent is folded into the base. */
  if (rec_exemption_refresh(*ctx.object)) {
    commit_layers_change(*ctx.depsgraph, *ctx.object);
  }

  /* Fan out: mint ONE shared #SculptLayerTreeNode::sync_uid for the active object's new layer and
   * every sync-group member's new layer, so #node_find_by_sync_uid can resolve "the same" layer
   * across the group. `eligible[0]` is always `ctx.object` (appended first, above), hence the
   * `member == ctx.object` skip below rather than a `drop_front(1)`, which would silently
   * misbehave if that ordering guarantee ever changed. */
  if (ctx.object->sculpt_layer_sync_group != 0) {
    const int new_sync_uid = bke::sculpt_layers::layer_sync_uid_unique(*bmain);
    layer->base.sync_uid = new_sync_uid;

    for (Object *member : eligible) {
      if (member == ctx.object) {
        continue;
      }
      if (tree_only_members.contains(member)) {
        Mesh &member_mesh = mesh_of(*member);
        SculptLayer *member_layer = bke::sculpt_layers::add(
            member_mesh, DATA_("Layer"), domain_for(*member), element_count(*member));
        member_layer->base.sync_uid = new_sync_uid;
        undo::push_sculpt_layer_list_change(
            *member, {}, Vector<int>({member_layer->base.uid}), false);
        DEG_id_tag_update(&member_mesh.id, ID_RECALC_GEOMETRY);
        continue;
      }
      /* Eligibility (#is_supported, #mask_edit_refuse_ccg_rebuild) and the multires pending-base
       * flush already ran for every member of `eligible` in the gather pass above, before the undo
       * bracket opened -- nothing here can fail or need skipping. */
      const bool member_grids = bke::object::pbvh_get(*member)->type() == bke::pbvh::Type::Grids;
      /* Mirrors #session_state_ensure's call above for the active object: it has to run before this
       * member's layer set changes and before its REC exemption moves below, or the mesh_base
       * captured here would fold an already-moved exemption's contribution permanently into the
       * base (see #commit_layers_change's comment on capture ordering). */
      session_state_ensure(*member);

      Mesh &member_mesh = mesh_of(*member);
      SculptLayer *member_layer = nullptr;
      if (member_grids) {
        const SculptSession *member_ss = member->runtime->sculpt_session;
        const short totlvl = member_ss->multires_modifier ?
                                 member_ss->multires_modifier->totlvl :
                                 short(member_ss->subdiv_ccg ? member_ss->subdiv_ccg->level : 0);
        member_layer = bke::sculpt_layers::add(
            member_mesh, DATA_("Layer"), SCULPT_LAYER_DOMAIN_GRID, 0, totlvl);
      }
      else {
        member_layer = bke::sculpt_layers::add(
            member_mesh, DATA_("Layer"), domain_for(*member), element_count(*member));
      }
      member_layer->base.sync_uid = new_sync_uid;
      undo::push_sculpt_layer_list_change(
          *member, {}, Vector<int>({member_layer->base.uid}), false);
      if (SculptSession *member_ss_mut = session_of(*member)) {
        member_ss_mut->layers.rec_active = true;
      }
      if (rec_exemption_refresh(*member)) {
        commit_layers_change(*ctx.depsgraph, *member);
      }
    }
  }

  Vector<Object *> eligible_finish;
  eligible_finish.reserve(eligible.size());
  for (Object *ob : eligible) {
    if (!tree_only_members.contains(ob)) {
      eligible_finish.append(ob);
    }
  }
  undo::finish_multi_object(C, eligible_finish.as_span(), UpdateType::Position);
  for (Object *member : eligible) {
    layers_ui_notify(C, *member);
  }
  return OPERATOR_FINISHED;
}

void SCULPT_OT_layer_add(wmOperatorType *ot)
{
  ot->name = "Add Sculpt Layer";
  ot->idname = "SCULPT_OT_layer_add";
  ot->description = "Add a new sculpt layer and make it active";
  ot->exec = layer_add_exec;
  ot->poll = layers_poll;
  /* No #OPTYPE_UNDO: the operator pushes its own sculpt undo step; a global push would insert a
   * memfile step that does not compose with the stroke SCULPT steps (see #SCULPT_OT_layer_solo_base). */
  ot->flag = OPTYPE_REGISTER;
}

/* One sync-group member's counterpart set for a removal: the subset of the active object's
 * `targets` that this member actually has a layer synced to. Resolved once, in the gather pass
 * below, before the active object's own `targets` are freed by its own removal loop further
 * down — #bke::sculpt_layers::remove deletes the node outright, so re-deriving these from
 * `target->base.sync_uid` *after* that point (the idiom every other find-shaped fan-out in this
 * file uses in its own second loop, since their targets are never removed) would dereference
 * freed memory. Kept instead as pointers resolved into this member's own mesh, which nothing
 * touches until this member's own turn in the fan-out loop below. */
struct RemoveFanoutMember {
  Object *object;
  Vector<SculptLayer *> targets;
};

static wmOperatorStatus layer_remove_exec(bContext *C, wmOperator *op)
{
  OpContext ctx;
  if (!op_context_get(C, op, ctx)) {
    return OPERATOR_CANCELLED;
  }
  /* The tree view can hold several layers selected at once, so a removal targets the whole
   * selection plus the active layer. The active layer is included even when it sits outside the
   * selection: it is what the rest of the panel points at, so leaving it behind would contradict
   * the row the user pressed #X on. Collected in list order, which the payload capture below
   * relies on. */
  const SculptLayer *active = bke::sculpt_layers::active_get(*ctx.mesh);
  Vector<SculptLayer *> targets;
  for (SculptLayer *layer : bke::sculpt_layers::layers(*ctx.mesh)) {
    if ((layer->base.flag & SCULPT_LAYER_SELECTED) || layer == active) {
      targets.append(layer);
    }
  }
  if (targets.is_empty()) {
    return OPERATOR_CANCELLED;
  }
  Main *bmain = CTX_data_main(C);
  const SyncScope sync_scope = sync_scope_get(op);

  /* Gather-then-gate: see #layer_add_exec's own comment for why the fan-out set has to be built
   * before any undo bracket opens. The active object is included unconditionally with its full
   * `targets` list; every other sync-group member is brought up to date and gated exactly as
   * #op_context_get gates the active object, plus a find-shaped gate: of `targets`, only the
   * subset with a synced counterpart on this member is ever removed there. A member with none of
   * them synced is skipped entirely; a member missing only some of them still has the rest
   * removed, with a warning naming what was left behind.
   *
   * Unlike #layer_add_exec / #layer_clear_exec this gather does NOT call
   * #mask_edit_refuse_ccg_rebuild on a member: removal is one of the operators that *closes* an
   * open mask-edit session rather than refusing on its account (see #layer_add_exec's own comment
   * on that split, and #layer_group_delete_exec for the group-shaped equivalent), and the
   * per-member loop below closes each member's own session unconditionally, exactly as the active
   * object's own body does. */
  Vector<Object *> eligible;
  eligible.append(ctx.object);
  Vector<RemoveFanoutMember> fanout_members;
  for (Object *member : sync_scope_targets(*bmain, *ctx.object, sync_scope)) {
    if (member == ctx.object) {
      continue;
    }
    /* Mirrors #layer_add_exec's gather loop: guarded on an existing session, since a member that
     * was never taken into sculpt mode has none, and #BKE_sculpt_update_object_for_edit
     * unconditionally dereferences it. Refreshed before #is_supported below so a member that IS in
     * sculpt mode but has not built its PBVH yet is not misreported as unsupported. */
    if (member->runtime->sculpt_session != nullptr) {
      BKE_sculpt_update_object_for_edit(ctx.depsgraph, member, false);
    }
    if (!is_supported(*member)) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Remove Sculpt Layer: skipping \"%s\" (sculpt layers are not available for "
                  "this object)",
                  member->id.name + 2);
      continue;
    }
    /* #node_find_by_sync_uid resolves the SAME layer wherever it sits in the member's own tree,
     * one target at a time; a target with #sync_uid == 0 (never fanned-out-created) simply misses
     * on every member, degrading that one layer's removal to the single-object behavior it always
     * had. The kind check is folded into the single #node_as_layer call below (it returns null for
     * a group), rather than a separate two-step resolve-then-check as elsewhere in this file --
     * #sync_uid should only ever link a layer to a layer, but the tree is user-editable, so the
     * defensive intent is the same either way. */
    Mesh &member_mesh = mesh_of(*member);
    Vector<SculptLayer *> member_targets;
    Vector<const char *> missing_names;
    for (SculptLayer *target : targets) {
      SculptLayer *member_layer = bke::sculpt_layers::node_as_layer(
          bke::sculpt_layers::node_find_by_sync_uid(member_mesh, target->base.sync_uid));
      if (!member_layer) {
        missing_names.append(target->base.name);
        continue;
      }
      /* #node_find_by_sync_uid is first-match: two entries of the active object's own `targets`
       * sharing the same non-zero #sync_uid would otherwise resolve to the SAME member layer
       * twice, and the removal loop further down would double-free it (#bke::sculpt_layers::remove
       * unlinks and #MEM_delete_void's the node; a second call on the same pointer is a
       * use-after-free, not a no-op). No longer produced by duplication -- #bke::sculpt_layers::
       * duplicate now clears the copy's #base.sync_uid and #layer_duplicate_exec re-mints a fresh
       * shared one -- but still reachable from any file saved before that fix, where a
       * duplicated-then-selected layer does share its original's sync_uid. Not treated as
       * "missing": the active object itself carries the same collision without complaint, so a
       * repeat is silently folded into the one entry already collected rather than warned about. */
      if (!member_targets.contains(member_layer)) {
        member_targets.append(member_layer);
      }
    }
    if (member_targets.is_empty()) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Remove Sculpt Layer: skipping \"%s\" (no synced layers to remove)",
                  member->id.name + 2);
      continue;
    }
    if (!missing_names.is_empty()) {
      /* Not a skip: the rest of `targets` still has a synced counterpart here, and removing those
       * is worth doing even though this member cannot keep the whole set in lock-step. */
      std::string joined = missing_names[0];
      for (const char *name : missing_names.as_span().drop_front(1)) {
        joined += ", ";
        joined += name;
      }
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Remove Sculpt Layer: \"%s\" has no synced counterpart for: %s (its other "
                  "synced layers are still removed)",
                  member->id.name + 2,
                  joined.c_str());
    }
    if (bke::object::pbvh_get(*member)->type() == bke::pbvh::Type::Grids) {
      /* Mirrors #op_context_get / #layer_add_exec: consume any pending base sculpt edits first,
       * while the live CCG still matches the stored layers, before the removal below invalidates
       * that. */
      flush_pending_multires_base(*member);
    }
    eligible.append(member);
    RemoveFanoutMember fanout_member;
    fanout_member.object = member;
    fanout_member.targets = std::move(member_targets);
    fanout_members.append(std::move(fanout_member));
  }

  /* Read before the close below, which clears it. Zero when no session is open, which is the common
   * case. */
  const SculptSession *ss = session_of(*ctx.object);
  const int session_uid = ss ? ss->layers.mask_edit.node_uid : 0;
  /* A removal closes an open weight-mask editing session, exactly as a change of active layer does
   * (see #layer_select_exec). Left open across the deletion of its own node the session becomes
   * invisible — the row and its mask icon are gone — while the node's weights stay in the standard
   * mask storage with the user's own mask parked, so every subsequent brush is silently masked by a
   * layer that no longer exists. Closed *before* the payload capture below so the capture takes the
   * settled mask #mask_edit_end just compressed onto the node, rather than the pre-session value. */
  /* Before the close, which clears the session struct the parked tool idname lives on. */
  mask_edit_exit_ui(C, *ctx.object);
  mask_edit_end(*ctx.object);
  /* Derive the mesh base from the still-consistent pre-change state. */
  session_state_ensure(*ctx.object);
  undo::push_begin_multi_object(*CTX_data_scene(C), op, eligible.as_span());
  /* Active object: unchanged from the single-object path. */
  if (session_uid != 0) {
    undo::push_sculpt_layer_mask_session(*ctx.object, session_uid, false);
  }
  /* Capture every payload while the list is still intact so each records the neighbour it really
   * followed; undo re-inserts them in capture order and rebuilds the original stack. Capturing and
   * removing one at a time would make each layer record a stale neighbour instead (see
   * #SCULPT_OT_layer_merge_selected for the same trap). Capturing does not unlink, so removal is a
   * second pass. */
  Vector<undo::SculptLayerUndoPayload> removed;
  for (SculptLayer *layer : targets) {
    removed.append(undo::sculpt_layer_payload_capture(
        *ctx.mesh, layer->base, undo::PayloadCapture::NodeRemoved));
  }
  /* No explicit #active_set afterwards: #bke::sculpt_layers::remove hands the active marker to a
   * neighbour, and when that neighbour is itself a target its own removal hands it on again, so
   * the marker walks off the removed run on its own. */
  for (SculptLayer *layer : targets) {
    bke::sculpt_layers::remove(*ctx.mesh, *layer);
  }
  undo::push_sculpt_layer_list_change(*ctx.object, std::move(removed), {}, false);
  commit_layers_change(*ctx.depsgraph, *ctx.object);

  /* Fan out: mirror the active object's own body exactly, scoped to each member and the subset of
   * `targets` it actually had synced -- resolved above, before `targets` itself was freed by the
   * active object's own removal loop just above. */
  for (RemoveFanoutMember &fanout_member : fanout_members) {
    Object &member = *fanout_member.object;
    Mesh &member_mesh = mesh_of(member);
    /* Read before the close below, which clears it -- this member's own session, not the active
     * object's. */
    const SculptSession *member_ss = session_of(member);
    const int member_session_uid = member_ss ? member_ss->layers.mask_edit.node_uid : 0;
    mask_edit_exit_ui(C, member);
    mask_edit_end(member);
    session_state_ensure(member);
    if (member_session_uid != 0) {
      undo::push_sculpt_layer_mask_session(member, member_session_uid, false);
    }
    Vector<undo::SculptLayerUndoPayload> member_removed;
    for (SculptLayer *layer : fanout_member.targets) {
      member_removed.append(undo::sculpt_layer_payload_capture(
          member_mesh, layer->base, undo::PayloadCapture::NodeRemoved));
    }
    for (SculptLayer *layer : fanout_member.targets) {
      bke::sculpt_layers::remove(member_mesh, *layer);
    }
    undo::push_sculpt_layer_list_change(member, std::move(member_removed), {}, false);
    commit_layers_change(*ctx.depsgraph, member);
  }

  /* #commit_layers_change unconditionally recomposes the surface, on the active object and every
   * fanned-out member alike, so #UpdateType::Position is required the same way #layer_add_exec
   * requires it. */
  undo::finish_multi_object(C, eligible.as_span(), UpdateType::Position);
  layers_ui_notify(C, *ctx.object);
  sync_scope_unlink_after_success(*bmain, *ctx.object, sync_scope);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus layer_remove_invoke(bContext *C, wmOperator *op, const wmEvent * /*event*/)
{
  OpContext ctx;
  if (!op_context_get(C, op, ctx)) {
    return OPERATOR_CANCELLED;
  }
  if (ctx.object->sculpt_layer_sync_group == 0) {
    return layer_remove_exec(C, op);
  }

  const SculptLayer *active = bke::sculpt_layers::active_get(*ctx.mesh);
  Vector<SculptLayer *> targets;
  for (SculptLayer *layer : bke::sculpt_layers::layers(*ctx.mesh)) {
    if ((layer->base.flag & SCULPT_LAYER_SELECTED) || layer == active) {
      targets.append(layer);
    }
  }
  if (targets.is_empty()) {
    return OPERATOR_CANCELLED;
  }

  bool has_unsynced = false;
  for (const SculptLayer *layer : targets) {
    if (layer->base.sync_uid == 0) {
      has_unsynced = true;
      break;
    }
  }

  if (has_unsynced) {
    RNA_enum_set(op->ptr, "sync_scope", int(SyncScope::ActiveOnly));
    return WM_operator_confirm_ex(
        C,
        op,
        IFACE_("Remove Unsynced Sculpt Layer"),
        IFACE_("Removing a layer that is not synced across the group breaks the sync link. "
               "This object will be removed from the sync group."),
        IFACE_("Remove and Unlink"),
        ui::AlertIcon::Warning,
        false);
  }

  return sync_scope_choice_dialog(
      C,
      op,
      IFACE_("Remove Sculpt Layer (Sync Group)"),
      IFACE_("Remove the synced layer from every object in the sync group and keep objects "
             "linked, remove it only on this object and unlink this object from the group, or "
             "cancel."));
}

void SCULPT_OT_layer_remove(wmOperatorType *ot)
{
  ot->name = "Remove Sculpt Layer";
  ot->idname = "SCULPT_OT_layer_remove";
  ot->description = "Remove the selected sculpt layers and their contribution";
  ot->exec = layer_remove_exec;
  ot->invoke = layer_remove_invoke;
  ot->poll = layers_poll;
  /* No #OPTYPE_UNDO: the operator pushes its own sculpt undo step; a global push would insert a
   * memfile step that does not compose with the stroke SCULPT steps (see #SCULPT_OT_layer_solo_base). */
  ot->flag = OPTYPE_REGISTER;
  sync_scope_rna_def(ot);
}

static wmOperatorStatus layer_move_exec(bContext *C, wmOperator *op)
{
  OpContext ctx;
  if (!op_context_get(C, op, ctx)) {
    return OPERATOR_CANCELLED;
  }
  SculptLayer *layer = bke::sculpt_layers::active_get(*ctx.mesh);
  if (!layer) {
    return OPERATOR_CANCELLED;
  }
  SculptLayerTreeNode &node = layer->base;
  /* Every node but the root sits in a folder, and the root is never a layer. */
  BLI_assert(node.parent != nullptr);
  SculptLayerGroup &parent = *node.parent;
  const int dir = RNA_enum_get(op->ptr, "direction");

  /* The neighbouring *row*, which is simply the next sibling — of either kind. A folder is one
   * indivisible row, so the layer steps over it rather than into it: the swap stays among the
   * layer's own siblings and Move Up/Down therefore never reparents. Only dragging
   * (#SCULPT_OT_layer_move_to) does. */
  SculptLayerTreeNode *neighbour = (dir == 0) ? node.prev : node.next;
  if (!neighbour) {
    /* Already the first / last row of its folder. Stepping further would leave the folder,
     * which is a reparent this operator does not do. No sync-group fan-out is attempted when the
     * active object itself has nothing to do, matching the single-object behavior this operator
     * always had. */
    return OPERATOR_FINISHED;
  }

  Main *bmain = CTX_data_main(C);

  /* Gather-then-gate: see #layer_add_exec's own comment for why the fan-out set has to be built
   * before any undo bracket opens. The active object is included unconditionally; every other
   * sync-group member is brought up to date and gated exactly as #op_context_get gates the active
   * object, plus a find-shaped gate #layer_add_exec has no analog for: the member must actually
   * hold a layer synced to the active one, AND that layer must itself have a neighbour to swap
   * with in the SAME direction among ITS OWN siblings. A member's tree is independently ordered
   * outside of #sync_uid identity, so its layer may already sit at the edge of its own folder even
   * though the active object's does not -- that is not a failure to warn about, only nothing to do
   * for this member, exactly like the active object's own early return just above, so such a
   * member is simply left out of `eligible` without a report.
   *
   * No per-member #flush_pending_multires_base or #mask_edit_refuse_ccg_rebuild is needed here.
   * #op_context_get already ran #flush_pending_multires_base for the active object (if grids)
   * before this function's own body starts -- that is not skipped, it already happened -- and a
   * SECOND run is not needed for the same reason a per-member run is not needed either: a
   * same-folder reorder never calls #commit_layers_change and never recomposes the surface (see
   * #layer_group_color_tag_exec's own comment for the same reasoning applied to a color tag), so
   * nothing after that one flush ever invalidates the CCG-to-layer correspondence it established.
   * #mask_edit_refuse_ccg_rebuild only refuses when a multires rebuild is imminent, and this
   * operator triggers none. */
  Vector<Object *> eligible;
  Set<Object *> tree_only_members;
  eligible.append(ctx.object);
  for (Object *member : fanout_targets(*bmain, *ctx.object)) {
    if (member == ctx.object) {
      continue;
    }
    bool tree_only = false;
    if (!sync_group_tree_create_member_prepare(ctx.depsgraph,
                                               *ctx.object,
                                               member,
                                               op,
                                               "Move Sculpt Layer",
                                               tree_only))
    {
      continue;
    }
    if (tree_only) {
      tree_only_members.add(member);
    }
    /* #node_find_by_sync_uid resolves the SAME layer wherever it sits in the member's own tree;
     * the member's own active layer is unrelated. A zero #sync_uid (the active layer was never
     * fanned-out-created) makes every member miss here by design -- see the function's own doc
     * comment -- which degrades this operator to the single-object behavior it always had. */
    Mesh &member_mesh = mesh_of(*member);
    SculptLayerTreeNode *member_node = bke::sculpt_layers::node_find_by_sync_uid(member_mesh,
                                                                                  node.sync_uid);
    if (!member_node) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Move Sculpt Layer: skipping \"%s\" (no layer synced to the active one)",
                  member->id.name + 2);
      continue;
    }
    /* Defensive: #sync_uid should only ever link a layer to a layer, but the tree is
     * user-editable, so the kind found is not assumed. */
    if (!bke::sculpt_layers::node_as_layer(member_node)) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Move Sculpt Layer: skipping \"%s\" (the synced node is a group, not a layer)",
                  member->id.name + 2);
      continue;
    }
    /* Every node but the root sits in a folder, and the root is never a layer -- mirrors the
     * active object's own assert above. */
    BLI_assert(member_node->parent != nullptr);
    const SculptLayerTreeNode *member_neighbour = (dir == 0) ? member_node->prev :
                                                                member_node->next;
    if (!member_neighbour) {
      /* Already the first / last row of the member's OWN folder -- nothing to do here, not a
       * failure (see the comment above this loop). */
      continue;
    }
    eligible.append(member);
  }

  undo::push_begin_multi_object(*CTX_data_scene(C), op, eligible.as_span());
  /* Active object: unchanged from the single-object path. */
  const int prev_uid_from = node_prev_uid(node);
  /* Up: land where the neighbour sat, i.e. after whatever the neighbour followed (null = the head
   * of the folder). Down: land directly after the neighbour. */
  SculptLayerTreeNode *after = (dir == 0) ? neighbour->prev : neighbour;
  bke::sculpt_layers::node_move_into(*ctx.mesh, node, parent, after);
  bke::sculpt_layers::active_set(*ctx.mesh, layer);
  tag_layer_overlays_dirty(*ctx.object);
  /* Move Up/Down never leaves the layer's folder, so both group fields carry the same uid. */
  const int parent_uid = parent.base.uid;
  Vector<undo::ReparentMove> moves = {
      {node.uid, prev_uid_from, node_prev_uid(node), parent_uid, parent_uid}};
  undo::push_sculpt_layer_reparent(*ctx.object, std::move(moves));

  for (Object *member : eligible) {
    if (member == ctx.object) {
      continue;
    }
    /* Eligibility (#is_supported, sync_uid resolution, the kind check, and the neighbour check)
     * already ran for every member of `eligible` in the gather pass above -- nothing here can fail
     * or need skipping (mirrors #layer_add_exec's own second loop). Re-resolved rather than
     * cached: nothing moves in this member's own tree until this loop reaches it, so the resolve
     * is still first-match-correct (mirrors #layer_toggle_visibility_exec's own second loop).
     *
     * #active_set is deliberately NOT called for a member: which layer is "active" is local UI
     * state, not synced data -- #layer_select_exec itself has no fan-out at all for the same
     * reason. */
    Mesh &member_mesh = mesh_of(*member);
    SculptLayer *member_layer = bke::sculpt_layers::node_as_layer(
        bke::sculpt_layers::node_find_by_sync_uid(member_mesh, node.sync_uid));
    SculptLayerTreeNode &member_node = member_layer->base;
    /* Every node but the root sits in a folder, and the root is never a layer -- mirrors the
     * assert in the gather pass above; re-asserted here since this is a fresh resolve, not the
     * same pointer. */
    BLI_assert(member_node.parent != nullptr);
    SculptLayerGroup &member_parent = *member_node.parent;
    SculptLayerTreeNode *member_neighbour = (dir == 0) ? member_node.prev : member_node.next;
    const int member_prev_uid_from = node_prev_uid(member_node);
    SculptLayerTreeNode *member_after = (dir == 0) ? member_neighbour->prev : member_neighbour;
    bke::sculpt_layers::node_move_into(member_mesh, member_node, member_parent, member_after);
    tag_layer_overlays_dirty(*member);
    const int member_parent_uid = member_parent.base.uid;
    Vector<undo::ReparentMove> member_moves = {{member_node.uid,
                                                 member_prev_uid_from,
                                                 node_prev_uid(member_node),
                                                 member_parent_uid,
                                                 member_parent_uid}};
    undo::push_sculpt_layer_reparent(*member, std::move(member_moves));
    if (tree_only_members.contains(member)) {
      DEG_id_tag_update(&member_mesh.id, ID_RECALC_GEOMETRY);
    }
  }

  /* Order does not affect the additive combination and nothing here reads or writes the runtime
   * mesh base or recomposes the surface, so #finish_multi_object is not used to close the step --
   * it would call #flush_update_done per object, which does work (PBVH ensure, viewport/geometry
   * tags) the single-object do-path never performed for a reorder. #push_end_all_ex(false, true)
   * is the same close #finish_multi_object itself uses internally, minus that per-object flush
   * loop -- the documented multi-object idiom in `sculpt_undo.hh` for exactly this case, and the
   * same one #layer_group_color_tag_exec already relies on for the same reasoning. */
  undo::push_end_all_ex(false, true);
  for (Object *member : eligible) {
    layers_ui_notify(C, *member);
  }
  return OPERATOR_FINISHED;
}

void SCULPT_OT_layer_move(wmOperatorType *ot)
{
  static const EnumPropertyItem direction_items[] = {
      {0, "UP", 0, "Up", ""},
      {1, "DOWN", 0, "Down", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };
  ot->name = "Move Sculpt Layer";
  ot->idname = "SCULPT_OT_layer_move";
  ot->description = "Move the active sculpt layer up or down in the list";
  ot->exec = layer_move_exec;
  ot->poll = layers_poll;
  /* No #OPTYPE_UNDO: the operator pushes its own sculpt undo step; a global push would insert a
   * memfile step that does not compose with the stroke SCULPT steps (see #SCULPT_OT_layer_solo_base). */
  ot->flag = OPTYPE_REGISTER;
  RNA_def_enum(ot->srna, "direction", direction_items, 0, "Direction", "");
}

/* The folder \a uid names, or null when it names no folder the user can act on.
 *
 * Three uids are refused, and all three would otherwise resolve to something: uid 0 is the root
 * folder — #bke::sculpt_layers::node_find_by_uid hands it back, but it is never drawn as a row
 * and holds every other node, so an operator addressing it would act on the whole tree behind a
 * control the user cannot see. A layer's uid and a stale uid are refused by the kind-checked cast
 * and the lookup respectively. One counter spans both kinds now, so the uid cannot be assumed to
 * name a folder just because a folder operator was the one asking.
 */
static SculptLayerGroup *group_row_lookup(Mesh &mesh, const int uid)
{
  if (uid == 0) {
    return nullptr;
  }
  return bke::sculpt_layers::node_as_group(bke::sculpt_layers::node_find_by_uid(mesh, uid));
}

static SculptLayer *layer_row_lookup(Mesh &mesh, const int uid)
{
  if (uid == 0) {
    return bke::sculpt_layers::active_get(mesh);
  }
  return bke::sculpt_layers::node_as_layer(bke::sculpt_layers::node_find_by_uid(mesh, uid));
}

/* Every selected node at or below \a group, depth-first in tree order — which is exactly the order
 * the rows are drawn in, so a batch move keeps the selection's mutual order.
 *
 * The two kinds carry their selection in different bits (#SCULPT_LAYER_SELECTED vs
 * #SCULPT_LAYER_GROUP_SELECTED), so the walk has to dispatch on the kind even though both now live
 * in one children list.
 *
 * A selected folder is appended whole and its subtree is *not* descended into: the folder is one
 * indivisible row, so anything selected inside it travels with it. Collecting such a child
 * separately would move it next to its own folder instead, silently lifting it out — and, because
 * a selected folder always precedes its own descendants here, skipping the subtree is also what
 * keeps the caller from ever being handed a node nested inside another node of the same batch.
 */
static void selected_nodes_gather(const SculptLayerGroup &group,
                                  Vector<SculptLayerTreeNode *> &r_nodes)
{
  for (SculptLayerTreeNode &node : group.children) {
    if (const SculptLayerGroup *child = bke::sculpt_layers::node_as_group(&node)) {
      if (node.flag & SCULPT_LAYER_GROUP_SELECTED) {
        r_nodes.append(&node);
      }
      else {
        selected_nodes_gather(*child, r_nodes);
      }
      continue;
    }
    if (node.flag & SCULPT_LAYER_SELECTED) {
      r_nodes.append(&node);
    }
  }
}

/* One sync-group member's resolved move for #layer_move_to_exec, gathered once before any
 * mutation runs (mirrors #RemoveFanoutMember's own reasoning): #dst / #cursor / #moved are
 * resolved from THIS member's own tree via #sync_uid, and #moves carries every
 * #undo::ReparentMove field but \a prev_to, which is only known once the move itself has actually
 * run. Nothing here is re-derived after the fact -- #sync_group_members dedups fan-out targets by
 * #Object::data, so no two members ever share a mesh, but a member's OWN #moved pointers would
 * still go stale the moment its OWN #node_move_into calls start relinking them. */
struct MoveToFanoutMember {
  Object *object;
  SculptLayerGroup *dst;
  SculptLayerTreeNode *cursor;
  Vector<SculptLayerTreeNode *> moved;
  Vector<undo::ReparentMove> moves;
};

static wmOperatorStatus layer_move_to_exec(bContext *C, wmOperator *op)
{
  OpContext ctx;
  if (!op_context_get(C, op, ctx)) {
    return OPERATOR_CANCELLED;
  }
  if (mask_edit_refuse_ccg_rebuild(op, *ctx.object)) {
    return OPERATOR_CANCELLED;
  }
  Mesh &mesh = *ctx.mesh;

  const int anchor_uid = RNA_int_get(op->ptr, "anchor_uid");
  const MoveLocation location = MoveLocation(RNA_enum_get(op->ptr, "location"));

  SculptLayerGroup &root = *bke::sculpt_layers::root_group(mesh);

  /* The anchor row. The uid alone names it: one counter (#bke::sculpt_layers::node_unique_uid)
   * now spans layers and folders, so a uid identifies exactly one node of either kind and the
   * caller no longer has to say which list to resolve it in — which is the whole reason the former
   * `anchor_is_group` property existed.
   *
   * Uid 0 resolves to the root folder, which is never drawn as a row: it stands for "the top
   * level" rather than for a node to sit beside, so it is normalized to "no anchor" here
   * (Before/After then mean the head of the root's children, Into means the root itself). Any
   * other uid that fails to resolve is a stale drag against a row that has since gone. */
  SculptLayerTreeNode *anchor = bke::sculpt_layers::node_find_by_uid(mesh, anchor_uid);
  if (anchor == &root.base) {
    anchor = nullptr;
  }
  else if (!anchor) {
    BKE_report(op->reports, RPT_ERROR, "No sculpt layer or group with that id");
    return OPERATOR_CANCELLED;
  }

  /* Destination folder and the sibling the first moved node lands after (null = the folder's
   * head).
   *
   * This is what the tree bought: a folder's children hold both kinds and their order *is* the
   * sibling order, so "next to the anchor" is one insertion into one list — the anchor's own — no
   * matter whether the anchor or the moved node is a layer or a folder. The two independent
   * cursors this used to chain (one per list) could not express "a folder right after this layer"
   * at all. */
  SculptLayerGroup *dst = nullptr;
  SculptLayerTreeNode *cursor = nullptr;
  if (location == MoveLocation::Into) {
    /* Into names a folder: there is nothing inside a layer. */
    dst = anchor ? bke::sculpt_layers::node_as_group(anchor) : &root;
    if (!dst) {
      BKE_report(op->reports, RPT_ERROR, "Can only move sculpt layers into a group");
      return OPERATOR_CANCELLED;
    }
    /* Append rather than prepend, and a null cursor means head, so start from the current last
     * child. */
    cursor = static_cast<SculptLayerTreeNode *>(dst->children.last);
  }
  else {
    /* Before/After: the drop lands beside the anchor, i.e. among its siblings. */
    dst = anchor ? anchor->parent : &root;
    cursor = (location == MoveLocation::After) ? anchor : (anchor ? anchor->prev : nullptr);
  }

  /* Selected nodes, in their current relative order (drag data carries no explicit list — it is
   * always this same selection, see #SculptLayerDragController::create_drag_data). Falls back to
   * the active layer so a plain "no selection yet" script call still does something sensible. */
  Vector<SculptLayerTreeNode *> moved;
  selected_nodes_gather(root, moved);
  if (moved.is_empty()) {
    if (SculptLayer *active = bke::sculpt_layers::active_get(mesh)) {
      moved.append(&active->base);
    }
  }
  if (moved.is_empty()) {
    return OPERATOR_CANCELLED;
  }

  /* Reject a folder dropped into its own subtree, *including into itself*: either would detach
   * that subtree from the root, and #bke::sculpt_layers::node_move_into asserts both rather than
   * handling them. #node_is_descendant_of is strict — a node is not its own descendant — so it
   * answers only the "strictly below" half and the identity test is spelled out alongside it. */
  for (const SculptLayerTreeNode *node : moved) {
    const SculptLayerGroup *group = bke::sculpt_layers::node_as_group(node);
    if (!group) {
      continue;
    }
    if (dst == group || bke::sculpt_layers::node_is_descendant_of(dst->base, *group)) {
      BKE_report(op->reports, RPT_ERROR, "Cannot move a sculpt layer group into itself");
      return OPERATOR_CANCELLED;
    }
  }

  /* Captured before anything moves: the `from` half of each entry describes where the node sat. */
  Vector<undo::ReparentMove> moves;
  moves.reserve(moved.size());
  for (const SculptLayerTreeNode *node : moved) {
    undo::ReparentMove move;
    move.uid = node->uid;
    move.prev_from = node_prev_uid(*node);
    move.group_from = node_parent_uid(*node);
    move.group_to = dst->base.uid;
    moves.append(move);
  }

  Main *bmain = CTX_data_main(C);

  /* Gather-then-gate: see #layer_add_exec's own comment for why the fan-out set has to be built
   * before any undo bracket opens. The active object is included unconditionally; every other
   * sync-group member is brought up to date and gated exactly as #op_context_get gates the active
   * object -- including the grids #flush_pending_multires_base gate #op_context_get itself already
   * ran for the active object, mirrored per-member below immediately before
   * #mask_edit_refuse_ccg_rebuild, exactly as #layer_add_exec / #layer_remove_exec /
   * #layer_clear_exec do: unlike #layer_move_exec, this operator DOES call #commit_layers_change
   * (a folder-boundary move can change what composes), so a pending base edit really would be
   * invalidated by the move below if it were not flushed first -- plus find-shaped gates
   * #layer_add_exec has no analog for: the member must resolve BOTH the destination and every one
   * of the moved source nodes via #sync_uid before it participates at all. Unlike
   * #layer_remove_exec's per-target degradation, a member missing even one of these is skipped
   * ENTIRELY with a named warning, never partially applied -- a move whose destination or source
   * set differs from what the active object did is not a smaller version of the same edit, it is
   * a different edit (see this task's own brief). */
  Vector<Object *> eligible;
  eligible.append(ctx.object);
  Set<Object *> tree_only_members;
  Vector<MoveToFanoutMember> fanout_members;
  for (Object *member : fanout_targets(*bmain, *ctx.object)) {
    if (member == ctx.object) {
      continue;
    }
    bool tree_only = false;
    if (!sync_group_tree_create_member_prepare(ctx.depsgraph,
                                               *ctx.object,
                                               member,
                                               op,
                                               "Move Sculpt Layer To",
                                               tree_only))
    {
      continue;
    }
    if (tree_only) {
      tree_only_members.add(member);
    }
    Mesh &member_mesh = mesh_of(*member);

    /* Resolve the destination the same way the source nodes are resolved below: via #sync_uid,
     * since #anchor_uid is a raw #SculptLayerTreeNode::uid that only means something inside the
     * tree it was read from. A null #anchor stands for the top level, i.e. this member's own
     * root, which always resolves without a lookup -- mirroring how #anchor itself was normalized
     * above. A non-null #anchor whose #sync_uid is 0 (never fanned-out-created) makes every member
     * miss here by design, degrading to the single-object behavior this operator always had. */
    SculptLayerTreeNode *member_anchor = nullptr;
    if (anchor) {
      member_anchor = bke::sculpt_layers::node_find_by_sync_uid(member_mesh, anchor->sync_uid);
      if (!member_anchor) {
        BKE_reportf(op->reports,
                    RPT_WARNING,
                    "Move Sculpt Layer To: skipping \"%s\" (no destination synced to the active "
                    "one)",
                    member->id.name + 2);
        continue;
      }
    }
    SculptLayerGroup *member_dst = nullptr;
    SculptLayerTreeNode *member_cursor = nullptr;
    if (location == MoveLocation::Into) {
      member_dst = member_anchor ? bke::sculpt_layers::node_as_group(member_anchor) :
                                    bke::sculpt_layers::root_group(member_mesh);
      if (!member_dst) {
        BKE_reportf(op->reports,
                    RPT_WARNING,
                    "Move Sculpt Layer To: skipping \"%s\" (the synced destination is a layer, "
                    "not a group)",
                    member->id.name + 2);
        continue;
      }
      member_cursor = static_cast<SculptLayerTreeNode *>(member_dst->children.last);
    }
    else {
      member_dst = member_anchor ? member_anchor->parent :
                                    bke::sculpt_layers::root_group(member_mesh);
      member_cursor = (location == MoveLocation::After) ?
                          member_anchor :
                          (member_anchor ? member_anchor->prev : nullptr);
    }

    /* Resolve every moved source node via #sync_uid, one at a time; missing even one skips the
     * whole member (see the comment above this loop). */
    Vector<SculptLayerTreeNode *> member_moved;
    Vector<undo::ReparentMove> member_moves;
    member_moved.reserve(moved.size());
    member_moves.reserve(moved.size());
    bool member_missing_source = false;
    for (const SculptLayerTreeNode *node : moved) {
      SculptLayerTreeNode *member_node = bke::sculpt_layers::node_find_by_sync_uid(member_mesh,
                                                                                    node->sync_uid);
      if (!member_node) {
        BKE_reportf(op->reports,
                    RPT_WARNING,
                    "Move Sculpt Layer To: skipping \"%s\" (no layer or group synced to \"%s\")",
                    member->id.name + 2,
                    node->name);
        member_missing_source = true;
        break;
      }
      /* #node_find_by_sync_uid is first-match: two entries of `moved` sharing the same non-zero
       * #sync_uid would otherwise resolve to the SAME member node twice, and the chain loop below
       * would try to move it twice as if it were two distinct rows. Reachable the same way
       * #layer_remove_exec's own dedup comment explains: no longer produced by duplication, but
       * still present in any file saved before #bke::sculpt_layers::duplicate started clearing the
       * copy's #base.sync_uid. Not treated as "missing": the active object itself carries the same
       * collision without complaint, so a repeat is silently folded into the one entry already
       * collected. */
      if (member_moved.contains(member_node)) {
        continue;
      }
      member_moved.append(member_node);
      undo::ReparentMove member_move;
      member_move.uid = member_node->uid;
      member_move.prev_from = node_prev_uid(*member_node);
      member_move.group_from = node_parent_uid(*member_node);
      member_move.group_to = member_dst->base.uid;
      member_moves.append(member_move);
    }
    if (member_missing_source) {
      continue;
    }

    /* Mirrors the active object's own self-containment check above, evaluated against THIS
     * member's own tree: the active object's tree not rejecting a move is no guarantee a
     * sync-group member's independently-ordered tree would not, since the two are only linked by
     * #sync_uid identity, not by shared structure. */
    bool member_self_contained = false;
    for (const SculptLayerTreeNode *member_node : member_moved) {
      const SculptLayerGroup *member_group = bke::sculpt_layers::node_as_group(member_node);
      if (!member_group) {
        continue;
      }
      if (member_dst == member_group ||
          bke::sculpt_layers::node_is_descendant_of(member_dst->base, *member_group))
      {
        BKE_reportf(op->reports,
                    RPT_WARNING,
                    "Move Sculpt Layer To: skipping \"%s\" (would move a group into itself)",
                    member->id.name + 2);
        member_self_contained = true;
        break;
      }
    }
    if (member_self_contained) {
      continue;
    }

    if (!tree_only) {
      if (bke::object::pbvh_get(*member)->type() == bke::pbvh::Type::Grids) {
        /* Mirrors #op_context_get / #layer_add_exec: consume any pending base sculpt edits first,
         * while the live CCG still matches the stored layers, before the move below invalidates
         * that. */
        flush_pending_multires_base(*member);
      }
      if (mask_edit_refuse_ccg_rebuild(op, *member)) {
        continue;
      }
    }

    eligible.append(member);
    MoveToFanoutMember fanout_member;
    fanout_member.object = member;
    fanout_member.dst = member_dst;
    fanout_member.cursor = member_cursor;
    fanout_member.moved = std::move(member_moved);
    fanout_member.moves = std::move(member_moves);
    fanout_members.append(std::move(fanout_member));
  }

  session_state_ensure(*ctx.object);
  undo::push_begin_multi_object(*CTX_data_scene(C), op, eligible.as_span());
  /* Active object: unchanged from the single-object path. */
  /* Chain: the first moved node lands next to the drop anchor, every following one right after
   * the previously placed one, which preserves the selection's mutual order. One cursor for both
   * kinds, because there is one list. */
  for (SculptLayerTreeNode *node : moved) {
    /* Already sitting exactly where it would be re-inserted — reached by dropping a selection
     * onto one of its own members, or back into the folder it is already the last child of.
     * #node_move_into asserts `after != &node` rather than tolerating it, since re-linking a node
     * after itself would splice it into its own stale links. */
    if (cursor != node) {
      bke::sculpt_layers::node_move_into(mesh, *node, *dst, cursor);
    }
    cursor = node;
  }
  for (const int64_t i : moved.index_range()) {
    moves[i].prev_to = node_prev_uid(*moved[i]);
  }
  undo::push_sculpt_layer_reparent(*ctx.object, std::move(moves));
  /* A move across a folder boundary changes what is visible (the destination folder may be
   * disabled), so unlike a plain same-folder reorder this is not a UI-only refresh. */
  group_cascade_resync_with_undo(*ctx.object, mesh);
  commit_layers_change(*ctx.depsgraph, *ctx.object);

  for (MoveToFanoutMember &fanout_member : fanout_members) {
    Object &member = *fanout_member.object;
    Mesh &member_mesh = mesh_of(member);
    const bool member_tree_only = tree_only_members.contains(&member);
    if (!member_tree_only) {
      /* Mirrors #layer_add_exec's own per-member call: has to run before this member's tree changes
       * below, or the mesh_base captured here would fold an already-moved state into the base (see
       * #commit_layers_change's comment on capture ordering). */
      session_state_ensure(member);
    }
    SculptLayerTreeNode *member_cursor = fanout_member.cursor;
    for (SculptLayerTreeNode *member_node : fanout_member.moved) {
      if (member_cursor != member_node) {
        bke::sculpt_layers::node_move_into(member_mesh, *member_node, *fanout_member.dst,
                                            member_cursor);
      }
      member_cursor = member_node;
    }
    for (const int64_t i : fanout_member.moved.index_range()) {
      fanout_member.moves[i].prev_to = node_prev_uid(*fanout_member.moved[i]);
    }
    undo::push_sculpt_layer_reparent(member, std::move(fanout_member.moves));
    group_cascade_resync_with_undo(member, member_mesh);
    if (member_tree_only) {
      DEG_id_tag_update(&member_mesh.id, ID_RECALC_GEOMETRY);
    }
    else {
      commit_layers_change(*ctx.depsgraph, member);
    }
  }

  /* #commit_layers_change unconditionally recomposes the surface on sculpt-ready members; tree-only
   * members only had their DNA tree updated. */
  Vector<Object *> eligible_finish;
  eligible_finish.reserve(eligible.size());
  for (Object *ob : eligible) {
    if (!tree_only_members.contains(ob)) {
      eligible_finish.append(ob);
    }
  }
  undo::finish_multi_object(C, eligible_finish.as_span(), UpdateType::Position);
  for (Object *member : eligible) {
    layers_ui_notify(C, *member);
  }
  return OPERATOR_FINISHED;
}

void SCULPT_OT_layer_move_to(wmOperatorType *ot)
{
  static const EnumPropertyItem location_items[] = {
      {int(MoveLocation::Before), "BEFORE", 0, "Before", "Insert before the anchor"},
      {int(MoveLocation::After), "AFTER", 0, "After", "Insert after the anchor"},
      {int(MoveLocation::Into), "INTO", 0, "Into", "Insert inside the anchor group"},
      {0, nullptr, 0, nullptr, nullptr},
  };
  ot->name = "Move Sculpt Layer To";
  ot->idname = "SCULPT_OT_layer_move_to";
  ot->description = "Move the selected sculpt layers and groups next to, or into, another item";
  ot->exec = layer_move_to_exec;
  ot->poll = layers_poll;
  /* No #OPTYPE_UNDO: the operator pushes its own sculpt undo step; a global push would insert a
   * memfile step that does not compose with the stroke SCULPT steps (see #SCULPT_OT_layer_solo_base). */
  ot->flag = OPTYPE_REGISTER;
  /* One uid names one node, of either kind (#bke::sculpt_layers::node_unique_uid), so the caller
   * no longer has to state which kind the anchor is. */
  RNA_def_int(ot->srna,
              "anchor_uid",
              0,
              0,
              INT_MAX,
              "Anchor ID",
              "Uid of the layer or group to move next to (0 = the top level)",
              0,
              INT_MAX);
  RNA_def_enum(ot->srna,
               "location",
               location_items,
               int(MoveLocation::After),
               "Location",
               "Where to place the moved items relative to the anchor");
}

/* Both find-shaped and create-shaped: the source is looked up per member by #sync_uid the way
 * #layer_clear_exec looks its target up, but what the operator produces is a *new* node on every
 * participant, the way #layer_add_exec does.
 *
 * This is also where a live bug was fixed. #bke::sculpt_layers::duplicate used to hand back a copy
 * that still carried the source's #SculptLayerTreeNode::sync_uid (a blind struct copy), so one mesh
 * could end up holding two nodes under one sync_uid -- the collision #layer_remove_exec and
 * #layer_move_to_exec each had to defend against, since #node_find_by_sync_uid is first-match. That
 * is now cleared at the source, in #bke::sculpt_layers::duplicate itself, so every caller gets a
 * copy with no cross-object identity at all; this operator is the one place that re-establishes it,
 * minting a single fresh shared value only when the duplication actually fans out across a sync
 * group. An ungrouped object's duplicate keeps sync_uid 0 -- it is nobody else's layer. */
static wmOperatorStatus layer_duplicate_exec(bContext *C, wmOperator *op)
{
  OpContext ctx;
  if (!op_context_get(C, op, ctx)) {
    return OPERATOR_CANCELLED;
  }
  if (mask_edit_refuse_ccg_rebuild(op, *ctx.object)) {
    return OPERATOR_CANCELLED;
  }
  SculptLayer *src = bke::sculpt_layers::active_get(*ctx.mesh);
  if (!src) {
    return OPERATOR_CANCELLED;
  }
  Main *bmain = CTX_data_main(C);

  /* Gather-then-gate: see #layer_add_exec's own comment for why the fan-out set has to be built
   * before any undo bracket opens. The active object is included unconditionally; every other
   * sync-group member is brought up to date and gated exactly as #op_context_get gates the active
   * object, plus a find-shaped gate #layer_add_exec has no analog for: the member must actually
   * hold a layer synced to the one being duplicated. */
  Vector<Object *> eligible;
  eligible.append(ctx.object);
  for (Object *member : fanout_targets(*bmain, *ctx.object)) {
    if (member == ctx.object) {
      continue;
    }
    /* Mirrors #layer_add_exec's gather loop: guarded on an existing session, since a member that
     * was never taken into sculpt mode has none, and #BKE_sculpt_update_object_for_edit
     * unconditionally dereferences it. Refreshed before #is_supported below so a member that IS in
     * sculpt mode but has not built its PBVH yet is not misreported as unsupported. */
    if (member->runtime->sculpt_session != nullptr) {
      BKE_sculpt_update_object_for_edit(ctx.depsgraph, member, false);
    }
    if (!is_supported(*member)) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Duplicate Sculpt Layer: skipping \"%s\" (sculpt layers are not available for "
                  "this object)",
                  member->id.name + 2);
      continue;
    }
    /* #node_find_by_sync_uid resolves the SAME layer wherever it sits in the member's own tree;
     * the member's own active layer is unrelated. A zero #sync_uid (the source layer was never
     * fanned-out-created, or is itself an earlier duplicate) makes every member miss here by
     * design -- see the function's own doc comment -- which degrades this operator to the
     * single-object behavior it always had. */
    Mesh &member_mesh = mesh_of(*member);
    SculptLayerTreeNode *member_node = bke::sculpt_layers::node_find_by_sync_uid(
        member_mesh, src->base.sync_uid);
    if (!member_node) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Duplicate Sculpt Layer: skipping \"%s\" (no layer synced to the active one)",
                  member->id.name + 2);
      continue;
    }
    /* Defensive: #sync_uid should only ever link a layer to a layer, but the tree is
     * user-editable, so the kind found is not assumed. */
    if (!bke::sculpt_layers::node_as_layer(member_node)) {
      BKE_reportf(
          op->reports,
          RPT_WARNING,
          "Duplicate Sculpt Layer: skipping \"%s\" (the synced node is a group, not a layer)",
          member->id.name + 2);
      continue;
    }
    if (bke::object::pbvh_get(*member)->type() == bke::pbvh::Type::Grids) {
      /* Mirrors #op_context_get / #layer_add_exec: consume any pending base sculpt edits first,
       * while the live CCG still matches the stored layers. */
      flush_pending_multires_base(*member);
    }
    if (mask_edit_refuse_ccg_rebuild(op, *member)) {
      continue;
    }
    eligible.append(member);
  }

  session_state_ensure(*ctx.object);
  undo::push_begin_multi_object(*CTX_data_scene(C), op, eligible.as_span());
  /* Active object: unchanged from the single-object path. */
  SculptLayer *copy = bke::sculpt_layers::duplicate(*ctx.mesh, *src);
  undo::push_sculpt_layer_list_change(*ctx.object, {}, Vector<int>({copy->base.uid}), false);
  /* The duplicated contribution doubles up in the combined result. */
  commit_layers_change(*ctx.depsgraph, *ctx.object);

  /* Fan out: mint ONE shared #SculptLayerTreeNode::sync_uid for the active object's copy and every
   * member's copy, so #node_find_by_sync_uid can resolve "the same" copy across the group -- the
   * same mint-once-stamp-everywhere pattern as #layer_add_exec, overwriting the 0 that
   * #bke::sculpt_layers::duplicate leaves on every copy. Skipped entirely when nothing else is
   * eligible, so a duplicate that is not part of a fan-out keeps sync_uid 0 rather than consuming a
   * value and claiming an identity no other object shares. `eligible[0]` is always `ctx.object`
   * (appended first, above), hence the `member == ctx.object` skip below rather than a
   * `drop_front(1)`, which would silently misbehave if that ordering guarantee ever changed. */
  if (eligible.size() > 1) {
    const int new_sync_uid = bke::sculpt_layers::layer_sync_uid_unique(*bmain);
    copy->base.sync_uid = new_sync_uid;

    for (Object *member : eligible) {
      if (member == ctx.object) {
        continue;
      }
      /* Eligibility (#is_supported, sync_uid resolution, #mask_edit_refuse_ccg_rebuild) and the
       * multires pending-base flush already ran for every member of `eligible` in the gather pass
       * above -- nothing here can fail or need skipping (mirrors #layer_add_exec's own second
       * loop). Re-resolved rather than carried forward, as in #layer_clear_exec: this operator
       * frees no node, and the copy each iteration inserts carries sync_uid 0 until it is stamped
       * below, so the lookup cannot start matching a copy instead of a source. */
      session_state_ensure(*member);
      Mesh &member_mesh = mesh_of(*member);
      SculptLayer *member_src = bke::sculpt_layers::node_as_layer(
          bke::sculpt_layers::node_find_by_sync_uid(member_mesh, src->base.sync_uid));
      SculptLayer *member_copy = bke::sculpt_layers::duplicate(member_mesh, *member_src);
      member_copy->base.sync_uid = new_sync_uid;
      undo::push_sculpt_layer_list_change(
          *member, {}, Vector<int>({member_copy->base.uid}), false);
      commit_layers_change(*ctx.depsgraph, *member);
    }
  }

  /* #commit_layers_change unconditionally recomposes the surface, on the active object and every
   * fanned-out member alike, so #UpdateType::Position is required the same way #layer_add_exec
   * requires it. */
  undo::finish_multi_object(C, eligible.as_span(), UpdateType::Position);
  layers_ui_notify(C, *ctx.object);
  return OPERATOR_FINISHED;
}

void SCULPT_OT_layer_duplicate(wmOperatorType *ot)
{
  ot->name = "Duplicate Sculpt Layer";
  ot->idname = "SCULPT_OT_layer_duplicate";
  ot->description = "Duplicate the active sculpt layer";
  ot->exec = layer_duplicate_exec;
  ot->poll = layers_poll;
  /* No #OPTYPE_UNDO: the operator pushes its own sculpt undo step; a global push would insert a
   * memfile step that does not compose with the stroke SCULPT steps (see #SCULPT_OT_layer_solo_base). */
  ot->flag = OPTYPE_REGISTER;
}

/* The layer visually below \a layer in the stack, or null when it is the bottom one of its folder.
 *
 * The only place where sibling *order* carries meaning: the combination is additive and
 * order-independent everywhere else, so the order is otherwise presentation.
 *
 * "Below" still means the next sibling *within the same folder*, and that reason survives the tree
 * unchanged — merging into a layer of another folder would silently move geometry across a folder
 * boundary. Only the walk changes: siblings now interleave both kinds, so the next link may be a
 * folder. Folders stay invisible to Merge Down (one indivisible row it does not reach into), so
 * they are stepped over, and the sibling walk itself is what keeps the search inside the folder —
 * the flat list this had to filter by folder tag is gone.
 */
static SculptLayer *layer_below(SculptLayer &layer)
{
  for (SculptLayerTreeNode *node = layer.base.next; node; node = node->next) {
    if (SculptLayer *below = bke::sculpt_layers::node_as_layer(node)) {
      return below;
    }
  }
  return nullptr;
}

/* True when \a node's contribution is attenuated by a weight mask — its own, or one carried by any
 * folder above it (the chain #bke::sculpt_layers::chain_mask folds).
 *
 * The merge operators no longer refuse on this — #MergeMaskPolicy says what becomes of the masks —
 * but they still ask, because every mask-aware step of a merge (the fold, the combined mask, the
 * policy report) is skipped outright when the answer is false, which is the overwhelmingly common
 * case.
 *
 * Presence is tested rather than #bke::sculpt_layers::node_mask_for_composite, deliberately: that
 * resolver needs the domain's element count and drops any mask that does not describe it, so a mask
 * gone stale behind a topology change — still the user's data, still destroyed by the merge — would
 * answer "unmasked" here. #find_stale_mask_in_fold is what turns that into a refusal; answering
 * "masked" here is what makes sure the question is asked at all. Fails closed, which is the point.
 */
/**
 * What a merge does with the weight masks of the layers and folders it dissolves.
 *
 * Every option preserves the composed surface; #Combine does so up to the `uint8` quantization of
 * the mask it builds, the other two exactly. They differ in what the merged layer keeps as an
 * editable mask afterwards.
 */
enum class MergeMaskPolicy : int8_t {
  /** Fold every mask into the merged data. The result carries no mask. */
  Bake = 0,
  /** One mask on the result: the per-element maximum of the participants' folded mask factors. */
  Combine = 1,
  /** Keep the merged folder's own mask; fold the layers' and nested folders' masks into the data. */
  KeepGroup = 2,
};

static const EnumPropertyItem merge_mask_policy_items[] = {
    {int(MergeMaskPolicy::Bake),
     "BAKE",
     0,
     "Apply Masks",
     "Bake every mask into the merged shape; the result carries no mask"},
    {int(MergeMaskPolicy::Combine),
     "COMBINE",
     0,
     "Combine Masks",
     "Keep one mask on the result, covering wherever any participant was in force"},
    {int(MergeMaskPolicy::KeepGroup),
     "KEEP_GROUP",
     0,
     "Keep Group Mask",
     "Keep the folder's own mask on the result; bake the layers' own masks into the shape"},
    {0, nullptr, 0, nullptr, nullptr},
};

static void merge_mask_policy_prop(wmOperatorType *ot)
{
  RNA_def_enum(ot->srna,
               "mask_policy",
               merge_mask_policy_items,
               int(MergeMaskPolicy::Bake),
               "Masks",
               "What to do with the weight masks of the merged layers and folders");
}

static MergeMaskPolicy merge_mask_policy_get(wmOperator *op)
{
  return MergeMaskPolicy(RNA_enum_get(op->ptr, "mask_policy"));
}

/** Tell the user what became of the weight masks a merge just dissolved. */
static void merge_report_mask_policy(wmOperator *op, const MergeMaskPolicy policy)
{
  /* Three spellings rather than one format string chosen by a ternary, so each stays extractable for
   * translation. The layer merges pass the *applied* policy, not the one picked in the list, so the
   * message never names an option that collapsed to another. */
  switch (policy) {
    case MergeMaskPolicy::Bake:
      BKE_report(op->reports, RPT_INFO, "Weight masks were baked into the merged shape");
      break;
    case MergeMaskPolicy::Combine:
      BKE_report(op->reports, RPT_INFO, "Weight masks were combined into one on the result");
      break;
    case MergeMaskPolicy::KeepGroup:
      BKE_report(op->reports, RPT_INFO, "The folder's weight mask was kept on the result");
      break;
  }
}

/**
 * Per-element maximum of \a participants' folded mask factors, cut to \a elem_num — the mask
 * #MergeMaskPolicy::Combine keeps on the merged layer.
 *
 * The maximum specifically, and not a product or an average: it is the only combination for which a
 * zero implies every term was zero, which is what makes the division in #merge_settle_mask well
 * defined. A product would reach zero as soon as any single participant did, leaving a non-zero sum
 * with nothing to divide by.
 *
 * Participants with no data are skipped, \a survivor excepted — its buffer exists by the time this
 * runs. They contribute nothing to the sum, so letting an unmasked empty one pin the maximum to 1
 * would degenerate the result into #MergeMaskPolicy::Bake for no reason.
 */
static Array<float> merge_combined_mask(const Span<SculptLayer *> participants,
                                        const SculptLayer &survivor,
                                        const SculptLayerGroup *stop_above,
                                        const int64_t elem_num)
{
  Array<float> combined(elem_num);
  combined.as_mutable_span().fill(0.0f);
  Array<float> scratch;
  for (const SculptLayer *layer : participants) {
    if (layer != &survivor && !layer->data) {
      continue;
    }
    if (!gather_fold_mask(layer->base, stop_above, elem_num, scratch)) {
      /* Unmasked participant: it acts everywhere, so no attenuation of the result can be correct. */
      combined.as_mutable_span().fill(1.0f);
      break;
    }
    for (const int64_t i : combined.index_range()) {
      combined[i] = std::max(combined[i], scratch[i]);
    }
  }
  return combined;
}

/**
 * Leave the merged layer with the mask \a policy asks for, dividing the already-accumulated
 * \a survivor_data by it wherever one is kept so the composed surface does not move.
 *
 * \a combined is what #merge_combined_mask returned, empty when no combined mask is wanted.
 * \a keep_group is the dissolving folder whose mask #MergeMaskPolicy::KeepGroup preserves, null for
 * the merges that dissolve no folder.
 *
 * Must run after the accumulation and before anything frees \a keep_group.
 */
static void merge_settle_mask(Object &object,
                              SculptLayer &survivor,
                              MutableSpan<float3> survivor_data,
                              const MergeMaskPolicy policy,
                              const Span<float> combined,
                              const int block_size,
                              const SculptLayerGroup *keep_group)
{
  /* Nothing to record and nothing to change when no mask is in play, which is the common merge. Kept
   * here rather than at the three call sites so that none can forget it: the push below would
   * otherwise name the survivor in the step's mask slot for a merge that never touched a mask. */
  if (survivor.base.mask == nullptr && combined.is_empty() &&
      (keep_group == nullptr || keep_group->base.mask == nullptr))
  {
    return;
  }
  /* The survivor's own mask has been folded into its data under every policy, so it must not stay on
   * the node. Captured before it is freed; the masks of the *other* participants ride out in their
   * own removal payloads. */
  undo::push_sculpt_layer_mask(object, survivor.base);
  bke::sculpt_layers::mask_free(survivor.base.mask);
  survivor.base.mask = nullptr;
  switch (policy) {
    case MergeMaskPolicy::Bake:
      break;
    case MergeMaskPolicy::Combine: {
      if (combined.is_empty()) {
        break;
      }
      /* Compressed and then expanded again *before* the division, not after: the mask is stored as
       * `uint8`, so dividing by the unquantized value and storing the quantized one would leave
       * `data * mask` off by the rounding error at every element. Dividing by exactly what gets
       * stored keeps the product right to float rounding instead. */
      SculptLayerMask *combined_mask = bke::sculpt_layers::mask_compress(combined, block_size);
      if (combined_mask == nullptr) {
        break;
      }
      Array<float> quantized(combined.size());
      bke::sculpt_layers::mask_expand(*combined_mask, quantized.as_mutable_span());
      for (const int64_t i : survivor_data.index_range()) {
        /* `quantized` is cut to the domain while the data buffer can be longer on grids. */
        if (i >= quantized.size()) {
          break;
        }
        /* Zero means every participant was zero here, so the sum is zero too — not an indeterminate
         * form. The one lossy case is a small non-zero maximum quantizing to zero, which drops a
         * contribution of at most 1/255 of its weight. */
        survivor_data[i] = (quantized[i] > 0.0f) ? survivor_data[i] / quantized[i] : float3(0.0f);
      }
      survivor.base.mask = combined_mask;
      break;
    }
    case MergeMaskPolicy::KeepGroup:
      if (keep_group != nullptr && keep_group->base.mask != nullptr) {
        /* Copied, not moved. #remove_folder_subtree_with_undo captures the folder as
         * #PayloadCapture::NodeRemoved, and moving the mask out first would put a *maskless* folder
         * into that payload — undo would then restore an empty folder and the mask would be gone for
         * good. Capturing the folder's mask separately instead would name one node in two payloads,
         * which this module's undo ordering rules exist to avoid. A copy sidesteps both. */
        survivor.base.mask = bke::sculpt_layers::mask_copy(*keep_group->base.mask);
      }
      break;
  }
}

/**
 * Refuse a merge whose participants do not share the survivor's surviving folder chain while that
 * chain carries a mask. True when refused (and reported).
 *
 * Nothing dissolves in the layer merges, so the survivor's chain goes on attenuating the result. If a
 * participant did not sit under that chain, its content would have to be divided by it to come out
 * right — and where the chain reads zero no value can, since the result is silenced there while the
 * participant's contribution is not. Unrepresentable rather than merely imprecise, hence a refusal.
 *
 * Narrow on purpose: same-folder merges always pass (identical chains cancel), and cross-folder ones
 * pass whenever the survivor's chain is unmasked — which is what lets the fold simply walk to the
 * root for the sources.
 */
static bool merge_refuse_foreign_chain(wmOperator *op,
                                       const SculptLayer &survivor,
                                       const Span<SculptLayer *> participants)
{
  const bool survivor_chain_masked = bke::sculpt_layers::find_ancestor(
                                         survivor.base.parent,
                                         [](const SculptLayerGroup &group) {
                                           return group.base.mask != nullptr;
                                         }) != nullptr;
  if (!survivor_chain_masked) {
    return false;
  }
  for (const SculptLayer *participant : participants) {
    if (participant->base.parent != survivor.base.parent) {
      BKE_report(op->reports,
                 RPT_ERROR,
                 "Cannot merge layers from different folders while the target layer's folder "
                 "carries a weight mask; merge them inside one folder, or remove that mask");
      return true;
    }
  }
  return false;
}

static bool node_chain_carries_mask(const SculptLayerTreeNode &node)
{
  if (node.mask != nullptr) {
    return true;
  }
  /* Routed through #find_ancestor rather than walking `parent` by hand: that is the module's single
   * cycle-safe ancestor walk, and a chain that closes on itself — a corrupt file, a future editing
   * bug — would otherwise hang this operator instead of merely refusing it. */
  return bke::sculpt_layers::find_ancestor(node.parent, [](const SculptLayerGroup &group) {
           return group.base.mask != nullptr;
         }) != nullptr;
}

/* One sync-group member's resolved participants for a merge-down: the layer synced to the active
 * object's active layer, and the sibling directly below THAT layer in the member's OWN tree.
 *
 * #below carries no sync identity of its own. It is found positionally, by #layer_below on the
 * member's own tree, the same way #layer_move_exec derives its neighbour and #layer_move_to_exec
 * its insertion cursor -- so a member may merge a different pair of rows than the active object
 * did. That is expected rather than an error: trees are independently ordered outside of
 * #sync_uid identity.
 *
 * Resolved once, in the gather pass, and carried forward as pointers rather than re-derived in the
 * fan-out loop -- the same reason #RemoveFanoutMember exists: the merge frees #active with
 * #bke::sculpt_layers::remove, so re-resolving by #sync_uid after any member's turn would
 * dereference freed memory. Safe because #fanout_targets dedups by #Object::data, so no two
 * members ever share a #Mesh and nothing touches this member's tree until its own turn below. */
struct MergeDownFanoutMember {
  Object *object;
  SculptLayer *active;
  SculptLayer *below;
};

static wmOperatorStatus layer_merge_down_exec(bContext *C, wmOperator *op)
{
  OpContext ctx;
  if (!op_context_get(C, op, ctx)) {
    return OPERATOR_CANCELLED;
  }
  SculptLayer *active = bke::sculpt_layers::active_get(*ctx.mesh);
  SculptLayer *below = active ? layer_below(*active) : nullptr;
  if (!below) {
    BKE_report(op->reports, RPT_ERROR, "No layer below to merge into");
    return OPERATOR_CANCELLED;
  }
  /* #effective is 0 for a layer inside a disabled group even though the layer itself is enabled, so
   * the merge math below would write zeros: the surviving layer's content would be destroyed while
   * the "combined surface is unchanged" guarantee held only vacuously. Unlike an individually
   * disabled layer, nobody asked for these to contribute nothing. */
  if ((active->base.flag & SCULPT_LAYER_GROUP_HIDDEN) ||
      (below->base.flag & SCULPT_LAYER_GROUP_HIDDEN))
  {
    BKE_report(op->reports, RPT_ERROR, "Cannot merge sculpt layers inside a disabled group");
    return OPERATOR_CANCELLED;
  }
  /* Neither guard was needed while a masked participant was refused outright — a session implies a
   * mask, so it could not be reached. Now that masks are folded, an open session has to be settled
   * back onto its node first (it parks the weights in the standard mask storage and leaves
   * #SculptLayerTreeNode::mask holding the pre-session snapshot), and a grid session is refused
   * rather than closed, as everywhere else in this module. */
  if (mask_edit_refuse_ccg_rebuild(op, *ctx.object)) {
    return OPERATOR_CANCELLED;
  }
  mask_edit_end(*ctx.object);

  const MergeMaskPolicy policy_raw = merge_mask_policy_get(op);
  /* No folder dissolves here, so there is no group mask to keep and the option collapses to the one
   * that keeps a mask at all. Silently, because the enum is shared with the folder merge and a
   * refusal would be a dead end the user cannot act on. */
  const MergeMaskPolicy policy = (policy_raw == MergeMaskPolicy::KeepGroup) ?
                                     MergeMaskPolicy::Combine :
                                     policy_raw;
  /* #std::array and not a raw C array: #Span converts from the former (see its constructors) and not
   * from the latter. Non-const elements, because #Span<SculptLayer *> will not bind to an array of
   * `const SculptLayer *` either. */
  const std::array<SculptLayer *, 2> participants = {active, below};
  /* #layer_below walks `base.next`, the sibling chain, so these two are always in one folder and this
   * can never fire. Called anyway: it is the same code path as Merge Selected, and a future change to
   * how the neighbour is found must not quietly lose the guard. */
  if (merge_refuse_foreign_chain(op, *below, participants)) {
    return OPERATOR_CANCELLED;
  }

  /* Nothing dissolves, so only the participants' own masks stop acting on the result; the folder
   * chain they share survives to attenuate it exactly as before. */
  const SculptLayerGroup *stop_above = below->base.parent;
  const MaskLayout layout = mask_layout_for_object(*ctx.object);
  const int64_t mask_elem_num = layout.totelem;
  for (const SculptLayer *participant : participants) {
    if (const SculptLayerTreeNode *stale = find_stale_mask_in_fold(
            participant->base, stop_above, mask_elem_num))
    {
      BKE_reportf(op->reports,
                  RPT_ERROR,
                  "The weight mask of '%s' does not match the mesh; fix or remove it before merging",
                  stale->name);
      return OPERATOR_CANCELLED;
    }
  }
  const bool any_masked = node_chain_carries_mask(active->base) ||
                          node_chain_carries_mask(below->base);
  /* See #layer_group_merge_exec: an armed REC makes the composite ignore the layer's masks, so
   * folding them in would move the surface right now. */
  if (any_masked && ((active->base.flag | below->base.flag) & SCULPT_LAYER_REC_EXEMPT)) {
    BKE_report(
        op->reports, RPT_ERROR, "Disarm REC before merging sculpt layers that carry a weight mask");
    return OPERATOR_CANCELLED;
  }

  Main *bmain = CTX_data_main(C);
  const SyncScope sync_scope = sync_scope_get(op);

  /* Gather-then-gate: see #layer_add_exec's own comment for why the fan-out set has to be built
   * before any undo bracket opens. The active object was already validated above and is included
   * unconditionally; every other sync-group member is brought up to date and gated exactly as
   * #op_context_get gates the active object, and then re-runs EVERY one of this operator's own
   * guards against ITS OWN pair of participants.
   *
   * Re-running them is the whole point. The guards ask about weight masks, folder visibility and
   * the REC exemption -- all per-object state that #sync_uid identity says nothing about -- so
   * carrying the active object's verdict over to a member could fold a stale mask into that
   * member's data, zero a hidden folder's content, or move its surface under an armed REC. Every
   * per-object value the fold consumes (#MaskLayout, the element count, `stop_above`, the
   * masked-ness) is therefore recomputed from the member itself, never reused.
   *
   * A member that fails a guard is skipped with a warning naming it and the guard, not a hard
   * cancel: one member's local problem must not abort the merge on all the others. The ACTIVE
   * object's own guards above still hard-cancel the whole operator, unchanged -- it is the row the
   * user pressed, so refusing it refuses the operation.
   *
   * The per-member #flush_pending_multires_base mirrors the one #op_context_get already ran for the
   * active object: this merge rewrites layer data and removes a node from the layer set, so a
   * pending base edit really would be stranded if it were not consumed while the live CCG still
   * matches the stored layers. */
  Vector<Object *> eligible;
  eligible.append(ctx.object);
  Vector<MergeDownFanoutMember> fanout_members;
  for (Object *member : sync_scope_targets(*bmain, *ctx.object, sync_scope)) {
    if (member == ctx.object) {
      continue;
    }
    /* Mirrors #layer_add_exec's gather loop: guarded on an existing session, since a member that
     * was never taken into sculpt mode has none, and #BKE_sculpt_update_object_for_edit
     * unconditionally dereferences it. Refreshed before #is_supported below so a member that IS in
     * sculpt mode but has not built its PBVH yet is not misreported as unsupported. */
    if (member->runtime->sculpt_session != nullptr) {
      BKE_sculpt_update_object_for_edit(ctx.depsgraph, member, false);
    }
    if (!is_supported(*member)) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Merge Sculpt Layer Down: skipping \"%s\" (sculpt layers are not available for "
                  "this object)",
                  member->id.name + 2);
      continue;
    }
    /* #node_find_by_sync_uid resolves the SAME layer wherever it sits in the member's own tree;
     * the member's own active layer is unrelated. A zero #sync_uid (the active layer was never
     * fanned-out-created) makes every member miss here by design, degrading this operator to the
     * single-object behavior it always had. */
    Mesh &member_mesh = mesh_of(*member);
    SculptLayerTreeNode *member_node = bke::sculpt_layers::node_find_by_sync_uid(
        member_mesh, active->base.sync_uid);
    if (!member_node) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Merge Sculpt Layer Down: skipping \"%s\" (no layer synced to the active one)",
                  member->id.name + 2);
      continue;
    }
    /* Defensive: #sync_uid should only ever link a layer to a layer, but the tree is
     * user-editable, so the kind found is not assumed. */
    SculptLayer *member_active = bke::sculpt_layers::node_as_layer(member_node);
    if (!member_active) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Merge Sculpt Layer Down: skipping \"%s\" (the synced node is a group, not a "
                  "layer)",
                  member->id.name + 2);
      continue;
    }
    /* The member's own neighbour, not a second synced node -- see #MergeDownFanoutMember. Its
     * absence is this member's own version of the early-out at the top of this function. */
    SculptLayer *member_below = layer_below(*member_active);
    if (!member_below) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Merge Sculpt Layer Down: skipping \"%s\" (no layer below to merge into)",
                  member->id.name + 2);
      continue;
    }
    /* Where the destination itself carries cross-object identity, the positional answer must agree
     * with it. #layer_move_exec needs no such check -- a reorder that lands differently in a
     * differently-ordered tree destroys nothing -- but this operator FREES a node, so a member
     * whose sibling order differs would silently merge and destroy the wrong pair of layers, with
     * nothing in the UI to say it happened. A zero #sync_uid means the destination was never
     * fanned-out-created and has no counterpart to compare against, so the purely positional
     * neighbour stands, exactly as it did before this check existed. */
    if (below->base.sync_uid != 0 && member_below->base.sync_uid != below->base.sync_uid) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Merge Sculpt Layer Down: skipping \"%s\" (the layer below '%s' is not the "
                  "synced counterpart of '%s')",
                  member->id.name + 2,
                  member_active->base.name,
                  below->base.name);
      continue;
    }
    /* The active object's own disabled-folder guard, re-asked of this member's pair: folder
     * visibility is per-object state, so the active object's participants being visible says
     * nothing about this member's. */
    if ((member_active->base.flag & SCULPT_LAYER_GROUP_HIDDEN) ||
        (member_below->base.flag & SCULPT_LAYER_GROUP_HIDDEN))
    {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Merge Sculpt Layer Down: skipping \"%s\" (its layers are inside a disabled "
                  "group)",
                  member->id.name + 2);
      continue;
    }
    if (bke::object::pbvh_get(*member)->type() == bke::pbvh::Type::Grids) {
      /* Mirrors #op_context_get / #layer_add_exec: consume any pending base sculpt edits first,
       * while the live CCG still matches the stored layers, before the merge below invalidates
       * that. Placed after the cheap gates so a member that is skipped anyway is left alone. */
      flush_pending_multires_base(*member);
    }
    /* The active object's own pair of session guards, in the same order and for the same reasons.
     * #mask_edit_end has to run before every mask-reading guard below, exactly as it does above:
     * it settles an open session's weights back onto the node, and the guards must see the settled
     * mask rather than the pre-session snapshot #SculptLayerTreeNode::mask still holds. */
    if (mask_edit_refuse_ccg_rebuild(op, *member)) {
      continue;
    }
    mask_edit_end(*member);

    const SculptLayerGroup *member_stop_above = member_below->base.parent;
    const int64_t member_mask_elem_num = mask_layout_for_object(*member).totelem;
    /* #merge_refuse_foreign_chain's own condition, spelled out rather than called: that helper
     * reports a hard RPT_ERROR, and a member's local problem has to degrade to a named skip
     * instead of a message that reads like the whole operator failed. As on the active object it
     * can never fire -- #layer_below walks the sibling chain, so the pair is always in one folder
     * -- and it is kept for the same reason: a future change to how the neighbour is found must
     * not quietly lose the guard. */
    if (member_active->base.parent != member_below->base.parent &&
        bke::sculpt_layers::find_ancestor(member_below->base.parent,
                                          [](const SculptLayerGroup &group) {
                                            return group.base.mask != nullptr;
                                          }) != nullptr)
    {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Merge Sculpt Layer Down: skipping \"%s\" (its layers sit in different folders "
                  "and the target's folder carries a weight mask)",
                  member->id.name + 2);
      continue;
    }
    /* Asked of this member's own element count: a mask that describes the active object's mesh
     * says nothing about whether it describes this one. */
    const std::array<SculptLayer *, 2> member_participants = {member_active, member_below};
    const SculptLayerTreeNode *member_stale = nullptr;
    for (const SculptLayer *participant : member_participants) {
      if (const SculptLayerTreeNode *stale = find_stale_mask_in_fold(
              participant->base, member_stop_above, member_mask_elem_num))
      {
        member_stale = stale;
        break;
      }
    }
    if (member_stale) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Merge Sculpt Layer Down: skipping \"%s\" (the weight mask of '%s' does not "
                  "match its mesh)",
                  member->id.name + 2,
                  member_stale->name);
      continue;
    }
    /* See the active object's own guard above: an armed REC makes the composite ignore the layer's
     * masks, so folding them in would move this member's surface right now. */
    if ((node_chain_carries_mask(member_active->base) ||
         node_chain_carries_mask(member_below->base)) &&
        ((member_active->base.flag | member_below->base.flag) & SCULPT_LAYER_REC_EXEMPT))
    {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Merge Sculpt Layer Down: skipping \"%s\" (disarm REC before merging layers that "
                  "carry a weight mask)",
                  member->id.name + 2);
      continue;
    }

    eligible.append(member);
    MergeDownFanoutMember fanout_member;
    fanout_member.object = member;
    fanout_member.active = member_active;
    fanout_member.below = member_below;
    fanout_members.append(fanout_member);
  }

  session_state_ensure(*ctx.object);
  undo::push_begin_multi_object(*CTX_data_scene(C), op, eligible.as_span());
  /* Active object: unchanged from the single-object path. */
  /* Snapshot the surviving layer's pre-merge metadata and data for undo. */
  undo::push_sculpt_layer_data(*ctx.object, *below);

  /* Merging with the grid domain requires both buffers at the canonical size (the layers are
   * validated to the top level, so a missing buffer just means zero displacement). */
  const int64_t n = ctx.grids ? std::max(active->totelem, below->totelem) :
                                element_count(*ctx.object);
  MutableSpan<float3> below_data = bke::sculpt_layers::data_ensure(*below, n);
  if (ctx.grids && active->data && below->level != active->level) {
    below->level = active->level;
  }
  /* Built before the accumulation overwrites the buffer, since it reads the masks as they stand. */
  const Array<float> combined = (policy == MergeMaskPolicy::Combine && any_masked) ?
                                    merge_combined_mask(
                                        participants, *below, stop_above, mask_elem_num) :
                                    Array<float>();

  const float active_eff = effective(*active);
  const float below_eff = effective(*below);
  /* The survivor is scaled over its whole buffer while the source is only added over the part the
   * two share, which is why the two passes are not one loop. */
  Array<float> fold;
  const bool below_masked = any_masked &&
                            gather_fold_mask(below->base, stop_above, mask_elem_num, fold);
  for (const int64_t i : below_data.index_range()) {
    below_data[i] *= below_eff * (below_masked && i < fold.size() ? fold[i] : 1.0f);
  }
  if (active->data) {
    Array<float> active_fold;
    const bool active_masked = any_masked && gather_fold_mask(
                                                 active->base, stop_above, mask_elem_num, active_fold);
    const Span<float3> active_data(static_cast<const float3 *>(active->data), active->totelem);
    const int64_t count = std::min<int64_t>(below_data.size(), active_data.size());
    for (int64_t i = 0; i < count; i++) {
      below_data[i] += active_data[i] * active_eff *
                       (active_masked && i < active_fold.size() ? active_fold[i] : 1.0f);
    }
  }
  below->influence = 1.0f;
  below->base.flag |= SCULPT_LAYER_ENABLED;

  /* No folder dissolves here, so there is no group mask to keep and the last argument is null. */
  merge_settle_mask(
      *ctx.object, *below, below_data, policy, combined, layout.block_size, nullptr);

  Vector<undo::SculptLayerUndoPayload> removed;
  removed.append(undo::sculpt_layer_payload_capture(
      *ctx.mesh, active->base, undo::PayloadCapture::NodeRemoved));
  bke::sculpt_layers::remove(*ctx.mesh, *active);
  bke::sculpt_layers::active_set(*ctx.mesh, below);
  tag_layer_overlays_dirty(*ctx.object);
  /* The active layer changed, and the exemption bit died with the layer that carried it. Left
   * unrefreshed, an armed REC would record into `below` with no layer exempt at all, which is the
   * one state the exemption exists to prevent. Cheap enough to call unconditionally, and it cannot
   * move the surface here: the exemption only ever changes the composite of a *masked* layer, and a
   * masked participant with REC armed was refused above — which is why no #commit_layers_change
   * follows. */
  rec_exemption_refresh(*ctx.object);
  undo::push_sculpt_layer_list_change(*ctx.object, std::move(removed), {}, false);

  /* Fan out: mirror the active object's own body exactly, scoped to each member's pair -- resolved
   * in the gather pass, before anything was freed. Every per-object quantity is recomputed from
   * the member rather than reused from `ctx`: a member's PBVH type, element count, mask layout,
   * folder chain and mask presence are all independent of the active object's, and folding the
   * active object's numbers into a member's data would corrupt that member's mesh.
   *
   * Recomputing here rather than caching it in #MergeDownFanoutMember is safe for the same reason
   * the resolved pointers stay valid: #fanout_targets dedups by #Object::data, so nothing between
   * the gather pass and this member's turn has touched this member's mesh. */
  bool any_masked_anywhere = any_masked;
  for (const MergeDownFanoutMember &fanout_member : fanout_members) {
    Object &member = *fanout_member.object;
    Mesh &member_mesh = mesh_of(member);
    SculptLayer &member_active = *fanout_member.active;
    SculptLayer &member_below = *fanout_member.below;
    /* Eligibility, both session guards and every one of this operator's own guards already ran for
     * every entry of `fanout_members` in the gather pass above, before the undo bracket opened --
     * nothing here can fail or need skipping (mirrors #layer_add_exec's own second loop). */
    const bool member_grids = bke::object::pbvh_get(member)->type() == bke::pbvh::Type::Grids;
    /* Mirrors #session_state_ensure's call above for the active object: it has to run before this
     * member's own data changes, or the mesh_base captured here would fold an already-merged state
     * into the base (see #commit_layers_change's comment on capture ordering). */
    session_state_ensure(member);
    undo::push_sculpt_layer_data(member, member_below);

    const SculptLayerGroup *member_stop_above = member_below.base.parent;
    const MaskLayout member_layout = mask_layout_for_object(member);
    const int64_t member_mask_elem_num = member_layout.totelem;
    const std::array<SculptLayer *, 2> member_participants = {&member_active, &member_below};
    const bool member_any_masked = node_chain_carries_mask(member_active.base) ||
                                   node_chain_carries_mask(member_below.base);
    any_masked_anywhere |= member_any_masked;

    const int64_t member_n = member_grids ?
                                 std::max(member_active.totelem, member_below.totelem) :
                                 element_count(member);
    MutableSpan<float3> member_below_data = bke::sculpt_layers::data_ensure(member_below, member_n);
    if (member_grids && member_active.data && member_below.level != member_active.level) {
      member_below.level = member_active.level;
    }
    const Array<float> member_combined =
        (policy == MergeMaskPolicy::Combine && member_any_masked) ?
            merge_combined_mask(
                member_participants, member_below, member_stop_above, member_mask_elem_num) :
            Array<float>();

    const float member_active_eff = effective(member_active);
    const float member_below_eff = effective(member_below);
    Array<float> member_fold;
    const bool member_below_masked =
        member_any_masked &&
        gather_fold_mask(member_below.base, member_stop_above, member_mask_elem_num, member_fold);
    for (const int64_t i : member_below_data.index_range()) {
      member_below_data[i] *= member_below_eff *
                              (member_below_masked && i < member_fold.size() ? member_fold[i] :
                                                                              1.0f);
    }
    if (member_active.data) {
      Array<float> member_active_fold;
      const bool member_active_masked = member_any_masked &&
                                        gather_fold_mask(member_active.base,
                                                         member_stop_above,
                                                         member_mask_elem_num,
                                                         member_active_fold);
      const Span<float3> member_active_data(static_cast<const float3 *>(member_active.data),
                                            member_active.totelem);
      const int64_t member_count = std::min<int64_t>(member_below_data.size(),
                                                     member_active_data.size());
      for (int64_t i = 0; i < member_count; i++) {
        member_below_data[i] += member_active_data[i] * member_active_eff *
                                (member_active_masked && i < member_active_fold.size() ?
                                     member_active_fold[i] :
                                     1.0f);
      }
    }
    member_below.influence = 1.0f;
    member_below.base.flag |= SCULPT_LAYER_ENABLED;

    merge_settle_mask(member,
                      member_below,
                      member_below_data,
                      policy,
                      member_combined,
                      member_layout.block_size,
                      nullptr);

    Vector<undo::SculptLayerUndoPayload> member_removed;
    member_removed.append(undo::sculpt_layer_payload_capture(
        member_mesh, member_active.base, undo::PayloadCapture::NodeRemoved));
    bke::sculpt_layers::remove(member_mesh, member_active);
    tag_layer_overlays_dirty(member);
    /* No #active_set for a member, unlike the active object above: which layer is "active" is local
     * UI state and not synced data -- #layer_select_exec has no fan-out at all for that reason, and
     * #layer_move_exec's own second loop states the same rule. Nothing is left dangling either way:
     * #bke::sculpt_layers::remove hands the active marker to a neighbour when the removed node was
     * the one carrying it.
     *
     * #rec_exemption_refresh IS still called, for the active object's own reason: the exemption bit
     * died with the removed layer here too, and an armed REC left with no layer exempt at all is the
     * one state the exemption exists to prevent. It cannot move this member's surface -- a masked
     * participant with REC armed was skipped in the gather pass -- so no #commit_layers_change
     * follows here either. */
    rec_exemption_refresh(member);
    undo::push_sculpt_layer_list_change(member, std::move(member_removed), {}, false);
  }

  /* `Σ(data · effective · folded mask)` at influence 1 equals the previous combination, so the
   * combined surface (and the live positions) are unchanged — and therefore no
   * #commit_layers_change is needed. The masks that stopped acting on the result are exactly the
   * ones folded into the data, and the folder chain both participants share was left alone because
   * it goes on acting; that is what keeps the two sides equal. Under Combine the equality is up to
   * the mask's `uint8` quantization rather than exact, which #merge_settle_mask divides by the
   * quantized value to keep within float rounding. That reasoning is per object and holds for every
   * fanned-out member for the same reasons, so none of them commits either.
   *
   * That absence of #commit_layers_change is exactly the criterion #layer_move_exec and
   * #layer_group_color_tag_exec close on, and this operator meets it: #finish_multi_object would
   * call #flush_update_done per object, which does work (#fake_neighbors_free, #store_bounds_orig,
   * a conditional #DEG_id_tag_update) the single-object do-path never performed for a merge that
   * provably moves nothing. #push_end_all_ex(false, true) is the same close #finish_multi_object
   * uses internally, minus that per-object flush loop -- the documented multi-object idiom in
   * `sculpt_undo.hh` for exactly this case -- so the fan-out's do-path stays identical to what the
   * active-object-only version always did. (#layer_remove_exec is NOT the precedent here despite
   * being the other node-freeing operator: its own body does call #commit_layers_change, which is
   * why it needs the #UpdateType::Position flush this one does not.) #layers_ui_notify has none of
   * that work (just two #WM_event_add_notifier calls), so it is looped over every member below to
   * still redraw each one -- which is also what publishes `NC_GEOM | ND_DATA` for a member whose
   * layer list just lost a row. */
  undo::push_end_all_ex(false, true);
  /* One report for the whole operation, not one per object: the policy is a single choice made in
   * the operator's own properties. Raised by any participating object that carried a mask, so a
   * member whose masks were folded is still explained even when the active object had none. */
  if (any_masked_anywhere) {
    merge_report_mask_policy(op, policy);
  }
  for (Object *member : eligible) {
    layers_ui_notify(C, *member);
  }
  sync_scope_unlink_after_success(*bmain, *ctx.object, sync_scope);
  return OPERATOR_FINISHED;
}

void SCULPT_OT_layer_merge_down(wmOperatorType *ot)
{
  ot->name = "Merge Sculpt Layer Down";
  ot->idname = "SCULPT_OT_layer_merge_down";
  ot->description =
      "Merge the active sculpt layer into the one below it. Weight masks are handled as the Masks "
      "option says, and the combined surface is unchanged";
  ot->exec = layer_merge_down_exec;
  ot->poll = layers_poll;
  merge_mask_policy_prop(ot);
  sync_scope_rna_def(ot);
  /* No #OPTYPE_UNDO: the operator pushes its own sculpt undo step; a global push would insert a
   * memfile step that does not compose with the stroke SCULPT steps (see #SCULPT_OT_layer_solo_base). */
  ot->flag = OPTYPE_REGISTER;
}

/* One sync-group member's resolved participants for a Merge Selected: the layer synced to the
 * active object's active layer -- the survivor everything is merged into -- and whichever of the
 * active object's source layers have a #sync_uid counterpart in this member's own tree.
 *
 * #sources is a #Vector rather than the fixed pair #MergeDownFanoutMember holds, and it is
 * deliberately allowed to be a SUBSET of the active object's selection. That is the one place this
 * operator departs from the all-or-nothing rule the other multi-node fan-outs follow: the selection
 * here is an arbitrary set of independent synced identities, so a member carrying only some of them
 * still has a well-defined merge to perform for exactly those -- the same partial shape
 * #RemoveFanoutMember already uses. (#MergeDownFanoutMember cannot be partial in that sense: its
 * `below` carries no sync identity at all and is found positionally.) A member where NOT EVEN ONE
 * source resolves has nothing to merge and is skipped whole instead, as is one whose survivor does
 * not resolve -- there is then no destination to merge into.
 *
 * Resolved once, in the gather pass, and carried forward as pointers rather than re-derived in the
 * fan-out loop -- the same reason #RemoveFanoutMember exists, and doubly so here since this
 * operator frees SEVERAL nodes per object with #bke::sculpt_layers::remove. Safe because
 * #fanout_targets dedups by #Object::data, so no two members ever share a #Mesh and nothing touches
 * this member's tree until its own turn below. */
struct MergeSelectedFanoutMember {
  Object *object;
  SculptLayer *active;
  Vector<SculptLayer *> sources;
};

static wmOperatorStatus layer_merge_selected_exec(bContext *C, wmOperator *op)
{
  OpContext ctx;
  if (!op_context_get(C, op, ctx)) {
    return OPERATOR_CANCELLED;
  }
  SculptLayer *active = bke::sculpt_layers::active_get(*ctx.mesh);
  if (!active) {
    return OPERATOR_CANCELLED;
  }

  /* Sources: every selected layer except the active one (a self-merge is a no-op). Mirrors
   * #layer_merge_down_exec generalized from one fixed neighbour to an arbitrary selection. */
  Vector<SculptLayer *> sources;
  for (SculptLayer *layer : bke::sculpt_layers::layers(*ctx.mesh)) {
    if (layer != active && (layer->base.flag & SCULPT_LAYER_SELECTED)) {
      sources.append(layer);
    }
  }
  if (sources.is_empty()) {
    BKE_report(op->reports, RPT_ERROR, "No selected layers to merge");
    return OPERATOR_CANCELLED;
  }
  /* See #layer_merge_down_exec: at a group-hidden layer's weight of 0 the merge silently zeroes the
   * participants' content instead of preserving the combined surface. */
  bool any_group_hidden = (active->base.flag & SCULPT_LAYER_GROUP_HIDDEN) != 0;
  for (const SculptLayer *source : sources) {
    any_group_hidden |= (source->base.flag & SCULPT_LAYER_GROUP_HIDDEN) != 0;
  }
  if (any_group_hidden) {
    BKE_report(op->reports, RPT_ERROR, "Cannot merge sculpt layers inside a disabled group");
    return OPERATOR_CANCELLED;
  }
  /* See #layer_merge_down_exec for why both guards are needed only now. */
  if (mask_edit_refuse_ccg_rebuild(op, *ctx.object)) {
    return OPERATOR_CANCELLED;
  }
  mask_edit_end(*ctx.object);

  const MergeMaskPolicy policy_raw = merge_mask_policy_get(op);
  /* No folder dissolves here, so KeepGroup has no group mask to keep and collapses to Combine, as in
   * #layer_merge_down_exec. */
  const MergeMaskPolicy policy = (policy_raw == MergeMaskPolicy::KeepGroup) ?
                                     MergeMaskPolicy::Combine :
                                     policy_raw;
  /* The survivor first, so #merge_combined_mask and the accumulation walk one list. */
  Vector<SculptLayer *> participants;
  participants.append(active);
  participants.extend(sources);
  /* This is where the guard really works: sources may sit in folders the active layer does not, and
   * a masked chain over the survivor cannot be divided out of their content. */
  if (merge_refuse_foreign_chain(op, *active, participants)) {
    return OPERATOR_CANCELLED;
  }

  const SculptLayerGroup *stop_above = active->base.parent;
  const MaskLayout layout = mask_layout_for_object(*ctx.object);
  const int64_t mask_elem_num = layout.totelem;
  for (const SculptLayer *participant : participants) {
    if (const SculptLayerTreeNode *stale = find_stale_mask_in_fold(
            participant->base, stop_above, mask_elem_num))
    {
      BKE_reportf(op->reports,
                  RPT_ERROR,
                  "The weight mask of '%s' does not match the mesh; fix or remove it before merging",
                  stale->name);
      return OPERATOR_CANCELLED;
    }
  }
  bool any_masked = false;
  for (const SculptLayer *participant : participants) {
    any_masked |= node_chain_carries_mask(participant->base);
  }
  /* See #layer_group_merge_exec: an armed REC makes the composite ignore that layer's masks. */
  if (any_masked) {
    for (const SculptLayer *participant : participants) {
      if (participant->base.flag & SCULPT_LAYER_REC_EXEMPT) {
        BKE_report(op->reports,
                   RPT_ERROR,
                   "Disarm REC before merging sculpt layers that carry a weight mask");
        return OPERATOR_CANCELLED;
      }
    }
  }

  Main *bmain = CTX_data_main(C);

  /* Gather-then-gate: see #layer_add_exec's own comment for why the fan-out set has to be built
   * before any undo bracket opens. The active object was already validated above and is included
   * unconditionally; every other sync-group member is brought up to date and gated exactly as
   * #op_context_get gates the active object, and then re-runs EVERY one of this operator's own
   * guards against ITS OWN set of participants.
   *
   * Re-running them is the whole point, for the reasons spelled out in #layer_merge_down_exec's
   * matching comment: the guards ask about weight masks, folder visibility and the REC exemption,
   * all per-object state that #sync_uid identity says nothing about. Every per-object value the
   * fold consumes (#MaskLayout, the element count, `stop_above`, the masked-ness) is therefore
   * recomputed from the member itself, never reused.
   *
   * Where this differs from #layer_merge_down_exec is participation: a member takes part with
   * whichever SUBSET of the selection resolves on it, rather than being skipped for the first
   * missing source -- see #MergeSelectedFanoutMember for why a partial set is still a well-defined
   * merge here. Only a member with no survivor, or with no source at all, has nothing to do.
   *
   * A member that fails a guard is skipped with a warning naming it and the guard, not a hard
   * cancel: one member's local problem must not abort the merge on all the others. The ACTIVE
   * object's own guards above still hard-cancel the whole operator, unchanged -- it is the row the
   * user pressed, so refusing it refuses the operation.
   *
   * The per-member #flush_pending_multires_base mirrors the one #op_context_get already ran for the
   * active object: this merge rewrites layer data and removes nodes from the layer set, so a
   * pending base edit really would be stranded if it were not consumed while the live CCG still
   * matches the stored layers. */
  Vector<Object *> eligible;
  eligible.append(ctx.object);
  Vector<MergeSelectedFanoutMember> fanout_members;
  for (Object *member : fanout_targets(*bmain, *ctx.object)) {
    if (member == ctx.object) {
      continue;
    }
    /* Mirrors #layer_add_exec's gather loop: guarded on an existing session, since a member that
     * was never taken into sculpt mode has none, and #BKE_sculpt_update_object_for_edit
     * unconditionally dereferences it. Refreshed before #is_supported below so a member that IS in
     * sculpt mode but has not built its PBVH yet is not misreported as unsupported. */
    if (member->runtime->sculpt_session != nullptr) {
      BKE_sculpt_update_object_for_edit(ctx.depsgraph, member, false);
    }
    if (!is_supported(*member)) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Merge Selected Sculpt Layers: skipping \"%s\" (sculpt layers are not available "
                  "for this object)",
                  member->id.name + 2);
      continue;
    }
    /* The survivor first: it is the destination everything else is folded into, so a member that
     * cannot resolve it has nowhere to merge and is skipped whole -- unlike a missing source, which
     * only shrinks the set. #node_find_by_sync_uid resolves the SAME layer wherever it sits in the
     * member's own tree; the member's own active layer is unrelated. A zero #sync_uid (the active
     * layer was never fanned-out-created) makes every member miss here by design, degrading this
     * operator to the single-object behavior it always had. */
    Mesh &member_mesh = mesh_of(*member);
    SculptLayerTreeNode *member_node = bke::sculpt_layers::node_find_by_sync_uid(
        member_mesh, active->base.sync_uid);
    if (!member_node) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Merge Selected Sculpt Layers: skipping \"%s\" (no layer synced to the active "
                  "one to merge into)",
                  member->id.name + 2);
      continue;
    }
    /* Defensive: #sync_uid should only ever link a layer to a layer, but the tree is
     * user-editable, so the kind found is not assumed. */
    SculptLayer *member_active = bke::sculpt_layers::node_as_layer(member_node);
    if (!member_active) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Merge Selected Sculpt Layers: skipping \"%s\" (the synced node is a group, not "
                  "a layer)",
                  member->id.name + 2);
      continue;
    }
    /* Each source resolved by its OWN #sync_uid, independently of the others: they are unrelated
     * identities and nothing says a member must carry all of them. The kind check is folded into
     * the single #node_as_layer call (it returns null for a group), as in #layer_remove_exec. */
    Vector<SculptLayer *> member_sources;
    Vector<const char *> missing_names;
    for (const SculptLayer *source : sources) {
      SculptLayer *member_source = bke::sculpt_layers::node_as_layer(
          bke::sculpt_layers::node_find_by_sync_uid(member_mesh, source->base.sync_uid));
      if (!member_source) {
        missing_names.append(source->base.name);
        continue;
      }
      /* #node_find_by_sync_uid is first-match, so two of the active object's own layers sharing one
       * non-zero #sync_uid would resolve to the SAME member layer twice -- and the removal loop
       * below would double-free it (see #layer_remove_exec's longer note: #bke::sculpt_layers::
       * remove unlinks and deletes the node, so a second call is a use-after-free). The survivor is
       * excluded for the same reason and a worse outcome: merging the destination into itself and
       * then freeing it would destroy the very layer the result lives in. Neither is reachable on
       * the active object, whose `sources` are distinct pointers that exclude `active` by
       * construction, and neither is produced by duplication any more -- but both are still
       * reachable from files saved before #bke::sculpt_layers::duplicate started clearing the
       * copy's #sync_uid. Silently folded rather than reported: the collision is the file's, not
       * this member's. */
      if (member_source != member_active && !member_sources.contains(member_source)) {
        member_sources.append(member_source);
      }
    }
    if (member_sources.is_empty()) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Merge Selected Sculpt Layers: skipping \"%s\" (no synced layers to merge)",
                  member->id.name + 2);
      continue;
    }
    if (!missing_names.is_empty()) {
      /* Not a skip: the rest of the selection does have a synced counterpart here, and merging
       * those is worth doing even though this member cannot keep the whole set in lock-step. The
       * surface stays unchanged either way -- the identity the merge preserves is a sum over
       * whichever layers actually take part. */
      std::string joined = missing_names[0];
      for (const char *name : missing_names.as_span().drop_front(1)) {
        joined += ", ";
        joined += name;
      }
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Merge Selected Sculpt Layers: \"%s\" has no synced counterpart for: %s (its "
                  "other synced layers are still merged)",
                  member->id.name + 2,
                  joined.c_str());
    }
    /* The active object's own disabled-folder guard, re-asked of this member's own participants:
     * folder visibility is per-object state, so the active object's participants being visible says
     * nothing about this member's. */
    bool member_group_hidden = (member_active->base.flag & SCULPT_LAYER_GROUP_HIDDEN) != 0;
    for (const SculptLayer *member_source : member_sources) {
      member_group_hidden |= (member_source->base.flag & SCULPT_LAYER_GROUP_HIDDEN) != 0;
    }
    if (member_group_hidden) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Merge Selected Sculpt Layers: skipping \"%s\" (its layers are inside a disabled "
                  "group)",
                  member->id.name + 2);
      continue;
    }
    if (bke::object::pbvh_get(*member)->type() == bke::pbvh::Type::Grids) {
      /* Mirrors #op_context_get / #layer_add_exec: consume any pending base sculpt edits first,
       * while the live CCG still matches the stored layers, before the merge below invalidates
       * that. Placed after the cheap gates so a member that is skipped anyway is left alone. */
      flush_pending_multires_base(*member);
    }
    /* The active object's own pair of session guards, in the same order and for the same reasons.
     * #mask_edit_end has to run before every mask-reading guard below, exactly as it does above:
     * it settles an open session's weights back onto the node, and the guards must see the settled
     * mask rather than the pre-session snapshot #SculptLayerTreeNode::mask still holds. */
    if (mask_edit_refuse_ccg_rebuild(op, *member)) {
      continue;
    }
    mask_edit_end(*member);

    Vector<SculptLayer *> member_participants;
    member_participants.append(member_active);
    member_participants.extend(member_sources);
    const SculptLayerGroup *member_stop_above = member_active->base.parent;
    const int64_t member_mask_elem_num = mask_layout_for_object(*member).totelem;
    /* #merge_refuse_foreign_chain's own condition, spelled out rather than called: that helper
     * reports a hard RPT_ERROR, and a member's local problem has to degrade to a named skip instead
     * of a message that reads like the whole operator failed. Unlike in #layer_merge_down_exec this
     * one really can fire -- the selection is free to span folders -- and it matters for the same
     * reason it does on the active object: a masked chain over the survivor cannot be divided out
     * of a source that never sat under it. */
    if (bke::sculpt_layers::find_ancestor(member_active->base.parent,
                                          [](const SculptLayerGroup &group) {
                                            return group.base.mask != nullptr;
                                          }) != nullptr)
    {
      bool member_foreign = false;
      for (const SculptLayer *participant : member_participants) {
        member_foreign |= participant->base.parent != member_active->base.parent;
      }
      if (member_foreign) {
        BKE_reportf(op->reports,
                    RPT_WARNING,
                    "Merge Selected Sculpt Layers: skipping \"%s\" (its layers sit in different "
                    "folders and the target's folder carries a weight mask)",
                    member->id.name + 2);
        continue;
      }
    }
    /* Asked of this member's own element count: a mask that describes the active object's mesh says
     * nothing about whether it describes this one. */
    const SculptLayerTreeNode *member_stale = nullptr;
    for (const SculptLayer *participant : member_participants) {
      if (const SculptLayerTreeNode *stale = find_stale_mask_in_fold(
              participant->base, member_stop_above, member_mask_elem_num))
      {
        member_stale = stale;
        break;
      }
    }
    if (member_stale) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Merge Selected Sculpt Layers: skipping \"%s\" (the weight mask of '%s' does not "
                  "match its mesh)",
                  member->id.name + 2,
                  member_stale->name);
      continue;
    }
    /* See the active object's own guard above: an armed REC makes the composite ignore the layer's
     * masks, so folding them in would move this member's surface right now. */
    bool member_any_masked = false;
    bool member_rec_exempt = false;
    for (const SculptLayer *participant : member_participants) {
      member_any_masked |= node_chain_carries_mask(participant->base);
      member_rec_exempt |= (participant->base.flag & SCULPT_LAYER_REC_EXEMPT) != 0;
    }
    if (member_any_masked && member_rec_exempt) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Merge Selected Sculpt Layers: skipping \"%s\" (disarm REC before merging layers "
                  "that carry a weight mask)",
                  member->id.name + 2);
      continue;
    }

    eligible.append(member);
    MergeSelectedFanoutMember fanout_member;
    fanout_member.object = member;
    fanout_member.active = member_active;
    fanout_member.sources = std::move(member_sources);
    fanout_members.append(std::move(fanout_member));
  }

  session_state_ensure(*ctx.object);
  undo::push_begin_multi_object(*CTX_data_scene(C), op, eligible.as_span());
  /* Active object: unchanged from the single-object path. */
  /* Snapshot the surviving layer's pre-merge metadata and data for undo. */
  undo::push_sculpt_layer_data(*ctx.object, *active);

  /* Merging with the grid domain requires every buffer at the canonical size (the layers are
   * validated to the top level, so a missing buffer just means zero displacement). */
  int64_t n = ctx.grids ? active->totelem : element_count(*ctx.object);
  if (ctx.grids) {
    for (const SculptLayer *source : sources) {
      n = std::max(n, source->totelem);
    }
  }
  MutableSpan<float3> active_data = bke::sculpt_layers::data_ensure(*active, n);

  /* Built before the accumulation overwrites the buffer, since it reads the masks as they stand. */
  const Array<float> combined = (policy == MergeMaskPolicy::Combine && any_masked) ?
                                    merge_combined_mask(
                                        participants, *active, stop_above, mask_elem_num) :
                                    Array<float>();

  Array<float> fold;
  const float active_eff = effective(*active);
  const bool active_masked = any_masked &&
                             gather_fold_mask(active->base, stop_above, mask_elem_num, fold);
  for (const int64_t i : active_data.index_range()) {
    active_data[i] *= active_eff * (active_masked && i < fold.size() ? fold[i] : 1.0f);
  }
  for (const SculptLayer *source : sources) {
    if (ctx.grids && source->data && active->level != source->level) {
      active->level = source->level;
    }
    if (!source->data) {
      continue;
    }
    const float source_eff = effective(*source);
    const bool source_masked = any_masked &&
                               gather_fold_mask(source->base, stop_above, mask_elem_num, fold);
    const Span<float3> source_data(static_cast<const float3 *>(source->data), source->totelem);
    const int64_t count = std::min<int64_t>(active_data.size(), source_data.size());
    for (int64_t i = 0; i < count; i++) {
      /* `fold` is cut to the domain while the data buffers can be longer on grids. */
      active_data[i] += source_data[i] * source_eff *
                        (source_masked && i < fold.size() ? fold[i] : 1.0f);
    }
  }
  active->influence = 1.0f;
  active->base.flag |= SCULPT_LAYER_ENABLED;

  /* No folder dissolves here, so there is no group mask to keep and the last argument is null. */
  merge_settle_mask(
      *ctx.object, *active, active_data, policy, combined, layout.block_size, nullptr);

  /* Every payload is captured while the list is still intact, so each records the neighbour it
   * really followed; undo then re-inserts them in capture order and rebuilds the original stack.
   * Capturing and removing one at a time would make each layer record a stale neighbour instead
   * (see #SCULPT_OT_layer_bake for the same trap). Capturing does not unlink, so removal is a
   * second pass. */
  Vector<undo::SculptLayerUndoPayload> removed;
  for (SculptLayer *source : sources) {
    removed.append(undo::sculpt_layer_payload_capture(
        *ctx.mesh, source->base, undo::PayloadCapture::NodeRemoved));
  }
  for (SculptLayer *source : sources) {
    bke::sculpt_layers::remove(*ctx.mesh, *source);
  }
  bke::sculpt_layers::active_set(*ctx.mesh, active);
  tag_layer_overlays_dirty(*ctx.object);
  undo::push_sculpt_layer_list_change(*ctx.object, std::move(removed), {}, false);

  /* Fan out: mirror the active object's own body exactly, scoped to each member's own survivor and
   * its own resolved subset of the selection -- both resolved in the gather pass, before anything
   * was freed. Every per-object quantity is recomputed from the member rather than reused from
   * `ctx`: a member's PBVH type, element count, mask layout, folder chain and mask presence are all
   * independent of the active object's, and folding the active object's numbers into a member's
   * data would corrupt that member's mesh.
   *
   * Recomputing here rather than caching it in #MergeSelectedFanoutMember is safe for the same
   * reason the resolved pointers stay valid: #fanout_targets dedups by #Object::data, so nothing
   * between the gather pass and this member's turn has touched this member's mesh. */
  bool any_masked_anywhere = any_masked;
  for (const MergeSelectedFanoutMember &fanout_member : fanout_members) {
    Object &member = *fanout_member.object;
    Mesh &member_mesh = mesh_of(member);
    SculptLayer &member_active = *fanout_member.active;
    const Span<SculptLayer *> member_sources = fanout_member.sources;
    /* Eligibility, both session guards and every one of this operator's own guards already ran for
     * every entry of `fanout_members` in the gather pass above, before the undo bracket opened --
     * nothing here can fail or need skipping (mirrors #layer_add_exec's own second loop). */
    const bool member_grids = bke::object::pbvh_get(member)->type() == bke::pbvh::Type::Grids;
    /* Mirrors #session_state_ensure's call above for the active object: it has to run before this
     * member's own data changes, or the mesh_base captured here would fold an already-merged state
     * into the base (see #commit_layers_change's comment on capture ordering). */
    session_state_ensure(member);
    undo::push_sculpt_layer_data(member, member_active);

    Vector<SculptLayer *> member_participants;
    member_participants.append(&member_active);
    member_participants.extend(member_sources);
    const SculptLayerGroup *member_stop_above = member_active.base.parent;
    const MaskLayout member_layout = mask_layout_for_object(member);
    const int64_t member_mask_elem_num = member_layout.totelem;
    bool member_any_masked = false;
    for (const SculptLayer *participant : member_participants) {
      member_any_masked |= node_chain_carries_mask(participant->base);
    }
    any_masked_anywhere |= member_any_masked;

    int64_t member_n = member_grids ? member_active.totelem : element_count(member);
    if (member_grids) {
      for (const SculptLayer *member_source : member_sources) {
        member_n = std::max(member_n, member_source->totelem);
      }
    }
    MutableSpan<float3> member_active_data = bke::sculpt_layers::data_ensure(member_active,
                                                                            member_n);

    const Array<float> member_combined =
        (policy == MergeMaskPolicy::Combine && member_any_masked) ?
            merge_combined_mask(
                member_participants, member_active, member_stop_above, member_mask_elem_num) :
            Array<float>();

    Array<float> member_fold;
    const float member_active_eff = effective(member_active);
    const bool member_active_masked =
        member_any_masked &&
        gather_fold_mask(member_active.base, member_stop_above, member_mask_elem_num, member_fold);
    for (const int64_t i : member_active_data.index_range()) {
      member_active_data[i] *= member_active_eff * (member_active_masked && i < member_fold.size() ?
                                                        member_fold[i] :
                                                        1.0f);
    }
    for (const SculptLayer *member_source : member_sources) {
      if (member_grids && member_source->data && member_active.level != member_source->level) {
        member_active.level = member_source->level;
      }
      if (!member_source->data) {
        continue;
      }
      const float member_source_eff = effective(*member_source);
      const bool member_source_masked = member_any_masked &&
                                        gather_fold_mask(member_source->base,
                                                         member_stop_above,
                                                         member_mask_elem_num,
                                                         member_fold);
      const Span<float3> member_source_data(static_cast<const float3 *>(member_source->data),
                                            member_source->totelem);
      const int64_t member_count = std::min<int64_t>(member_active_data.size(),
                                                     member_source_data.size());
      for (int64_t i = 0; i < member_count; i++) {
        member_active_data[i] += member_source_data[i] * member_source_eff *
                                 (member_source_masked && i < member_fold.size() ? member_fold[i] :
                                                                                   1.0f);
      }
    }
    member_active.influence = 1.0f;
    member_active.base.flag |= SCULPT_LAYER_ENABLED;

    merge_settle_mask(member,
                      member_active,
                      member_active_data,
                      policy,
                      member_combined,
                      member_layout.block_size,
                      nullptr);

    /* Capture every payload before removing any of them, for the reason spelled out on the active
     * object's own two passes above: a payload captured after an earlier sibling was unlinked would
     * record a stale neighbour. */
    Vector<undo::SculptLayerUndoPayload> member_removed;
    for (SculptLayer *member_source : member_sources) {
      member_removed.append(undo::sculpt_layer_payload_capture(
          member_mesh, member_source->base, undo::PayloadCapture::NodeRemoved));
    }
    for (SculptLayer *member_source : member_sources) {
      bke::sculpt_layers::remove(member_mesh, *member_source);
    }
    tag_layer_overlays_dirty(member);
    /* No #active_set for a member, unlike the active object above: which layer is "active" is local
     * UI state and not synced data -- #layer_select_exec has no fan-out at all for that reason, and
     * #layer_move_exec's own second loop states the same rule. Nothing is left dangling either way:
     * #bke::sculpt_layers::remove hands the active marker to a neighbour when the removed node was
     * the one carrying it.
     *
     * #rec_exemption_refresh IS called here even though the active object's own body above does not
     * call it, and the asymmetry is the point. On the active object the survivor is by definition
     * that object's #active_get, so the layer #rec_exempt_set marks is the one layer this operator
     * never frees -- an armed REC cannot be stranded, and skipping the refresh is provably safe. A
     * member's survivor carries no such guarantee: `member_active` is resolved by #sync_uid and, as
     * the gather pass above says, the member's own active layer is unrelated to it. That member's
     * #active_get may well be one of `member_sources`, which the loop just freed, handing the
     * active marker to a sibling and leaving REC armed with NO layer exempt -- the one state the
     * exemption exists to prevent. The gather pass does not catch it either: it only refuses
     * masked-and-REC-exempt members, so an all-unmasked one reaches here and only breaks later,
     * once the new active layer acquires a mask. Refreshed unconditionally, exactly as
     * #layer_merge_down_exec's own member loop does, and it cannot move this member's surface for
     * that same operator's reason: the exemption only ever changes the composite of a *masked*
     * layer, and a masked participant with REC armed was skipped in the gather pass -- which is why
     * no #commit_layers_change follows here either. */
    rec_exemption_refresh(member);
    undo::push_sculpt_layer_list_change(member, std::move(member_removed), {}, false);
  }

  /* `Σ(data · effective · folded mask)` at influence 1 equals the previous combination, so the
   * combined surface (and the live positions) are unchanged — and therefore no
   * #commit_layers_change is needed. See #layer_merge_down_exec for why the equality holds and for
   * the one way Combine departs from it. That reasoning is per object and holds for every
   * fanned-out member for the same reasons, so none of them commits either.
   *
   * That absence of #commit_layers_change is exactly the criterion #layer_move_exec and
   * #layer_group_color_tag_exec close on, and this operator meets it, so it closes the same way:
   * #finish_multi_object would call #flush_update_done per object, which does work the
   * single-object do-path never performed for a merge that provably moves nothing.
   * #push_end_all_ex(false, true) is the same close #finish_multi_object uses internally, minus
   * that per-object flush loop -- the documented multi-object idiom in `sculpt_undo.hh` for exactly
   * this case. #layers_ui_notify has none of that work (just two #WM_event_add_notifier calls), so
   * it is looped over every member below to still redraw each one -- which is also what publishes
   * `NC_GEOM | ND_DATA` for a member whose layer list just lost rows. */
  undo::push_end_all_ex(false, true);
  /* One report for the whole operation, not one per object: the policy is a single choice made in
   * the operator's own properties. Raised by any participating object that carried a mask, so a
   * member whose masks were folded is still explained even when the active object had none. */
  if (any_masked_anywhere) {
    merge_report_mask_policy(op, policy);
  }
  for (Object *member : eligible) {
    layers_ui_notify(C, *member);
  }
  return OPERATOR_FINISHED;
}

void SCULPT_OT_layer_merge_selected(wmOperatorType *ot)
{
  ot->name = "Merge Selected Sculpt Layers";
  ot->idname = "SCULPT_OT_layer_merge_selected";
  ot->description =
      "Merge the selected sculpt layers into the active one. Weight masks are handled as the Masks "
      "option says, and the combined surface is unchanged";
  ot->exec = layer_merge_selected_exec;
  ot->poll = layers_poll;
  merge_mask_policy_prop(ot);
  /* No #OPTYPE_UNDO: the operator pushes its own sculpt undo step; a global push would insert a
   * memfile step that does not compose with the stroke SCULPT steps (see #SCULPT_OT_layer_solo_base). */
  ot->flag = OPTYPE_REGISTER;
}

/* One fan-out target of #SCULPT_OT_layer_bake / the bootstrap path of
 * #SCULPT_OT_layer_bake_to_shape_key: the member and ITS own full layer list, captured in the gather
 * pass. Carrying the list rather than re-deriving it in the fan-out loop is mandatory --
 * #bke::sculpt_layers::remove invalidates the cached span, and the active object's removals run
 * before any member is touched. Safe because #fanout_targets dedups by #Object::data. */
struct BakeFanoutMember {
  Object *object;
  Vector<SculptLayer *> layers;
};

/* Whole-tree fan-out: bake drops EVERY layer on each eligible sync-group member (no sync_uid match). */
static wmOperatorStatus layer_bake_exec(bContext *C, wmOperator *op)
{
  OpContext ctx;
  if (!op_context_get(C, op, ctx)) {
    return OPERATOR_CANCELLED;
  }
  /* Baking keeps the combined result and drops all layers: their effect becomes part of the base
   * geometry. Which carrier that is depends on the session:
   * - Multires: fold each enabled layer's tangent displacement into the base MDisps (an exact
   *   linear operation in tangent space).
   * - Mesh without shape keys: the live positions already contain the contributions, so only the
   *   runtime base has to be re-derived.
   * - Mesh with shape keys: the positions and the key blocks hold the un-layered basis and the
   *   layers are composed on top at evaluation, so dropping the layer list alone would delete the
   *   sculpted form. The combined layer contribution becomes one new shape key at value 1 (see
   *   #bke::sculpt_layers::bake_vert_layers_into_new_shape_key), which keeps the surface as it is
   *   while leaving the baked result mutable / dial-able like any other key. Absolute keys have no
   *   such dial, so there the contribution is folded into every block instead. */
  Main *bmain = CTX_data_main(C);
  const SyncScope sync_scope = sync_scope_get(op);

  /* Gather-then-gate: see #layer_add_exec's own comment for why the fan-out set has to be built
   * before any undo bracket opens. Whole-tree (not find-shaped): every eligible member gets the
   * same full-stack bake; there is no per-node #sync_uid gate. The active object was already
   * validated by #op_context_get and is included unconditionally.
   *
   * Unlike #layer_add_exec / #layer_clear_exec this gather does NOT call
   * #mask_edit_refuse_ccg_rebuild on a member: bake is one of the operators that *closes* an open
   * mask-edit session rather than refusing on its account (see #layer_add_exec's comment on that
   * split), and the per-member loop below closes each member's own session unconditionally. */
  Vector<Object *> eligible;
  eligible.append(ctx.object);
  Vector<BakeFanoutMember> fanout_members;
  for (Object *member : sync_scope_targets(*bmain, *ctx.object, sync_scope)) {
    if (member == ctx.object) {
      continue;
    }
    /* Mirrors #layer_add_exec's gather loop: guarded on an existing session, since a member that
     * was never taken into sculpt mode has none, and #BKE_sculpt_update_object_for_edit
     * unconditionally dereferences it. Refreshed before #is_supported below so a member that IS in
     * sculpt mode but has not built its PBVH yet is not misreported as unsupported. */
    if (member->runtime->sculpt_session != nullptr) {
      BKE_sculpt_update_object_for_edit(ctx.depsgraph, member, false);
    }
    if (!is_supported(*member)) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Bake Sculpt Layers: skipping \"%s\" (sculpt layers are not available for this "
                  "object)",
                  member->id.name + 2);
      continue;
    }
    Mesh &member_mesh = mesh_of(*member);
    /* Captured here, on this member's own tree, and carried forward -- see #BakeFanoutMember.
     * Must happen before ANY #bke::sculpt_layers::remove runs (the active object's included). */
    Vector<SculptLayer *> member_layers(bke::sculpt_layers::layers(member_mesh));
    if (bke::object::pbvh_get(*member)->type() == bke::pbvh::Type::Grids) {
      /* Mirrors #op_context_get / #layer_add_exec: consume any pending base sculpt edits first,
       * while the live CCG still matches the stored layers, before the bake below folds them. */
      flush_pending_multires_base(*member);
    }
    eligible.append(member);
    BakeFanoutMember fanout_member;
    fanout_member.object = member;
    fanout_member.layers = std::move(member_layers);
    fanout_members.append(std::move(fanout_member));
  }

  /* Read before the close below, which clears it. Zero when no session is open, which is the common
   * case. */
  const SculptSession *bake_ss = session_of(*ctx.object);
  const int session_uid = bake_ss ? bake_ss->layers.mask_edit.node_uid : 0;
  /* A bake drops every layer, so it closes an open weight-mask editing session for the same reason
   * #layer_remove_exec does: left open across the deletion of its own node the session becomes
   * unreachable — the row and its mask icon are gone with the layer — while the node's weights stay
   * in the standard mask storage with the user's own mask parked, silently masking every subsequent
   * brush by a layer that no longer exists. Closed *before* the fold below so the bake applies the
   * mask the user is currently painting: the fold reads the mask off the node
   * (#node_mask_for_composite / #grid_masks_for_composite), and during a session the node still
   * holds the pre-session snapshot while the live edits sit in the standard mask storage. */
  /* Before the close, which clears the session struct the parked tool idname lives on. */
  mask_edit_exit_ui(C, *ctx.object);
  mask_edit_end(*ctx.object);
  undo::push_begin_multi_object(*CTX_data_scene(C), op, eligible.as_span());
  /* Active object: unchanged from the single-object path. */
  if (session_uid != 0) {
    undo::push_sculpt_layer_mask_session(*ctx.object, session_uid, false);
  }
  const KeyBlock *bake_key = ctx.grids ?
                                 nullptr :
                                 bke::sculpt_layers::bake_vert_layers_into_new_shape_key(*ctx.mesh);
  /* Every payload is captured while the list is still intact, so each records the layer it really
   * followed; undo then re-inserts them in this order and rebuilds the original stack. Capturing
   * and removing one at a time would make each layer record the head as its neighbour and undo
   * would rebuild the stack reversed. Capturing does not unlink, so the removal is a second pass. */
  /* An owning copy, not the cached span: #bke::sculpt_layers::remove below invalidates the span,
   * and the removal pass has to walk the same set the capture pass did. */
  const Vector<SculptLayer *> all(bke::sculpt_layers::layers(*ctx.mesh));
  Vector<undo::SculptLayerUndoPayload> baked;
  for (SculptLayer *layer : all) {
    if (ctx.grids && layer->domain == SCULPT_LAYER_DOMAIN_GRID && layer->data) {
      BKE_multires_sculpt_layer_apply_to_mdisps(
          *ctx.mesh, *layer, bke::sculpt_layers::effective(*layer));
    }
    else if (!bake_key) {
      /* Absolute shape keys only; a no-op without shape keys, where the live positions already
       * carry the layer. */
      bke::sculpt_layers::apply_vert_layer_to_shape_keys(
          *ctx.mesh, *layer, bke::sculpt_layers::effective(*layer));
    }
    baked.append(undo::sculpt_layer_payload_capture(
        *ctx.mesh, layer->base, undo::PayloadCapture::NodeRemoved));
  }
  for (SculptLayer *layer : all) {
    bke::sculpt_layers::remove(*ctx.mesh, *layer);
  }
  undo::push_sculpt_layer_list_change(*ctx.object, std::move(baked), {}, true);
  if (bake_key) {
    undo::push_sculpt_layer_bake_shape_key(*ctx.object, bake_key->uid);
  }
  /* Tagged by hand because this operator does not recompose — #commit_layers_change, which tags for
   * everything else, is deliberately not called here. The surface is unchanged, but every layer just
   * went away, and with them whatever mask the overlay was drawing. */
  tag_layer_overlays_dirty(*ctx.object);
  /* The combined surface is unchanged; the runtime mesh base now equals the live positions. */
  invalidate_runtime(*ctx.object);
  session_state_ensure(*ctx.object);
  /* No #rec_exemption_refresh: the single-object path never called it, and after every layer is
   * gone the exemption bit has nowhere to live -- it cannot move the (already-unchanged) surface. */

  /* Fan out: mirror the active object's own body exactly on each member's own full layer list,
   * captured in the gather pass before ANY removal ran anywhere. */
  for (const BakeFanoutMember &fanout_member : fanout_members) {
    Object &member = *fanout_member.object;
    Mesh &member_mesh = mesh_of(member);
    const bool member_grids = bke::object::pbvh_get(member)->type() == bke::pbvh::Type::Grids;
    const Span<SculptLayer *> member_all = fanout_member.layers;
    /* Eligibility and the session guards already ran for every entry of `fanout_members` in the
     * gather pass above, before the undo bracket opened -- nothing here can fail or need skipping
     * (mirrors #layer_add_exec's own second loop). */

    /* Read before the close below, which clears it -- this member's own session, not the active
     * object's. Both halves of the close fan out: #mask_edit_end because the member's own layers
     * are about to be destroyed under its own session, and #mask_edit_exit_ui because it is safe
     * to fan out per member (Task 9d1: returns immediately when this object has no open session). */
    const SculptSession *member_ss = session_of(member);
    const int member_session_uid = member_ss ? member_ss->layers.mask_edit.node_uid : 0;
    mask_edit_exit_ui(C, member);
    mask_edit_end(member);
    if (member_session_uid != 0) {
      undo::push_sculpt_layer_mask_session(member, member_session_uid, false);
    }

    const KeyBlock *member_bake_key =
        member_grids ? nullptr :
                       bke::sculpt_layers::bake_vert_layers_into_new_shape_key(member_mesh);
    Vector<undo::SculptLayerUndoPayload> member_baked;
    for (SculptLayer *layer : member_all) {
      if (member_grids && layer->domain == SCULPT_LAYER_DOMAIN_GRID && layer->data) {
        BKE_multires_sculpt_layer_apply_to_mdisps(
            member_mesh, *layer, bke::sculpt_layers::effective(*layer));
      }
      else if (!member_bake_key) {
        bke::sculpt_layers::apply_vert_layer_to_shape_keys(
            member_mesh, *layer, bke::sculpt_layers::effective(*layer));
      }
      member_baked.append(undo::sculpt_layer_payload_capture(
          member_mesh, layer->base, undo::PayloadCapture::NodeRemoved));
    }
    for (SculptLayer *layer : member_all) {
      bke::sculpt_layers::remove(member_mesh, *layer);
    }
    undo::push_sculpt_layer_list_change(member, std::move(member_baked), {}, true);
    if (member_bake_key) {
      undo::push_sculpt_layer_bake_shape_key(member, member_bake_key->uid);
    }
    tag_layer_overlays_dirty(member);
    invalidate_runtime(member);
    session_state_ensure(member);
  }

  /* No #commit_layers_change on the active object or any member (surface unchanged), so
   * #finish_multi_object is not used -- it would call #flush_update_done per object, which does
   * work the single-object do-path never performed. #push_end_all_ex(false, true) is the same close
   * #finish_multi_object uses internally, minus that flush, matching #layer_move_exec /
   * #layer_merge_down_exec. #layers_ui_notify is looped over every member to redraw each one. */
  undo::push_end_all_ex(false, true);
  for (Object *member : eligible) {
    Mesh &member_mesh = mesh_of(*member);
    if (bke::object::pbvh_get(*member)->type() != bke::pbvh::Type::Grids &&
        member_mesh.key != nullptr)
    {
      /* The key data changed and the session's #deform_cos is a snapshot of the pre-bake surface:
       * re-evaluate so the display is rebuilt from the new keys. */
      DEG_id_tag_update(&member_mesh.id, ID_RECALC_GEOMETRY);
    }
    layers_ui_notify(C, *member);
  }
  sync_scope_unlink_after_success(*bmain, *ctx.object, sync_scope);
  return OPERATOR_FINISHED;
}

static const char *layer_bake_confirm_message(const OpContext &ctx)
{
  if (ctx.grids) {
    return IFACE_("All sculpt layers will be baked into the multires displacement.");
  }
  if (ctx.mesh->key == nullptr) {
    return IFACE_(
        "All sculpt layers will be permanently baked into the base geometry and removed.");
  }
  if (ctx.mesh->key->type == KEY_RELATIVE) {
    return IFACE_(
        "This mesh has shape keys, so all sculpt layers will be baked into a new dial-able shape "
        "key.");
  }
  return IFACE_(
      "This mesh has shape keys, so all sculpt layers will be baked into the existing shape "
      "keys.");
}

static wmOperatorStatus layer_bake_invoke(bContext *C, wmOperator *op, const wmEvent * /*event*/)
{
  OpContext ctx;
  if (!op_context_get(C, op, ctx)) {
    return OPERATOR_CANCELLED;
  }
  const char *message = layer_bake_confirm_message(ctx);

  return WM_operator_confirm_ex(
      C, op, IFACE_("Bake Sculpt Layers"), message, IFACE_("Bake"), ui::AlertIcon::Warning, false);
}

void SCULPT_OT_layer_bake(wmOperatorType *ot)
{
  ot->name = "Bake Sculpt Layers";
  ot->idname = "SCULPT_OT_layer_bake";
  ot->description = "Apply all sculpt layers permanently to the base geometry and remove them";
  ot->exec = layer_bake_exec;
  /* Irreversible-feeling from the UI (drops every layer into the base), so confirm before running.
   * A custom invoke tailors the message to the carrier (permanent base fold, multires, or a new /
   * existing shape key when the mesh already has one). */
  ot->invoke = layer_bake_invoke;
  ot->poll = layers_poll;
  /* No #OPTYPE_UNDO: the operator pushes its own sculpt undo step; a global push would insert a
   * memfile step that does not compose with the stroke SCULPT steps (see #SCULPT_OT_layer_solo_base). */
  ot->flag = OPTYPE_REGISTER;
  sync_scope_rna_def(ot);
}

static wmOperatorStatus call_layer_bake_with_scope(bContext *C, wmOperator *op)
{
  PointerRNA props = WM_operator_properties_create("SCULPT_OT_layer_bake");
  RNA_enum_set(&props, "sync_scope", int(sync_scope_get(op)));
  const wmOperatorStatus status = WM_operator_name_call(
      C, "SCULPT_OT_layer_bake", wm::OpCallContext::ExecDefault, &props, nullptr);
  WM_operator_properties_free(&props);
  return status;
}

/* Bootstrap path fans out; existing-Key branch delegates to the now-fanned-out #SCULPT_OT_layer_bake. */
static wmOperatorStatus layer_bake_to_shape_key_exec(bContext *C, wmOperator *op)
{
  OpContext ctx;
  if (!op_context_get(C, op, ctx)) {
    return OPERATOR_CANCELLED;
  }
  if (ctx.grids) {
    BKE_report(op->reports, RPT_ERROR, "Not available for Multires; only Mesh data");
    return OPERATOR_CANCELLED;
  }
  if (bke::sculpt_layers::layers(*ctx.mesh).is_empty()) {
    BKE_report(op->reports, RPT_ERROR, "No sculpt layers to bake");
    return OPERATOR_CANCELLED;
  }
  /* Mesh already has shape keys: #SCULPT_OT_layer_bake already appends one new dial-able relative
   * block to the existing key (its #bake_key / #bake_key_uid undo path). Delegate rather than
   * duplicate that logic; the delegate's own sculpt-undo push provides the single undo step, so
   * this operator adds none of its own here. Mirrors #layer_bake_and_editmode_enter_exec's
   * #wm::OpCallContext::ExecDefault delegation (which is likewise #OPTYPE_UNDO, so no double push).
   * The bootstrap below (and its #created_key undo machinery) runs only when there is no key yet.
   * Once #layer_bake_exec fans out, this branch covers every eligible sync-group member too. */
  if (ctx.mesh->key != nullptr) {
    return call_layer_bake_with_scope(C, op);
  }

  Main *bmain = CTX_data_main(C);
  const SyncScope sync_scope = sync_scope_get(op);

  /* Gather-then-gate: see #layer_add_exec's own comment for why the fan-out set has to be built
   * before any undo bracket opens. Whole-tree bootstrap (not find-shaped): every eligible mesh
   * member that still has layers is baked. Members that already own a #Key take the existing-key
   * bake path inside this same undo step (not a second #BKE_key_add); members without a #Key get
   * the same bootstrap as the active object. Multires members are skipped -- this operator is
   * mesh-only, matching the active-object guard above. Empty-layer members are skipped for the
   * same reason the active object cancels above.
   *
   * Unlike #layer_add_exec this gather does NOT call #mask_edit_refuse_ccg_rebuild: bake closes
   * an open mask-edit session rather than refusing (see #layer_bake_exec). */
  Vector<Object *> eligible;
  eligible.append(ctx.object);
  Vector<BakeFanoutMember> fanout_members;
  for (Object *member : sync_scope_targets(*bmain, *ctx.object, sync_scope)) {
    if (member == ctx.object) {
      continue;
    }
    /* Mirrors #layer_add_exec's gather loop: guarded on an existing session, since a member that
     * was never taken into sculpt mode has none, and #BKE_sculpt_update_object_for_edit
     * unconditionally dereferences it. */
    if (member->runtime->sculpt_session != nullptr) {
      BKE_sculpt_update_object_for_edit(ctx.depsgraph, member, false);
    }
    if (!is_supported(*member)) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Bake Sculpt Layers to Shape Keys: skipping \"%s\" (sculpt layers are not "
                  "available for this object)",
                  member->id.name + 2);
      continue;
    }
    if (bke::object::pbvh_get(*member)->type() == bke::pbvh::Type::Grids) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Bake Sculpt Layers to Shape Keys: skipping \"%s\" (not available for Multires; "
                  "only Mesh data)",
                  member->id.name + 2);
      continue;
    }
    Mesh &member_mesh = mesh_of(*member);
    Vector<SculptLayer *> member_layers(bke::sculpt_layers::layers(member_mesh));
    if (member_layers.is_empty()) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Bake Sculpt Layers to Shape Keys: skipping \"%s\" (no sculpt layers to bake)",
                  member->id.name + 2);
      continue;
    }
    eligible.append(member);
    BakeFanoutMember fanout_member;
    fanout_member.object = member;
    fanout_member.layers = std::move(member_layers);
    fanout_members.append(std::move(fanout_member));
  }

  /* Only the bootstrap path reaches here; the delegating branch above closes the session inside
   * #layer_bake_exec. Same reasoning as there: the layers this drops include the session's own node,
   * and the fold below has to see the mask the user is currently painting. */
  const SculptSession *bake_ss = session_of(*ctx.object);
  const int session_uid = bake_ss ? bake_ss->layers.mask_edit.node_uid : 0;
  /* Before the close, which clears the session struct the parked tool idname lives on. */
  mask_edit_exit_ui(C, *ctx.object);
  mask_edit_end(*ctx.object);

  const short pre_bake_shapenr = ctx.object->shapenr;
  undo::push_begin_multi_object(*CTX_data_scene(C), op, eligible.as_span());
  /* Active object: unchanged from the single-object bootstrap path. */
  if (session_uid != 0) {
    undo::push_sculpt_layer_mask_session(*ctx.object, session_uid, false);
  }

  /* Bootstrap the Key: mirrors #insert_meshkey (object.cc). #BKE_key_add's ID_ME case already
   * strips the layer contribution out of `vert_positions` (see
   * #bke::sculpt_layers::strip_vert_layers_from_positions), so `basis` below is seeded with the
   * un-layered original shape. */
  Key *key = ctx.mesh->key = BKE_key_add(bmain, &ctx.mesh->id);
  key->type = KEY_RELATIVE;
  KeyBlock *basis = BKE_keyblock_add_ctime(key, DATA_("Basis"), false);
  BKE_keyblock_convert_from_mesh(ctx.mesh, key, basis);

  KeyBlock *baked = bke::sculpt_layers::bake_vert_layers_into_new_shape_key(*ctx.mesh);

  /* Captured in list order before anything is unlinked, so undo restores the original stack — see
   * #layer_bake_exec for why the two passes cannot be merged. */
  const Vector<SculptLayer *> all(bke::sculpt_layers::layers(*ctx.mesh));
  Vector<undo::SculptLayerUndoPayload> baked_layers;
  for (SculptLayer *layer : all) {
    baked_layers.append(undo::sculpt_layer_payload_capture(
        *ctx.mesh, layer->base, undo::PayloadCapture::NodeRemoved));
  }
  for (SculptLayer *layer : all) {
    bke::sculpt_layers::remove(*ctx.mesh, *layer);
  }
  undo::push_sculpt_layer_list_change(*ctx.object, std::move(baked_layers), {}, true);
  undo::push_sculpt_layer_bake_to_shape_key(*ctx.object, pre_bake_shapenr);
  /* Tagged by hand for the reason #layer_bake_exec gives: baking does not recompose, so nothing else
   * clears the overlay of the masks that just left with their layers. */
  tag_layer_overlays_dirty(*ctx.object);

  /* Select the usable key as active (matching the pattern #object_shape_key_add uses,
   * object_shapekey.cc): the baked block when it has data, the Basis otherwise (degenerate
   * case — see the design spec's "Degenerate case" section). */
  KeyBlock *active_key = (baked && baked->data) ? baked : basis;
  ctx.object->shapenr = BLI_findindex(&key->block, active_key) + 1;

  /* The combined surface is unchanged; the runtime mesh base now equals the live positions
   * composed through the new key instead of the (now absent) layer list. */
  invalidate_runtime(*ctx.object);
  session_state_ensure(*ctx.object);
  /* No #rec_exemption_refresh: mirrors #layer_bake_exec -- every layer is gone. */

  /* Fan out: each member without a #Key gets the same bootstrap; each member that already has one
   * takes the existing-key bake path (#bake_vert_layers_into_new_shape_key + remove), so a mixed
   * sync group does not get a second Key created on meshes that already own one. */
  for (const BakeFanoutMember &fanout_member : fanout_members) {
    Object &member = *fanout_member.object;
    Mesh &member_mesh = mesh_of(member);
    const Span<SculptLayer *> member_all = fanout_member.layers;

    const SculptSession *member_ss = session_of(member);
    const int member_session_uid = member_ss ? member_ss->layers.mask_edit.node_uid : 0;
    mask_edit_exit_ui(C, member);
    mask_edit_end(member);
    if (member_session_uid != 0) {
      undo::push_sculpt_layer_mask_session(member, member_session_uid, false);
    }

    if (member_mesh.key != nullptr) {
      /* Existing-key path: same fold #layer_bake_exec uses for a mesh that already has shape keys. */
      const KeyBlock *member_bake_key =
          bke::sculpt_layers::bake_vert_layers_into_new_shape_key(member_mesh);
      Vector<undo::SculptLayerUndoPayload> member_baked;
      for (SculptLayer *layer : member_all) {
        if (!member_bake_key) {
          bke::sculpt_layers::apply_vert_layer_to_shape_keys(
              member_mesh, *layer, bke::sculpt_layers::effective(*layer));
        }
        member_baked.append(undo::sculpt_layer_payload_capture(
            member_mesh, layer->base, undo::PayloadCapture::NodeRemoved));
      }
      for (SculptLayer *layer : member_all) {
        bke::sculpt_layers::remove(member_mesh, *layer);
      }
      undo::push_sculpt_layer_list_change(member, std::move(member_baked), {}, true);
      if (member_bake_key) {
        undo::push_sculpt_layer_bake_shape_key(member, member_bake_key->uid);
      }
      tag_layer_overlays_dirty(member);
      invalidate_runtime(member);
      session_state_ensure(member);
      continue;
    }

    /* Bootstrap path for this member: mirrors the active object's body above. */
    const short member_pre_bake_shapenr = member.shapenr;
    Key *member_key = member_mesh.key = BKE_key_add(bmain, &member_mesh.id);
    member_key->type = KEY_RELATIVE;
    KeyBlock *member_basis = BKE_keyblock_add_ctime(member_key, DATA_("Basis"), false);
    BKE_keyblock_convert_from_mesh(&member_mesh, member_key, member_basis);

    KeyBlock *member_baked_kb = bke::sculpt_layers::bake_vert_layers_into_new_shape_key(
        member_mesh);

    Vector<undo::SculptLayerUndoPayload> member_baked_layers;
    for (SculptLayer *layer : member_all) {
      member_baked_layers.append(undo::sculpt_layer_payload_capture(
          member_mesh, layer->base, undo::PayloadCapture::NodeRemoved));
    }
    for (SculptLayer *layer : member_all) {
      bke::sculpt_layers::remove(member_mesh, *layer);
    }
    undo::push_sculpt_layer_list_change(member, std::move(member_baked_layers), {}, true);
    undo::push_sculpt_layer_bake_to_shape_key(member, member_pre_bake_shapenr);
    tag_layer_overlays_dirty(member);

    KeyBlock *member_active_key = (member_baked_kb && member_baked_kb->data) ? member_baked_kb :
                                                                              member_basis;
    member.shapenr = BLI_findindex(&member_key->block, member_active_key) + 1;

    invalidate_runtime(member);
    session_state_ensure(member);
  }

  /* No #commit_layers_change (surface unchanged) -- close like #layer_bake_exec. */
  undo::push_end_all_ex(false, true);
  for (Object *member : eligible) {
    /* Key data always changed on this path (bootstrap or existing-key bake); re-evaluate. */
    DEG_id_tag_update(&mesh_of(*member).id, ID_RECALC_GEOMETRY);
    layers_ui_notify(C, *member);
  }
  sync_scope_unlink_after_success(*bmain, *ctx.object, sync_scope);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus layer_bake_to_shape_key_invoke(bContext *C,
                                                       wmOperator *op,
                                                       const wmEvent * /*event*/)
{
  OpContext ctx;
  if (!op_context_get(C, op, ctx)) {
    return OPERATOR_CANCELLED;
  }
  /* Pre-check the exec guards here so the popup never appears just to be followed by an error
   * (confirm-then-fail). The UI already disables this operator for these states; this covers the
   * search / scripting entry points. */
  if (ctx.grids) {
    BKE_report(op->reports, RPT_ERROR, "Not available for Multires; only Mesh data");
    return OPERATOR_CANCELLED;
  }
  if (bke::sculpt_layers::layers(*ctx.mesh).is_empty()) {
    BKE_report(op->reports, RPT_ERROR, "No sculpt layers to bake");
    return OPERATOR_CANCELLED;
  }
  /* State-aware confirmation, matching what exec (and the delegate #SCULPT_OT_layer_bake) actually
   * does: no key -> bootstrap Basis + Sculpt Layers; a relative key -> one more dial-able block;
   * an absolute key -> the contribution folded into every existing block (no dial, see
   * #bake_vert_layers_into_new_shape_key returning null for non-relative keys). */
  const char *message;
  if (ctx.mesh->key == nullptr) {
    message = IFACE_(
        "All sculpt layers will be baked into a new Basis and a dial-able \"Sculpt Layers\" "
        "shape key.");
  }
  else if (ctx.mesh->key->type == KEY_RELATIVE) {
    message = IFACE_(
        "This mesh already has shape keys. All sculpt layers will be baked into a new dial-able "
        "shape key.");
  }
  else {
    message = IFACE_(
        "This mesh already has shape keys. All sculpt layers will be baked into the existing "
        "shape keys.");
  }
  return WM_operator_confirm_ex(C,
                                op,
                                IFACE_("Bake Sculpt Layers to Shape Keys"),
                                message,
                                IFACE_("Bake"),
                                ui::AlertIcon::Warning,
                                false);
}

void SCULPT_OT_layer_bake_to_shape_key(wmOperatorType *ot)
{
  ot->name = "Bake Sculpt Layers to Shape Keys";
  ot->idname = "SCULPT_OT_layer_bake_to_shape_key";
  ot->description =
      "Convert the sculpt layer stack into a Basis and a dial-able relative Shape Key holding "
      "the combined result";
  ot->exec = layer_bake_to_shape_key_exec;
  /* Irreversible-feeling from the UI (drops every layer into the base), so confirm before running.
   * A custom invoke tailors the message to whether the mesh already has shape keys. */
  ot->invoke = layer_bake_to_shape_key_invoke;
  ot->poll = layers_poll;
  /* No #OPTYPE_UNDO: the operator pushes its own sculpt undo step; a global push would insert a
   * memfile step that does not compose with the stroke SCULPT steps (see #SCULPT_OT_layer_solo_base). */
  ot->flag = OPTYPE_REGISTER;
  sync_scope_rna_def(ot);
}

/* "Bake All Layers" choice of #SCULPT_PT_layer_editmode_confirm (see
 * #OBJECT_OT_editmode_toggle's sculpt-layers warning): bake, then re-enter the Edit Mode toggle
 * with the warning bypassed so the mode switch actually proceeds. The warning can fire from any
 * mode (not just Sculpt Mode), but #SCULPT_OT_layer_bake needs a live sculpt session, so enter
 * Sculpt Mode first if the object is not already in it; #OBJECT_OT_editmode_toggle's own
 * mode-compat handling below exits Sculpt Mode again on the way into Edit Mode. */
/* Active-object-only wrapper for mode toggles: bake fans out via #SCULPT_OT_layer_bake (honoring
 * sync_scope); mode toggles must not fan out. */
static wmOperatorStatus layer_bake_and_editmode_enter_exec(bContext *C, wmOperator *op)
{
  const Object *object = CTX_data_active_object(C);
  if (object && !(object->mode & OB_MODE_SCULPT)) {
    WM_operator_name_call(
        C, "SCULPT_OT_sculptmode_toggle", wm::OpCallContext::ExecDefault, nullptr, nullptr);
  }

  /* Only claim the layers are baked if the bake actually ran. #SCULPT_OT_layer_bake cancels when
   * the object has no usable PBVH (or a dyntopo one), and entering Edit Mode anyway would bypass
   * the warning with the layers still unbaked, silently dropping them on the way out.
   * Sync-group fan-out of the bake itself lives inside #layer_bake_exec; this wrapper deliberately
   * does not also fan out #sculptmode_toggle / #editmode_toggle across the group -- entering Edit
   * Mode on every member is a different product decision and wrong for a confirm that only names
   * the active object. */
  const wmOperatorStatus bake_status = call_layer_bake_with_scope(C, op);
  if (!(bake_status & OPERATOR_FINISHED)) {
    return OPERATOR_CANCELLED;
  }

  PointerRNA props = WM_operator_properties_create("OBJECT_OT_editmode_toggle");
  RNA_boolean_set(&props, "sculpt_layers_bake_confirmed", true);
  const wmOperatorStatus status = WM_operator_name_call(
      C, "OBJECT_OT_editmode_toggle", wm::OpCallContext::ExecDefault, &props, nullptr);
  WM_operator_properties_free(&props);
  return status;
}

static wmOperatorStatus layer_bake_and_editmode_enter_invoke(bContext *C,
                                                             wmOperator *op,
                                                             const wmEvent * /*event*/)
{
  return WM_operator_confirm_message(
      C, op, "All sculpt layers will be merged into the base geometry. Continue?");
}

void SCULPT_OT_layer_bake_and_editmode_enter(wmOperatorType *ot)
{
  ot->name = "Bake All Layers and Enter Edit Mode";
  ot->idname = "SCULPT_OT_layer_bake_and_editmode_enter";
  ot->description =
      "Apply all sculpt layers permanently to the base geometry, then enter Edit Mode";
  ot->exec = layer_bake_and_editmode_enter_exec;
  /* Confirm before merging every layer into the base, same as #SCULPT_OT_layer_bake's own
   * confirm; called via #wm::OpCallContext::ExecDefault from other exec functions in this file,
   * so this only fires for the interactive (menu-clicked) path. */
  ot->invoke = layer_bake_and_editmode_enter_invoke;
  /* Not #layers_poll: unlike the other layer operators this one enters Sculpt Mode itself when
   * needed (see #layer_bake_and_editmode_enter_exec), so it must stay callable from any mode the
   * sculpt-layers Edit Mode warning can fire from. */
  ot->poll = ED_operator_object_active_editable_mesh;
  /* No #OPTYPE_UNDO: the delegated operators (#SCULPT_OT_layer_bake, #OBJECT_OT_editmode_toggle)
   * push their own undo steps; an extra global push would insert a memfile step that does not
   * compose with the stroke SCULPT steps (see #SCULPT_OT_layer_solo_base). */
  ot->flag = OPTYPE_REGISTER;
  sync_scope_rna_def(ot);
}

static wmOperatorStatus layer_clear_exec(bContext *C, wmOperator *op)
{
  OpContext ctx;
  if (!op_context_get(C, op, ctx)) {
    return OPERATOR_CANCELLED;
  }
  if (mask_edit_refuse_ccg_rebuild(op, *ctx.object)) {
    return OPERATOR_CANCELLED;
  }
  SculptLayer *layer = bke::sculpt_layers::active_get(*ctx.mesh);
  if (!layer) {
    return OPERATOR_CANCELLED;
  }
  Main *bmain = CTX_data_main(C);

  /* Gather-then-gate: see #layer_add_exec's own comment for why the fan-out set has to be built
   * before any undo bracket opens. The active object is included unconditionally; every other
   * sync-group member is brought up to date and gated exactly as #op_context_get gates the active
   * object, plus a find-shaped gate #layer_add_exec has no analog for: the member must actually
   * hold a layer synced to the active one. */
  Vector<Object *> eligible;
  eligible.append(ctx.object);
  for (Object *member : fanout_targets(*bmain, *ctx.object)) {
    if (member == ctx.object) {
      continue;
    }
    /* Mirrors #layer_add_exec's gather loop: guarded on an existing session, since a member that
     * was never taken into sculpt mode has none, and #BKE_sculpt_update_object_for_edit
     * unconditionally dereferences it. Refreshed before #is_supported below so a member that IS in
     * sculpt mode but has not built its PBVH yet is not misreported as unsupported. */
    if (member->runtime->sculpt_session != nullptr) {
      BKE_sculpt_update_object_for_edit(ctx.depsgraph, member, false);
    }
    if (!is_supported(*member)) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Clear Sculpt Layer: skipping \"%s\" (sculpt layers are not available for this "
                  "object)",
                  member->id.name + 2);
      continue;
    }
    /* #node_find_by_sync_uid resolves the SAME layer wherever it sits in the member's own tree;
     * the member's own active layer is unrelated. A zero #sync_uid (the active layer was never
     * fanned-out-created) makes every member miss here by design -- see the function's own doc
     * comment -- which degrades this operator to the single-object behavior it always had. */
    Mesh &member_mesh = mesh_of(*member);
    SculptLayerTreeNode *member_node = bke::sculpt_layers::node_find_by_sync_uid(
        member_mesh, layer->base.sync_uid);
    if (!member_node) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Clear Sculpt Layer: skipping \"%s\" (no layer synced to the active one)",
                  member->id.name + 2);
      continue;
    }
    /* Defensive: #sync_uid should only ever link a layer to a layer, but the tree is
     * user-editable, so the kind found is not assumed. */
    if (!bke::sculpt_layers::node_as_layer(member_node)) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Clear Sculpt Layer: skipping \"%s\" (the synced node is a group, not a layer)",
                  member->id.name + 2);
      continue;
    }
    if (bke::object::pbvh_get(*member)->type() == bke::pbvh::Type::Grids) {
      /* Mirrors #op_context_get / #layer_add_exec: consume any pending base sculpt edits first,
       * while the live CCG still matches the stored layers. */
      flush_pending_multires_base(*member);
    }
    if (mask_edit_refuse_ccg_rebuild(op, *member)) {
      continue;
    }
    eligible.append(member);
  }

  session_state_ensure(*ctx.object);
  undo::push_begin_multi_object(*CTX_data_scene(C), op, eligible.as_span());
  /* Active object: unchanged from the single-object path. */
  undo::push_sculpt_layer_data(*ctx.object, *layer);
  bke::sculpt_layers::data_clear(*layer);
  commit_layers_change(*ctx.depsgraph, *ctx.object);

  for (Object *member : eligible) {
    if (member == ctx.object) {
      continue;
    }
    /* Eligibility (#is_supported, sync_uid resolution, #mask_edit_refuse_ccg_rebuild) and the
     * multires pending-base flush already ran for every member of `eligible` in the gather pass
     * above -- nothing here can fail or need skipping (mirrors #layer_add_exec's own second
     * loop). */
    session_state_ensure(*member);
    Mesh &member_mesh = mesh_of(*member);
    SculptLayer *member_layer = bke::sculpt_layers::node_as_layer(
        bke::sculpt_layers::node_find_by_sync_uid(member_mesh, layer->base.sync_uid));
    undo::push_sculpt_layer_data(*member, *member_layer);
    bke::sculpt_layers::data_clear(*member_layer);
    commit_layers_change(*ctx.depsgraph, *member);
  }

  /* #commit_layers_change unconditionally recomposes the surface, on the active object and every
   * fanned-out member alike, so #UpdateType::Position is required the same way #layer_add_exec
   * requires it. */
  undo::finish_multi_object(C, eligible.as_span(), UpdateType::Position);
  layers_ui_notify(C, *ctx.object);
  return OPERATOR_FINISHED;
}

void SCULPT_OT_layer_clear(wmOperatorType *ot)
{
  ot->name = "Clear Sculpt Layer";
  ot->idname = "SCULPT_OT_layer_clear";
  ot->description = "Reset the active sculpt layer's displacement to zero";
  ot->exec = layer_clear_exec;
  ot->poll = layers_poll;
  ot->flag = OPTYPE_REGISTER;
}

static wmOperatorStatus layer_invert_exec(bContext *C, wmOperator *op)
{
  OpContext ctx;
  if (!op_context_get(C, op, ctx)) {
    return OPERATOR_CANCELLED;
  }
  if (mask_edit_refuse_ccg_rebuild(op, *ctx.object)) {
    return OPERATOR_CANCELLED;
  }
  SculptLayer *layer = bke::sculpt_layers::active_get(*ctx.mesh);
  if (!layer || !layer->data) {
    return OPERATOR_CANCELLED;
  }
  Main *bmain = CTX_data_main(C);

  /* Gather-then-gate: see #layer_add_exec's own comment for why the fan-out set has to be built
   * before any undo bracket opens. The active object is included unconditionally; every other
   * sync-group member is brought up to date and gated exactly as #op_context_get gates the active
   * object, plus two find-shaped gates #layer_add_exec has no analog for: the member must actually
   * hold a layer synced to the active one, and that layer must itself have data (mirrors the
   * active-object early-return above, but as a per-member skip rather than an operator abort). */
  Vector<Object *> eligible;
  eligible.append(ctx.object);
  for (Object *member : fanout_targets(*bmain, *ctx.object)) {
    if (member == ctx.object) {
      continue;
    }
    /* Mirrors #layer_add_exec's gather loop: guarded on an existing session, since a member that
     * was never taken into sculpt mode has none, and #BKE_sculpt_update_object_for_edit
     * unconditionally dereferences it. Refreshed before #is_supported below so a member that IS in
     * sculpt mode but has not built its PBVH yet is not misreported as unsupported. */
    if (member->runtime->sculpt_session != nullptr) {
      BKE_sculpt_update_object_for_edit(ctx.depsgraph, member, false);
    }
    if (!is_supported(*member)) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Invert Sculpt Layer: skipping \"%s\" (sculpt layers are not available for this "
                  "object)",
                  member->id.name + 2);
      continue;
    }
    /* #node_find_by_sync_uid resolves the SAME layer wherever it sits in the member's own tree;
     * the member's own active layer is unrelated. A zero #sync_uid (the active layer was never
     * fanned-out-created) makes every member miss here by design -- see the function's own doc
     * comment -- which degrades this operator to the single-object behavior it always had. */
    Mesh &member_mesh = mesh_of(*member);
    SculptLayerTreeNode *member_node = bke::sculpt_layers::node_find_by_sync_uid(
        member_mesh, layer->base.sync_uid);
    if (!member_node) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Invert Sculpt Layer: skipping \"%s\" (no layer synced to the active one)",
                  member->id.name + 2);
      continue;
    }
    /* Defensive: #sync_uid should only ever link a layer to a layer, but the tree is
     * user-editable, so the kind found is not assumed. */
    SculptLayer *member_layer = bke::sculpt_layers::node_as_layer(member_node);
    if (!member_layer) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Invert Sculpt Layer: skipping \"%s\" (the synced node is a group, not a "
                  "layer)",
                  member->id.name + 2);
      continue;
    }
    /* Mirrors the active-object early return: a layer with no allocated data has nothing to
     * invert. */
    if (!member_layer->data) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Invert Sculpt Layer: skipping \"%s\" (the synced layer has no data)",
                  member->id.name + 2);
      continue;
    }
    if (bke::object::pbvh_get(*member)->type() == bke::pbvh::Type::Grids) {
      /* Mirrors #op_context_get / #layer_add_exec: consume any pending base sculpt edits first,
       * while the live CCG still matches the stored layers. */
      flush_pending_multires_base(*member);
    }
    if (mask_edit_refuse_ccg_rebuild(op, *member)) {
      continue;
    }
    eligible.append(member);
  }

  session_state_ensure(*ctx.object);
  undo::push_begin_multi_object(*CTX_data_scene(C), op, eligible.as_span());
  /* Active object: unchanged from the single-object path. */
  undo::push_sculpt_layer_data(*ctx.object, *layer);
  for (float3 &v : bke::sculpt_layers::data_get(*layer)) {
    v = -v;
  }
  commit_layers_change(*ctx.depsgraph, *ctx.object);

  for (Object *member : eligible) {
    if (member == ctx.object) {
      continue;
    }
    /* Eligibility (#is_supported, sync_uid resolution, the layer-has-data check,
     * #mask_edit_refuse_ccg_rebuild) and the multires pending-base flush already ran for every
     * member of `eligible` in the gather pass above -- nothing here can fail or need skipping
     * (mirrors #layer_add_exec's own second loop). */
    session_state_ensure(*member);
    Mesh &member_mesh = mesh_of(*member);
    SculptLayer *member_layer = bke::sculpt_layers::node_as_layer(
        bke::sculpt_layers::node_find_by_sync_uid(member_mesh, layer->base.sync_uid));
    undo::push_sculpt_layer_data(*member, *member_layer);
    for (float3 &v : bke::sculpt_layers::data_get(*member_layer)) {
      v = -v;
    }
    commit_layers_change(*ctx.depsgraph, *member);
  }

  /* #commit_layers_change unconditionally recomposes the surface, on the active object and every
   * fanned-out member alike, so #UpdateType::Position is required the same way #layer_add_exec
   * requires it. */
  undo::finish_multi_object(C, eligible.as_span(), UpdateType::Position);
  layers_ui_notify(C, *ctx.object);
  return OPERATOR_FINISHED;
}

void SCULPT_OT_layer_invert(wmOperatorType *ot)
{
  ot->name = "Invert Sculpt Layer";
  ot->idname = "SCULPT_OT_layer_invert";
  ot->description = "Invert the displacement stored in the active sculpt layer";
  ot->exec = layer_invert_exec;
  ot->poll = layers_poll;
  ot->flag = OPTYPE_REGISTER;
}

enum class ValidateAction {
  Clear = 0,
  Remove = 1,
};

/* Stale layers hold a per-element displacement for an element count the mesh no longer has, so
 * there is no correct way to apply them: every consumer skips them and the RNA setters refuse to
 * change their values (see #bke::sculpt_layers::is_stale). Repairing means giving up the stored
 * shape either way — the deltas cannot be mapped onto the new topology — so the only choice is
 * whether to keep the layer (and its name / place in the stack) or drop it. */
static Vector<SculptLayer *> stale_layers_gather(Mesh &mesh)
{
  Vector<SculptLayer *> stale;
  for (SculptLayer *layer : bke::sculpt_layers::layers(mesh)) {
    if (bke::sculpt_layers::is_stale(mesh, *layer)) {
      stale.append(layer);
    }
  }
  return stale;
}

/* One fan-out target of #SCULPT_OT_layer_validate: the member and ITS own stale-layer set, captured
 * in the gather pass. Carrying the list rather than re-deriving it in the fan-out loop is mandatory
 * for Remove (node-freeing) -- #bke::sculpt_layers::remove invalidates the cached span, and the
 * active object's removals run before any member is touched. Safe because #fanout_targets dedups by
 * #Object::data. Clear also carries the list so both actions share one gather shape. */
struct ValidateFanoutMember {
  Object *object;
  Vector<SculptLayer *> stale;
};

/* Whole-tree fan-out: repair each eligible member's OWN stale set (no sync_uid match); members with
 * none are skipped, while an empty set on the active object still cancels. */
static wmOperatorStatus layer_validate_exec(bContext *C, wmOperator *op)
{
  OpContext ctx;
  if (!op_context_get(C, op, ctx)) {
    return OPERATOR_CANCELLED;
  }
  const Vector<SculptLayer *> stale = stale_layers_gather(*ctx.mesh);
  if (stale.is_empty()) {
    BKE_report(op->reports, RPT_INFO, "No stale sculpt layers to repair");
    return OPERATOR_CANCELLED;
  }
  const ValidateAction action = ValidateAction(RNA_enum_get(op->ptr, "action"));
  Main *bmain = CTX_data_main(C);

  /* Gather-then-gate: see #layer_add_exec's own comment for why the fan-out set has to be built
   * before any undo bracket opens. Whole-tree (not find-shaped): every eligible member repairs its
   * own stale set; there is no per-node #sync_uid gate. The active object already passed the
   * non-empty stale gate above and is included unconditionally.
   *
   * Unlike #layer_add_exec / #layer_clear_exec this gather does NOT call
   * #mask_edit_refuse_ccg_rebuild on a member: validate is one of the operators that *closes* an
   * open mask-edit session rather than refusing on its account (see #layer_add_exec's comment on
   * that split), and the per-member loop below closes each member's own session unconditionally. */
  Vector<Object *> eligible;
  eligible.append(ctx.object);
  Vector<ValidateFanoutMember> fanout_members;
  for (Object *member : fanout_targets(*bmain, *ctx.object)) {
    if (member == ctx.object) {
      continue;
    }
    /* Mirrors #layer_add_exec's gather loop: guarded on an existing session, since a member that
     * was never taken into sculpt mode has none, and #BKE_sculpt_update_object_for_edit
     * unconditionally dereferences it. Refreshed before #is_supported below so a member that IS in
     * sculpt mode but has not built its PBVH yet is not misreported as unsupported. */
    if (member->runtime->sculpt_session != nullptr) {
      BKE_sculpt_update_object_for_edit(ctx.depsgraph, member, false);
    }
    if (!is_supported(*member)) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Repair Stale Sculpt Layers: skipping \"%s\" (sculpt layers are not available "
                  "for this object)",
                  member->id.name + 2);
      continue;
    }
    Mesh &member_mesh = mesh_of(*member);
    /* Captured here, on this member's own tree, and carried forward -- see #ValidateFanoutMember.
     * Must happen before ANY #bke::sculpt_layers::remove runs (the active object's included). A
     * member with nothing stale is skipped (not a cancel): only the active object's empty set
     * cancels the whole operator. */
    Vector<SculptLayer *> member_stale = stale_layers_gather(member_mesh);
    if (member_stale.is_empty()) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Repair Stale Sculpt Layers: skipping \"%s\" (no stale sculpt layers to repair)",
                  member->id.name + 2);
      continue;
    }
    if (bke::object::pbvh_get(*member)->type() == bke::pbvh::Type::Grids) {
      /* Mirrors #op_context_get / #layer_add_exec: consume any pending base sculpt edits first,
       * while the live CCG still matches the stored layers, before the repair below rewrites them. */
      flush_pending_multires_base(*member);
    }
    eligible.append(member);
    ValidateFanoutMember fanout_member;
    fanout_member.object = member;
    fanout_member.stale = std::move(member_stale);
    fanout_members.append(std::move(fanout_member));
  }

  /* Read before the close below, which clears it. Zero when no session is open. */
  const SculptSession *ss = session_of(*ctx.object);
  const int session_uid = ss ? ss->layers.mask_edit.node_uid : 0;
  /* Both actions rewrite the very nodes a session could be open on — Remove deletes them outright,
   * Clear drops a mask that no longer fits (below) — so the session is closed first, for the reason
   * spelled out in #layer_remove_exec. Closing it also puts the node's weights back on the node,
   * which is what lets #undo::push_sculpt_layer_mask below capture a mask that is not stale by
   * construction. */
  /* Before the close, which clears the session struct the parked tool idname lives on. */
  mask_edit_exit_ui(C, *ctx.object);
  mask_edit_end(*ctx.object);

  session_state_ensure(*ctx.object);
  undo::push_begin_multi_object(*CTX_data_scene(C), op, eligible.as_span());
  /* Active object: unchanged from the single-object path. */
  if (session_uid != 0) {
    undo::push_sculpt_layer_mask_session(*ctx.object, session_uid, false);
  }
  if (action == ValidateAction::Remove) {
    /* Captured in list order while the list is still intact, then removed, so undo restores each
     * layer next to the neighbour it really had (see #layer_bake_exec). */
    Vector<undo::SculptLayerUndoPayload> removed;
    for (SculptLayer *layer : stale) {
      removed.append(undo::sculpt_layer_payload_capture(
          *ctx.mesh, layer->base, undo::PayloadCapture::NodeRemoved));
    }
    for (SculptLayer *layer : stale) {
      bke::sculpt_layers::remove(*ctx.mesh, *layer);
    }
    undo::push_sculpt_layer_list_change(*ctx.object, std::move(removed), {}, false);
  }
  else {
    Vector<undo::SculptLayerUndoPayload> resized;
    for (SculptLayer *layer : stale) {
      /* Takes the unmappable buffer off the layer and hands it to the undo step, leaving the layer
       * itself (uid, name, place in the stack) in the list. */
      resized.append(undo::sculpt_layer_payload_capture(
          *ctx.mesh, layer->base, undo::PayloadCapture::DataOnly));
      /* Re-fit to the live count of the layer's *own* domain (not the object's current sculpt
       * domain, which says nothing about a vertex layer sitting under a multires modifier).
       * #data_ensure allocates zeroed, which is exactly the wanted result: an empty but usable
       * layer on the new topology rather than a buffer that cannot be indexed. */
      const int64_t new_count = bke::sculpt_layers::element_count(*ctx.mesh, *layer);
      bke::sculpt_layers::data_ensure(*layer, new_count);
      /* The weight mask is the layer's second per-element carrier and has to be re-fitted with the
       * first, or it keeps its old `totelem` forever: #is_stale_mask then rejects it and the layer
       * composites at full strength, while #mask_button_draw still offers a mask button (it tests
       * only for a non-null mask) that can no longer do anything. Dropped rather than resampled —
       * the deltas this repair gives up cannot be mapped onto the new topology, and neither can the
       * weights that select among them; the grid resamplers only move a mask between *levels*, not
       * between topologies, and the vertex domain has no resampler at all. A mask that already fits
       * the new count is kept: nothing about it needs repairing. */
      if (layer->base.mask != nullptr &&
          bke::sculpt_layers::is_stale_mask(*layer->base.mask, new_count))
      {
        /* Recorded before it is freed so undo puts the user's weights back. Safe here because the
         * session close above guarantees no node carries live weights in the standard storage. */
        undo::push_sculpt_layer_mask(*ctx.object, layer->base);
        bke::sculpt_layers::mask_free(layer->base.mask);
        layer->base.mask = nullptr;
        /* Reported for the same reason #layer_group_remove_exec reports its own: nothing is
         * corrupted, but the layer going back to contributing in full would otherwise be a shape
         * change with no explanation on screen. */
        BKE_reportf(op->reports,
                    RPT_INFO,
                    "Removed the weight mask of sculpt layer '%s'; it no longer matches the mesh "
                    "and the layer now contributes in full",
                    layer->base.name);
      }
    }
    undo::push_sculpt_layer_data_resize(*ctx.object, std::move(resized));
  }
  /* A stale layer contributed nothing to the surface, so neither action moves any vertex; the
   * commit is still needed to rebuild the runtime base and the display from the new layer set. */
  commit_layers_change(*ctx.depsgraph, *ctx.object);

  /* Fan out: mirror the active object's own body exactly on each member's own stale set, captured
   * in the gather pass before ANY removal ran anywhere. */
  for (const ValidateFanoutMember &fanout_member : fanout_members) {
    Object &member = *fanout_member.object;
    Mesh &member_mesh = mesh_of(member);
    const Span<SculptLayer *> member_stale = fanout_member.stale;
    /* Eligibility and the session guards already ran for every entry of `fanout_members` in the
     * gather pass above, before the undo bracket opened -- nothing here can fail or need skipping
     * (mirrors #layer_add_exec's own second loop). */

    /* Read before the close below, which clears it -- this member's own session, not the active
     * object's. Both halves of the close fan out: #mask_edit_end because the member's own stale
     * layers are about to be rewritten under its own session, and #mask_edit_exit_ui because it is
     * safe to fan out per member (Task 9d1: returns immediately when this object has no open
     * session). */
    const SculptSession *member_ss = session_of(member);
    const int member_session_uid = member_ss ? member_ss->layers.mask_edit.node_uid : 0;
    mask_edit_exit_ui(C, member);
    mask_edit_end(member);
    session_state_ensure(member);
    if (member_session_uid != 0) {
      undo::push_sculpt_layer_mask_session(member, member_session_uid, false);
    }

    if (action == ValidateAction::Remove) {
      Vector<undo::SculptLayerUndoPayload> removed;
      for (SculptLayer *layer : member_stale) {
        removed.append(undo::sculpt_layer_payload_capture(
            member_mesh, layer->base, undo::PayloadCapture::NodeRemoved));
      }
      for (SculptLayer *layer : member_stale) {
        bke::sculpt_layers::remove(member_mesh, *layer);
      }
      undo::push_sculpt_layer_list_change(member, std::move(removed), {}, false);
    }
    else {
      Vector<undo::SculptLayerUndoPayload> resized;
      for (SculptLayer *layer : member_stale) {
        resized.append(undo::sculpt_layer_payload_capture(
            member_mesh, layer->base, undo::PayloadCapture::DataOnly));
        const int64_t new_count = bke::sculpt_layers::element_count(member_mesh, *layer);
        bke::sculpt_layers::data_ensure(*layer, new_count);
        if (layer->base.mask != nullptr &&
            bke::sculpt_layers::is_stale_mask(*layer->base.mask, new_count))
        {
          undo::push_sculpt_layer_mask(member, layer->base);
          bke::sculpt_layers::mask_free(layer->base.mask);
          layer->base.mask = nullptr;
          BKE_reportf(op->reports,
                      RPT_INFO,
                      "Removed the weight mask of sculpt layer '%s'; it no longer matches the "
                      "mesh and the layer now contributes in full",
                      layer->base.name);
        }
      }
      undo::push_sculpt_layer_data_resize(member, std::move(resized));
    }
    commit_layers_change(*ctx.depsgraph, member);
  }

  /* #commit_layers_change unconditionally recomposes the surface, on the active object and every
   * fanned-out member alike, so #UpdateType::Position is required the same way #layer_add_exec
   * requires it. No explicit #rec_exemption_refresh: it already runs inside #commit_layers_change. */
  undo::finish_multi_object(C, eligible.as_span(), UpdateType::Position);
  /* Active-object-only, matching #layer_add_exec / every other #finish_multi_object-based fan-out
   * in this file: #finish_multi_object already sent `NC_OBJECT | ND_DRAW` for every member above. */
  layers_ui_notify(C, *ctx.object);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus layer_validate_invoke(bContext *C,
                                              wmOperator *op,
                                              const wmEvent * /*event*/)
{
  OpContext ctx;
  if (!op_context_get(C, op, ctx)) {
    return OPERATOR_CANCELLED;
  }
  /* Pre-check the exec guard so the popup never appears just to be followed by an error
   * (confirm-then-fail); the UI already disables this operator when nothing is stale. */
  const Vector<SculptLayer *> stale = stale_layers_gather(*ctx.mesh);
  if (stale.is_empty()) {
    BKE_report(op->reports, RPT_INFO, "No stale sculpt layers to repair");
    return OPERATOR_CANCELLED;
  }
  const ValidateAction action = ValidateAction(RNA_enum_get(op->ptr, "action"));
  const char *message = (action == ValidateAction::Remove) ?
                            IFACE_(
                                "The stale sculpt layers will be deleted. Their stored "
                                "displacement cannot be mapped onto the current topology and is "
                                "lost either way.") :
                            IFACE_(
                                "The stale sculpt layers will be kept but their stored "
                                "displacement will be discarded: it cannot be mapped onto the "
                                "current topology.");
  return WM_operator_confirm_ex(C,
                                op,
                                IFACE_("Repair Stale Sculpt Layers"),
                                message,
                                (action == ValidateAction::Remove) ? IFACE_("Remove") :
                                                                     IFACE_("Reset"),
                                ui::AlertIcon::Warning,
                                false);
}

void SCULPT_OT_layer_validate(wmOperatorType *ot)
{
  static const EnumPropertyItem action_items[] = {
      {int(ValidateAction::Clear),
       "CLEAR",
       0,
       "Reset Data",
       "Keep the layers, re-fitted to the current topology and zeroed"},
      {int(ValidateAction::Remove), "REMOVE", 0, "Remove Layers", "Delete the stale layers"},
      {0, nullptr, 0, nullptr, nullptr},
  };
  ot->name = "Repair Stale Sculpt Layers";
  ot->idname = "SCULPT_OT_layer_validate";
  ot->description =
      "Repair sculpt layers whose stored displacement no longer matches the mesh topology, by "
      "resetting their data or removing them";
  ot->exec = layer_validate_exec;
  /* Destructive in both modes (the stored displacement is dropped), so confirm before running. */
  ot->invoke = layer_validate_invoke;
  ot->poll = layers_poll;
  /* No #OPTYPE_UNDO: the operator pushes its own sculpt undo step; a global push would insert a
   * memfile step that does not compose with the stroke SCULPT steps (see #SCULPT_OT_layer_solo_base). */
  ot->flag = OPTYPE_REGISTER;

  RNA_def_enum(ot->srna,
               "action",
               action_items,
               int(ValidateAction::Clear),
               "Action",
               "What to do with the stale layers");
}

/**
 * Dense per-element weights of \a node's own weight mask, or false when it has none usable.
 *
 * The sibling #gather_layer_mask resolves the *user's* paint mask; this one resolves the layer or
 * folder's own #SculptLayerMask. The two are unrelated buffers with confusingly similar names, which
 * is why they sit together.
 *
 * A switched-off mask answers false, matching the composite exactly: #mask_enabled gates both
 * #node_mask_for_composite and the #chain_mask rebuild, so such a mask attenuates nothing and folding
 * its weights into the data would move the surface by precisely the amount the user switched off.
 *
 * Answers false for a stale mask as well as for a missing one, and the callers must not read that as
 * "unmasked": #find_stale_mask_in_fold exists to separate the two *before* anything is folded, and
 * every caller runs it first.
 */
bool gather_node_weight_mask(const SculptLayerTreeNode &node,
                             const int64_t elem_num,
                             Array<float> &r_dense)
{
  if (node.mask == nullptr || !bke::sculpt_layers::mask_enabled(node) ||
      bke::sculpt_layers::is_stale_mask(*node.mask, elem_num))
  {
    return false;
  }
  r_dense.reinitialize(elem_num);
  bke::sculpt_layers::mask_expand(*node.mask, r_dense.as_mutable_span());
  return true;
}

/**
 * Dense product of \a node's own mask and the masks of every folder strictly below \a stop_above.
 *
 * \a stop_above is *exclusive*, and that is the whole design: a mask must be folded into the merged
 * data exactly when its carrier stops acting on the result, and every caller expresses that as "the
 * folders that dissolve end here". Passing \a node's own parent therefore folds nothing but its own
 * mask; passing null folds the whole chain up to the root. Returns false when nothing in that range
 * carries a mask, which callers read as a factor of 1.
 *
 * The ancestor walk goes through #find_ancestor and not by hand. That helper is the module's single
 * cycle-safe ancestor walk (Floyd, see its doc), and a parent chain that closes on itself — a corrupt
 * file, a future editing bug — must not hang a merge. Its return value is discarded on purpose: the
 * predicate accumulates as a side effect and reports true only at the exclusive bound, so the walk
 * stops there and the "found" node is never wanted.
 */
bool gather_fold_mask(const SculptLayerTreeNode &node,
                      const SculptLayerGroup *stop_above,
                      const int64_t elem_num,
                      Array<float> &r_dense)
{
  bool any = false;
  Array<float> scratch;
  auto fold = [&](const SculptLayerTreeNode &folded) {
    if (!gather_node_weight_mask(folded, elem_num, scratch)) {
      return;
    }
    if (!any) {
      r_dense = std::move(scratch);
      any = true;
      return;
    }
    for (const int64_t i : r_dense.index_range()) {
      r_dense[i] *= scratch[i];
    }
  };

  fold(node);
  bke::sculpt_layers::find_ancestor(node.parent, [&](const SculptLayerGroup &group) {
    if (&group == stop_above) {
      return true;
    }
    fold(group.base);
    return false;
  });
  return any;
}

/**
 * The first node in the same range #gather_fold_mask folds that carries a mask which does not
 * describe \a elem_num elements, or null when every mask in range is usable.
 *
 * Split from the fold rather than folded into it because the two answers must not be confused: the
 * fold reports "no factor", and a stale mask would ride out as a factor of 1 — silently dropping the
 * user's weights and moving the surface. #node_chain_carries_mask fails closed on exactly this case
 * today (see its comment), and the merges must go on doing so.
 *
 * A switched-off mask is skipped, in step with #gather_node_weight_mask: it attenuates nothing in the
 * composite either, so however it is cut it cannot make the folded shape disagree with the composed
 * one — and that disagreement is the only thing this refusal protects.
 */
const SculptLayerTreeNode *find_stale_mask_in_fold(const SculptLayerTreeNode &node,
                                                   const SculptLayerGroup *stop_above,
                                                   const int64_t elem_num)
{
  const SculptLayerTreeNode *stale = nullptr;
  auto check = [&](const SculptLayerTreeNode &checked) {
    if (stale == nullptr && checked.mask != nullptr && bke::sculpt_layers::mask_enabled(checked) &&
        bke::sculpt_layers::is_stale_mask(*checked.mask, elem_num))
    {
      stale = &checked;
    }
  };

  check(node);
  bke::sculpt_layers::find_ancestor(node.parent, [&](const SculptLayerGroup &group) {
    if (&group == stop_above) {
      return true;
    }
    check(group.base);
    return false;
  });
  return stale;
}

/* Resolves the paint mask into an array indexed exactly like `layer.data`. Vertex layers read
 * the `.sculpt_mask` mesh attribute directly (same index space as the layer). Grid layers read
 * the live `SubdivCCG::masks`, which sits at the CCG's *current* sculpt level and must be
 * upsampled to the layer's own top level (see the multires-domain comment at the top of this
 * file) before it lines up with `layer.data`. Returns false when no usable mask exists, so
 * callers can refuse rather than silently zero an entire layer on an empty mask. */
static bool gather_layer_mask(Object &object, const SculptLayer &layer, Array<float> &r_mask)
{
  if (layer.domain == SCULPT_LAYER_DOMAIN_VERT) {
    const Mesh &mesh = mesh_of(object);
    if (layer.data == nullptr || bke::sculpt_layers::is_stale(layer, mesh.verts_num)) {
      return false;
    }
    const bke::AttributeAccessor attributes = mesh.attributes();
    const VArray<float> mask = *attributes.lookup<float>(".sculpt_mask", bke::AttrDomain::Point);
    if (!mask) {
      return false;
    }
    r_mask.reinitialize(layer.totelem);
    mask.materialize(r_mask.as_mutable_span());
  }
  else {
    SculptSession *ss = session_of(object);
    if (!ss || !ss->subdiv_ccg || ss->subdiv_ccg->masks.is_empty()) {
      return false;
    }
    SubdivCCG &subdiv_ccg = *ss->subdiv_ccg;
    const int cur_level = subdiv_ccg.level;
    const int grids_num = subdiv_ccg.grids_num;
    if (int64_t(bke::grid_totelem(grids_num, cur_level)) != subdiv_ccg.masks.size()) {
      return false;
    }

    if (cur_level == layer.level) {
      r_mask.reinitialize(subdiv_ccg.masks.size());
      r_mask.as_mutable_span().copy_from(subdiv_ccg.masks);
    }
    else if (cur_level < layer.level) {
      /* Pack the scalar mask into the x channel of a float3 field and reuse the existing
       * per-grid bilinear upsampler (already used elsewhere in this file to compose grid layers
       * onto the CCG): interpolation is per-component independent, so this is exact for a
       * scalar field. */
      Array<float3> packed(subdiv_ccg.masks.size());
      for (const int64_t i : packed.index_range()) {
        packed[i] = float3(subdiv_ccg.masks[i], 0.0f, 0.0f);
      }
      const Array<float3> upsampled = bke::grid_upsample(
          packed, cur_level, layer.level, grids_num);
      r_mask.reinitialize(upsampled.size());
      for (const int64_t i : r_mask.index_range()) {
        r_mask[i] = upsampled[i].x;
      }
    }
    else {
      /* cur_level > layer.level should not happen: layer.level tracks the multires modifier's
       * top level, which the current sculpt level cannot exceed. Guard defensively rather than
       * indexing out of bounds. */
      return false;
    }

    if (r_mask.size() != layer.totelem) {
      return false;
    }
  }

  float max_value = 0.0f;
  for (const float value : r_mask) {
    max_value = std::max(max_value, value);
  }
  /* Nothing painted: every value is zero, so isolating would zero the whole layer. Treat this
   * the same as "no mask" rather than silently wiping the layer on an accidental click. */
  return max_value > 0.0f;
}

static bool layer_has_usable_paint_mask(Object &object, const SculptLayer &layer)
{
  Array<float> mask;
  return gather_layer_mask(object, layer, mask);
}

static bool sync_group_member_has_layer_paint_mask(Main &bmain,
                                                   Object &active_ob,
                                                   const SculptLayer &active_layer,
                                                   const Object *skip_member)
{
  if (active_layer.base.sync_uid == 0) {
    return false;
  }
  for (Object *member : fanout_targets(bmain, active_ob)) {
    if (member == skip_member) {
      continue;
    }
    if (member->type != OB_MESH || member->data == nullptr) {
      continue;
    }
    Mesh &member_mesh = mesh_of(*member);
    SculptLayerTreeNode *member_node = bke::sculpt_layers::node_find_by_sync_uid(
        member_mesh, active_layer.base.sync_uid);
    SculptLayer *member_layer = bke::sculpt_layers::node_as_layer(member_node);
    if (!member_layer || !member_layer->data) {
      continue;
    }
    if (layer_has_usable_paint_mask(*member, *member_layer)) {
      return true;
    }
  }
  return false;
}

static bool layer_mask_isolate_weight_mask_editing(const Object &object)
{
  const SculptSession *ss = session_of(const_cast<Object &>(object));
  return ss != nullptr && ss->layers.mask_edit.node_uid != 0;
}

static void layer_mask_isolate_apply_mask_to_data(SculptLayer &layer,
                                                  const Span<float> mask,
                                                  const bool invert)
{
  MutableSpan<float3> data = bke::sculpt_layers::data_get(layer);
  for (const int64_t i : data.index_range()) {
    const float factor = invert ? 1.0f - mask[i] : mask[i];
    data[i] *= factor;
  }
}

static void layer_mask_isolate_clear_paint_mask(bContext *C, ViewLayer *view_layer, Object &object)
{
  Base *base = BKE_view_layer_base_find(view_layer, &object);
  if (base == nullptr) {
    return;
  }
  ed::object::base_activate(C, base);
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  BKE_sculpt_update_object_for_edit(depsgraph, &object, false);
  PointerRNA props = WM_operator_properties_create("PAINT_OT_mask_flood_fill");
  RNA_enum_set_identifier(C, &props, "mode", "VALUE");
  RNA_float_set(&props, "value", 0.0f);
  WM_operator_name_call(
      C, "PAINT_OT_mask_flood_fill", wm::OpCallContext::ExecDefault, &props, nullptr);
  WM_operator_properties_free(&props);
}

struct LayerMaskIsolateTarget {
  Object *object;
  SculptLayer *layer;
};

static wmOperatorStatus layer_mask_isolate_exec(bContext *C, wmOperator *op)
{
  OpContext ctx;
  if (!op_context_get(C, op, ctx)) {
    return OPERATOR_CANCELLED;
  }
  if (mask_edit_refuse_ccg_rebuild(op, *ctx.object)) {
    return OPERATOR_CANCELLED;
  }
  SculptLayer *active_layer = bke::sculpt_layers::active_get(*ctx.mesh);
  if (!active_layer || !active_layer->data) {
    return OPERATOR_CANCELLED;
  }
  if (layer_mask_isolate_weight_mask_editing(*ctx.object)) {
    BKE_report(op->reports,
               RPT_ERROR,
               "A sculpt layer weight mask is being edited; finish the mask edit before isolating "
               "by mask");
    return OPERATOR_CANCELLED;
  }

  Main *bmain = CTX_data_main(C);
  const SyncScope sync_scope = sync_scope_get(op);
  const bool invert = RNA_boolean_get(op->ptr, "invert");
  const bool clear_mask = RNA_boolean_get(op->ptr, "clear_mask");

  Vector<Object *> eligible;
  Vector<LayerMaskIsolateTarget> targets;

  auto try_add = [&](Object *member) {
    if (layer_mask_isolate_weight_mask_editing(*member)) {
      if (member != ctx.object) {
        BKE_reportf(op->reports,
                    RPT_WARNING,
                    "Isolate Layer by Mask: skipping \"%s\" (a sculpt layer weight mask is being "
                    "edited)",
                    member->id.name + 2);
      }
      return;
    }
    if (member->runtime->sculpt_session != nullptr) {
      BKE_sculpt_update_object_for_edit(ctx.depsgraph, member, false);
    }
    if (!is_supported(*member)) {
      if (member != ctx.object) {
        BKE_reportf(op->reports,
                    RPT_WARNING,
                    "Isolate Layer by Mask: skipping \"%s\" (sculpt layers are not available for "
                    "this object)",
                    member->id.name + 2);
      }
      return;
    }

    Mesh &member_mesh = mesh_of(*member);
    SculptLayer *layer = active_layer;
    if (member != ctx.object) {
      SculptLayerTreeNode *member_node = bke::sculpt_layers::node_find_by_sync_uid(
          member_mesh, active_layer->base.sync_uid);
      layer = bke::sculpt_layers::node_as_layer(member_node);
      if (!layer) {
        BKE_reportf(op->reports,
                    RPT_WARNING,
                    "Isolate Layer by Mask: skipping \"%s\" (no layer synced to the active one)",
                    member->id.name + 2);
        return;
      }
    }
    if (!layer->data) {
      return;
    }
    if (!layer_has_usable_paint_mask(*member, *layer)) {
      if (member == ctx.object) {
        if (sync_scope == SyncScope::ActiveOnly) {
          BKE_report(op->reports, RPT_ERROR, "No mask painted");
        }
        else {
          BKE_reportf(op->reports,
                      RPT_WARNING,
                      "Isolate Layer by Mask: skipping \"%s\" (no mask painted)",
                      member->id.name + 2);
        }
      }
      else {
        BKE_reportf(op->reports,
                    RPT_WARNING,
                    "Isolate Layer by Mask: skipping \"%s\" (no mask painted)",
                    member->id.name + 2);
      }
      return;
    }
    if (bke::object::pbvh_get(*member)->type() == bke::pbvh::Type::Grids) {
      flush_pending_multires_base(*member);
    }
    if (mask_edit_refuse_ccg_rebuild(op, *member)) {
      return;
    }
    if (!eligible.contains(member)) {
      eligible.append(member);
    }
    targets.append({member, layer});
  };

  for (Object *member : sync_scope_targets(*bmain, *ctx.object, sync_scope)) {
    try_add(member);
  }

  if (targets.is_empty()) {
    if (sync_scope == SyncScope::ActiveOnly) {
      BKE_report(op->reports, RPT_ERROR, "No mask painted");
    }
    else {
      BKE_report(op->reports,
                 RPT_ERROR,
                 "No mask painted on any object in the sync group for this layer");
    }
    return OPERATOR_CANCELLED;
  }

  for (Object *member : eligible) {
    session_state_ensure(*member);
  }
  undo::push_begin_multi_object(*CTX_data_scene(C), op, eligible.as_span());
  for (const LayerMaskIsolateTarget &target : targets) {
    Array<float> mask;
    if (!gather_layer_mask(*target.object, *target.layer, mask)) {
      BLI_assert_unreachable();
      continue;
    }
    undo::push_sculpt_layer_data(*target.object, *target.layer);
    layer_mask_isolate_apply_mask_to_data(*target.layer, mask, invert);
  }
  for (Object *member : eligible) {
    commit_layers_change(*ctx.depsgraph, *member);
  }
  undo::finish_multi_object(C, eligible.as_span(), UpdateType::Position);

  if (clear_mask) {
    ViewLayer *view_layer = CTX_data_view_layer(C);
    Base *restore_base = BKE_view_layer_base_find(view_layer, ctx.object);
    for (const LayerMaskIsolateTarget &target : targets) {
      layer_mask_isolate_clear_paint_mask(C, view_layer, *target.object);
    }
    if (restore_base != nullptr) {
      ed::object::base_activate(C, restore_base);
    }
  }

  for (Object *member : eligible) {
    layers_ui_notify(C, *member);
  }
  return OPERATOR_FINISHED;
}

static wmOperatorStatus layer_mask_isolate_invoke(bContext *C, wmOperator *op, const wmEvent * /*event*/)
{
  OpContext ctx;
  if (!op_context_get(C, op, ctx)) {
    return OPERATOR_CANCELLED;
  }
  if (layer_mask_isolate_weight_mask_editing(*ctx.object)) {
    BKE_report(op->reports,
               RPT_ERROR,
               "A sculpt layer weight mask is being edited; finish the mask edit before isolating "
               "by mask");
    return OPERATOR_CANCELLED;
  }
  SculptLayer *layer = bke::sculpt_layers::active_get(*ctx.mesh);
  if (!layer || !layer->data) {
    return OPERATOR_CANCELLED;
  }

  Main *bmain = CTX_data_main(C);
  const bool active_has_mask = layer_has_usable_paint_mask(*ctx.object, *layer);

  if (ctx.object->sculpt_layer_sync_group == 0 || layer->base.sync_uid == 0) {
    if (!active_has_mask) {
      BKE_report(op->reports, RPT_ERROR, "No mask painted");
      return OPERATOR_CANCELLED;
    }
    return layer_mask_isolate_exec(C, op);
  }

  const bool other_has_mask = sync_group_member_has_layer_paint_mask(
      *bmain, *ctx.object, *layer, ctx.object);

  if (!active_has_mask && !other_has_mask) {
    BKE_report(op->reports, RPT_ERROR, "No mask painted");
    return OPERATOR_CANCELLED;
  }

  if (active_has_mask && !other_has_mask) {
    RNA_enum_set(op->ptr, "sync_scope", int(SyncScope::ActiveOnly));
    return layer_mask_isolate_exec(C, op);
  }

  if (!active_has_mask && other_has_mask) {
    return sync_scope_choice_dialog(
        C,
        op,
        IFACE_("Isolate Layer by Mask (Sync Group)"),
        IFACE_("The active object has no painted mask on this layer. Other objects in the sync "
               "group do. Isolate only on the active object when it has a mask, on every group "
               "object that has a mask, or cancel."),
        false);
  }

  return sync_scope_choice_dialog(
      C,
      op,
      IFACE_("Isolate Layer by Mask (Sync Group)"),
      IFACE_("Isolate using the paint mask on the active object only, on every sync-group object "
             "that has a mask on the synced layer, or cancel."));
}

void SCULPT_OT_layer_mask_isolate(wmOperatorType *ot)
{
  ot->name = "Isolate Layer by Mask";
  ot->idname = "SCULPT_OT_layer_mask_isolate";
  ot->description =
      "Keep only the masked part of the active sculpt layer's displacement, zeroing the rest";
  ot->exec = layer_mask_isolate_exec;
  ot->invoke = layer_mask_isolate_invoke;
  ot->poll = layers_poll;
  ot->flag = OPTYPE_REGISTER;
  sync_scope_rna_def(ot);

  RNA_def_boolean(
      ot->srna, "invert", false, "Invert", "Keep the unmasked part instead of the masked part");
  RNA_def_boolean(ot->srna,
                   "clear_mask",
                   false,
                   "Clear Mask",
                   "Clear the mask after isolating the layer");
}

static wmOperatorStatus layer_set_influence_exec(bContext *C, wmOperator *op)
{
  OpContext ctx;
  if (!op_context_get(C, op, ctx)) {
    return OPERATOR_CANCELLED;
  }
  if (mask_edit_refuse_ccg_rebuild(op, *ctx.object)) {
    return OPERATOR_CANCELLED;
  }
  SculptLayer *layer = bke::sculpt_layers::active_get(*ctx.mesh);
  if (!layer) {
    return OPERATOR_CANCELLED;
  }
  /* Read once from the active object's RNA prop and applied identically to every member: an
   * influence drag is not per-member-relative. */
  const float value = RNA_float_get(op->ptr, "influence");
  Main *bmain = CTX_data_main(C);

  /* Gather-then-gate: see #layer_add_exec's own comment for why the fan-out set has to be built
   * before any undo bracket opens. The active object is included unconditionally; every other
   * sync-group member is brought up to date and gated exactly as #op_context_get gates the active
   * object, plus a find-shaped gate #layer_add_exec has no analog for: the member must actually
   * hold a layer synced to the active one. */
  Vector<Object *> eligible;
  Set<Object *> tree_only_members;
  eligible.append(ctx.object);
  for (Object *member : fanout_targets(*bmain, *ctx.object)) {
    if (member == ctx.object) {
      continue;
    }
    bool tree_only = false;
    if (!sync_group_tree_create_member_prepare(ctx.depsgraph,
                                               *ctx.object,
                                               member,
                                               op,
                                               "Set Sculpt Layer Influence",
                                               tree_only))
    {
      continue;
    }
    if (tree_only) {
      tree_only_members.add(member);
    }
    /* #node_find_by_sync_uid resolves the SAME layer wherever it sits in the member's own tree;
     * the member's own active layer is unrelated. A zero #sync_uid (the active layer was never
     * fanned-out-created) makes every member miss here by design -- see the function's own doc
     * comment -- which degrades this operator to the single-object behavior it always had. */
    Mesh &member_mesh = mesh_of(*member);
    SculptLayerTreeNode *member_node = bke::sculpt_layers::node_find_by_sync_uid(
        member_mesh, layer->base.sync_uid);
    if (!member_node) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Set Sculpt Layer Influence: skipping \"%s\" (no layer synced to the active "
                  "one)",
                  member->id.name + 2);
      continue;
    }
    /* Defensive: #sync_uid should only ever link a layer to a layer, but the tree is
     * user-editable, so the kind found is not assumed. */
    if (!bke::sculpt_layers::node_as_layer(member_node)) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Set Sculpt Layer Influence: skipping \"%s\" (the synced node is a group, not "
                  "a layer)",
                  member->id.name + 2);
      continue;
    }
    if (bke::object::pbvh_get(*member)->type() == bke::pbvh::Type::Grids) {
      /* Mirrors #op_context_get / #layer_add_exec: consume any pending base sculpt edits first,
       * while the live CCG still matches the stored layers. */
      flush_pending_multires_base(*member);
    }
    if (!tree_only && mask_edit_refuse_ccg_rebuild(op, *member)) {
      continue;
    }
    eligible.append(member);
  }

  session_state_ensure(*ctx.object);
  undo::push_begin_multi_object(*CTX_data_scene(C), op, eligible.as_span());
  /* Active object: unchanged from the single-object path. */
  undo::push_sculpt_layer_metadata(*ctx.object, layer->base);
  layer->influence = value;
  commit_layers_change(*ctx.depsgraph, *ctx.object);

  for (Object *member : eligible) {
    if (member == ctx.object) {
      continue;
    }
    /* Eligibility (#is_supported, sync_uid resolution, #mask_edit_refuse_ccg_rebuild) and the
     * multires pending-base flush already ran for every member of `eligible` in the gather pass
     * above -- nothing here can fail or need skipping (mirrors #layer_add_exec's own second
     * loop). */
    Mesh &member_mesh = mesh_of(*member);
    SculptLayer *member_layer = bke::sculpt_layers::node_as_layer(
        bke::sculpt_layers::node_find_by_sync_uid(member_mesh, layer->base.sync_uid));
    if (tree_only_members.contains(member)) {
      undo::push_sculpt_layer_metadata(*member, member_layer->base);
      member_layer->influence = value;
      DEG_id_tag_update(&member_mesh.id, ID_RECALC_GEOMETRY);
      continue;
    }
    session_state_ensure(*member);
    undo::push_sculpt_layer_metadata(*member, member_layer->base);
    member_layer->influence = value;
    commit_layers_change(*ctx.depsgraph, *member);
  }

  Vector<Object *> eligible_finish;
  eligible_finish.reserve(eligible.size());
  for (Object *ob : eligible) {
    if (!tree_only_members.contains(ob)) {
      eligible_finish.append(ob);
    }
  }
  /* #commit_layers_change unconditionally recomposes the surface, on the active object and every
   * fanned-out member alike, so #UpdateType::Position is required the same way #layer_add_exec
   * requires it. */
  undo::finish_multi_object(C, eligible_finish.as_span(), UpdateType::Position);
  for (Object *member : eligible) {
    layers_ui_notify(C, *member);
  }
  return OPERATOR_FINISHED;
}

void SCULPT_OT_layer_set_influence(wmOperatorType *ot)
{
  ot->name = "Set Sculpt Layer Influence";
  ot->idname = "SCULPT_OT_layer_set_influence";
  ot->description = "Set the influence of the active sculpt layer";
  ot->exec = layer_set_influence_exec;
  ot->poll = layers_poll;
  ot->flag = OPTYPE_REGISTER;
  RNA_def_float_factor(
      ot->srna, "influence", 1.0f, -10.0f, 10.0f, "Influence", "", 0.0f, 1.0f);
}

/* -------------------------------------------------------------------------------------------------
 * Interactive influence drag (modal)
 *
 * A panel slider gives no "drag end" signal, so it cannot separate a cheap per-tick update from the
 * full-quality finalize. This modal operator provides that: while dragging it refreshes only the
 * GPU position buffers (deferring node bounds and normals), and runs a single full
 * #tag_positions_changed + #update_bounds_mesh when the drag ends. */

struct InfluenceDragData {
  Object *object;
  Mesh *mesh;
  SculptLayer *layer;
  float start_influence;
  /* Drag bounds: normally the default [-1, 1] range, but widened on whichever side the influence
   * already exceeded that range at invoke time, so starting a drag from an out-of-range value
   * does not suddenly clamp it back in. */
  float clamp_min;
  float clamp_max;
  /* Last influence value already accounted for (CPU positions when on the CPU path, accumulated GPU
   * delta when on the GPU path). */
  float prev_influence;
  int start_mouse_x;
  /* When true the per-tick visual update is done on the GPU (#Tree::add_influence_drag_delta) and
   * the CPU positions are reconciled once at the end; otherwise the CPU path applies every tick. */
  bool gpu_active;
  /* Effective influence (0 when the layer is disabled) that the CPU mesh positions currently
   * reflect. On the GPU path the CPU positions stay put except on periodic normal-refresh ticks, so
   * the final reconcile and the refresh both apply their delta relative to this, not the start. */
  float accounted_eff;
  /* Counts value-changing ticks so normals are refreshed only every Nth one (see below). */
  int ticks_since_normal_refresh;
  /* True when the active object uses a grids PBVH (multires); drives the per-tick and finish paths. */
  bool grids;
  /* True for a vertex-domain mesh under a shape key or deforming modifier: the live positions are
   * not a direct copy of the composed surface (the basis is untouched and the layers are re-composed
   * on top at evaluation), so the per-tick fast paths that write the mesh positions would deform the
   * base. Such a drag only changes the influence and relies on an honest depsgraph re-evaluation,
   * exactly like the grids path. */
  bool deg_reeval;
  /* Multires only: the layer's object-space contribution per CCG element at influence 1.0,
   * computed once on invoke (tangent coefficients subsampled to the current level and transformed
   * by the limit-surface tangent frames). Per tick the live CCG positions are updated
   * incrementally as `positions += grid_contrib * (new_eff - accounted_eff)`; the release commits
   * with an honest re-evaluation. Empty when the contribution could not be computed (the drag
   * then only changes the influence value and relies on the final re-evaluation). */
  Array<float3> grid_contrib;
  /* Last influence value already pushed to other sync-group members during this drag (lightweight
   * path without #commit_layers_change). */
  float last_sync_propagated_influence = std::numeric_limits<float>::quiet_NaN();
};

/* Influence units changed per pixel of horizontal mouse motion. */
static constexpr float influence_drag_sensitivity = 0.01f;

/* Default drag range; widened toward #influence_drag_hard_min/max on invoke when the layer's
 * influence already falls outside it (see #InfluenceDragData::clamp_min/max). */
static constexpr float influence_drag_default_min = -1.0f;
static constexpr float influence_drag_default_max = 1.0f;
static constexpr float influence_drag_hard_min = -10.0f;
static constexpr float influence_drag_hard_max = 10.0f;

/* Refresh the (expensive) GPU normal buffers only every Nth value-changing tick. The position
 * buffers update every tick on the GPU; normals lag this many ticks so shading still visibly tracks
 * the drag without paying the full normal re-extract on every tick. */
static constexpr int influence_drag_normal_refresh_interval = 3;

/* Apply the incremental influence delta to the live mesh positions for the active vert-domain
 * layer, mirroring #rna_SculptLayer_apply_mesh_delta. */
static void influence_drag_apply(Mesh &mesh, SculptLayer &layer, const float new_influence)
{
  const float old_eff = effective(layer);
  layer.influence = new_influence;
  const float new_eff = effective(layer);
  if (new_eff == old_eff) {
    return;
  }
  MutableSpan<float3> positions = mesh.vert_positions_for_write();
  bke::sculpt_layers::apply_delta_mesh(layer, new_eff - old_eff, positions);
  mesh.tag_positions_changed_no_normals();
}

/* Periodically reconcile the CPU positions to the current influence and refresh only the normal draw
 * buffers, so the viewport shading visibly tracks the drag. Positions keep being driven by the GPU
 * compute (GPU path) or the per-tick CPU apply, so this never re-extracts the position buffers. */
static void influence_drag_refresh_normals(Object &object,
                                           Mesh &mesh,
                                           SculptLayer &layer,
                                           InfluenceDragData &data)
{
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const float cur_eff = effective(layer);
  if (cur_eff != data.accounted_eff) {
    /* GPU path: the CPU positions stayed at #accounted_eff during the cheap ticks; advance them to
     * the current influence so the recomputed normals match what the GPU is displaying. (On the CPU
     * path the positions are already current, so this is a no-op.) */
    MutableSpan<float3> positions = mesh.vert_positions_for_write();
    bke::sculpt_layers::apply_delta_mesh(layer, cur_eff - data.accounted_eff, positions);
    data.accounted_eff = cur_eff;
  }
  /* Invalidate the mesh normal cache (recomputed lazily during the normal extract) and re-extract
   * only the normal buffers; the position buffers are left to the drag's own fast path. */
  mesh.tag_positions_changed();
  IndexMaskMemory memory;
  const IndexMask node_mask = bke::pbvh::all_leaf_nodes(pbvh, memory);
  pbvh.tag_normals_changed(node_mask);
  SLP_PERF("[DEBUG-perf] influence_drag_refresh_normals: eff=%.4f leaves=%lld\n",
           cur_eff,
           int64_t(node_mask.size()));
}

static void influence_drag_propagate_sync_group(bContext *C,
                                                Object &source_ob,
                                                const SculptLayer &layer,
                                                Depsgraph *depsgraph)
{
  if (source_ob.sculpt_layer_sync_group == 0 || layer.base.sync_uid == 0) {
    return;
  }
  Main *bmain = CTX_data_main(C);
  if (bmain == nullptr) {
    return;
  }
  sync_group_propagate_layer_influence(*bmain, source_ob, layer, depsgraph);
}

static void influence_drag_sync_group_during_modal(bContext *C,
                                                   InfluenceDragData &data,
                                                   const float value)
{
  if (value == data.last_sync_propagated_influence) {
    return;
  }
  influence_drag_propagate_sync_group(C, *data.object, *data.layer, nullptr);
  data.last_sync_propagated_influence = value;
}

static void influence_drag_finish(bContext *C, wmOperator *op, const bool cancel)
{
  InfluenceDragData *data = static_cast<InfluenceDragData *>(op->customdata);
  Object &object = *data->object;
  Mesh &mesh = *data->mesh;
  SculptLayer &layer = *data->layer;

  if (data->grids || data->deg_reeval) {
    /* Grid (multires) path, or a shape-key / deform-modifier mesh session: restore or commit the
     * final influence, then re-evaluate honestly — the depsgraph rebuilds the composed surface from
     * `base + sum(enabled layers)` with the new influence. The base (MDisps / basis) is never
     * written (it does not contain layer contributions). */
    layer.influence = cancel ? data->start_influence : layer.influence;
    undo::push_end(object);
    DEG_id_tag_update(&mesh.id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, &object.id);
    WM_event_add_notifier(C, NC_GEOM | ND_DATA, &mesh.id);
    if (!cancel) {
      Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
      influence_drag_propagate_sync_group(C, object, layer, depsgraph);
    }
    MEM_delete(data);
    op->customdata = nullptr;
    return;
  }

  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);

  if (data->gpu_active) {
    /* End the GPU drag and reconcile the CPU positions once. They stayed at the start baseline the
     * whole drag (only #layer.influence and the GPU buffers changed), so a single apply from that
     * baseline to the final (or, on cancel, the start) influence is correct. */
    pbvh.end_influence_drag();
    const float target = cancel ? data->start_influence : layer.influence;
    /* Mirrors #effective at the target influence: a layer that is disabled — on its own or through
     * a disabled folder — contributes nothing however far the slider was dragged. */
    const bool contributes = (layer.base.flag & SCULPT_LAYER_ENABLED) &&
                             !(layer.base.flag & SCULPT_LAYER_GROUP_HIDDEN);
    const float target_eff = contributes ? target : 0.0f;
    layer.influence = target;
    /* The CPU positions sit at #accounted_eff (the start, advanced by any periodic normal-refresh
     * ticks during the drag), so reconcile from there to the final/cancel influence. */
    if (target_eff != data->accounted_eff) {
      MutableSpan<float3> positions = mesh.vert_positions_for_write();
      bke::sculpt_layers::apply_delta_mesh(layer, target_eff - data->accounted_eff, positions);
      data->accounted_eff = target_eff;
    }
  }
  else if (cancel) {
    /* CPU path updated positions every tick, so revert by applying the delta back to the start. */
    influence_drag_apply(mesh, layer, data->start_influence);
  }

  /* Full-quality reconcile of the data that was deferred during the drag: GPU normal buffers, node
   * bounds and the object bounding box. The full mesh tag also invalidates the mesh normal cache
   * that the per-tick #tag_positions_changed_no_normals left stale. */
  mesh.tag_positions_changed();
  IndexMaskMemory memory;
  const IndexMask node_mask = bke::pbvh::all_leaf_nodes(pbvh, memory);
  pbvh.tag_positions_changed(node_mask);
  pbvh.update_bounds_mesh(mesh.vert_positions());
  /* Keep the "original" bounds in step with the moved positions; see #recompute_mesh_canonical for
   * why a stale copy tears the next stroke along node borders. */
  bke::pbvh::store_bounds_orig(pbvh);
  mesh.bounds_set_eager(bke::pbvh::bounds_get(pbvh));
  if (object.runtime->bounds_eval) {
    object.runtime->bounds_eval = mesh.bounds_min_max();
  }

  undo::push_end(object);

  debug_validate_mesh_invariant(object, "drag_finish");

  DEG_id_tag_update(&object.id, ID_RECALC_SHADING);
  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, &object);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, &mesh.id);

  if (!cancel) {
    Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
    influence_drag_propagate_sync_group(C, object, layer, depsgraph);
  }

  MEM_delete(data);
  op->customdata = nullptr;
}

static wmOperatorStatus layer_influence_drag_invoke(bContext *C,
                                                    wmOperator *op,
                                                    const wmEvent *event)
{
  Object &object = *CTX_data_active_object(C);
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  BKE_sculpt_update_object_for_edit(depsgraph, &object, false);
  if (!is_supported(object)) {
    BKE_report(op->reports, RPT_ERROR, "Sculpt layers are not available for this object");
    return OPERATOR_CANCELLED;
  }
  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(object);
  if (!pbvh || (pbvh->type() != bke::pbvh::Type::Mesh && pbvh->type() != bke::pbvh::Type::Grids)) {
    BKE_report(op->reports, RPT_ERROR, "Influence drag is not available for this object");
    return OPERATOR_CANCELLED;
  }
  const bool grids = (pbvh->type() == bke::pbvh::Type::Grids);
  Mesh &mesh = mesh_of(object);
  SculptLayer *layer = bke::sculpt_layers::active_get(mesh);
  if (!layer || layer->domain != domain_for(object) || (layer->base.flag & SCULPT_LAYER_LOCKED)) {
    return OPERATOR_CANCELLED;
  }
  if (!grids) {
    /* Must run before #session_state_ensure: the re-derivation silently absorbs any existing
     * inconsistency into the base, hiding the corruption source. */
    debug_validate_mesh_invariant(object, "drag_invoke");
  }
  session_state_ensure(object);

  InfluenceDragData *data = MEM_new<InfluenceDragData>(__func__);
  data->object = &object;
  data->mesh = &mesh;
  data->layer = layer;
  data->start_influence = layer->influence;
  data->last_sync_propagated_influence = layer->influence;
  data->clamp_min = layer->influence < influence_drag_default_min ? influence_drag_hard_min :
                                                                     influence_drag_default_min;
  data->clamp_max = layer->influence > influence_drag_default_max ? influence_drag_hard_max :
                                                                     influence_drag_default_max;
  data->prev_influence = layer->influence;
  data->start_mouse_x = event->xy[0];
  data->accounted_eff = effective(*layer);
  data->ticks_since_normal_refresh = 0;
  data->grids = grids;
  const SculptSession *drag_ss = object.runtime->sculpt_session;
  /* A shape key / deforming modifier keeps the composed surface out of the mesh positions, so the
   * per-tick fast paths (GPU compute / CPU write) would deform the base. Route those sessions
   * through an honest re-evaluation instead, like the grids path (see #InfluenceDragData). */
  data->deg_reeval = !grids && drag_ss &&
                     (drag_ss->shapekey_active || drag_ss->deform_modifiers_active);
  /* Use the GPU compute path when a draw cache exists (object has been drawn). Otherwise fall back
   * to the CPU per-tick update. Grid (multires) layers and the re-eval path are always off the GPU
   * fast path — the GPU influence compute targets only the direct vertex-domain mesh buffers. */
  data->gpu_active = (grids || data->deg_reeval) ?
                         false :
                         bke::object::pbvh_get(object)->begin_influence_drag(layer->base.uid);
  if (grids) {
    /* The drag changes the layer's influence; consume pending base edits first, while the live
     * CCG still matches the stored layers (see #flush_pending_multires_base). */
    flush_pending_multires_base(object);
    /* Compute the layer's object-space contribution once, so each drag tick is a cheap
     * `positions += contribution * delta` instead of a full re-evaluation. */
    SculptSession *ss = object.runtime->sculpt_session;
    if (ss && ss->subdiv_ccg && layer->data) {
      data->grid_contrib.reinitialize(ss->subdiv_ccg->positions.size());
      if (!BKE_multires_sculpt_layer_object_contribution(
              mesh, *ss->subdiv_ccg, *layer, data->grid_contrib))
      {
        data->grid_contrib = {};
      }
    }
  }
  op->customdata = data;

  SLP_PERF("[DEBUG-perf] influence_drag_invoke: gpu_active=%d uid=%d enabled=%d has_data=%d totelem=%d\n",
           int(data->gpu_active),
           layer->base.uid,
           int((layer->base.flag & SCULPT_LAYER_ENABLED) != 0),
           int(layer->data != nullptr),
           layer->totelem);

  /* Capture the pre-drag layer metadata so Ctrl+Z reverts the influence change. */
  undo::push_begin(*CTX_data_scene(C), object, op);
  undo::push_sculpt_layer_metadata(object, layer->base);

  WM_event_add_modal_handler(C, op);
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus layer_influence_drag_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  InfluenceDragData *data = static_cast<InfluenceDragData *>(op->customdata);

  switch (event->type) {
    case MOUSEMOVE: {
      Object &object = *data->object;
      Mesh &mesh = *data->mesh;
      SculptLayer &layer = *data->layer;
      const float value = std::clamp(
          data->start_influence +
              float(event->xy[0] - data->start_mouse_x) * influence_drag_sensitivity,
          data->clamp_min,
          data->clamp_max);
      if (data->grids) {
        /* Grid (multires) path: update the live CCG positions incrementally from the
         * pre-computed object-space contribution; MDisps are never touched and the release
         * commits with an honest re-evaluation. */
        const float old_eff = effective(layer);
        layer.influence = value;
        const float new_eff = effective(layer);
        SculptSession *ss = object.runtime->sculpt_session;
        if (new_eff != old_eff && ss && ss->subdiv_ccg &&
            data->grid_contrib.size() == ss->subdiv_ccg->positions.size())
        {
          SubdivCCG &subdiv_ccg = *ss->subdiv_ccg;
          MutableSpan<float3> positions = subdiv_ccg.positions;
          const Span<float3> contrib = data->grid_contrib;
          const float delta = new_eff - data->accounted_eff;
          threading::parallel_for(positions.index_range(), 8192, [&](const IndexRange range) {
            for (const int64_t i : range) {
              positions[i] += contrib[i] * delta;
            }
          });
          data->accounted_eff = new_eff;
          /* Keep grid boundaries watertight and shading in sync, then refresh the PBVH. */
          BKE_subdiv_ccg_average_grids(subdiv_ccg);
          BKE_subdiv_ccg_recalc_normals(subdiv_ccg);
          bke::pbvh::Tree &grids_pbvh = *bke::object::pbvh_get(object);
          IndexMaskMemory memory;
          const IndexMask node_mask = bke::pbvh::all_leaf_nodes(grids_pbvh, memory);
          grids_pbvh.tag_positions_changed(node_mask);
          grids_pbvh.update_bounds_grids(subdiv_ccg.positions, subdiv_ccg.grid_area);
          mesh.bounds_set_eager(bke::pbvh::bounds_get(grids_pbvh));
          if (object.runtime->bounds_eval) {
            object.runtime->bounds_eval = mesh.bounds_min_max();
          }
          DEG_id_tag_update(&object.id, ID_RECALC_SHADING);
        }
        ED_region_tag_redraw(CTX_wm_region(C));
        WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, &object.id);
        influence_drag_sync_group_during_modal(C, *data, value);
        break;
      }
      if (data->deg_reeval) {
        /* Shape key / deform modifier: the composed surface is not a direct copy of the mesh
         * positions (the basis is untouched and the layers are re-composed on top at evaluation), so
         * a direct write would deform the base. Change only the influence and let the depsgraph
         * rebuild the surface from `base + sum(enabled layers)` with the new value. */
        layer.influence = value;
        DEG_id_tag_update(&mesh.id, ID_RECALC_GEOMETRY);
        ED_region_tag_redraw(CTX_wm_region(C));
        WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, &object.id);
        WM_event_add_notifier(C, NC_GEOM | ND_DATA, &mesh.id);
        influence_drag_sync_group_during_modal(C, *data, value);
        break;
      }
      bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
      const float old_eff = effective(layer);
      layer.influence = value;
      const float new_eff = effective(layer);
      const bool changed = new_eff != old_eff;
      if (data->gpu_active) {
        /* GPU path: queue this tick's effective-influence delta; the compute that updates the
         * position buffers runs at draw time. CPU positions are reconciled on the periodic normal
         * refresh below and once more on release.
         *
         * NOTE: this delta is a scalar applied to the whole position buffer, so it does not honor a
         * weight mask on the layer or on a folder above it, while the CPU reconcile does. A masked
         * layer therefore displays as if unmasked *during* a drag and snaps to the correct shape on
         * release. Display only — nothing is stored from this path, and the reconcile is what the
         * positions end up being. Left as is deliberately: masking it means resolving
         * #bke::sculpt_layers::node_mask_for_composite per element and uploading the result, which
         * is a second spelling of the composite's fold living in the drag path. It belongs with
         * whoever owns the GPU drag, together with the mask upload it needs. */
        pbvh.add_influence_drag_delta(new_eff - old_eff);
        data->prev_influence = value;
        SLP_PERF("[DEBUG-perf] influence_drag_modal: gpu tick value=%.4f scale=%.5f\n",
                 value,
                 new_eff - old_eff);
      }
      else if (changed) {
        /* CPU fallback: write the mesh positions and refresh only the GPU position buffers. */
        MutableSpan<float3> positions = mesh.vert_positions_for_write();
        bke::sculpt_layers::apply_delta_mesh(layer, new_eff - old_eff, positions);
        mesh.tag_positions_changed_no_normals();
        data->accounted_eff = new_eff;
        IndexMaskMemory memory;
        const IndexMask node_mask = bke::pbvh::all_leaf_nodes(pbvh, memory);
        pbvh.tag_positions_changed_no_normals(node_mask);
      }
      /* Periodically refresh the normal buffers so the shading visibly tracks the drag without
       * paying the full normal re-extract on every tick. */
      if (changed &&
          ++data->ticks_since_normal_refresh >= influence_drag_normal_refresh_interval)
      {
        data->ticks_since_normal_refresh = 0;
        influence_drag_refresh_normals(object, mesh, layer, *data);
      }
      DEG_id_tag_update(&object.id, ID_RECALC_SHADING);
      /* The operator is invoked from the Properties panel button, so #CTX_wm_region is that panel's
       * region, not the 3D viewport. Tag it for the live slider value, but the viewport (where the
       * GPU influence compute runs at draw time) is only refreshed by this object-draw notifier. */
      ED_region_tag_redraw(CTX_wm_region(C));
      WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, &object.id);
      influence_drag_sync_group_during_modal(C, *data, value);
      break;
    }
    case LEFTMOUSE:
      if (event->val == KM_RELEASE) {
        influence_drag_finish(C, op, false);
        return OPERATOR_FINISHED;
      }
      break;
    case RIGHTMOUSE:
    case EVT_ESCKEY:
      /* NOTE: #influence_drag_finish reverts the influence and then still calls #undo::push_end,
       * so a content-neutral step is left on the stack even though this reports as cancelled.
       * Deliberate: #push_begin ran in #invoke and the sculpt undo API offers no way to discard an
       * open step, and leaving it unclosed would corrupt the stack. Reporting #OPERATOR_FINISHED
       * instead would be worse — callers and macros would read Escape as a confirm. */
      influence_drag_finish(C, op, true);
      return OPERATOR_CANCELLED;
    default:
      break;
  }
  return OPERATOR_RUNNING_MODAL;
}

void SCULPT_OT_layer_influence_drag(wmOperatorType *ot)
{
  ot->name = "Drag Sculpt Layer Influence";
  ot->idname = "SCULPT_OT_layer_influence_drag";
  ot->description =
      "Interactively adjust the active sculpt layer's influence by dragging the mouse";
  ot->invoke = layer_influence_drag_invoke;
  ot->modal = layer_influence_drag_modal;
  ot->poll = layers_poll;
  /* No #OPTYPE_UNDO: the metadata sculpt undo step pushed across invoke/finish handles Ctrl+Z. */
  ot->flag = OPTYPE_REGISTER | OPTYPE_BLOCKING;
}

/* The object-mode half of #layer_toggle_visibility_exec — mirrors
 * #layer_group_toggle_visibility_object_mode_exec. */
static wmOperatorStatus layer_toggle_visibility_object_mode_exec(bContext *C,
                                                                 wmOperator *op,
                                                                 Object &object)
{
  Mesh &mesh = mesh_of(object);
  const int layer_uid = RNA_int_get(op->ptr, "layer_uid");
  const bool use_sync_group = RNA_boolean_get(op->ptr, "use_sync_group");
  SculptLayer *layer = layer_row_lookup(mesh, layer_uid);
  if (!layer) {
    BKE_report(op->reports, RPT_ERROR, "No sculpt layer with that id");
    return OPERATOR_CANCELLED;
  }
  Main *bmain = CTX_data_main(C);
  const bool enable = !(layer->base.flag & SCULPT_LAYER_ENABLED);

  Vector<Object *> eligible;
  eligible.append(&object);
  if (use_sync_group && layer->base.sync_uid != 0 && object.sculpt_layer_sync_group != 0) {
    for (Object *member : sync_group_members(*bmain, object)) {
      if (member->type != OB_MESH) {
        BKE_reportf(op->reports,
                    RPT_WARNING,
                    "Toggle Sculpt Layer Visibility: skipping \"%s\" (not a mesh object)",
                    member->id.name + 2);
        continue;
      }
      Mesh &member_mesh = mesh_of(*member);
      SculptLayerTreeNode *member_node = bke::sculpt_layers::node_find_by_sync_uid(
          member_mesh, layer->base.sync_uid);
      if (!bke::sculpt_layers::node_as_layer(member_node)) {
        BKE_reportf(op->reports,
                    RPT_WARNING,
                    "Toggle Sculpt Layer Visibility: skipping \"%s\" (no layer synced to the "
                    "active one)",
                    member->id.name + 2);
        continue;
      }
      eligible.append(member);
    }
  }

  for (Object *target : eligible) {
    Mesh &target_mesh = mesh_of(*target);
    SculptLayer *target_layer = layer;
    if (target != &object) {
      target_layer = bke::sculpt_layers::node_as_layer(
          bke::sculpt_layers::node_find_by_sync_uid(target_mesh, layer->base.sync_uid));
    }
    apply_layer_enabled_value_to_mesh(*bmain, target_mesh, *target_layer, enable);
    DEG_id_tag_update(&target_mesh.id, ID_RECALC_GEOMETRY);
  }

  ED_undo_push(C, "Toggle Sculpt Layer Visibility");
  for (Object *target : eligible) {
    layers_ui_notify(C, *target);
  }
  return OPERATOR_FINISHED;
}

static wmOperatorStatus layer_toggle_visibility_exec(bContext *C, wmOperator *op)
{
  Object *object_poll = CTX_data_active_object(C);
  if (object_poll != nullptr && !(object_poll->mode & OB_MODE_SCULPT)) {
    return layer_toggle_visibility_object_mode_exec(C, op, *object_poll);
  }
  OpContext ctx;
  if (!op_context_get(C, op, ctx)) {
    return OPERATOR_CANCELLED;
  }
  if (mask_edit_refuse_ccg_rebuild(op, *ctx.object)) {
    return OPERATOR_CANCELLED;
  }
  const int layer_uid = RNA_int_get(op->ptr, "layer_uid");
  const bool use_sync_group = RNA_boolean_get(op->ptr, "use_sync_group");
  SculptLayer *layer = layer_row_lookup(*ctx.mesh, layer_uid);
  if (!layer) {
    BKE_report(op->reports, RPT_ERROR, "No sculpt layer with that id");
    return OPERATOR_CANCELLED;
  }
  Main *bmain = CTX_data_main(C);
  /* One target state for every object touched: derived from the active mesh row, then applied
   * absolutely (not per-member XOR) so Alt+sync-group can turn every member on or off together
   * even when their stored bits had drifted apart. */
  const bool enable = !(layer->base.flag & SCULPT_LAYER_ENABLED);

  Vector<Object *> eligible;
  eligible.append(ctx.object);
  if (use_sync_group && layer->base.sync_uid != 0) {
    for (Object *member : fanout_targets(*bmain, *ctx.object)) {
      if (member == ctx.object) {
        continue;
      }
      if (member->runtime->sculpt_session != nullptr) {
        BKE_sculpt_update_object_for_edit(ctx.depsgraph, member, false);
      }
      if (!is_supported(*member)) {
        BKE_reportf(op->reports,
                    RPT_WARNING,
                    "Toggle Sculpt Layer Visibility: skipping \"%s\" (sculpt layers are not "
                    "available for this object)",
                    member->id.name + 2);
        continue;
      }
      Mesh &member_mesh = mesh_of(*member);
      SculptLayerTreeNode *member_node = bke::sculpt_layers::node_find_by_sync_uid(
          member_mesh, layer->base.sync_uid);
      if (!member_node) {
        BKE_reportf(op->reports,
                    RPT_WARNING,
                    "Toggle Sculpt Layer Visibility: skipping \"%s\" (no layer synced to the "
                    "active one)",
                    member->id.name + 2);
        continue;
      }
      if (!bke::sculpt_layers::node_as_layer(member_node)) {
        BKE_reportf(op->reports,
                    RPT_WARNING,
                    "Toggle Sculpt Layer Visibility: skipping \"%s\" (the synced node is a group, "
                    "not a layer)",
                    member->id.name + 2);
        continue;
      }
      if (bke::object::pbvh_get(*member)->type() == bke::pbvh::Type::Grids) {
        flush_pending_multires_base(*member);
      }
      if (mask_edit_refuse_ccg_rebuild(op, *member)) {
        continue;
      }
      eligible.append(member);
    }
  }

  session_state_ensure(*ctx.object);
  undo::push_begin_multi_object(*CTX_data_scene(C), op, eligible.as_span());
  undo::push_sculpt_layer_metadata(*ctx.object, layer->base);
  SET_FLAG_FROM_TEST(layer->base.flag, enable, SCULPT_LAYER_ENABLED);
  commit_layers_change(*ctx.depsgraph, *ctx.object);

  for (Object *member : eligible) {
    if (member == ctx.object) {
      continue;
    }
    session_state_ensure(*member);
    Mesh &member_mesh = mesh_of(*member);
    SculptLayer *member_layer = bke::sculpt_layers::node_as_layer(
        bke::sculpt_layers::node_find_by_sync_uid(member_mesh, layer->base.sync_uid));
    undo::push_sculpt_layer_metadata(*member, member_layer->base);
    SET_FLAG_FROM_TEST(member_layer->base.flag, enable, SCULPT_LAYER_ENABLED);
    commit_layers_change(*ctx.depsgraph, *member);
  }

  undo::finish_multi_object(C, eligible.as_span(), UpdateType::Position);
  layers_ui_notify(C, *ctx.object);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus layer_toggle_visibility_invoke(bContext *C,
                                                       wmOperator *op,
                                                       const wmEvent *event)
{
  RNA_boolean_set(op->ptr, "use_sync_group", (event->modifier & KM_ALT) != 0);
  return layer_toggle_visibility_exec(C, op);
}

void SCULPT_OT_layer_toggle_visibility(wmOperatorType *ot)
{
  ot->name = "Toggle Sculpt Layer Visibility";
  ot->idname = "SCULPT_OT_layer_toggle_visibility";
  ot->description =
      "Toggle whether this sculpt layer contributes to the result. Hold Alt to apply the same "
      "visibility to every object in the sculpt-layer sync group";
  ot->invoke = layer_toggle_visibility_invoke;
  ot->exec = layer_toggle_visibility_exec;
  ot->poll = layers_object_mode_poll;
  ot->flag = OPTYPE_REGISTER;
  RNA_def_int(ot->srna,
              "layer_uid",
              0,
              0,
              INT_MAX,
              "Layer ID",
              "Unique id of the sculpt layer to toggle (0 uses the active layer)",
              0,
              INT_MAX);
  RNA_def_boolean(ot->srna,
                  "use_sync_group",
                  false,
                  "Sync Group",
                  "Apply the toggled visibility to every object in the sculpt-layer sync group");
}

/* Solo Base: sculpting the base while layers are visible bakes the layer residual into the base
 * wherever a surface-dependent brush (smooth / grab / flatten) reshapes the composed surface —
 * the flush must store `surface - layers` to keep the result intact. Isolating the base removes
 * the layers from the surface the brush sees, so base edits stay clean. The enabled flags of the
 * hidden layers are preserved via #SCULPT_LAYER_SOLO_HIDDEN and restored on the second toggle. */

/* One fan-out target of #SCULPT_OT_layer_solo_base: the member and ITS own affected-layer set for
 * the once-computed toggle direction, captured in the gather pass so the fan-out loop does not
 * re-walk the tree under a different (member-local) #solo_active. Flags are not node-freeing, but
 * carrying the list keeps gather and do aligned on the same set the empty-skip already judged. */
struct SoloBaseFanoutMember {
  Object *object;
  Vector<SculptLayer *> affected;
};

/* Whole-tree fan-out: apply the active object's once-computed solo direction to each member's own
 * affected set (mirrors #layer_toggle_rec_exec); do not independently toggle per member. */
static wmOperatorStatus layer_solo_base_exec(bContext *C, wmOperator *op)
{
  OpContext ctx;
  if (!op_context_get(C, op, ctx)) {
    return OPERATOR_CANCELLED;
  }
  if (mask_edit_refuse_ccg_rebuild(op, *ctx.object)) {
    return OPERATOR_CANCELLED;
  }
  Mesh &mesh = *ctx.mesh;

  /* Solo is active when any layer carries the marker (single authority in BKE). Computed once from
   * the active object and applied uniformly to every fanned-out member -- not each member flipping
   * its own independently-current #solo_active (mirrors Task 8c2 #layer_toggle_rec_exec computing
   * target state once). */
  const bool solo_active = bke::sculpt_layers::solo_active(mesh);
  const int test_flag = solo_active ? SCULPT_LAYER_SOLO_HIDDEN : SCULPT_LAYER_ENABLED;

  /* Layers the toggle changes: the enabled ones when activating, the marked ones when ending. */
  Vector<SculptLayer *> affected;
  for (SculptLayer *layer : bke::sculpt_layers::layers(mesh)) {
    if (layer->base.flag & test_flag) {
      affected.append(layer);
    }
  }
  if (affected.is_empty()) {
    return OPERATOR_CANCELLED;
  }
  Main *bmain = CTX_data_main(C);

  /* Gather-then-gate: see #layer_add_exec's own comment for why the fan-out set has to be built
   * before any undo bracket opens. Whole-tree (not find-shaped): every eligible member gets the
   * same batch-hide/restore direction on its own tree; there is no per-node #sync_uid gate. The
   * active object already passed #mask_edit_refuse_ccg_rebuild and the non-empty affected gate
   * above and is included unconditionally. Members whose own affected set is empty for that
   * direction are skipped with a warning (not a cancel). */
  Vector<Object *> eligible;
  eligible.append(ctx.object);
  Vector<SoloBaseFanoutMember> fanout_members;
  for (Object *member : fanout_targets(*bmain, *ctx.object)) {
    if (member == ctx.object) {
      continue;
    }
    /* Mirrors #layer_add_exec's gather loop: guarded on an existing session, since a member that
     * was never taken into sculpt mode has none, and #BKE_sculpt_update_object_for_edit
     * unconditionally dereferences it. Refreshed before #is_supported below so a member that IS in
     * sculpt mode but has not built its PBVH yet is not misreported as unsupported. */
    if (member->runtime->sculpt_session != nullptr) {
      BKE_sculpt_update_object_for_edit(ctx.depsgraph, member, false);
    }
    if (!is_supported(*member)) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Solo Base: skipping \"%s\" (sculpt layers are not available for this object)",
                  member->id.name + 2);
      continue;
    }
    Mesh &member_mesh = mesh_of(*member);
    /* Same #test_flag derived from the active object's #solo_active, applied to this member's own
     * layers -- see the decision comment at the top of this function. */
    Vector<SculptLayer *> member_affected;
    for (SculptLayer *layer : bke::sculpt_layers::layers(member_mesh)) {
      if (layer->base.flag & test_flag) {
        member_affected.append(layer);
      }
    }
    if (member_affected.is_empty()) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Solo Base: skipping \"%s\" (no layers affected by this solo toggle)",
                  member->id.name + 2);
      continue;
    }
    if (bke::object::pbvh_get(*member)->type() == bke::pbvh::Type::Grids) {
      /* Mirrors #op_context_get / #layer_add_exec: consume any pending base sculpt edits first,
       * while the live CCG still matches the stored layers. */
      flush_pending_multires_base(*member);
    }
    /* Mirrors the active object's own gate above: an open weight-mask edit session on this member
     * would have its multires rebuild silently discard the session. Reports the error itself. */
    if (mask_edit_refuse_ccg_rebuild(op, *member)) {
      continue;
    }
    eligible.append(member);
    SoloBaseFanoutMember fanout_member;
    fanout_member.object = member;
    fanout_member.affected = std::move(member_affected);
    fanout_members.append(std::move(fanout_member));
  }

  /* Derive the mesh-domain runtime base from the still-consistent pre-change state (pending
   * multires base edits were already consumed by #op_context_get). */
  session_state_ensure(*ctx.object);

  undo::push_begin_multi_object(*CTX_data_scene(C), op, eligible.as_span());
  /* Active object: unchanged from the single-object path. */
  Vector<int> uids;
  Vector<int> flags;
  uids.reserve(affected.size());
  flags.reserve(affected.size());
  for (const SculptLayer *layer : affected) {
    uids.append(layer->base.uid);
    flags.append(layer->base.flag);
  }
  undo::push_sculpt_layer_flags_batch(*ctx.object, std::move(uids), std::move(flags));

  for (SculptLayer *layer : affected) {
    if (solo_active) {
      layer->base.flag |= SCULPT_LAYER_ENABLED;
      layer->base.flag &= ~SCULPT_LAYER_SOLO_HIDDEN;
    }
    else {
      layer->base.flag &= ~SCULPT_LAYER_ENABLED;
      layer->base.flag |= SCULPT_LAYER_SOLO_HIDDEN;
    }
  }

  commit_layers_change(*ctx.depsgraph, *ctx.object);

  /* Fan out: same direction (#solo_active) on each member's own pre-gathered affected set. */
  for (const SoloBaseFanoutMember &fanout_member : fanout_members) {
    Object &member = *fanout_member.object;
    /* Eligibility (#is_supported, non-empty affected, #mask_edit_refuse_ccg_rebuild) and the
     * multires pending-base flush already ran for every entry of `fanout_members` in the gather
     * pass above -- nothing here can fail or need skipping (mirrors #layer_add_exec's own second
     * loop). */
    session_state_ensure(member);
    Vector<int> member_uids;
    Vector<int> member_flags;
    member_uids.reserve(fanout_member.affected.size());
    member_flags.reserve(fanout_member.affected.size());
    for (const SculptLayer *layer : fanout_member.affected) {
      member_uids.append(layer->base.uid);
      member_flags.append(layer->base.flag);
    }
    undo::push_sculpt_layer_flags_batch(member, std::move(member_uids), std::move(member_flags));

    for (SculptLayer *layer : fanout_member.affected) {
      if (solo_active) {
        layer->base.flag |= SCULPT_LAYER_ENABLED;
        layer->base.flag &= ~SCULPT_LAYER_SOLO_HIDDEN;
      }
      else {
        layer->base.flag &= ~SCULPT_LAYER_ENABLED;
        layer->base.flag |= SCULPT_LAYER_SOLO_HIDDEN;
      }
    }
    commit_layers_change(*ctx.depsgraph, member);
  }

  /* #commit_layers_change unconditionally recomposes the surface, on the active object and every
   * fanned-out member alike, so #UpdateType::Position is required the same way #layer_add_exec
   * requires it. No explicit #rec_exemption_refresh: it already runs inside #commit_layers_change. */
  undo::finish_multi_object(C, eligible.as_span(), UpdateType::Position);
  /* Active-object-only, matching #layer_add_exec / every other #finish_multi_object-based fan-out
   * in this file: #finish_multi_object already sent `NC_OBJECT | ND_DRAW` for every member above. */
  layers_ui_notify(C, *ctx.object);
  return OPERATOR_FINISHED;
}

void SCULPT_OT_layer_solo_base(wmOperatorType *ot)
{
  ot->name = "Solo Base";
  ot->idname = "SCULPT_OT_layer_solo_base";
  ot->description =
      "Toggle isolating the base shape: temporarily hide all sculpt layers so the base can be "
      "sculpted directly without baking layer residue into it, then restore them";
  ot->exec = layer_solo_base_exec;
  ot->poll = layers_poll;
  /* Metadata-only change: the sculpt undo step pushed by the operator handles Ctrl+Z (matching
   * the visibility / influence / REC toggles); a global undo push would not compose with the
   * stroke SCULPT steps. */
  ot->flag = OPTYPE_REGISTER;
}

/* The object-mode half of #layer_group_toggle_visibility_exec.
 *
 * The mutation itself is identical and mode-independent: flip #SCULPT_LAYER_GROUP_ENABLED, then let
 * #resync_group_state re-bake #SCULPT_LAYER_GROUP_HIDDEN and #group_influence_cached onto every
 * descendant. What cannot come along is everything around it — #op_context_get dereferences the
 * PBVH, and #undo::push_begin dereferences both the session and the PBVH — so this path is written
 * out separately rather than guarded, exactly as #layer_select_object_mode_exec is.
 *
 * The surface is brought in line the way the RNA setters already do it outside sculpt mode
 * (#rna_SculptLayerGroup_influence_set): per descendant, apply the difference between its effective
 * weight before and after. #commit_layers_change is not usable here — it recomposes from the runtime
 * base, which only exists inside a session. */
static wmOperatorStatus layer_group_toggle_visibility_object_mode_exec(bContext *C,
                                                                      wmOperator *op,
                                                                      Object &object)
{
  Mesh &mesh = mesh_of(object);
  const int group_uid = RNA_int_get(op->ptr, "group_uid");
  SculptLayerGroup *group = group_row_lookup(mesh, group_uid);
  if (!group) {
    BKE_report(op->reports, RPT_ERROR, "No sculpt layer group with that id");
    return OPERATOR_CANCELLED;
  }
  Main *bmain = CTX_data_main(C);

  /* Gather-then-gate, as every fan-out elsewhere in this file: the active object is included
   * unconditionally, and every other sync-group member is brought in only if it clears two gates.
   * There is no #is_supported / session / PBVH gate here, unlike the in-sculpt-mode variant above --
   * this whole code path has no #SculptSession to begin with -- but #mesh_of is still an unguarded
   * cast to `Mesh *`, and #sync_group_members applies no object-type filter of its own, so a member
   * of a different type has to be turned away before it reaches #mesh_of. */
  Vector<Object *> eligible;
  eligible.append(&object);
  for (Object *member : sync_group_members(*bmain, object)) {
    if (member->type != OB_MESH) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Toggle Sculpt Layer Group Visibility: skipping \"%s\" (not a mesh object)",
                  member->id.name + 2);
      continue;
    }
    /* #node_find_by_sync_uid resolves the SAME group wherever it sits in the member's own tree;
     * the member's own group is unrelated. A zero #sync_uid (the active group was never
     * fanned-out-created) makes every member miss here by design, which degrades this operator to
     * the single-object behavior it always had. */
    Mesh &member_mesh = mesh_of(*member);
    SculptLayerTreeNode *member_node = bke::sculpt_layers::node_find_by_sync_uid(
        member_mesh, group->base.sync_uid);
    if (!member_node) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Toggle Sculpt Layer Group Visibility: skipping \"%s\" (no group synced to the "
                  "active one)",
                  member->id.name + 2);
      continue;
    }
    /* Defensive: #sync_uid should only ever link a group to a group, but the tree is
     * user-editable, so the kind found is not assumed. */
    if (!bke::sculpt_layers::node_as_group(member_node)) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Toggle Sculpt Layer Group Visibility: skipping \"%s\" (the synced node is a "
                  "layer, not a group)",
                  member->id.name + 2);
      continue;
    }
    eligible.append(member);
  }

  /* The exact mutation the single-object path always ran, once per fanned-out target.
   * #ED_undo_push is a single global memfile snapshot of the whole file rather than a per-object
   * push, so unlike the mutation itself it is not repeated per target -- it runs exactly once below,
   * after every target's tree (and, where writable, positions) have settled. */
  for (Object *target : eligible) {
    Mesh &target_mesh = mesh_of(*target);
    SculptLayerGroup *target_group = group;
    if (target != &object) {
      target_group = bke::sculpt_layers::node_as_group(
          bke::sculpt_layers::node_find_by_sync_uid(target_mesh, group->base.sync_uid));
    }

    const Span<SculptLayer *> descendants = bke::sculpt_layers::layers(*target_group);
    Vector<float> old_effective(descendants.size());
    for (const int64_t i : descendants.index_range()) {
      old_effective[i] = bke::sculpt_layers::effective(*descendants[i]);
    }

    target_group->base.flag ^= SCULPT_LAYER_GROUP_ENABLED;
    bke::sculpt_layers::resync_group_state(target_mesh);

    /* Skipped wholesale for the cases where the stored positions are not what the surface is
     * composed from, and where writing a delta into them would corrupt the basis rather than update
     * it: an edit mesh owns the live positions, and under a shape key the layers are an
     * evaluation-time overlay over an untouched basis (#apply_vert_layers_eval). Both are covered by
     * the re-evaluation tagged below instead. Grid-domain descendants are skipped per layer for the
     * same reason, inside the loop. */
    const bool positions_writable = target_mesh.verts_num > 0 &&
                                    target_mesh.runtime->edit_mesh == nullptr &&
                                    target_mesh.key == nullptr;
    if (positions_writable) {
      MutableSpan<float3> positions = target_mesh.vert_positions_for_write();
      bool moved = false;
      for (const int64_t i : descendants.index_range()) {
        SculptLayer &layer = *descendants[i];
        const float delta = bke::sculpt_layers::effective(layer) - old_effective[i];
        if (delta == 0.0f || layer.domain != SCULPT_LAYER_DOMAIN_VERT) {
          continue;
        }
        bke::sculpt_layers::apply_delta_mesh(layer, delta, positions);
        moved = true;
      }
      if (moved) {
        target_mesh.tag_positions_changed();
      }
    }

    /* Unconditional, and the only thing that updates the shape-key and multires cases: there the
     * incremental write above is deliberately skipped and the composite is rebuilt from stored data
     * at evaluation. */
    DEG_id_tag_update(&target_mesh.id, ID_RECALC_GEOMETRY);
  }

  /* A single plain memfile step for the whole fan-out, for the reason given in
   * #layer_select_object_mode_exec: this path has no per-object sculpt undo stack to push into, so
   * one global step covers every target's mutation above. */
  ED_undo_push(C, "Toggle Sculpt Layer Group Visibility");
  for (Object *target : eligible) {
    layers_ui_notify(C, *target);
  }
  return OPERATOR_FINISHED;
}

static wmOperatorStatus layer_group_toggle_visibility_exec(bContext *C, wmOperator *op)
{
  Object *object_poll = CTX_data_active_object(C);
  if (object_poll != nullptr && !(object_poll->mode & OB_MODE_SCULPT)) {
    return layer_group_toggle_visibility_object_mode_exec(C, op, *object_poll);
  }
  OpContext ctx;
  if (!op_context_get(C, op, ctx)) {
    return OPERATOR_CANCELLED;
  }
  if (mask_edit_refuse_ccg_rebuild(op, *ctx.object)) {
    return OPERATOR_CANCELLED;
  }
  const int group_uid = RNA_int_get(op->ptr, "group_uid");
  SculptLayerGroup *group = group_row_lookup(*ctx.mesh, group_uid);
  if (!group) {
    BKE_report(op->reports, RPT_ERROR, "No sculpt layer group with that id");
    return OPERATOR_CANCELLED;
  }
  Main *bmain = CTX_data_main(C);

  /* Gather-then-gate: see #layer_add_exec's own comment for why the fan-out set has to be built
   * before any undo bracket opens. The active object is included unconditionally; every other
   * sync-group member is brought up to date and gated exactly as #op_context_get gates the active
   * object, plus a find-shaped gate #layer_add_exec has no analog for: the member must actually
   * hold a group synced to the active one. */
  Vector<Object *> eligible;
  eligible.append(ctx.object);
  for (Object *member : fanout_targets(*bmain, *ctx.object)) {
    if (member == ctx.object) {
      continue;
    }
    /* Mirrors #layer_add_exec's gather loop: guarded on an existing session, since a member that
     * was never taken into sculpt mode has none, and #BKE_sculpt_update_object_for_edit
     * unconditionally dereferences it. Refreshed before #is_supported below so a member that IS in
     * sculpt mode but has not built its PBVH yet is not misreported as unsupported. */
    if (member->runtime->sculpt_session != nullptr) {
      BKE_sculpt_update_object_for_edit(ctx.depsgraph, member, false);
    }
    if (!is_supported(*member)) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Toggle Sculpt Layer Group Visibility: skipping \"%s\" (sculpt layers are not "
                  "available for this object)",
                  member->id.name + 2);
      continue;
    }
    /* #node_find_by_sync_uid resolves the SAME group wherever it sits in the member's own tree;
     * the member's own group is unrelated. A zero #sync_uid (the active group was never
     * fanned-out-created) makes every member miss here by design -- see the function's own doc
     * comment -- which degrades this operator to the single-object behavior it always had. */
    Mesh &member_mesh = mesh_of(*member);
    SculptLayerTreeNode *member_node = bke::sculpt_layers::node_find_by_sync_uid(
        member_mesh, group->base.sync_uid);
    if (!member_node) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Toggle Sculpt Layer Group Visibility: skipping \"%s\" (no group synced to the "
                  "active one)",
                  member->id.name + 2);
      continue;
    }
    /* Defensive: #sync_uid should only ever link a group to a group, but the tree is
     * user-editable, so the kind found is not assumed. */
    if (!bke::sculpt_layers::node_as_group(member_node)) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Toggle Sculpt Layer Group Visibility: skipping \"%s\" (the synced node is a "
                  "layer, not a group)",
                  member->id.name + 2);
      continue;
    }
    if (bke::object::pbvh_get(*member)->type() == bke::pbvh::Type::Grids) {
      /* Mirrors #op_context_get / #layer_add_exec: consume any pending base sculpt edits first,
       * while the live CCG still matches the stored layers. */
      flush_pending_multires_base(*member);
    }
    if (mask_edit_refuse_ccg_rebuild(op, *member)) {
      continue;
    }
    eligible.append(member);
  }

  /* Derive the mesh-domain runtime base from the still-consistent pre-change state (pending multires
   * base edits were already consumed by #op_context_get), as the Solo Base toggle does. */
  session_state_ensure(*ctx.object);

  undo::push_begin_multi_object(*CTX_data_scene(C), op, eligible.as_span());
  /* Active object: unchanged from the single-object path. */
  undo::push_sculpt_layer_metadata(*ctx.object, group->base);
  group->base.flag ^= SCULPT_LAYER_GROUP_ENABLED;
  group_cascade_resync_with_undo(*ctx.object, *ctx.mesh);
  commit_layers_change(*ctx.depsgraph, *ctx.object);

  for (Object *member : eligible) {
    if (member == ctx.object) {
      continue;
    }
    /* Eligibility (#is_supported, sync_uid resolution, #mask_edit_refuse_ccg_rebuild) and the
     * multires pending-base flush already ran for every member of `eligible` in the gather pass
     * above -- nothing here can fail or need skipping (mirrors #layer_add_exec's own second
     * loop). */
    session_state_ensure(*member);
    Mesh &member_mesh = mesh_of(*member);
    SculptLayerGroup *member_group = bke::sculpt_layers::node_as_group(
        bke::sculpt_layers::node_find_by_sync_uid(member_mesh, group->base.sync_uid));
    undo::push_sculpt_layer_metadata(*member, member_group->base);
    member_group->base.flag ^= SCULPT_LAYER_GROUP_ENABLED;
    group_cascade_resync_with_undo(*member, member_mesh);
    commit_layers_change(*ctx.depsgraph, *member);
  }

  /* #commit_layers_change unconditionally recomposes the surface, on the active object and every
   * fanned-out member alike, so #UpdateType::Position is required the same way #layer_add_exec
   * requires it. */
  undo::finish_multi_object(C, eligible.as_span(), UpdateType::Position);
  layers_ui_notify(C, *ctx.object);
  return OPERATOR_FINISHED;
}

void SCULPT_OT_layer_group_toggle_visibility(wmOperatorType *ot)
{
  ot->name = "Toggle Sculpt Layer Group Visibility";
  ot->idname = "SCULPT_OT_layer_group_toggle_visibility";
  ot->description = "Toggle visibility of a sculpt layer group and everything inside it";
  ot->exec = layer_group_toggle_visibility_exec;
  ot->poll = layers_object_mode_poll;
  /* No #OPTYPE_UNDO: the operator pushes its own sculpt undo step; a global push would insert a
   * memfile step that does not compose with the stroke SCULPT steps (see #SCULPT_OT_layer_solo_base). */
  ot->flag = OPTYPE_REGISTER;
  RNA_def_int(ot->srna,
              "group_uid",
              0,
              0,
              INT_MAX,
              "Group ID",
              "Unique id of the sculpt layer group to toggle",
              0,
              INT_MAX);
}

static wmOperatorStatus layer_group_color_tag_exec(bContext *C, wmOperator *op)
{
  OpContext ctx;
  if (!op_context_get(C, op, ctx)) {
    return OPERATOR_CANCELLED;
  }
  const int group_uid = RNA_int_get(op->ptr, "group_uid");
  SculptLayerGroup *group = group_row_lookup(*ctx.mesh, group_uid);
  if (!group) {
    BKE_report(op->reports, RPT_ERROR, "No sculpt layer group with that id");
    return OPERATOR_CANCELLED;
  }
  Main *bmain = CTX_data_main(C);

  /* Gather-then-gate: see #layer_add_exec's own comment for why the fan-out set has to be built
   * before any undo bracket opens. The active object is included unconditionally; every other
   * sync-group member is brought up to date and gated exactly as #op_context_get gates the active
   * object, plus a find-shaped gate #layer_add_exec has no analog for: the member must actually
   * hold a group synced to the active one. Unlike the layer/group operators that touch the
   * surface, there is no multires/CCG gate here -- a color tag never depends on the runtime base or
   * the PBVH, so #flush_pending_multires_base and #mask_edit_refuse_ccg_rebuild are not applicable. */
  Vector<Object *> eligible;
  eligible.append(ctx.object);
  for (Object *member : fanout_targets(*bmain, *ctx.object)) {
    if (member == ctx.object) {
      continue;
    }
    /* Mirrors #layer_add_exec's gather loop: guarded on an existing session, since a member that
     * was never taken into sculpt mode has none, and #BKE_sculpt_update_object_for_edit
     * unconditionally dereferences it. Refreshed before #is_supported below so a member that IS in
     * sculpt mode but has not built its PBVH yet is not misreported as unsupported. */
    if (member->runtime->sculpt_session != nullptr) {
      BKE_sculpt_update_object_for_edit(ctx.depsgraph, member, false);
    }
    if (!is_supported(*member)) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Set Sculpt Layer Group Color Tag: skipping \"%s\" (sculpt layers are not "
                  "available for this object)",
                  member->id.name + 2);
      continue;
    }
    /* #node_find_by_sync_uid resolves the SAME group wherever it sits in the member's own tree;
     * the member's own group is unrelated. A zero #sync_uid (the active group was never
     * fanned-out-created) makes every member miss here by design -- see the function's own doc
     * comment -- which degrades this operator to the single-object behavior it always had. */
    Mesh &member_mesh = mesh_of(*member);
    SculptLayerTreeNode *member_node = bke::sculpt_layers::node_find_by_sync_uid(
        member_mesh, group->base.sync_uid);
    if (!member_node) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Set Sculpt Layer Group Color Tag: skipping \"%s\" (no group synced to the "
                  "active one)",
                  member->id.name + 2);
      continue;
    }
    /* Defensive: #sync_uid should only ever link a group to a group, but the tree is
     * user-editable, so the kind found is not assumed. */
    if (!bke::sculpt_layers::node_as_group(member_node)) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Set Sculpt Layer Group Color Tag: skipping \"%s\" (the synced node is a "
                  "layer, not a group)",
                  member->id.name + 2);
      continue;
    }
    eligible.append(member);
  }

  /* Neither #session_state_ensure nor #commit_layers_change, unlike every neighbouring group
   * operator: a color tag is drawn state only, so nothing about the sculpted shape or the layer
   * cascade depends on it. The undo step is still a sculpt step, so it interleaves correctly with
   * the stroke steps around it.
   *
   * #finish_multi_object is not used to close the step either: it would call #flush_update_done
   * per object, which does work (PBVH ensure, viewport/geometry tags) the single-object do-path
   * never performed for a color tag. #push_end_all_ex(false, true) is the same close
   * #finish_multi_object itself uses internally, minus that per-object flush loop -- the documented
   * multi-object idiom in `sculpt_undo.hh` for exactly this case -- so the fan-out's do-path stays
   * identical to what the active-object-only version always did. #layers_ui_notify has none of
   * that work (just two #WM_event_add_notifier calls), so it is looped over every member below to
   * still redraw each one. */
  const int8_t color_tag = int8_t(RNA_enum_get(op->ptr, "color_tag"));
  undo::push_begin_multi_object(*CTX_data_scene(C), op, eligible.as_span());
  /* Active object: unchanged from the single-object path. */
  undo::push_sculpt_layer_metadata(*ctx.object, group->base);
  group->base.color_tag = color_tag;

  for (Object *member : eligible) {
    if (member == ctx.object) {
      continue;
    }
    /* Eligibility (#is_supported, sync_uid resolution) already ran for every member of `eligible`
     * in the gather pass above -- nothing here can fail or need skipping (mirrors
     * #layer_add_exec's own second loop). */
    Mesh &member_mesh = mesh_of(*member);
    SculptLayerGroup *member_group = bke::sculpt_layers::node_as_group(
        bke::sculpt_layers::node_find_by_sync_uid(member_mesh, group->base.sync_uid));
    undo::push_sculpt_layer_metadata(*member, member_group->base);
    member_group->base.color_tag = color_tag;
  }

  undo::push_end_all_ex(false, true);
  for (Object *member : eligible) {
    layers_ui_notify(C, *member);
  }
  return OPERATOR_FINISHED;
}

void SCULPT_OT_layer_group_color_tag(wmOperatorType *ot)
{
  ot->name = "Set Sculpt Layer Group Color Tag";
  ot->idname = "SCULPT_OT_layer_group_color_tag";
  ot->description = "Set the color tag shown on a sculpt layer folder";
  ot->exec = layer_group_color_tag_exec;
  ot->poll = layers_poll;
  /* No #OPTYPE_UNDO: the operator pushes its own sculpt undo step; a global push would insert a
   * memfile step that does not compose with the stroke SCULPT steps (see #SCULPT_OT_layer_solo_base). */
  ot->flag = OPTYPE_REGISTER;
  RNA_def_int(ot->srna,
              "group_uid",
              0,
              0,
              INT_MAX,
              "Group ID",
              "Unique id of the sculpt layer group to tag",
              0,
              INT_MAX);
  RNA_def_enum(ot->srna,
               "color_tag",
               rna_enum_sculpt_layergroup_color_items,
               SCULPT_LAYER_COLOR_NONE,
               "Color Tag",
               "Color tag to assign to the group");
}

static wmOperatorStatus layer_group_add_exec(bContext *C, wmOperator *op)
{
  OpContext ctx;
  if (!op_context_get(C, op, ctx)) {
    return OPERATOR_CANCELLED;
  }
  if (mask_edit_refuse_ccg_rebuild(op, *ctx.object)) {
    return OPERATOR_CANCELLED;
  }
  Mesh &mesh = *ctx.mesh;
  Main *bmain = CTX_data_main(C);

  /* Gather-then-gate: see #layer_add_exec's own comment for why the fan-out set has to be built
   * before any undo bracket opens. The active object is included unconditionally; every other
   * sync-group member is brought up to date and gated exactly as #op_context_get gates the active
   * object. */
  Vector<Object *> eligible;
  Set<Object *> tree_only_members;
  eligible.append(ctx.object);
  for (Object *member : fanout_targets(*bmain, *ctx.object)) {
    if (member == ctx.object) {
      continue;
    }
    bool tree_only = false;
    if (!sync_group_tree_create_member_prepare(ctx.depsgraph,
                                               *ctx.object,
                                               member,
                                               op,
                                               "Add Sculpt Layer Group",
                                               tree_only))
    {
      continue;
    }
    if (tree_only) {
      tree_only_members.add(member);
      eligible.append(member);
      continue;
    }
    if (bke::object::pbvh_get(*member)->type() == bke::pbvh::Type::Grids) {
      flush_pending_multires_base(*member);
    }
    if (mask_edit_refuse_ccg_rebuild(op, *member)) {
      continue;
    }
    eligible.append(member);
  }

  /* Wrap the current selection, if any: the new folder takes the place of the first selected row
   * (so it appears where the user was looking) and everything selected moves into it.
   *
   * The gather skips whatever sits inside a selected folder, which is what makes the wrap safe as
   * well as right: the first wrapped node is then never nested in another wrapped folder, so the
   * new folder — a sibling of that first node — is never inside one either, and no move below can
   * detach a subtree.
   *
   * This reads the active object's own tree-panel selection only; a sync-group member's tree is
   * not shown while fanning out from the active object, so there is nothing analogous to wrap for
   * a member (see the plain #group_add below, in the fan-out loop). */
  Vector<SculptLayerTreeNode *> wrapped;
  selected_nodes_gather(*bke::sculpt_layers::root_group(mesh), wrapped);

  /* The new folder is a sibling of what it wraps, so it lands in that node's parent, in that
   * node's own slot; with nothing selected it is appended to the root. Both are read before
   * anything moves. */
  SculptLayerGroup *parent = wrapped.is_empty() ? bke::sculpt_layers::root_group(mesh) :
                                                  wrapped.first()->parent;
  SculptLayerTreeNode *slot = wrapped.is_empty() ? nullptr : wrapped.first()->prev;

  session_state_ensure(*ctx.object);
  undo::push_begin_multi_object(*CTX_data_scene(C), op, eligible.as_span());
  /* Active object: unchanged from the single-object path. */

  SculptLayerGroup *group = bke::sculpt_layers::group_add(mesh, "Group", parent->base.uid);

  /* #group_add appends to the parent's children; take the wrapped selection's slot instead. With
   * one children list holding both kinds, a folder can now take a *layer's* slot — the flat lists
   * could only ever put it before another folder. */
  if (!wrapped.is_empty()) {
    bke::sculpt_layers::node_move_into(mesh, group->base, *parent, slot);
  }
  /* Only the uid is recorded here; the anchor is read off the live group when undo extracts it, so
   * this push does not care whether it runs before or after the placement above. */
  undo::push_sculpt_layer_group_list_change(*ctx.object, {}, {group->base.uid});

  if (!wrapped.is_empty()) {
    Vector<undo::ReparentMove> moves;
    moves.reserve(wrapped.size());
    for (const SculptLayerTreeNode *node : wrapped) {
      undo::ReparentMove move;
      move.uid = node->uid;
      move.prev_from = node_prev_uid(*node);
      move.group_from = node_parent_uid(*node);
      move.group_to = group->base.uid;
      moves.append(move);
    }
    /* Into the (empty) new folder in selection order, chaining after the previously placed node —
     * the same running cursor #layer_move_to_exec uses, and for the same reason: it preserves the
     * selection's mutual order. */
    SculptLayerTreeNode *cursor = nullptr;
    for (SculptLayerTreeNode *node : wrapped) {
      bke::sculpt_layers::node_move_into(mesh, *node, *group, cursor);
      cursor = node;
    }
    for (const int64_t i : wrapped.index_range()) {
      moves[i].prev_to = node_prev_uid(*wrapped[i]);
    }
    undo::push_sculpt_layer_reparent(*ctx.object, std::move(moves));
  }

  group_cascade_resync_with_undo(*ctx.object, mesh);
  commit_layers_change(*ctx.depsgraph, *ctx.object);

  /* Fan out: mint ONE shared #SculptLayerTreeNode::sync_uid for the active object's new folder and
   * every sync-group member's new folder, so #node_find_by_sync_uid can resolve "the same" folder
   * across the group. `eligible[0]` is always `ctx.object` (appended first, above), hence the
   * `member == ctx.object` skip below rather than a `drop_front(1)`, which would silently
   * misbehave if that ordering guarantee ever changed. */
  if (ctx.object->sculpt_layer_sync_group != 0) {
    const int new_sync_uid = bke::sculpt_layers::layer_sync_uid_unique(*bmain);
    group->base.sync_uid = new_sync_uid;

    for (Object *member : eligible) {
      if (member == ctx.object) {
        continue;
      }
      if (tree_only_members.contains(member)) {
        Mesh &member_mesh = mesh_of(*member);
        SculptLayerGroup *member_group = bke::sculpt_layers::group_add(
            member_mesh, "Group", bke::sculpt_layers::root_group(member_mesh)->base.uid);
        member_group->base.sync_uid = new_sync_uid;
        undo::push_sculpt_layer_group_list_change(*member, {}, {member_group->base.uid});
        bke::sculpt_layers::resync_group_state(member_mesh);
        DEG_id_tag_update(&member_mesh.id, ID_RECALC_GEOMETRY);
        continue;
      }
      /* Eligibility (#is_supported, #mask_edit_refuse_ccg_rebuild) and the multires pending-base
       * flush already ran for every member of `eligible` in the gather pass above -- nothing here
       * can fail or need skipping (mirrors #layer_add_exec's own second loop).
       *
       * A plain new folder at the member's own tree root, not a wrap of any selection: the member's
       * tree is not shown while fanning out from the active object, so there is no analogous
       * selection state to wrap (see the comment above #selected_nodes_gather's call). */
      session_state_ensure(*member);
      Mesh &member_mesh = mesh_of(*member);
      SculptLayerGroup *member_group = bke::sculpt_layers::group_add(
          member_mesh, "Group", bke::sculpt_layers::root_group(member_mesh)->base.uid);
      member_group->base.sync_uid = new_sync_uid;
      undo::push_sculpt_layer_group_list_change(*member, {}, {member_group->base.uid});
      group_cascade_resync_with_undo(*member, member_mesh);
      commit_layers_change(*ctx.depsgraph, *member);
    }
  }

  /* #commit_layers_change unconditionally recomposes the surface, on the active object and every
   * fanned-out member alike, so #UpdateType::Position is required the same way #layer_add_exec
   * requires it. */
  Vector<Object *> eligible_finish;
  eligible_finish.reserve(eligible.size());
  for (Object *ob : eligible) {
    if (!tree_only_members.contains(ob)) {
      eligible_finish.append(ob);
    }
  }
  undo::finish_multi_object(C, eligible_finish.as_span(), UpdateType::Position);
  for (Object *member : eligible) {
    layers_ui_notify(C, *member);
  }
  return OPERATOR_FINISHED;
}

void SCULPT_OT_layer_group_add(wmOperatorType *ot)
{
  ot->name = "Add Sculpt Layer Group";
  ot->idname = "SCULPT_OT_layer_group_add";
  ot->description = "Add a folder, moving the selected sculpt layers and groups into it";
  ot->exec = layer_group_add_exec;
  ot->poll = layers_poll;
  /* No #OPTYPE_UNDO: the operator pushes its own sculpt undo step; a global push would insert a
   * memfile step that does not compose with the stroke SCULPT steps (see #SCULPT_OT_layer_solo_base). */
  ot->flag = OPTYPE_REGISTER;
}

/* One sync-group member's counterpart for a folder disband: the member's own folder synced to the
 * one the active object is disbanding. Only one node is involved per object, so this is a single
 * pointer rather than the lists #RemoveFanoutMember / #MergeSelectedFanoutMember carry.
 *
 * Resolved once, in the gather pass, and carried forward rather than re-derived in the fan-out
 * loop (the idiom every non-destructive find-shaped fan-out in this file uses) — the same reason
 * #RemoveFanoutMember exists: #bke::sculpt_layers::group_remove frees the folder outright, so once
 * the active object's own disband has run, re-deriving a member's folder from
 * `group->base.sync_uid` would read freed memory. Safe because #fanout_targets dedups by
 * #Object::data, so no two members ever share a #Mesh and nothing touches this member's tree until
 * its own turn below. */
struct GroupRemoveFanoutMember {
  Object *object;
  SculptLayerGroup *group;
};

static wmOperatorStatus layer_group_remove_exec(bContext *C, wmOperator *op)
{
  OpContext ctx;
  if (!op_context_get(C, op, ctx)) {
    return OPERATOR_CANCELLED;
  }
  Mesh &mesh = *ctx.mesh;
  const int group_uid = RNA_int_get(op->ptr, "group_uid");
  SculptLayerGroup *group = group_row_lookup(mesh, group_uid);
  if (!group) {
    BKE_report(op->reports, RPT_ERROR, "No sculpt layer group with that id");
    return OPERATOR_CANCELLED;
  }
  /* Disbanding, not deleting: the direct children rise to the removed folder's own parent. Nothing
   * inside is removed — cascading deletion is a separate feature (design doc, non-goals). */
  SculptLayerGroup &parent = *group->base.parent;
  const int parent_uid = parent.base.uid;
  Main *bmain = CTX_data_main(C);

  /* Gather-then-gate: see #layer_add_exec's own comment for why the fan-out set has to be built
   * before any undo bracket opens. The active object is included unconditionally; every other
   * sync-group member is brought up to date and gated exactly as #op_context_get gates the active
   * object, plus a find-shaped gate #layer_add_exec has no analog for: the member must actually
   * hold a folder synced to the active one.
   *
   * Two gates the active object's own path needs are deliberately absent here. There is no
   * "does the folder have children" gate: an empty folder is a perfectly ordinary disband target
   * (the relink below simply moves nothing), so a member whose synced folder happens to be empty
   * still participates. And there is no "does it have a parent" gate: #node_find_by_sync_uid walks
   * the root group's descendants only and never hands back the root itself, so anything it
   * resolves is a child of some folder and always carries a non-null #base.parent — unlike
   * #group_row_lookup, which has to refuse uid 0 explicitly.
   *
   * Unlike #layer_add_exec / #layer_clear_exec this gather does NOT call
   * #mask_edit_refuse_ccg_rebuild on a member: a disband is one of the operators that *closes* an
   * open mask-edit session rather than refusing on its account (see #layer_add_exec's own comment
   * on that split), and the fan-out loop below closes each member's own session unconditionally,
   * exactly as the active object's own body does. */
  Vector<Object *> eligible;
  eligible.append(ctx.object);
  Vector<GroupRemoveFanoutMember> fanout_members;
  for (Object *member : fanout_targets(*bmain, *ctx.object)) {
    if (member == ctx.object) {
      continue;
    }
    /* Mirrors #layer_add_exec's gather loop: guarded on an existing session, since a member that
     * was never taken into sculpt mode has none, and #BKE_sculpt_update_object_for_edit
     * unconditionally dereferences it. Refreshed before #is_supported below so a member that IS in
     * sculpt mode but has not built its PBVH yet is not misreported as unsupported. */
    if (member->runtime->sculpt_session != nullptr) {
      BKE_sculpt_update_object_for_edit(ctx.depsgraph, member, false);
    }
    if (!is_supported(*member)) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Remove Sculpt Layer Group: skipping \"%s\" (sculpt layers are not "
                  "available for this object)",
                  member->id.name + 2);
      continue;
    }
    /* #node_find_by_sync_uid resolves the SAME folder wherever it sits in the member's own tree;
     * the member's own folders are unrelated. A zero #sync_uid (the active folder was never
     * fanned-out-created) makes every member miss here by design -- see the function's own doc
     * comment -- which degrades this operator to the single-object behavior it always had. */
    Mesh &member_mesh = mesh_of(*member);
    SculptLayerTreeNode *member_node = bke::sculpt_layers::node_find_by_sync_uid(
        member_mesh, group->base.sync_uid);
    if (!member_node) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Remove Sculpt Layer Group: skipping \"%s\" (no group synced to the active one)",
                  member->id.name + 2);
      continue;
    }
    /* Defensive: #sync_uid should only ever link a group to a group, but the tree is
     * user-editable, so the kind found is not assumed. */
    SculptLayerGroup *member_group = bke::sculpt_layers::node_as_group(member_node);
    if (!member_group) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Remove Sculpt Layer Group: skipping \"%s\" (the synced node is a layer, not a "
                  "group)",
                  member->id.name + 2);
      continue;
    }
    if (bke::object::pbvh_get(*member)->type() == bke::pbvh::Type::Grids) {
      /* Mirrors #op_context_get / #layer_add_exec: consume any pending base sculpt edits first,
       * while the live CCG still matches the stored layers, before the disband below invalidates
       * that. */
      flush_pending_multires_base(*member);
    }
    eligible.append(member);
    GroupRemoveFanoutMember fanout_member;
    fanout_member.object = member;
    fanout_member.group = member_group;
    fanout_members.append(fanout_member);
  }

  /* Read before the close below, which clears it. Zero when no session is open. */
  const SculptSession *ss = session_of(*ctx.object);
  const int session_uid = ss ? ss->layers.mask_edit.node_uid : 0;
  /* Disbanding destroys this folder, so an open session on it would be orphaned — see
   * #layer_remove_exec for what that state does to every subsequent brush. Closed before the mask
   * report just below as well as before the removal: #mask_edit_end compresses the session's weights
   * back onto the folder, so the report tests the mask the folder really ends up losing. */
  /* Before the close, which clears the session struct the parked tool idname lives on. */
  mask_edit_exit_ui(C, *ctx.object);
  mask_edit_end(*ctx.object);

  /* The folder's own weight mask attenuates its whole subtree through
   * #bke::sculpt_layers::chain_mask, and disbanding destroys it with the folder: the lifted-out
   * children go back to contributing in full. Nothing is corrupted — the surface is recomposed
   * below and the mask rides the undo payload — but the shape change would otherwise have no
   * explanation on screen. Reported before the removal, which frees the name. */
  if (group->base.mask != nullptr) {
    BKE_reportf(op->reports,
                RPT_INFO,
                "Removed the weight mask of group '%s'; its contents now contribute in full",
                group->base.name);
  }

  session_state_ensure(*ctx.object);
  undo::push_begin_multi_object(*CTX_data_scene(C), op, eligible.as_span());
  /* Active object: unchanged from the single-object path. */
  if (session_uid != 0) {
    undo::push_sculpt_layer_mask_session(*ctx.object, session_uid, false);
  }

  /* The lift-out is a real relink now, not a retag of a folder field: a folder owns its children,
   * and #bke::sculpt_layers::group_remove refuses one that still has any (it would leak the
   * subtree).
   *
   * The children land right after the folder itself, in order, so the disbanded contents take the
   * folder's own slot among its siblings rather than jumping to the end of the parent. The child
   * list is copied out first — the loop relinks the very nodes it walks. */
  Vector<SculptLayerTreeNode *> children;
  for (SculptLayerTreeNode &child : group->children) {
    children.append(&child);
  }
  /* Captured before anything moves, so the `from` half of each entry describes where the child
   * really sat. Reading `prev_from` mid-loop would see a list the earlier moves have already
   * unlinked from, so every child would record the head as its neighbour and undo would rebuild
   * the folder's contents reversed (see #SCULPT_OT_layer_move_to for the same two-pass shape). */
  Vector<undo::ReparentMove> moves;
  moves.reserve(children.size());
  for (const SculptLayerTreeNode *child : children) {
    undo::ReparentMove move;
    move.uid = child->uid;
    move.prev_from = node_prev_uid(*child);
    move.group_from = group_uid;
    move.group_to = parent_uid;
    moves.append(move);
  }

  SculptLayerTreeNode *cursor = &group->base;
  for (SculptLayerTreeNode *child : children) {
    bke::sculpt_layers::node_move_into(mesh, *child, parent, cursor);
    cursor = child;
  }
  for (const int64_t i : children.index_range()) {
    moves[i].prev_to = node_prev_uid(*children[i]);
  }
  undo::push_sculpt_layer_reparent(*ctx.object, std::move(moves));

  /* Captured after the lift-out, which leaves the folder's own slot untouched (the children
   * landed behind it), so the payload still records the sibling it really followed. The folder is
   * empty by now, which is what #group_remove requires and what the undo seam mirrors:
   * #restore_list re-inserts a folder payload *before* it replays the move batch, extracts it
   * *after*. */
  Vector<undo::SculptLayerUndoPayload> removed;
  removed.append(undo::sculpt_layer_payload_capture(
      mesh, group->base, undo::PayloadCapture::NodeRemoved));
  bke::sculpt_layers::group_remove(mesh, *group);
  undo::push_sculpt_layer_group_list_change(*ctx.object, std::move(removed), {});

  group_cascade_resync_with_undo(*ctx.object, mesh);
  commit_layers_change(*ctx.depsgraph, *ctx.object);

  /* Fan out: mirror the active object's own body exactly, scoped to each member's own folder --
   * resolved above, before the active object's own #group_remove freed its folder. Every value the
   * relink and the undo payloads depend on is re-read from the member: its folder's own children,
   * its folder's own parent (which need not be the counterpart of the active object's parent --
   * the trees are only linked through #sync_uid identity, not through shape) and both uids. */
  for (GroupRemoveFanoutMember &fanout_member : fanout_members) {
    Object &member = *fanout_member.object;
    Mesh &member_mesh = mesh_of(member);
    SculptLayerGroup &member_group = *fanout_member.group;
    SculptLayerGroup &member_parent = *member_group.base.parent;
    const int member_group_uid = member_group.base.uid;
    const int member_parent_uid = member_parent.base.uid;

    /* Read before the close below, which clears it -- this member's own session, not the active
     * object's. Both halves of the close fan out, as in #layer_remove_exec: #mask_edit_end because
     * the member's own folder is about to be destroyed under its own session, and
     * #mask_edit_exit_ui because it is a no-op for anything but the object whose session is
     * actually open (it returns immediately when the object has no session or none open, and only
     * then restores the parked tool). Calling it per member is therefore how the ONE object that
     * owns the open session gets its tool back, whichever object in the group that is. */
    const SculptSession *member_ss = session_of(member);
    const int member_session_uid = member_ss ? member_ss->layers.mask_edit.node_uid : 0;
    mask_edit_exit_ui(C, member);
    mask_edit_end(member);

    /* Per member and naming the member, for the reason the active object's own report gives: the
     * lifted-out children going back to contributing in full is a shape change on THAT object, and
     * an unattributed message would not say which one moved. Reported after the close (so it tests
     * the settled mask) and before the removal (which frees the name), as on the active object. */
    if (member_group.base.mask != nullptr) {
      BKE_reportf(op->reports,
                  RPT_INFO,
                  "Remove Sculpt Layer Group: removed the weight mask of group '%s' on "
                  "\"%s\"; its contents now contribute in full",
                  member_group.base.name,
                  member.id.name + 2);
    }

    session_state_ensure(member);
    if (member_session_uid != 0) {
      undo::push_sculpt_layer_mask_session(member, member_session_uid, false);
    }

    Vector<SculptLayerTreeNode *> member_children;
    for (SculptLayerTreeNode &child : member_group.children) {
      member_children.append(&child);
    }
    Vector<undo::ReparentMove> member_moves;
    member_moves.reserve(member_children.size());
    for (const SculptLayerTreeNode *child : member_children) {
      undo::ReparentMove move;
      move.uid = child->uid;
      move.prev_from = node_prev_uid(*child);
      move.group_from = member_group_uid;
      move.group_to = member_parent_uid;
      member_moves.append(move);
    }
    SculptLayerTreeNode *member_cursor = &member_group.base;
    for (SculptLayerTreeNode *child : member_children) {
      bke::sculpt_layers::node_move_into(member_mesh, *child, member_parent, member_cursor);
      member_cursor = child;
    }
    for (const int64_t i : member_children.index_range()) {
      member_moves[i].prev_to = node_prev_uid(*member_children[i]);
    }
    undo::push_sculpt_layer_reparent(member, std::move(member_moves));

    Vector<undo::SculptLayerUndoPayload> member_removed;
    member_removed.append(undo::sculpt_layer_payload_capture(
        member_mesh, member_group.base, undo::PayloadCapture::NodeRemoved));
    bke::sculpt_layers::group_remove(member_mesh, member_group);
    undo::push_sculpt_layer_group_list_change(member, std::move(member_removed), {});

    group_cascade_resync_with_undo(member, member_mesh);
    commit_layers_change(*ctx.depsgraph, member);
  }

  /* #commit_layers_change unconditionally recomposes the surface, on the active object and every
   * fanned-out member alike, so #UpdateType::Position is required the same way #layer_add_exec
   * requires it. */
  undo::finish_multi_object(C, eligible.as_span(), UpdateType::Position);
  layers_ui_notify(C, *ctx.object);
  return OPERATOR_FINISHED;
}

void SCULPT_OT_layer_group_remove(wmOperatorType *ot)
{
  ot->name = "Remove Sculpt Layer Group";
  ot->idname = "SCULPT_OT_layer_group_remove";
  ot->description =
      "Remove a sculpt layer group, moving its contents up to the containing group. The layers "
      "themselves are kept";
  ot->exec = layer_group_remove_exec;
  ot->poll = layers_poll;
  /* No #OPTYPE_UNDO: the operator pushes its own sculpt undo step; a global push would insert a
   * memfile step that does not compose with the stroke SCULPT steps (see #SCULPT_OT_layer_solo_base). */
  ot->flag = OPTYPE_REGISTER;
  RNA_def_int(ot->srna,
              "group_uid",
              0,
              0,
              INT_MAX,
              "Group ID",
              "Unique id of the sculpt layer group to remove",
              0,
              INT_MAX);
}

/* Lift every layer in \a subtree_layers out into \a parent, right after \a group's own slot, in the
 * given order, and record the reparent batch for undo. This empties every folder in the subtree,
 * which is what lets those folders be removed afterwards and — crucially — what keeps that removal
 * legal on redo: #restore_list replays this reparent batch (emptying the folders) *before* it
 * extracts them, so no folder is ever freed with a child still inside (see the seam note on
 * #push_sculpt_layer_group_list_change). A layer that the caller then removes rides this batch too so
 * that redo empties the folders the same way; on undo its move is skipped (the node is absent,
 * re-inserted from the removed list instead). Shared by the merge and delete folder operators. */
static void lift_subtree_layers_to_parent(Object &object,
                                          Mesh &mesh,
                                          SculptLayerGroup &group,
                                          SculptLayerGroup &parent,
                                          Span<SculptLayer *> subtree_layers)
{
  Vector<undo::ReparentMove> moves;
  moves.reserve(subtree_layers.size());
  SculptLayerTreeNode *cursor = &group.base;
  for (SculptLayer *layer : subtree_layers) {
    undo::ReparentMove move;
    move.uid = layer->base.uid;
    move.prev_from = node_prev_uid(layer->base);
    move.group_from = node_parent_uid(layer->base);
    move.group_to = parent.base.uid;
    bke::sculpt_layers::node_move_into(mesh, layer->base, parent, cursor);
    move.prev_to = node_prev_uid(layer->base);
    moves.append(move);
    cursor = &layer->base;
  }
  undo::push_sculpt_layer_reparent(object, std::move(moves));
}

/* Remove \a group and every folder nested in it, recording them for undo. Every folder must already
 * be empty — the caller lifts the layers out first (see #lift_subtree_layers_to_parent). Captured
 * top-down (the order #bke::sculpt_layers::groups yields) so undo re-inserts a parent before its
 * nested folder; removed deepest-first (the reverse) because a folder still owns its child folders
 * until those are gone, which #restore_list mirrors by extracting the batch in reverse on redo.
 * Shared by the merge and delete folder operators. */
static void remove_folder_subtree_with_undo(Object &object, Mesh &mesh, SculptLayerGroup &group)
{
  Vector<SculptLayerGroup *> folders;
  folders.append(&group);
  folders.extend(bke::sculpt_layers::groups(group));
  Vector<undo::SculptLayerUndoPayload> removed_groups;
  removed_groups.reserve(folders.size());
  for (SculptLayerGroup *folder : folders) {
    removed_groups.append(undo::sculpt_layer_payload_capture(
        mesh, folder->base, undo::PayloadCapture::NodeRemoved));
  }
  for (int64_t i = folders.size() - 1; i >= 0; i--) {
    bke::sculpt_layers::group_remove(mesh, *folders[i]);
  }
  undo::push_sculpt_layer_group_list_change(object, std::move(removed_groups), {});
}

/* One sync-group member's resolved target for a folder merge: the folder synced to the one the
 * user pressed, and every layer in THAT folder's own subtree, depth-first.
 *
 * Only the folder carries cross-object identity. The survivor is not resolved by #sync_uid at
 * all -- it is the first entry of #subtree_layers, the member's own depth-first first layer,
 * exactly the positional resolution #MergeDownFanoutMember uses for its `below`. There is nothing
 * to compare it against either, unlike #layer_merge_down_exec's synced-counterpart check: the
 * survivor is not one of two rows the user pointed at, it is whichever layer the dissolve happens
 * to keep, and every other layer in the subtree is folded into it rather than lost. So a member
 * whose subtree is ordered differently keeps a different layer -- the same "trees are
 * independently ordered outside of #sync_uid identity" rule the move operators state, and the
 * combined surface is preserved either way because the sum is over the whole subtree.
 *
 * Participation is therefore all-or-nothing, as in #layer_merge_down_exec and unlike
 * #MergeSelectedFanoutMember: the set is "every layer under this folder", not an externally
 * chosen selection that a member may hold only part of. A member whose synced folder is empty has
 * nothing to merge and is skipped whole, mirroring the active object's own early-out.
 *
 * #subtree_layers is an owning #Vector captured in the gather pass, not the cached #Span
 * #bke::sculpt_layers::layers hands out: the removals below invalidate that cache, and re-deriving
 * the list after any object's turn would walk a tree this operator has already freed nodes from.
 * Resolved once for the same reason #RemoveFanoutMember exists, and doubly so here -- this
 * operator frees every non-surviving layer AND the whole folder subtree per object. Safe because
 * #fanout_targets dedups by #Object::data, so no two members ever share a #Mesh and nothing
 * touches this member's tree until its own turn below. */
struct GroupMergeFanoutMember {
  Object *object;
  SculptLayerGroup *group;
  Vector<SculptLayer *> subtree_layers;
};

static wmOperatorStatus layer_group_merge_exec(bContext *C, wmOperator *op)
{
  OpContext ctx;
  if (!op_context_get(C, op, ctx)) {
    return OPERATOR_CANCELLED;
  }
  if (mask_edit_refuse_ccg_rebuild(op, *ctx.object)) {
    return OPERATOR_CANCELLED;
  }
  Mesh &mesh = *ctx.mesh;
  const int group_uid = RNA_int_get(op->ptr, "group_uid");
  SculptLayerGroup *group = group_row_lookup(mesh, group_uid);
  if (!group) {
    BKE_report(op->reports, RPT_ERROR, "No sculpt layer group with that id");
    return OPERATOR_CANCELLED;
  }
  /* Read before the removals below free the folder. */
  const std::string group_name = group->base.name;
  SculptLayerGroup &parent = *group->base.parent;

  /* Every layer in the folder's subtree, depth-first — an owning copy, because the removals below
   * invalidate the cached span. The first layer becomes the survivor the rest merge into, exactly as
   * the active layer is the survivor in #layer_merge_selected_exec. The first layer in depth-first
   * order can only be preceded by *folders* in its own container (any earlier layer would itself come
   * first), which is what lets its lift below record a `prev_from` that no #removed payload owns and
   * that #restore_list can always resolve. */
  const Vector<SculptLayer *> subtree_layers(bke::sculpt_layers::layers(*group));
  if (subtree_layers.is_empty()) {
    BKE_report(op->reports, RPT_ERROR, "No layers in group to merge");
    return OPERATOR_CANCELLED;
  }
  /* Weighting is by #effective, so a group-hidden layer (weight 0) would be silently zeroed rather
   * than preserved. Testing #SCULPT_LAYER_GROUP_HIDDEN on the layers covers the disabled-folder case
   * without a separate folder check: the cascade (#resync_group_state) has already stamped that bit
   * on every descendant layer of a disabled folder, this one or any above it. Refuse the whole
   * operation (same guard as #layer_merge_selected_exec) rather than merge into a zero layer. */
  for (const SculptLayer *layer : subtree_layers) {
    if (layer->base.flag & SCULPT_LAYER_GROUP_HIDDEN) {
      BKE_report(op->reports, RPT_ERROR, "Cannot merge sculpt layers inside a disabled group");
      return OPERATOR_CANCELLED;
    }
  }
  /* Settle an open session's weights back onto their node before any mask below is read, for the
   * reason #layer_bake_exec closes one: during a session #SculptLayerTreeNode::mask holds the
   * pre-session snapshot while the live weights sit in the standard mask storage, so folding the
   * node's mask would bake weights the user has since painted over. The grid domain never reaches
   * here — #mask_edit_refuse_ccg_rebuild above refused it. */
  mask_edit_end(*ctx.object);

  const MergeMaskPolicy policy = merge_mask_policy_get(op);
  /* Exclusive upper bound of the fold. Under KeepGroup the folder's own mask survives on the result
   * and must not also go into the data, so the fold stops *at* the folder; otherwise the folder
   * dissolves with the rest and is folded in. */
  const SculptLayerGroup *stop_above = (policy == MergeMaskPolicy::KeepGroup) ? group :
                                                                               group->base.parent;

  const MaskLayout layout = mask_layout_for_object(*ctx.object);
  /* Sized from the domain, never from the survivor's buffer: on grids that buffer is the maximum of
   * the participants' element counts, which is not what the masks are cut to. A mismatch would make
   * every mask read stale and be silently dropped. */
  const int64_t mask_elem_num = layout.totelem;

  /* Fails closed on a stale mask, exactly as #node_chain_carries_mask does today: a mask that does
   * not describe this domain is still the user's data, and folding "no factor" for it would drop it
   * without a word and move the surface. The subtree is non-empty, so a walk from any layer reaches
   * the folder itself and its mask is covered too. */
  for (const SculptLayer *layer : subtree_layers) {
    if (const SculptLayerTreeNode *stale = find_stale_mask_in_fold(
            layer->base, stop_above, mask_elem_num))
    {
      BKE_reportf(op->reports,
                  RPT_ERROR,
                  "The weight mask of '%s' does not match the mesh; fix or remove it before merging",
                  stale->name);
      return OPERATOR_CANCELLED;
    }
  }

  /* See #node_chain_carries_mask. Acute here: the folder being dissolved may itself carry the mask
   * that attenuated the whole subtree, and it does not survive the merge to carry it afterwards.
   * The folder's own node is tested explicitly as well, so an empty-of-masks subtree under a masked
   * folder is caught even though the layer walk below would already reach that folder as an
   * ancestor. */
  bool any_masked = node_chain_carries_mask(group->base);
  for (const SculptLayer *layer : subtree_layers) {
    any_masked |= node_chain_carries_mask(layer->base);
  }
  /* While REC is armed on a layer every composite ignores that layer's masks and its folders'
   * (#node_mask_for_composite), so the layer is *not* attenuated right now and folding its mask in
   * would move the surface. Refused for the reason #SCULPT_OT_layer_mask_apply refuses it: disarming
   * REC is one click, and a mask baked at a moment the user cannot see is not recoverable by eye. */
  if (any_masked) {
    for (const SculptLayer *layer : subtree_layers) {
      if (layer->base.flag & SCULPT_LAYER_REC_EXEMPT) {
        BKE_report(op->reports,
                   RPT_ERROR,
                   "Disarm REC before merging sculpt layers that carry a weight mask");
        return OPERATOR_CANCELLED;
      }
    }
  }

  SculptLayer *survivor = subtree_layers.first();

  Main *bmain = CTX_data_main(C);

  /* Gather-then-gate: see #layer_add_exec's own comment for why the fan-out set has to be built
   * before any undo bracket opens. The active object was already validated above and is included
   * unconditionally; every other sync-group member is brought up to date and gated exactly as
   * #op_context_get gates the active object, plus the find-shaped gate #layer_group_remove_exec
   * uses (the member must hold a folder synced to the active one), and then re-runs EVERY one of
   * this operator's own guards against ITS OWN subtree.
   *
   * Re-running them is the whole point, for the reasons spelled out in #layer_merge_down_exec's
   * matching comment: the guards ask about weight masks, folder visibility and the REC exemption,
   * all per-object state that #sync_uid identity says nothing about. Every per-object value the
   * fold consumes (#MaskLayout, the element count, `stop_above`, the masked-ness) is therefore
   * recomputed from the member itself, never reused. Only `policy` is shared -- it is a single
   * operator-wide RNA property, not per-object state.
   *
   * A member that fails a guard is skipped with a warning naming it and the guard, not a hard
   * cancel: one member's local problem must not abort the merge on all the others. The ACTIVE
   * object's own guards above still hard-cancel the whole operator, unchanged -- it is the row the
   * user pressed, so refusing it refuses the operation.
   *
   * The per-member #flush_pending_multires_base mirrors the one #op_context_get already ran for
   * the active object, and it really is needed despite the comment above about the grid domain
   * never reaching here: #mask_edit_refuse_ccg_rebuild refuses an open mask-edit SESSION on grids,
   * not the grid domain itself, so an ordinary multires object with no session open reaches this
   * merge (the `ctx.grids` branches below exist for exactly that). This merge rewrites layer data
   * and removes nodes from the layer set, so a pending base edit would be stranded if it were not
   * consumed while the live CCG still matches the stored layers. */
  Vector<Object *> eligible;
  eligible.append(ctx.object);
  Vector<GroupMergeFanoutMember> fanout_members;
  for (Object *member : fanout_targets(*bmain, *ctx.object)) {
    if (member == ctx.object) {
      continue;
    }
    /* Mirrors #layer_add_exec's gather loop: guarded on an existing session, since a member that
     * was never taken into sculpt mode has none, and #BKE_sculpt_update_object_for_edit
     * unconditionally dereferences it. Refreshed before #is_supported below so a member that IS in
     * sculpt mode but has not built its PBVH yet is not misreported as unsupported. */
    if (member->runtime->sculpt_session != nullptr) {
      BKE_sculpt_update_object_for_edit(ctx.depsgraph, member, false);
    }
    if (!is_supported(*member)) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Merge Sculpt Layer Group: skipping \"%s\" (sculpt layers are not available for "
                  "this object)",
                  member->id.name + 2);
      continue;
    }
    /* #node_find_by_sync_uid resolves the SAME folder wherever it sits in the member's own tree;
     * the member's own folders are unrelated. A zero #sync_uid (the active folder was never
     * fanned-out-created) makes every member miss here by design, degrading this operator to the
     * single-object behavior it always had. */
    Mesh &member_mesh = mesh_of(*member);
    SculptLayerTreeNode *member_node = bke::sculpt_layers::node_find_by_sync_uid(
        member_mesh, group->base.sync_uid);
    if (!member_node) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Merge Sculpt Layer Group: skipping \"%s\" (no group synced to the active one)",
                  member->id.name + 2);
      continue;
    }
    /* Defensive: #sync_uid should only ever link a group to a group, but the tree is
     * user-editable, so the kind found is not assumed. */
    SculptLayerGroup *member_group = bke::sculpt_layers::node_as_group(member_node);
    if (!member_group) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Merge Sculpt Layer Group: skipping \"%s\" (the synced node is a layer, not a "
                  "group)",
                  member->id.name + 2);
      continue;
    }
    /* Captured here, on this member's own tree, and carried forward -- see
     * #GroupMergeFanoutMember. No dedup pass is needed on it, unlike the #sync_uid-resolved lists
     * in #layer_remove_exec and #layer_merge_selected_exec: #bke::sculpt_layers::layers walks each
     * children list exactly once, so a node can appear at most once no matter what the file holds.
     * An empty subtree is this member's own version of the early-out at the top of this
     * function. */
    Vector<SculptLayer *> member_subtree_layers(bke::sculpt_layers::layers(*member_group));
    if (member_subtree_layers.is_empty()) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Merge Sculpt Layer Group: skipping \"%s\" (no layers in its synced group to "
                  "merge)",
                  member->id.name + 2);
      continue;
    }
    /* The active object's own disabled-folder guard, re-asked of this member's own subtree: folder
     * visibility is per-object state, so the active object's layers being visible says nothing
     * about this member's, and at a group-hidden layer's weight of 0 the fold silently zeroes the
     * subtree's content instead of preserving the combined surface. */
    bool member_group_hidden = false;
    for (const SculptLayer *layer : member_subtree_layers) {
      member_group_hidden |= (layer->base.flag & SCULPT_LAYER_GROUP_HIDDEN) != 0;
    }
    if (member_group_hidden) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Merge Sculpt Layer Group: skipping \"%s\" (its layers are inside a disabled "
                  "group)",
                  member->id.name + 2);
      continue;
    }
    if (bke::object::pbvh_get(*member)->type() == bke::pbvh::Type::Grids) {
      /* Placed after the cheap gates so a member that is skipped anyway is left alone. */
      flush_pending_multires_base(*member);
    }
    /* The active object's own pair of session guards, in the same order and for the same reasons.
     * #mask_edit_end has to run before every mask-reading guard below, exactly as it does above:
     * it settles an open session's weights back onto the node, and the guards must see the settled
     * mask rather than the pre-session snapshot #SculptLayerTreeNode::mask still holds. */
    if (mask_edit_refuse_ccg_rebuild(op, *member)) {
      continue;
    }
    mask_edit_end(*member);

    /* This member's own fold bound, derived from ITS folder -- the shared `policy` decides only
     * whether the folder is inside or outside the fold, exactly as on the active object. */
    const SculptLayerGroup *member_stop_above = (policy == MergeMaskPolicy::KeepGroup) ?
                                                    member_group :
                                                    member_group->base.parent;
    /* Asked of this member's own element count: a mask that describes the active object's mesh
     * says nothing about whether it describes this one. */
    const int64_t member_mask_elem_num = mask_layout_for_object(*member).totelem;
    const SculptLayerTreeNode *member_stale = nullptr;
    for (const SculptLayer *layer : member_subtree_layers) {
      if (const SculptLayerTreeNode *stale = find_stale_mask_in_fold(
              layer->base, member_stop_above, member_mask_elem_num))
      {
        member_stale = stale;
        break;
      }
    }
    if (member_stale) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Merge Sculpt Layer Group: skipping \"%s\" (the weight mask of '%s' does not "
                  "match its mesh)",
                  member->id.name + 2,
                  member_stale->name);
      continue;
    }
    /* See the active object's own guard above: an armed REC makes the composite ignore that
     * layer's masks, so folding them in would move this member's surface right now. The member's
     * folder is tested explicitly too, for the active object's reason -- the folder being
     * dissolved may itself carry the mask that attenuated the whole subtree. */
    bool member_any_masked = node_chain_carries_mask(member_group->base);
    bool member_rec_exempt = false;
    for (const SculptLayer *layer : member_subtree_layers) {
      member_any_masked |= node_chain_carries_mask(layer->base);
      member_rec_exempt |= (layer->base.flag & SCULPT_LAYER_REC_EXEMPT) != 0;
    }
    if (member_any_masked && member_rec_exempt) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Merge Sculpt Layer Group: skipping \"%s\" (disarm REC before merging layers "
                  "that carry a weight mask)",
                  member->id.name + 2);
      continue;
    }

    eligible.append(member);
    GroupMergeFanoutMember fanout_member;
    fanout_member.object = member;
    fanout_member.group = member_group;
    fanout_member.subtree_layers = std::move(member_subtree_layers);
    fanout_members.append(std::move(fanout_member));
  }

  session_state_ensure(*ctx.object);
  undo::push_begin_multi_object(*CTX_data_scene(C), op, eligible.as_span());
  /* Active object: unchanged from the single-object path. */

  /* Accumulate `data[i] * effective * folded mask` of every subtree layer into the survivor at
   * influence 1. The sum equals the folder's prior combined contribution, so the visible surface is
   * unchanged: every mask that stops acting once the folder dissolves is folded into the data, and
   * the ones above `stop_above` are left alone because they go on acting. Buffer sizing mirrors
   * #layer_merge_selected_exec: grid buffers all sit at the canonical (top) level. */
  int64_t n = ctx.grids ? survivor->totelem : element_count(*ctx.object);
  if (ctx.grids) {
    for (const SculptLayer *layer : subtree_layers) {
      n = std::max(n, layer->totelem);
    }
  }
  /* Grow the survivor's buffer to its final size *before* the undo snapshot below, so the snapshot
   * and the live buffer always match on restore. An initially-empty survivor would otherwise capture
   * `has_data = false`, the data swap on undo would be skipped, and undoing would leave the merged sum
   * in place while the sources are re-inserted — doubling their contribution. */
  MutableSpan<float3> survivor_data = bke::sculpt_layers::data_ensure(*survivor, n);

  /* Pre-merge snapshot of the survivor's data buffer *and* metadata (its name), captured before both
   * the accumulation and the rename below; undo restores both from this one payload. */
  undo::push_sculpt_layer_data(*ctx.object, *survivor);

  /* Built before the accumulation overwrites the survivor's buffer, since it reads the participants'
   * masks as they still stand. Left empty under the other two policies, which keep no mask. */
  const Array<float> combined = (policy == MergeMaskPolicy::Combine && any_masked) ?
                                    merge_combined_mask(
                                        subtree_layers, *survivor, stop_above, mask_elem_num) :
                                    Array<float>();

  Array<float> fold;
  const float survivor_eff = effective(*survivor);
  const bool survivor_masked = any_masked &&
                               gather_fold_mask(survivor->base, stop_above, mask_elem_num, fold);
  for (const int64_t i : survivor_data.index_range()) {
    survivor_data[i] *= survivor_eff * (survivor_masked && i < fold.size() ? fold[i] : 1.0f);
  }
  for (const SculptLayer *layer : subtree_layers) {
    if (layer == survivor) {
      continue;
    }
    if (ctx.grids && layer->data && survivor->level != layer->level) {
      /* Grid layers are kept at the canonical top level (#resample_grid_layers), so in practice
       * every layer here already agrees and this branch is defensive. It mutates #level after the
       * undo snapshot above, and neither #push_sculpt_layer_data nor the restore swaps #level, so a
       * level change would not be undone — acceptable precisely because the levels do not diverge.
       * Mirrors #layer_merge_selected_exec. */
      survivor->level = layer->level;
    }
    if (!layer->data) {
      continue;
    }
    const float layer_eff = effective(*layer);
    const bool layer_masked = any_masked &&
                              gather_fold_mask(layer->base, stop_above, mask_elem_num, fold);
    const Span<float3> layer_data(static_cast<const float3 *>(layer->data), layer->totelem);
    const int64_t count = std::min<int64_t>(survivor_data.size(), layer_data.size());
    for (int64_t i = 0; i < count; i++) {
      /* The bound test is not decorative: `fold` is cut to the domain (\a mask_elem_num) while
       * `survivor_data` can be longer on grids. */
      survivor_data[i] += layer_data[i] * layer_eff *
                          (layer_masked && i < fold.size() ? fold[i] : 1.0f);
    }
  }
  survivor->influence = 1.0f;
  survivor->base.flag |= SCULPT_LAYER_ENABLED;

  /* Runs while the folder is still alive: #remove_folder_subtree_with_undo below frees its mask, and
   * KeepGroup copies that mask onto the result. */
  merge_settle_mask(*ctx.object,
                    *survivor,
                    survivor_data,
                    policy,
                    combined,
                    layout.block_size,
                    group);
  /* Defensive: every folder whose chain changed shape here is itself dissolved a few lines below, so
   * nothing should be left holding a cached product. #chain_mask hands out a pointer into that cache
   * and the next rebuild frees what it replaces, which is why this is not left to inference. */
  bke::sculpt_layers::tag_chain_mask_dirty(*group);

  /* Capture the merged-away layers at their *original* positions, before the lift below moves them,
   * so undo re-inserts them where they really sat. Capturing transfers each buffer to the payload;
   * the merge already read it above, so this only hands over ownership. The survivor is excluded — it
   * stays as the merged result. */
  Vector<undo::SculptLayerUndoPayload> removed_layers;
  for (SculptLayer *layer : subtree_layers) {
    if (layer == survivor) {
      continue;
    }
    removed_layers.append(undo::sculpt_layer_payload_capture(
        mesh, layer->base, undo::PayloadCapture::NodeRemoved));
  }

  /* Lift every subtree layer (the survivor included) out into the folder's parent, right after the
   * folder itself, so the survivor ends up in the folder's own slot once the folder is gone. */
  lift_subtree_layers_to_parent(*ctx.object, mesh, *group, parent, subtree_layers);

  /* Remove the merged-away layers, now siblings of the survivor under the parent. */
  for (SculptLayer *layer : subtree_layers) {
    if (layer == survivor) {
      continue;
    }
    bke::sculpt_layers::remove(mesh, *layer);
  }
  undo::push_sculpt_layer_list_change(*ctx.object, std::move(removed_layers), {}, false);

  remove_folder_subtree_with_undo(*ctx.object, mesh, *group);

  /* Rename the survivor to the folder's name, made unique across the tree. No extra undo push: the
   * #push_sculpt_layer_data above already snapshotted the survivor's name. */
  STRNCPY_UTF8(survivor->base.name, group_name.c_str());
  bke::sculpt_layers::node_name_ensure_unique(survivor->base);

  bke::sculpt_layers::active_set(mesh, survivor);
  group_cascade_resync_with_undo(*ctx.object, mesh);
  commit_layers_change(*ctx.depsgraph, *ctx.object);

  /* Fan out: mirror the active object's own body exactly, scoped to each member's own folder and
   * its own subtree -- both resolved in the gather pass, before ANY removal ran anywhere, the
   * active object's own included. Every per-object quantity is recomputed from the member rather
   * than reused from `ctx`: a member's PBVH type, element count, mask layout, folder chain, folder
   * name, folder parent and mask presence are all independent of the active object's, and folding
   * the active object's numbers into a member's data would corrupt that member's mesh. The
   * member's parent in particular need not be the counterpart of the active object's parent -- the
   * trees are only linked through #sync_uid identity, not through shape (see
   * #layer_group_remove_exec).
   *
   * Recomputing here rather than caching it in #GroupMergeFanoutMember is safe for the same reason
   * the resolved pointers stay valid: #fanout_targets dedups by #Object::data, so nothing between
   * the gather pass and this member's turn has touched this member's mesh. */
  bool any_masked_anywhere = any_masked;
  for (const GroupMergeFanoutMember &fanout_member : fanout_members) {
    Object &member = *fanout_member.object;
    Mesh &member_mesh = mesh_of(member);
    SculptLayerGroup &member_group = *fanout_member.group;
    const Span<SculptLayer *> member_subtree_layers = fanout_member.subtree_layers;
    /* Eligibility, both session guards and every one of this operator's own guards already ran for
     * every entry of `fanout_members` in the gather pass above, before the undo bracket opened --
     * nothing here can fail or need skipping (mirrors #layer_add_exec's own second loop). */

    /* Read before the removals below free the folder -- THIS member's own folder name, which the
     * survivor is renamed to. Folder names are ordinary per-object metadata, not synced state, so
     * the counterpart folder may well be named differently and renaming to the active object's
     * name would silently rewrite it. */
    const std::string member_group_name = member_group.base.name;
    SculptLayerGroup &member_parent = *member_group.base.parent;
    /* The member's OWN depth-first first layer; see #GroupMergeFanoutMember on why this is
     * positional rather than resolved by #sync_uid. */
    SculptLayer &member_survivor = *member_subtree_layers.first();

    const bool member_grids = bke::object::pbvh_get(member)->type() == bke::pbvh::Type::Grids;
    const SculptLayerGroup *member_stop_above = (policy == MergeMaskPolicy::KeepGroup) ?
                                                    &member_group :
                                                    member_group.base.parent;
    const MaskLayout member_layout = mask_layout_for_object(member);
    const int64_t member_mask_elem_num = member_layout.totelem;
    bool member_any_masked = node_chain_carries_mask(member_group.base);
    for (const SculptLayer *layer : member_subtree_layers) {
      member_any_masked |= node_chain_carries_mask(layer->base);
    }
    any_masked_anywhere |= member_any_masked;

    /* Mirrors #session_state_ensure's call above for the active object: it has to run before this
     * member's own data changes -- #data_ensure below already counts as one -- or the mesh_base
     * captured here would fold an already-merged state into the base (see #commit_layers_change's
     * comment on capture ordering). */
    session_state_ensure(member);

    int64_t member_n = member_grids ? member_survivor.totelem : element_count(member);
    if (member_grids) {
      for (const SculptLayer *layer : member_subtree_layers) {
        member_n = std::max(member_n, layer->totelem);
      }
    }
    /* Grown before the snapshot below, for the active object's own reason: an initially-empty
     * survivor would otherwise capture `has_data = false` and undo would double the sources. */
    MutableSpan<float3> member_survivor_data = bke::sculpt_layers::data_ensure(member_survivor,
                                                                              member_n);
    undo::push_sculpt_layer_data(member, member_survivor);

    const Array<float> member_combined =
        (policy == MergeMaskPolicy::Combine && member_any_masked) ?
            merge_combined_mask(
                member_subtree_layers, member_survivor, member_stop_above, member_mask_elem_num) :
            Array<float>();

    Array<float> member_fold;
    const float member_survivor_eff = effective(member_survivor);
    const bool member_survivor_masked =
        member_any_masked && gather_fold_mask(member_survivor.base,
                                              member_stop_above,
                                              member_mask_elem_num,
                                              member_fold);
    for (const int64_t i : member_survivor_data.index_range()) {
      member_survivor_data[i] *= member_survivor_eff *
                                 (member_survivor_masked && i < member_fold.size() ?
                                      member_fold[i] :
                                      1.0f);
    }
    for (const SculptLayer *layer : member_subtree_layers) {
      if (layer == &member_survivor) {
        continue;
      }
      if (member_grids && layer->data && member_survivor.level != layer->level) {
        member_survivor.level = layer->level;
      }
      if (!layer->data) {
        continue;
      }
      const float layer_eff = effective(*layer);
      const bool layer_masked = member_any_masked && gather_fold_mask(layer->base,
                                                                     member_stop_above,
                                                                     member_mask_elem_num,
                                                                     member_fold);
      const Span<float3> layer_data(static_cast<const float3 *>(layer->data), layer->totelem);
      const int64_t count = std::min<int64_t>(member_survivor_data.size(), layer_data.size());
      for (int64_t i = 0; i < count; i++) {
        member_survivor_data[i] += layer_data[i] * layer_eff *
                                   (layer_masked && i < member_fold.size() ? member_fold[i] :
                                                                            1.0f);
      }
    }
    member_survivor.influence = 1.0f;
    member_survivor.base.flag |= SCULPT_LAYER_ENABLED;

    /* Runs while THIS member's folder is still alive, for the active object's reason: the removal
     * below frees its mask, and KeepGroup copies that mask onto the result. */
    merge_settle_mask(member,
                      member_survivor,
                      member_survivor_data,
                      policy,
                      member_combined,
                      member_layout.block_size,
                      &member_group);
    bke::sculpt_layers::tag_chain_mask_dirty(member_group);

    /* Captured at their original positions, before the lift moves them; see the active object's
     * own two passes above for why capture and removal cannot be one loop. */
    Vector<undo::SculptLayerUndoPayload> member_removed_layers;
    for (SculptLayer *layer : member_subtree_layers) {
      if (layer == &member_survivor) {
        continue;
      }
      member_removed_layers.append(undo::sculpt_layer_payload_capture(
          member_mesh, layer->base, undo::PayloadCapture::NodeRemoved));
    }

    lift_subtree_layers_to_parent(
        member, member_mesh, member_group, member_parent, member_subtree_layers);

    for (SculptLayer *layer : member_subtree_layers) {
      if (layer == &member_survivor) {
        continue;
      }
      bke::sculpt_layers::remove(member_mesh, *layer);
    }
    undo::push_sculpt_layer_list_change(member, std::move(member_removed_layers), {}, false);

    remove_folder_subtree_with_undo(member, member_mesh, member_group);

    /* No extra undo push, as on the active object: the #push_sculpt_layer_data above already
     * snapshotted this survivor's name. */
    STRNCPY_UTF8(member_survivor.base.name, member_group_name.c_str());
    bke::sculpt_layers::node_name_ensure_unique(member_survivor.base);

    /* No #active_set for a member, unlike the active object above: which layer is "active" is
     * local UI state and not synced data -- #layer_select_exec has no fan-out at all for that
     * reason. Nothing is left dangling either way: #bke::sculpt_layers::remove hands the active
     * marker to a neighbour when the removed node was the one carrying it.
     *
     * No explicit #rec_exemption_refresh either, on the member or on the active object above: the
     * removals here can strand an armed REC with no exempt layer, but #commit_layers_change below
     * already refreshes the exemption itself, on every path and before it recomposes. That is
     * what separates this operator from #layer_merge_down_exec and #layer_merge_selected_exec,
     * whose own member loops DO call it: those two provably move nothing and so never commit at
     * all, which leaves the refresh with no other caller. The same split decides the undo close
     * (see #finish_multi_object below). */

    group_cascade_resync_with_undo(member, member_mesh);
    commit_layers_change(*ctx.depsgraph, member);
  }

  /* #commit_layers_change unconditionally recomposes the surface, on the active object and every
   * fanned-out member alike, so #UpdateType::Position is required exactly as in
   * #layer_group_remove_exec -- and unlike #layer_merge_down_exec / #layer_merge_selected_exec,
   * whose do-paths provably move nothing and therefore close with #push_end_all_ex instead. */
  undo::finish_multi_object(C, eligible.as_span(), UpdateType::Position);
  /* `Σ(data · effective · folded mask)` at influence 1 equals the folder's prior combined
   * contribution, so the combined surface (and the live positions) are unchanged.
   *
   * One report for the whole operation, not one per object: the policy is a single choice made in
   * the operator's own properties. Raised by any participating object that carried a mask, so a
   * member whose masks were folded is still explained even when the active object had none. */
  if (any_masked_anywhere) {
    merge_report_mask_policy(op, policy);
  }
  for (Object *member : eligible) {
    layers_ui_notify(C, *member);
  }
  return OPERATOR_FINISHED;
}

void SCULPT_OT_layer_group_merge(wmOperatorType *ot)
{
  ot->name = "Merge Sculpt Layer Group";
  ot->idname = "SCULPT_OT_layer_group_merge";
  ot->description =
      "Merge every sculpt layer inside a folder into a single layer named after the folder, "
      "removing the folder and any nested folders. Weight masks are handled as the Masks option "
      "says, and the combined surface is unchanged";
  ot->exec = layer_group_merge_exec;
  ot->poll = layers_poll;
  merge_mask_policy_prop(ot);
  /* No #OPTYPE_UNDO: the operator pushes its own sculpt undo step; a global push would insert a
   * memfile step that does not compose with the stroke SCULPT steps (see #SCULPT_OT_layer_solo_base). */
  ot->flag = OPTYPE_REGISTER;
  RNA_def_int(ot->srna,
              "group_uid",
              0,
              0,
              INT_MAX,
              "Group ID",
              "Unique id of the sculpt layer group to merge",
              0,
              INT_MAX);
}

/* One fan-out target of #SCULPT_OT_layer_group_delete: the member, ITS counterpart folder and ITS
 * own subtree of layers, all resolved in the gather pass. Carrying the subtree rather than
 * re-deriving it in the fan-out loop is mandatory for the reason #GroupMergeFanoutMember carries
 * one: #bke::sculpt_layers::layers hands out a cached span that the removals below invalidate, and
 * the folder itself is freed by #remove_folder_subtree_with_undo, so nothing can be re-derived from
 * it afterwards. */
struct GroupDeleteFanoutMember {
  Object *object;
  SculptLayerGroup *group;
  Vector<SculptLayer *> subtree_layers;
};

static wmOperatorStatus layer_group_delete_exec(bContext *C, wmOperator *op)
{
  OpContext ctx;
  if (!op_context_get(C, op, ctx)) {
    return OPERATOR_CANCELLED;
  }
  Mesh &mesh = *ctx.mesh;
  const int group_uid = RNA_int_get(op->ptr, "group_uid");
  SculptLayerGroup *group = group_row_lookup(mesh, group_uid);
  if (!group) {
    BKE_report(op->reports, RPT_ERROR, "No sculpt layer group with that id");
    return OPERATOR_CANCELLED;
  }
  SculptLayerGroup &parent = *group->base.parent;

  /* Every layer in the folder's subtree, depth-first — an owning copy, because the removals below
   * invalidate the cached span. Unlike #SCULPT_OT_layer_group_remove (which disbands the folder and
   * keeps the layers), this deletes the whole subtree: every layer *and* every folder inside it. */
  const Vector<SculptLayer *> subtree_layers(bke::sculpt_layers::layers(*group));

  Main *bmain = CTX_data_main(C);

  /* Gather-then-gate: see #layer_add_exec's own comment for why the fan-out set has to be built
   * before any undo bracket opens. The active object was already validated by #op_context_get and is
   * included unconditionally; every other sync-group member is brought up to date and gated exactly
   * as #op_context_get gates the active object, plus the find-shaped gate #layer_group_remove_exec
   * uses -- the member must actually hold a folder synced to the active one.
   *
   * Beyond that this operator has no guards to re-ask of a member, and none are invented here. It
   * folds nothing and keeps nothing, so the mask, folder-visibility and REC-exemption guards
   * #layer_group_merge_exec re-runs per member have no counterpart: there is no arithmetic whose
   * result a per-object mask could corrupt. There is no non-empty-subtree gate either, on the
   * member or on the active object above -- deleting a folder that happens to hold no layers is an
   * ordinary request, and the loops below simply move and remove nothing before the folder itself
   * goes.
   *
   * Unlike #layer_add_exec / #layer_clear_exec this gather does NOT call
   * #mask_edit_refuse_ccg_rebuild on a member, matching this operator's own active-object path,
   * which does not call it either: a delete is one of the operators that *closes* an open mask-edit
   * session rather than refusing on its account (see #layer_add_exec's comment on that split), and
   * the fan-out loop below closes each member's own session unconditionally. */
  Vector<Object *> eligible;
  eligible.append(ctx.object);
  Vector<GroupDeleteFanoutMember> fanout_members;
  for (Object *member : fanout_targets(*bmain, *ctx.object)) {
    if (member == ctx.object) {
      continue;
    }
    /* Mirrors #layer_add_exec's gather loop: guarded on an existing session, since a member that
     * was never taken into sculpt mode has none, and #BKE_sculpt_update_object_for_edit
     * unconditionally dereferences it. Refreshed before #is_supported below so a member that IS in
     * sculpt mode but has not built its PBVH yet is not misreported as unsupported. */
    if (member->runtime->sculpt_session != nullptr) {
      BKE_sculpt_update_object_for_edit(ctx.depsgraph, member, false);
    }
    if (!is_supported(*member)) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Delete Sculpt Layer Group: skipping \"%s\" (sculpt layers are not available for "
                  "this object)",
                  member->id.name + 2);
      continue;
    }
    /* #node_find_by_sync_uid resolves the SAME folder wherever it sits in the member's own tree;
     * the member's own folders are unrelated. A zero #sync_uid (the active folder was never
     * fanned-out-created) makes every member miss here by design -- see the function's own doc
     * comment -- which degrades this operator to the single-object behavior it always had. */
    Mesh &member_mesh = mesh_of(*member);
    SculptLayerTreeNode *member_node = bke::sculpt_layers::node_find_by_sync_uid(
        member_mesh, group->base.sync_uid);
    if (!member_node) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Delete Sculpt Layer Group: skipping \"%s\" (no group synced to the active one)",
                  member->id.name + 2);
      continue;
    }
    /* Defensive: #sync_uid should only ever link a group to a group, but the tree is
     * user-editable, so the kind found is not assumed. */
    SculptLayerGroup *member_group = bke::sculpt_layers::node_as_group(member_node);
    if (!member_group) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Delete Sculpt Layer Group: skipping \"%s\" (the synced node is a layer, not a "
                  "group)",
                  member->id.name + 2);
      continue;
    }
    /* Captured here, on this member's own tree, and carried forward -- see
     * #GroupDeleteFanoutMember. No dedup pass is needed on it, unlike the #sync_uid-resolved lists
     * in #layer_remove_exec and #layer_merge_selected_exec: #bke::sculpt_layers::layers walks each
     * children list exactly once, so a node can appear at most once no matter what the file holds,
     * and a double #bke::sculpt_layers::remove of one node is therefore impossible. */
    Vector<SculptLayer *> member_subtree_layers(bke::sculpt_layers::layers(*member_group));
    if (bke::object::pbvh_get(*member)->type() == bke::pbvh::Type::Grids) {
      /* Mirrors #op_context_get / #layer_add_exec: consume any pending base sculpt edits first,
       * while the live CCG still matches the stored layers, before the removals below invalidate
       * that. Placed after the cheap gates so a member that is skipped anyway is left alone. */
      flush_pending_multires_base(*member);
    }
    eligible.append(member);
    GroupDeleteFanoutMember fanout_member;
    fanout_member.object = member;
    fanout_member.group = member_group;
    fanout_member.subtree_layers = std::move(member_subtree_layers);
    fanout_members.append(std::move(fanout_member));
  }

  /* Read before the close below, which clears it. Zero when no session is open. */
  const SculptSession *ss = session_of(*ctx.object);
  const int session_uid = ss ? ss->layers.mask_edit.node_uid : 0;
  /* This deletes the folder, every nested folder and every layer below it, so an open session on any
   * of them would be orphaned — see #layer_remove_exec. Closed before the payload captures below so
   * each captures the settled mask rather than the pre-session value. */
  /* Before the close, which clears the session struct the parked tool idname lives on. */
  mask_edit_exit_ui(C, *ctx.object);
  mask_edit_end(*ctx.object);

  /* Derive the mesh base from the still-consistent pre-change state: dropping the layers changes the
   * combined surface, and the commit below recomputes it from that base. */
  session_state_ensure(*ctx.object);
  undo::push_begin_multi_object(*CTX_data_scene(C), op, eligible.as_span());
  /* Active object: unchanged from the single-object path. */
  if (session_uid != 0) {
    undo::push_sculpt_layer_mask_session(*ctx.object, session_uid, false);
  }

  /* Capture every layer at its *original* position before the lift below moves it, so undo re-inserts
   * it where it really sat (with its data buffer). */
  Vector<undo::SculptLayerUndoPayload> removed_layers;
  removed_layers.reserve(subtree_layers.size());
  for (SculptLayer *layer : subtree_layers) {
    removed_layers.append(undo::sculpt_layer_payload_capture(
        mesh, layer->base, undo::PayloadCapture::NodeRemoved));
  }

  /* Lift every subtree layer out into the folder's parent before removing it. Same shape as
   * #layer_group_merge_exec, without a survivor — here nothing is kept. */
  lift_subtree_layers_to_parent(*ctx.object, mesh, *group, parent, subtree_layers);

  /* No explicit #active_set: #bke::sculpt_layers::remove hands the active marker to a neighbour, and
   * when that neighbour is itself removed it hands it on again, so the marker walks off the deleted
   * run on its own (mirrors #layer_remove_exec). */
  for (SculptLayer *layer : subtree_layers) {
    bke::sculpt_layers::remove(mesh, *layer);
  }
  undo::push_sculpt_layer_list_change(*ctx.object, std::move(removed_layers), {}, false);

  remove_folder_subtree_with_undo(*ctx.object, mesh, *group);

  group_cascade_resync_with_undo(*ctx.object, mesh);
  commit_layers_change(*ctx.depsgraph, *ctx.object);

  /* Fan out: mirror the active object's own body exactly, scoped to each member's own folder and its
   * own subtree -- both resolved in the gather pass, before ANY removal ran anywhere, the active
   * object's own included. Every per-object quantity is re-read from the member: its folder's own
   * parent need not be the counterpart of the active object's parent, since the trees are only
   * linked through #sync_uid identity and not through shape (see #layer_group_remove_exec).
   *
   * Re-reading the parent here rather than caching it in #GroupDeleteFanoutMember is safe for the
   * same reason the resolved pointers stay valid: #fanout_targets dedups by #Object::data, so
   * nothing between the gather pass and this member's turn has touched this member's mesh. */
  for (const GroupDeleteFanoutMember &fanout_member : fanout_members) {
    Object &member = *fanout_member.object;
    Mesh &member_mesh = mesh_of(member);
    SculptLayerGroup &member_group = *fanout_member.group;
    SculptLayerGroup &member_parent = *member_group.base.parent;
    const Span<SculptLayer *> member_subtree_layers = fanout_member.subtree_layers;
    /* Eligibility and the session guards already ran for every entry of `fanout_members` in the
     * gather pass above, before the undo bracket opened -- nothing here can fail or need skipping
     * (mirrors #layer_add_exec's own second loop). */

    /* Read before the close below, which clears it -- this member's own session, not the active
     * object's. Both halves of the close fan out, as in #layer_group_remove_exec: #mask_edit_end
     * because the member's own folder and layers are about to be destroyed under its own session,
     * and #mask_edit_exit_ui because it is a no-op for anything but the object whose session is
     * actually open (it returns immediately when the object has no session or none open, and only
     * then restores the parked tool). Calling it per member is therefore how the ONE object that
     * owns the open session gets its tool back, whichever object in the group that is. */
    const SculptSession *member_ss = session_of(member);
    const int member_session_uid = member_ss ? member_ss->layers.mask_edit.node_uid : 0;
    mask_edit_exit_ui(C, member);
    mask_edit_end(member);

    /* Mirrors #session_state_ensure's call above for the active object: it has to run before this
     * member's own data changes, or the mesh base captured here would fold an already-deleted state
     * into the base (see #commit_layers_change's comment on capture ordering). */
    session_state_ensure(member);
    if (member_session_uid != 0) {
      undo::push_sculpt_layer_mask_session(member, member_session_uid, false);
    }

    /* Captured at their original positions, before the lift moves them; see the active object's own
     * two passes above for why capture and removal cannot be one loop. */
    Vector<undo::SculptLayerUndoPayload> member_removed_layers;
    member_removed_layers.reserve(member_subtree_layers.size());
    for (SculptLayer *layer : member_subtree_layers) {
      member_removed_layers.append(undo::sculpt_layer_payload_capture(
          member_mesh, layer->base, undo::PayloadCapture::NodeRemoved));
    }

    lift_subtree_layers_to_parent(
        member, member_mesh, member_group, member_parent, member_subtree_layers);

    /* No #active_set for a member, exactly as on the active object above and for the same reason:
     * #bke::sculpt_layers::remove hands the active marker to a neighbour, and when that neighbour is
     * itself removed it hands it on again, so the marker walks off the deleted run on its own.
     *
     * No explicit #rec_exemption_refresh either, on the member or on the active object above: the
     * removals here can strand an armed REC with no exempt layer, but #commit_layers_change below
     * already refreshes the exemption itself, on every path and before it recomposes. That is what
     * separates this operator from #layer_merge_down_exec and #layer_merge_selected_exec, whose own
     * member loops DO call it: those two provably move nothing and so never commit at all, which
     * leaves the refresh with no other caller. The same split decides the undo close (see
     * #finish_multi_object below). */
    for (SculptLayer *layer : member_subtree_layers) {
      bke::sculpt_layers::remove(member_mesh, *layer);
    }
    undo::push_sculpt_layer_list_change(member, std::move(member_removed_layers), {}, false);

    remove_folder_subtree_with_undo(member, member_mesh, member_group);

    group_cascade_resync_with_undo(member, member_mesh);
    commit_layers_change(*ctx.depsgraph, member);
  }

  /* #commit_layers_change unconditionally recomposes the surface, on the active object and every
   * fanned-out member alike, so #UpdateType::Position is required the same way
   * #layer_group_remove_exec requires it -- and unlike #layer_merge_down_exec /
   * #layer_merge_selected_exec, whose do-paths provably move nothing and therefore close with
   * #push_end_all_ex instead. */
  undo::finish_multi_object(C, eligible.as_span(), UpdateType::Position);
  /* Active-object-only, matching #layer_group_remove_exec / #layer_add_exec: #finish_multi_object
   * already sent `NC_OBJECT | ND_DRAW` for every member above, so the only thing still missing is
   * the `NC_GEOM | ND_DATA` notifier the original always sent for the active object's own mesh, and
   * #layers_ui_notify sends exactly that pair. */
  layers_ui_notify(C, *ctx.object);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus layer_group_delete_invoke(bContext *C,
                                                  wmOperator *op,
                                                  const wmEvent * /*event*/)
{
  OpContext ctx;
  if (!op_context_get(C, op, ctx)) {
    return OPERATOR_CANCELLED;
  }
  /* Pre-check the exec guard here so the confirmation popup never appears just to be followed by an
   * error (confirm-then-fail), matching #layer_bake_to_shape_key_invoke. */
  const int group_uid = RNA_int_get(op->ptr, "group_uid");
  if (!group_row_lookup(*ctx.mesh, group_uid)) {
    BKE_report(op->reports, RPT_ERROR, "No sculpt layer group with that id");
    return OPERATOR_CANCELLED;
  }
  return WM_operator_confirm_ex(
      C,
      op,
      IFACE_("Delete Group and Layers"),
      IFACE_("The group and every sculpt layer inside it will be deleted."),
      IFACE_("Delete"),
      ui::AlertIcon::Warning,
      false);
}

void SCULPT_OT_layer_group_delete(wmOperatorType *ot)
{
  ot->name = "Delete Sculpt Layer Group and Layers";
  ot->idname = "SCULPT_OT_layer_group_delete";
  ot->description =
      "Delete a folder together with every sculpt layer and nested folder inside it";
  ot->exec = layer_group_delete_exec;
  ot->invoke = layer_group_delete_invoke;
  ot->poll = layers_poll;
  /* No #OPTYPE_UNDO: the operator pushes its own sculpt undo step; a global push would insert a
   * memfile step that does not compose with the stroke SCULPT steps (see #SCULPT_OT_layer_solo_base). */
  ot->flag = OPTYPE_REGISTER;
  RNA_def_int(ot->srna,
              "group_uid",
              0,
              0,
              INT_MAX,
              "Group ID",
              "Unique id of the sculpt layer group to delete",
              0,
              INT_MAX);
}

/* Carried from gather into the fan-out loop so each member's sync_uid-matched layer is resolved
 * once. Select never frees nodes, so re-resolving would also be safe; carrying matches the
 * tree-mutating ops' clarity without needing it for pointer validity. */
struct SelectFanoutMember {
  Object *object = nullptr;
  SculptLayer *layer = nullptr;
};

/* FIND-shaped fan-out: set each member's #sculpt_layers_active_uid to its sync_uid-matched layer's
 * own uid (never copy the active object's uid); one shared #ED_undo_push for the whole set. */
/* The object-mode half of #layer_select_exec, which shares none of its machinery.
 *
 * #Mesh::sculpt_layers_active_uid is plain persistent DNA, and outside sculpt mode nothing derives
 * from it: no composite consults it (#SCULPT_LAYER_REC_EXEMPT is cleared on the way out of the mode),
 * there is no session to keep in step and no surface to bring back in line. So this writes the field,
 * pushes a global undo step and stops — where the sculpt-mode path closes a mask session, settles the
 * runtime base, moves the REC exemption and recomposes on the result.
 *
 * Split rather than guarded inside the main path on purpose: #op_context_get dereferences the PBVH
 * (`bke::object::pbvh_get(object)->type()`), and the sculpt undo bracket around the selection has no
 * meaning without a sculpt undo stack to bracket. Neither exists here. */
static wmOperatorStatus layer_select_object_mode_exec(bContext *C, wmOperator *op, Object &object)
{
  Mesh &mesh = mesh_of(object);
  const int uid = RNA_int_get(op->ptr, "uid");
  SculptLayer *layer = bke::sculpt_layers::node_as_layer(
      bke::sculpt_layers::node_find_by_uid(mesh, uid));
  if (layer == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "No sculpt layer with that id");
    return OPERATOR_CANCELLED;
  }
  if (mesh.sculpt_layers_active_uid == uid) {
    return OPERATOR_FINISHED;
  }
  Main *bmain = CTX_data_main(C);

  /* Gather-then-gate, as every fan-out elsewhere in this file: the active object is included
   * unconditionally, and every other sync-group member is brought in only if it clears the
   * object-type and sync_uid-match gates. There is no #is_supported / session / PBVH gate here,
   * unlike the in-sculpt-mode variant below -- this whole code path has no #SculptSession to begin
   * with -- but #mesh_of is still an unguarded cast to `Mesh *`, and #sync_group_members applies no
   * object-type filter of its own, so a member of a different type has to be turned away before it
   * reaches #mesh_of. */
  Vector<Object *> eligible;
  eligible.append(&object);
  Vector<SelectFanoutMember> fanout_members;
  for (Object *member : sync_group_members(*bmain, object)) {
    if (member->type != OB_MESH) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Select Sculpt Layer: skipping \"%s\" (not a mesh object)",
                  member->id.name + 2);
      continue;
    }
    /* #node_find_by_sync_uid resolves the SAME layer wherever it sits in the member's own tree;
     * the member's own active layer is unrelated. A zero #sync_uid (the active layer was never
     * fanned-out-created) makes every member miss here by design, which degrades this operator to
     * the single-object behavior it always had. */
    Mesh &member_mesh = mesh_of(*member);
    SculptLayerTreeNode *member_node = bke::sculpt_layers::node_find_by_sync_uid(
        member_mesh, layer->base.sync_uid);
    if (!member_node) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Select Sculpt Layer: skipping \"%s\" (no layer synced to the active one)",
                  member->id.name + 2);
      continue;
    }
    /* Defensive: #sync_uid should only ever link a layer to a layer, but the tree is
     * user-editable, so the kind found is not assumed. */
    SculptLayer *member_layer = bke::sculpt_layers::node_as_layer(member_node);
    if (!member_layer) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Select Sculpt Layer: skipping \"%s\" (the synced node is a group, not a layer)",
                  member->id.name + 2);
      continue;
    }
    /* Already active on this member — nothing to do here, not a failure (mirrors the active
     * object's own early return above). */
    if (member_mesh.sculpt_layers_active_uid == member_layer->base.uid) {
      continue;
    }
    eligible.append(member);
    SelectFanoutMember fanout_member;
    fanout_member.object = member;
    fanout_member.layer = member_layer;
    fanout_members.append(fanout_member);
  }

  /* Active object first, then every eligible member: each gets its OWN layer's uid written into
   * #sculpt_layers_active_uid (uids are per-mesh; copying the active object's uid would name the
   * wrong layer — or none — on the member). */
  mesh.sculpt_layers_active_uid = uid;
  for (const SelectFanoutMember &fanout_member : fanout_members) {
    Mesh &member_mesh = mesh_of(*fanout_member.object);
    member_mesh.sculpt_layers_active_uid = fanout_member.layer->base.uid;
  }

  /* A single plain memfile step for the whole fan-out — the right kind here and the wrong kind in
   * sculpt mode (the reason #SCULPT_OT_layer_select carries no #OPTYPE_UNDO and pushes its own
   * sculpt step instead). Outside the mode there are no delta-based sculpt steps for a memfile
   * step to sit between, and unlike the mutation itself the push is not repeated per target. */
  ED_undo_push(C, "Select Sculpt Layer");
  for (Object *target : eligible) {
    layers_ui_notify(C, *target);
  }
  return OPERATOR_FINISHED;
}

/* FIND-shaped fan-out: activate each member's sync_uid-matched layer by that member's own uid;
 * mask-edit close/#push_sculpt_layer_mask_session stay ACTIVE-ONLY; close with
 * #push_end_all_ex(false, true)+#layers_ui_notify (commit is conditional on #rec_exemption_refresh). */
static wmOperatorStatus layer_select_exec(bContext *C, wmOperator *op)
{
  Object *object_poll = CTX_data_active_object(C);
  if (object_poll != nullptr && !(object_poll->mode & OB_MODE_SCULPT)) {
    return layer_select_object_mode_exec(C, op, *object_poll);
  }
  OpContext ctx;
  if (!op_context_get(C, op, ctx)) {
    return OPERATOR_CANCELLED;
  }
  const int uid = RNA_int_get(op->ptr, "uid");
  /* The kind-checked cast is what refuses a folder's uid — one counter spans both kinds now — and,
   * with it, uid 0: #node_find_by_uid resolves 0 to the root folder, which is not a layer. That is
   * a different convention from #Mesh::sculpt_layers_active_uid below, where 0 means "no active
   * layer"; the two must not be resolved through each other. */
  SculptLayer *layer = bke::sculpt_layers::node_as_layer(
      bke::sculpt_layers::node_find_by_uid(*ctx.mesh, uid));
  if (layer == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "No sculpt layer with that id");
    return OPERATOR_CANCELLED;
  }
  const int uid_from = ctx.mesh->sculpt_layers_active_uid;
  if (uid == uid_from) {
    /* Nothing changes, so an open session must survive: re-selecting the row whose mask is being
     * edited is exactly what a user does while working. */
    return OPERATOR_FINISHED;
  }
  Main *bmain = CTX_data_main(C);

  /* Gather-then-gate: see #layer_add_exec's own comment for why the fan-out set has to be built
   * before any undo bracket opens. The active object is included unconditionally; every other
   * sync-group member is brought up to date and gated exactly as #op_context_get gates the active
   * object, plus a find-shaped gate #layer_add_exec has no analog for: the member must actually
   * hold a layer synced to the one being selected. #mask_edit_refuse_ccg_rebuild is NOT applied
   * on members: the active path closes sessions instead of refusing, and that session close is
   * deliberately active-only (see the function's own decision comment). */
  Vector<Object *> eligible;
  eligible.append(ctx.object);
  Vector<SelectFanoutMember> fanout_members;
  for (Object *member : fanout_targets(*bmain, *ctx.object)) {
    if (member == ctx.object) {
      continue;
    }
    /* Mirrors #layer_add_exec's gather loop: guarded on an existing session, since a member that
     * was never taken into sculpt mode has none, and #BKE_sculpt_update_object_for_edit
     * unconditionally dereferences it. Refreshed before #is_supported below so a member that IS in
     * sculpt mode but has not built its PBVH yet is not misreported as unsupported. */
    if (member->runtime->sculpt_session != nullptr) {
      BKE_sculpt_update_object_for_edit(ctx.depsgraph, member, false);
    }
    if (!is_supported(*member)) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Select Sculpt Layer: skipping \"%s\" (sculpt layers are not available for this "
                  "object)",
                  member->id.name + 2);
      continue;
    }
    /* #node_find_by_sync_uid resolves the SAME layer wherever it sits in the member's own tree;
     * the member's own active layer is unrelated. A zero #sync_uid (the active layer was never
     * fanned-out-created) makes every member miss here by design -- see the function's own doc
     * comment -- which degrades this operator to the single-object behavior it always had. */
    Mesh &member_mesh = mesh_of(*member);
    SculptLayerTreeNode *member_node = bke::sculpt_layers::node_find_by_sync_uid(
        member_mesh, layer->base.sync_uid);
    if (!member_node) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Select Sculpt Layer: skipping \"%s\" (no layer synced to the active one)",
                  member->id.name + 2);
      continue;
    }
    /* Defensive: #sync_uid should only ever link a layer to a layer, but the tree is
     * user-editable, so the kind found is not assumed. */
    SculptLayer *member_layer = bke::sculpt_layers::node_as_layer(member_node);
    if (!member_layer) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Select Sculpt Layer: skipping \"%s\" (the synced node is a group, not a layer)",
                  member->id.name + 2);
      continue;
    }
    /* Already active on this member — nothing to do here, not a failure (mirrors the active
     * object's own early return above). */
    if (member_mesh.sculpt_layers_active_uid == member_layer->base.uid) {
      continue;
    }
    if (bke::object::pbvh_get(*member)->type() == bke::pbvh::Type::Grids) {
      /* Mirrors #op_context_get / #layer_add_exec: consume any pending base sculpt edits first,
       * while the live CCG still matches the stored layers -- load-bearing here because
       * #rec_exemption_refresh may still call #commit_layers_change below. */
      flush_pending_multires_base(*member);
    }
    eligible.append(member);
    SelectFanoutMember fanout_member;
    fanout_member.object = member;
    fanout_member.layer = member_layer;
    fanout_members.append(fanout_member);
  }

  /* Read before the close below, which clears it. Zero when no session is open, which is the
   * common case. ACTIVE OBJECT ONLY: a member's open mask-edit session is unrelated to which
   * layer the active object just selected, so #mask_edit_exit_ui / #mask_edit_end /
   * #push_sculpt_layer_mask_session must not fan out. */
  const SculptSession *ss = session_of(*ctx.object);
  const int session_uid = ss ? ss->layers.mask_edit.node_uid : 0;

  /* Any change of the active layer closes an open weight-mask editing session, whichever node it
   * was opened on. The session is presented as a mode of the row it belongs to, so leaving that row
   * with the mask tools still writing into the layer's mask — and the user's own mask still parked
   * — would be a state with no visible cause. Closed before the undo push below so the step
   * records the settled layer data rather than the borrowed mask storage. */
  /* Before the close, which clears the session struct the parked tool idname lives on. */
  mask_edit_exit_ui(C, *ctx.object);
  mask_edit_end(*ctx.object);
  /* Selection must ride on a sculpt undo step: without one, #OPTYPE_UNDO used to push a plain
   * memfile step, and undoing across it between two stroke SCULPT steps corrupted the delta-based
   * sculpt undo state. */
  undo::push_begin_multi_object(*CTX_data_scene(C), op, eligible.as_span());
  /* The close above is part of what this step did, so undoing it must reopen the session and
   * redoing it must close one again. No mask has to be captured alongside: #mask_edit_end compresses
   * the session's dense weights onto #SculptLayerTreeNode::mask, so the node already carries exactly
   * the mask the session was authoring, and the reopen on undo expands that same mask back into the
   * standard storage. Recording a mask here would be redundant at best — and actively wrong at
   * worst, since #undo::push_sculpt_layer_mask refuses a node with an open session and the pre-close
   * value of that field is stale by construction.
   *
   * Pushed after #push_begin because there is no step to record into before it, which is why the uid
   * is read out above rather than here. ACTIVE-ONLY (see above). */
  if (session_uid != 0) {
    undo::push_sculpt_layer_mask_session(*ctx.object, session_uid, false);
  }
  undo::push_sculpt_layer_active(*ctx.object, uid_from, uid);
  ctx.mesh->sculpt_layers_active_uid = uid;
  /* Written straight to the field rather than through #active_set, so the tag the other selection
   * paths get from their #active_set neighbors has to be placed by hand here. */
  tag_layer_overlays_dirty(*ctx.object);
  /* REC stays armed across a change of active layer, so the exemption has to follow the new active
   * layer rather than stay on the old one.
   *
   * Moving it off a masked layer puts that layer's weight map back into the composite, and onto
   * another masked layer takes that one's out, so the composed surface moves and has to be brought
   * back in line. The runtime base is settled first, against the pre-move exemption, for the reason
   * spelled out on #bke::sculpt_layers::rec_exempt_set: derived afterwards it would absorb the
   * difference permanently. Both halves are conditional, so the common case (REC disarmed, or
   * neither layer masked) costs one guard and one flag test per layer. Explicit
   * #rec_exemption_refresh is load-bearing here — do not rely on an unconditional commit. */
  const SculptSession *ss_select = session_of(*ctx.object);
  if (ss_select && !ss_select->layers.state_valid) {
    session_state_ensure(*ctx.object);
  }
  if (rec_exemption_refresh(*ctx.object)) {
    commit_layers_change(*ctx.depsgraph, *ctx.object);
  }

  for (const SelectFanoutMember &fanout_member : fanout_members) {
    Object &member = *fanout_member.object;
    Mesh &member_mesh = mesh_of(member);
    /* Eligibility (#is_supported, sync_uid resolution, already-active skip) and the multires
     * pending-base flush already ran for every member of `fanout_members` in the gather pass
     * above -- nothing here can fail or need skipping (mirrors #layer_add_exec's own second
     * loop). Write this member's OWN layer uid, never the active object's. */
    const int member_uid_from = member_mesh.sculpt_layers_active_uid;
    const int member_uid_to = fanout_member.layer->base.uid;
    undo::push_sculpt_layer_active(member, member_uid_from, member_uid_to);
    member_mesh.sculpt_layers_active_uid = member_uid_to;
    tag_layer_overlays_dirty(member);
    /* Mirror the active object's exact conditional pattern above, per member. */
    const SculptSession *member_ss = session_of(member);
    if (member_ss && !member_ss->layers.state_valid) {
      session_state_ensure(member);
    }
    if (rec_exemption_refresh(member)) {
      commit_layers_change(*ctx.depsgraph, member);
    }
  }

  /* #commit_layers_change is conditional on #rec_exemption_refresh and commonly skipped, so
   * #finish_multi_object is not used: it would call #flush_update_done per object, inventing work
   * (PBVH ensure, viewport/geometry tags) the single-object do-path never performed when refresh
   * returned false. #push_end_all_ex(false, true) is the same close #finish_multi_object uses
   * internally, minus that flush -- the documented multi-object idiom for never-/conditionally-
   * commit metadata paths (#layer_group_color_tag_exec, #layer_move_exec). #layers_ui_notify is
   * looped over every eligible member to still redraw each one. */
  undo::push_end_all_ex(false, true);
  for (Object *member : eligible) {
    layers_ui_notify(C, *member);
  }
  return OPERATOR_FINISHED;
}

void SCULPT_OT_layer_select(wmOperatorType *ot)
{
  ot->name = "Select Sculpt Layer";
  ot->idname = "SCULPT_OT_layer_select";
  ot->description = "Set the active sculpt layer";
  ot->exec = layer_select_exec;
  ot->poll = layers_object_mode_poll;
  /* No #OPTYPE_UNDO: the operator pushes its own sculpt undo step; a global push would insert a
   * memfile step that does not compose with the stroke SCULPT steps (see #SCULPT_OT_layer_solo_base). */
  ot->flag = OPTYPE_REGISTER;
  /* Identified by uid rather than by position: an operator's arguments outlive the call (they are
   * kept for redo), by which point a position may name a different layer. */
  RNA_def_int(
      ot->srna, "uid", 0, 0, INT_MAX, "Layer ID", "Unique id of the layer to make active", 0, INT_MAX);
}

/* Mints one shared #Object::sculpt_layer_sync_group id for every selected mesh object, linking
 * their sculpt-layer operations and REC recording together (see #sync_group_members /
 * #fanout_targets). Writes a plain DNA field on #Object and needs neither a session nor a PBVH,
 * so -- like #layer_select_object_mode_exec above -- it works from object mode as well as sculpt
 * mode; #ED_operator_view3d_active would wrongly refuse it from the Properties editor, which is
 * this operator's real UI home. */
static wmOperatorStatus layer_sync_group_create_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  Vector<Object *> selected;
  {
    Vector<PointerRNA> selected_ptrs;
    CTX_data_selected_objects(C, &selected_ptrs);
    for (const PointerRNA &ptr : selected_ptrs) {
      Object *ob = reinterpret_cast<Object *>(ptr.owner_id);
      if (!bke::sculpt_layers::sync_group_is_valid_mesh_member(*ob)) {
        continue;
      }
      selected.append(ob);
    }
  }
  if (selected.size() < 2) {
    BKE_report(op->reports, RPT_ERROR, "Select at least 2 mesh objects to sync");
    return OPERATOR_CANCELLED;
  }
  for (Object *ob : selected) {
    if (!bke::sculpt_layers::sync_group_object_membership_editable(*bmain, *ob)) {
      BKE_reportf(op->reports,
                  RPT_ERROR,
                  "Object '%s' is linked, non-editable, or not a valid mesh object",
                  ob->id.name + 2);
      return OPERATOR_CANCELLED;
    }
    if (ob->sculpt_layer_sync_group != 0) {
      BKE_reportf(op->reports,
                  RPT_ERROR,
                  "Object '%s' is already in a sync group; unlink it first",
                  ob->id.name + 2);
      return OPERATOR_CANCELLED;
    }
  }
  /* Two selected objects sharing one #Mesh (linked duplicates / Alt+D) would each still get their
   * own #SculptLayerTreeNode from a later fan-out #SCULPT_OT_layer_add, both stamped with the SAME
   * sync_uid onto the one tree they actually share -- #node_find_by_sync_uid (which returns the
   * first match) could then no longer tell the two apart. Refused here, at group-creation time,
   * rather than left for a fan-out operator to discover later. Same dedup idiom as
   * #gather_supported_objects in `node_group_operator.cc`: #Set::add returns false when the key was
   * already present. */
  Set<const ID *> object_data;
  for (Object *ob : selected) {
    if (!object_data.add(static_cast<const ID *>(ob->data))) {
      BKE_reportf(op->reports,
                  RPT_ERROR,
                  "Object '%s' shares its mesh data with another selected object; sync groups "
                  "require distinct mesh data per object",
                  ob->id.name + 2);
      return OPERATOR_CANCELLED;
    }
  }
  const int new_group = bke::sculpt_layers::sync_group_unique_id(*bmain);
  const uint64_t new_key = bke::sculpt_layers::sync_group_new_key(*bmain);
  char new_name[64];
  bke::sculpt_layers::sync_group_unique_name(*bmain, new_name);
  for (Object *ob : selected) {
    ob->sculpt_layer_sync_group = new_group;
    ob->sculpt_layer_sync_group_key = new_key;
    BLI_strncpy(
        ob->sculpt_layer_sync_group_name, new_name, sizeof(ob->sculpt_layer_sync_group_name));
  }
  for (Object *ob : selected) {
    WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, ob);
  }
  return OPERATOR_FINISHED;
}

void SCULPT_OT_layer_sync_group_create(wmOperatorType *ot)
{
  ot->name = "Sync Sculpt Layers";
  ot->idname = "SCULPT_OT_layer_sync_group_create";
  ot->description =
      "Link every selected mesh object into one sync group so sculpt-layer tree operations and "
      "layer-recording strokes apply across the whole group until an object is unlinked";
  ot->exec = layer_sync_group_create_exec;
  /* Matches #SCULPT_OT_layer_select and #SCULPT_OT_layer_group_toggle_visibility, the two other
   * operators the Properties editor's object-mode-or-sculpt-mode panel relies on -- see the
   * exec's comment above for why. */
  ot->poll = layers_object_mode_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/* Clears this object's own #Object::sculpt_layer_sync_group (and name) and, if that leaves exactly
 * one other raw member, dissolves that now-partnerless group of one too (see
 * #SCULPT_OT_layer_sync_group_create for why a group of one is meaningless). */
static wmOperatorStatus layer_sync_group_unlink_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  Object *ob = CTX_data_active_object(C);
  if (!ob || ob->sculpt_layer_sync_group == 0) {
    return OPERATOR_CANCELLED;
  }
  if (!bke::sculpt_layers::sync_group_object_membership_editable(*bmain, *ob)) {
    BKE_report(op->reports,
               RPT_ERROR,
               "Cannot unlink a linked, non-editable, or invalid mesh object from a sync group");
    return OPERATOR_CANCELLED;
  }

  sync_group_unlink_object(*bmain, *ob);
  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, ob);
  return OPERATOR_FINISHED;
}

void SCULPT_OT_layer_sync_group_unlink(wmOperatorType *ot)
{
  ot->name = "Unlink Sculpt Layer Sync";
  ot->idname = "SCULPT_OT_layer_sync_group_unlink";
  ot->description = "Remove this object from its sculpt-layer sync group";
  ot->exec = layer_sync_group_unlink_exec;
  /* Matches #SCULPT_OT_layer_sync_group_create: this operator's UI home is the Properties editor
   * in either object or sculpt mode, not sculpt mode only (#layers_poll). */
  ot->poll = layers_object_mode_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static wmOperatorStatus layer_sync_group_repair_names_exec(bContext *C, wmOperator * /*op*/)
{
  Main *bmain = CTX_data_main(C);
  bke::sculpt_layers::sync_group_repair_empty_names(*bmain);
  bke::sculpt_layers::sync_group_mint_keys_for_legacy_groups(*bmain);
  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, nullptr);
  return OPERATOR_FINISHED;
}

void SCULPT_OT_layer_sync_group_repair_names(wmOperatorType *ot)
{
  ot->name = "Repair Sculpt Layer Sync Group Names";
  ot->idname = "SCULPT_OT_layer_sync_group_repair_names";
  ot->description =
      "Fill empty sculpt-layer sync group names in DNA and mint persistent group keys for legacy "
      "groups";
  ot->exec = layer_sync_group_repair_names_exec;
  ot->poll = layers_object_mode_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static wmOperatorStatus layer_sync_group_select_members_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  Object *active = CTX_data_active_object(C);
  if (!active || active->sculpt_layer_sync_group == 0) {
    return OPERATOR_CANCELLED;
  }

  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  View3D *v3d = CTX_wm_view3d(C);
  BKE_view_layer_synced_ensure(*bmain, scene, view_layer);

  const bool enter_sculpt = (active->mode & OB_MODE_SCULPT) != 0;

  Vector<Object *> candidates;
  candidates.append(active);
  candidates.extend(sync_group_raw_members(*bmain, *active));

  /* From sculpt mode, replace the current selection with sync-group members so multi-object sculpt
   * targets exactly this group. In object mode, keep the additive selection behavior. */
  if (enter_sculpt) {
    ed::object::base_deselect_all(*bmain, scene, view_layer, v3d, SEL_DESELECT);
  }

  int skipped = 0;
  int selected = 0;
  Vector<Object *> selected_objects;
  for (Object *ob : candidates) {
    Base *base = BKE_view_layer_base_find(view_layer, ob);
    if (base == nullptr || !BASE_SELECTABLE(v3d, base)) {
      skipped++;
      continue;
    }
    ed::object::base_select(base, ed::object::BA_SELECT);
    selected_objects.append(ob);
    selected++;
  }

  if (selected == 0) {
    BKE_report(op->reports, RPT_WARNING, "No sync-group members are selectable in this view layer");
    return OPERATOR_CANCELLED;
  }
  if (skipped > 0) {
    BKE_reportf(op->reports,
                RPT_WARNING,
                "Selected %d sync-group member(s); skipped %d (hidden, unselectable, or not in "
                "this view layer)",
                selected,
                skipped);
  }

  Base *active_base = BKE_view_layer_base_find(view_layer, active);
  if (active_base) {
    ed::object::base_activate(C, active_base);
  }

  if (enter_sculpt) {
    Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
    if (!depsgraph) {
      return OPERATOR_CANCELLED;
    }

    Vector<Object *> newly_entered;
    for (Object *ob : selected_objects) {
      if (ob->type != OB_MESH) {
        continue;
      }
      if (ob->mode != OB_MODE_OBJECT) {
        continue;
      }
      ed::sculpt_paint::object_sculpt_mode_enter(
          *bmain, *depsgraph, *scene, *ob, false, op->reports);
      newly_entered.append(ob);
    }

    if (!newly_entered.is_empty()) {
      wmWindowManager *wm = CTX_wm_manager(C);
      if (wm->op_undo_depth <= 1) {
        bool sculpt_undo_started = false;
        for (Object *ob : newly_entered) {
          /* Dyntopo adds its own undo step; ask the live session, not #ME_SCULPT_DYNAMIC_TOPOLOGY
           * (see #sculpt_selection_enter_sculpt_mode_exec). */
          const SculptSession *ss = ob->runtime->sculpt_session;
          if (ss != nullptr && ss->bm != nullptr) {
            continue;
          }
          if (!sculpt_undo_started) {
            undo::push_enter_sculpt_mode(*scene, *ob, op);
            sculpt_undo_started = true;
          }
          else {
            undo::push_enter_sculpt_mode_add_object(*ob);
          }
        }
        if (sculpt_undo_started) {
          undo::push_end_all_ex(false, true);
        }
      }

      for (Object *ob : newly_entered) {
        ed::object::object_overlay_mode_transfer_animation_start(C, ob);
      }
    }

    WM_event_add_notifier(C, NC_SCENE | ND_MODE, scene);
    WM_toolsystem_update_from_context_view3d(C);
  }

  DEG_id_tag_update(&scene->id, ID_RECALC_SELECT);
  WM_event_add_notifier(C, NC_SCENE | ND_OB_SELECT, scene);
  return OPERATOR_FINISHED;
}

void SCULPT_OT_layer_sync_group_select_members(wmOperatorType *ot)
{
  ot->name = "Select Sync Group Members";
  ot->idname = "SCULPT_OT_layer_sync_group_select_members";
  ot->description =
      "Select every object in this object's sculpt-layer sync group; from Sculpt Mode, also enter "
      "every selected member into the current multi-object sculpt session";
  ot->exec = layer_sync_group_select_members_exec;
  ot->poll = layers_object_mode_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/* One sync-group member's REC-toggle state, resolved once in the gather pass below and carried
 * into the fan-out loop that actually flips it. Unlike #RemoveFanoutMember and every other
 * fan-out target in this file, there is no tree-node lookup here: the toggle sets the SAME
 * per-object session flag on every member (#rec_active_set, which takes an #Object&), not a
 * sync_uid-matched node. #layer and #create_layer are still resolved per member, up front, because
 * -- exactly like the active object -- arming on a member with no active layer must create one for
 * it, and whether to push a metadata undo step or a list-change undo step depends on which case
 * that member is in. */
struct RecFanoutMember {
  Object *object;
  /* This member's own active layer at gather time, or null if it currently has none. Mirrors the
   * active object's own #layer. */
  SculptLayer *layer;
  /* Whether arming REC (see #new_active below) finds this member with no active layer, and so must
   * create one for it. Mirrors the active object's own #create_layer, but judged against the
   * group's uniform target state rather than this member's own (possibly already-diverged) current
   * #rec_active, so that "armed implies an active layer" ends up holding for every member the
   * operator leaves armed -- not only the ones that already happened to agree with the active
   * object going in. */
  bool create_layer;
};

static wmOperatorStatus layer_toggle_rec_exec(bContext *C, wmOperator *op)
{
  OpContext ctx;
  if (!op_context_get(C, op, ctx)) {
    return OPERATOR_CANCELLED;
  }
  if (mask_edit_refuse_ccg_rebuild(op, *ctx.object)) {
    return OPERATOR_CANCELLED;
  }
  SculptSession *ss = session_of(*ctx.object);
  if (!ss) {
    return OPERATOR_CANCELLED;
  }
  SculptLayer *layer = bke::sculpt_layers::active_get(*ctx.mesh);
  /* Only the arming half is refused; disarming must always be possible. #effective stays 0 for a
   * folder-hidden layer whatever REC pins, so dabs would write both `layer->data` and the mesh
   * positions while the layer contributes nothing: that breaks
   * `positions == base + sum(data * effective)`, the stroke gets absorbed into the base, and
   * re-enabling the folder would apply it a second time. */
  if (!ss->layers.rec_active && layer && (layer->base.flag & SCULPT_LAYER_GROUP_HIDDEN)) {
    BKE_report(op->reports,
               RPT_ERROR,
               "The active sculpt layer is inside a disabled group; enable the group to record "
               "into it");
    return OPERATOR_CANCELLED;
  }
  /* Also only the arming half, and the mirror image of the refusal #mask_edit_begin already makes
   * against an armed REC. The two states are exclusive in both directions: while a session is open
   * the node's weights occupy the standard mask storage, and arming REC would exempt from the
   * composite a mask the user is looking at and painting into. */
  if (!ss->layers.rec_active && ss->layers.mask_edit.node_uid != 0) {
    BKE_report(op->reports,
               RPT_ERROR,
               "A sculpt layer weight mask is being edited; finish the mask edit before recording");
    return OPERATOR_CANCELLED;
  }

  /* The whole sync group arms or disarms together: computed once, from the active object's state
   * before anything below changes it, and then applied uniformly to every fanned-out member --
   * not each member flipping its own independently-current #rec_active. This is the "fan out means
   * setting the SAME session flag on every member" shape this operator's plan brief calls for,
   * unlike every other conversion in this file, which instead resolves a sync_uid-matched tree node
   * per member. */
  const bool new_active = !ss->layers.rec_active;

  Main *bmain = CTX_data_main(C);

  /* Gather-then-gate: see #layer_add_exec's own comment for why the fan-out set has to be built
   * before any undo bracket opens. The active object is included unconditionally, already
   * validated above; every other sync-group member is brought up to date and gated exactly as the
   * active object is gated above -- #mask_edit_refuse_ccg_rebuild unconditionally, the group-hidden
   * and open-mask-edit refusals only when arming (#new_active). A member that fails one is skipped
   * with a warning rather than aborting the whole batch. */
  Vector<Object *> eligible;
  eligible.append(ctx.object);
  Vector<RecFanoutMember> fanout_members;
  for (Object *member : fanout_targets(*bmain, *ctx.object)) {
    if (member == ctx.object) {
      continue;
    }
    /* Mirrors #layer_add_exec's gather loop: guarded on an existing session, since a member that
     * was never taken into sculpt mode has none, and #BKE_sculpt_update_object_for_edit
     * unconditionally dereferences it. Refreshed before #is_supported below so a member that IS in
     * sculpt mode but has not built its PBVH yet is not misreported as unsupported. */
    if (member->runtime->sculpt_session != nullptr) {
      BKE_sculpt_update_object_for_edit(ctx.depsgraph, member, false);
    }
    if (!is_supported(*member)) {
      BKE_reportf(op->reports,
                  RPT_WARNING,
                  "Toggle Sculpt Layer REC: skipping \"%s\" (sculpt layers are not available for "
                  "this object)",
                  member->id.name + 2);
      continue;
    }
    /* #is_supported guarantees a live session here (a non-null PBVH cannot exist without one) --
     * the same chain #layer_add_exec's own gather loop and every other fan-out in this file rely
     * on. */
    SculptSession *member_ss = session_of(*member);
    BLI_assert(member_ss != nullptr);
    if (bke::object::pbvh_get(*member)->type() == bke::pbvh::Type::Grids) {
      /* Mirrors #op_context_get / #layer_add_exec: consume any pending base sculpt edits first,
       * while the live CCG still matches the stored layers. */
      flush_pending_multires_base(*member);
    }
    if (mask_edit_refuse_ccg_rebuild(op, *member)) {
      continue;
    }
    Mesh &member_mesh = mesh_of(*member);
    SculptLayer *member_layer = bke::sculpt_layers::active_get(member_mesh);
    if (new_active) {
      /* Mirror of the active object's own two arming refusals above, judged against this
       * member's own layer and session state. */
      if (member_layer && (member_layer->base.flag & SCULPT_LAYER_GROUP_HIDDEN)) {
        BKE_reportf(op->reports,
                    RPT_WARNING,
                    "Toggle Sculpt Layer REC: skipping \"%s\" (its active layer is inside a "
                    "disabled group)",
                    member->id.name + 2);
        continue;
      }
      if (member_ss->layers.mask_edit.node_uid != 0) {
        BKE_reportf(op->reports,
                    RPT_WARNING,
                    "Toggle Sculpt Layer REC: skipping \"%s\" (a sculpt layer weight mask is "
                    "being edited on it)",
                    member->id.name + 2);
        continue;
      }
    }
    eligible.append(member);
    RecFanoutMember fanout_member;
    fanout_member.object = member;
    fanout_member.layer = member_layer;
    fanout_member.create_layer = new_active && member_layer == nullptr;
    fanout_members.append(fanout_member);
  }

  /* Arming REC on a mesh with no layers creates one, rather than leaving the button on with nothing
   * behind it — the state where every stroke edits the base while the UI says it is recording. Done
   * here rather than at stroke start because this is where the user expresses the intent, so the
   * layer appears in the tree the moment they press the button. #stroke_record_begin carries the same
   * creation as a fallback, for the ways this state can be reached without passing through here (the
   * last layer removed while REC stays armed).
   *
   * Before the undo bracket opens, mirroring #layer_add_exec: #session_state_ensure inverts the
   * composite to recover the runtime base and must measure the pre-change state. */
  const bool create_layer = new_active && layer == nullptr;
  if (create_layer) {
    session_state_ensure(*ctx.object);
  }

  /* Unconditional, unlike the original single-object bracket, which opened #undo::push_begin only
   * `if (layer || create_layer)` -- skipping it entirely when disarming with nothing active. A
   * multi-object span cannot compose that fine-grained a per-object conditional, and an eligible
   * member that ends up with nothing pushed into the step is harmless: #layer_add_exec's own
   * gather-then-gate comment already relies on exactly that degenerate case (a member surviving
   * eligibility but contributing no undo data) being fine. */
  undo::push_begin_multi_object(*CTX_data_scene(C), op, eligible.as_span());

  /* Active object: unchanged from the single-object path. */
  if (create_layer) {
    layer = auto_layer_create(*ctx.object);
    /* Recorded as a list change, exactly as #layer_add_exec does, so undoing the REC toggle also
     * removes the layer it brought into being. #bke::sculpt_layers::add already made it active. */
    undo::push_sculpt_layer_list_change(
        *ctx.object, {}, Vector<int>({layer->base.uid}), false);
  }
  else if (layer) {
    undo::push_sculpt_layer_metadata(*ctx.object, layer->base);
  }
  /* The whole state change lives there, shared with the mask-editing entry so that both go through
   * one ordering of the multires flush, the exemption refresh and the recompose. Only the undo
   * bracket, the refusals above and the notifier below are this operator's own. */
  rec_active_set(*ctx.object, new_active);

  for (const RecFanoutMember &member : fanout_members) {
    /* Mirrors the active object's own body immediately above, scoped to this member. Eligibility
     * (#is_supported, #mask_edit_refuse_ccg_rebuild, the arming refusals) already ran for every
     * member of `fanout_members` in the gather pass above -- nothing here can fail or need
     * skipping (mirrors #layer_add_exec's own second loop). */
    if (member.create_layer) {
      session_state_ensure(*member.object);
      SculptLayer *member_layer = auto_layer_create(*member.object);
      undo::push_sculpt_layer_list_change(
          *member.object, {}, Vector<int>({member_layer->base.uid}), false);
    }
    else if (member.layer) {
      undo::push_sculpt_layer_metadata(*member.object, member.layer->base);
    }
    /* Already a safe no-op if this member turns out to already be at #new_active, or lost its
     * session between the gather pass and here -- #rec_active_set checks both itself. */
    rec_active_set(*member.object, new_active);
    /* #rec_active_set short-circuits when this member was already at #new_active, taking its own
     * #rec_exemption_refresh with it -- so a layer created for an already-armed member would keep
     * neither #SCULPT_LAYER_REC_ARMED nor the exemption. Mirrors #layer_add_exec /
     * #stroke_ensure_rec_layer. A no-op when #rec_active_set already refreshed. */
    if (member.create_layer && rec_exemption_refresh(*member.object)) {
      commit_layers_change(*ctx.depsgraph, *member.object);
    }
  }

  /* #rec_active_set recomposes internally (via #commit_layers_change) whenever the toggle actually
   * moves the surface -- a masked layer's exemption flipping, or the enabled/influence
   * normalization on arming -- for the active object and every fanned-out member alike, so
   * #UpdateType::Position is required the same way #layer_add_exec requires it. */
  undo::finish_multi_object(C, eligible.as_span(), UpdateType::Position);
  /* Active-object-only, matching #layer_add_exec / #layer_clear_exec and every other
   * #finish_multi_object-based fan-out in this file: #finish_multi_object already sent
   * `NC_OBJECT | ND_DRAW` for every member above, so the only thing still missing is the
   * `NC_GEOM | ND_DATA` notifier the original always sent for the active object's own mesh, and
   * #layers_ui_notify sends exactly that pair. */
  layers_ui_notify(C, *ctx.object);
  return OPERATOR_FINISHED;
}

void SCULPT_OT_layer_toggle_rec(wmOperatorType *ot)
{
  ot->name = "Toggle Sculpt Layer REC";
  ot->idname = "SCULPT_OT_layer_toggle_rec";
  ot->description = "Toggle recording mode: sculpt strokes will be captured into the active layer";
  ot->exec = layer_toggle_rec_exec;
  ot->poll = layers_poll;
  ot->flag = OPTYPE_REGISTER;
}

}  // namespace blender::ed::sculpt_paint::layers
