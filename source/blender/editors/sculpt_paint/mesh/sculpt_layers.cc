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
#include "BLI_span.hh"
#include "BLI_string_utf8.h"
#include "BLI_task.hh"
#include "BLI_vector.hh"

#include "BLT_translation.hh"

#include "BKE_attribute.hh"
#include "BKE_context.hh"
#include "BKE_key.hh"
#include "BKE_main.hh"
#include "BKE_mesh.hh"
#include "BKE_multires.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_report.hh"
#include "BKE_multires_grid_resample.hh"
#include "BKE_sculpt_layers.hh"
#include "BKE_subdiv_ccg.hh"

#include "DEG_depsgraph.hh"

#include "ED_screen.hh"
#include "ED_sculpt.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "UI_interface_icons.hh"

#include "WM_api.hh"
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
#else
template<typename... Args> inline void slp_perf_discard(const Args &.../*args*/) {}
#  define SLP_PERF(...) slp_perf_discard(__VA_ARGS__)
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
  if (bke::sculpt_layers::layers(mesh).is_empty()) {
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

int element_count(const Object &object)
{
  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(object);
  if (pbvh && pbvh->type() == bke::pbvh::Type::Grids) {
    const SculptSession *ss = object.runtime->sculpt_session;
    return (ss && ss->subdiv_ccg) ? int(ss->subdiv_ccg->positions.size()) : 0;
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
  const int expected = bke::grid_totelem(mesh.corners_num, totlvl);
  bool warned = false;
  for (SculptLayer *layer : bke::sculpt_layers::layers(mesh)) {
    if (layer->domain != SCULPT_LAYER_DOMAIN_GRID || layer->data == nullptr) {
      continue;
    }
    if (!bke::sculpt_layers::is_stale(*layer, expected) && layer->level == totlvl) {
      continue;
    }
    MEM_delete_void(layer->data);
    layer->data = nullptr;
    layer->totelem = 0;
    layer->level = short(totlvl);
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

#if SCULPT_LAYERS_DEBUG_PERF
/* Debug validator for the mesh-domain canonical invariant `positions == mesh_base + sum(enabled
 * vert layers * influence)`. Prints a report when the live positions diverge, tagged with the
 * calling site, so a corruption can be attributed to the operation that introduced it. Only
 * meaningful for the common mesh path (no deform modifiers / shape keys). */
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
  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(object);
  if (pbvh && pbvh->type() == bke::pbvh::Type::Mesh) {
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

bool flush_interactive_update(Main &bmain, Mesh &mesh)
{
  const auto t_start = std::chrono::high_resolution_clock::now();

  /* Sculpt mode owns a single active object, so the first object using this mesh in sculpt mode
   * with a live session is the relevant one. */
  const auto t_search0 = std::chrono::high_resolution_clock::now();
  Object *object = nullptr;
  for (Object &ob : bmain.objects) {
    if (ob.data != &mesh.id || !(ob.mode & OB_MODE_SCULPT) || !ob.runtime->sculpt_session) {
      continue;
    }
    object = &ob;
    break;
  }
  const auto t_search1 = std::chrono::high_resolution_clock::now();
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
  const auto t_leaf0 = std::chrono::high_resolution_clock::now();
  const IndexMask node_mask = bke::pbvh::all_leaf_nodes(*pbvh, memory);
  const auto t_leaf1 = std::chrono::high_resolution_clock::now();
  pbvh->tag_positions_changed(node_mask);
  const auto t_tag1 = std::chrono::high_resolution_clock::now();
  pbvh->update_bounds_mesh(mesh.vert_positions());
  /* Keep the "original" bounds in step with the moved positions; see #recompute_mesh_canonical for
   * why a stale copy tears the next stroke along node borders. Cheap next to the bounds update
   * itself: one copy per node, not per vertex. */
  bke::pbvh::store_bounds_orig(*pbvh);
  const auto t_bounds_update1 = std::chrono::high_resolution_clock::now();
  mesh.bounds_set_eager(bke::pbvh::bounds_get(*pbvh));
  if (object->runtime->bounds_eval) {
    object->runtime->bounds_eval = mesh.bounds_min_max();
  }
  const auto t_bounds_block1 = std::chrono::high_resolution_clock::now();

  DEG_id_tag_update(&object->id, ID_RECALC_SHADING);
  WM_main_add_notifier(NC_OBJECT | ND_DRAW, &object->id);

  const auto t_end = std::chrono::high_resolution_clock::now();
  const auto us = [](const auto a, const auto b) {
    return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count();
  };
  SLP_PERF("[DEBUG-perf] flush_interactive_update: search=%lldus all_leaf_nodes=%lldus "
           "tag_positions_changed=%lldus update_bounds_mesh=%lldus bounds_block=%lldus "
           "total=%lldus leaf_nodes=%lld verts=%d\n",
           static_cast<long long>(us(t_search0, t_search1)),
           static_cast<long long>(us(t_leaf0, t_leaf1)),
           static_cast<long long>(us(t_leaf1, t_tag1)),
           static_cast<long long>(us(t_tag1, t_bounds_update1)),
           static_cast<long long>(us(t_bounds_update1, t_bounds_block1)),
           static_cast<long long>(us(t_start, t_end)),
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

  /* Honest re-evaluation: the depsgraph rebuilds the CCG from `MDisps + sum(enabled layers)`,
   * which already reflects the just-changed influence/visibility. MDisps are not touched (the
   * base never contains layer contributions). Undo is handled by the layer operators — an RNA
   * update callback runs after the value changed, so it cannot capture a pre-change snapshot. */
  DEG_id_tag_update(&mesh.id, ID_RECALC_GEOMETRY);

  WM_main_add_notifier(NC_OBJECT | ND_DRAW, &object->id);
  WM_main_add_notifier(NC_GEOM | ND_DATA, &mesh.id);
  return true;
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

  const auto t0 = std::chrono::high_resolution_clock::now();
  if (pbvh->type() == bke::pbvh::Type::Mesh && ss->shapekey_active) {
    ss->layers.base_view = base_view_from_layer_data(mesh, mesh.verts_num);
  }
  else if (pbvh->type() == bke::pbvh::Type::Mesh) {
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
    }
    ss->layers.base_view = std::move(total);
  }
  base_view_node_offset_bounds_ensure(depsgraph, object);
  const auto t1 = std::chrono::high_resolution_clock::now();
  SLP_PERF("[DEBUG-perf] base_view_ensure: %zu elems in %lld us\n",
           size_t(ss->layers.base_view.size()),
           static_cast<long long>(
               std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()));
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
    return;
  }

  if (!layer || layer->domain != domain_for(object) || (layer->base.flag & SCULPT_LAYER_LOCKED)) {
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
  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(object);
  Mesh &mesh = mesh_of(object);

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
      undo::foreach_recorded_position_mesh([&](const Span<int> verts, const Span<float3> orig) {
        for (const int64_t j : verts.index_range()) {
          const int v = verts[j];
          if (v >= 0 && v < int(base.size()) && v < int(positions.size())) {
            base[v] += positions[v] - orig[j];
          }
        }
      });
      debug_validate_mesh_invariant(object, "stroke_end_base");
    }
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
      undo::foreach_recorded_grids([&](const Span<int> grids) {
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

      if (!touched.is_empty()) {
        Vector<float3> undo_delta;
        if (multiresModifier_reshapeFromCCG_into_sculpt_layer(
                totlvl, &mesh, &subdiv_ccg, touched, *layer, undo_delta))
        {
          undo::store_active_sculpt_layer_grids(object, std::move(touched), std::move(undo_delta));
        }
      }
      /* The stroke is now fully captured in the layer and the base MDisps are unchanged. Clear
       * the dirty flag so a later flush does not bake the composed surface into the base, and
       * skip any depsgraph tag: the live CCG already equals base + layers, so a re-evaluation
       * would only rebuild the PBVH (the per-stroke freeze this system explicitly avoids). */
      subdiv_ccg.dirty.coords = false;
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
        undo::foreach_recorded_eval_position_mesh(collect);
      }
      else {
        undo::foreach_recorded_position_mesh(collect);
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
    undo::foreach_recorded_eval_position_mesh(revert);
  }
  else {
    undo::foreach_recorded_position_mesh(revert);
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
  session_state_ensure(*ctx.object);
  undo::push_begin(*CTX_data_scene(C), *ctx.object, op);
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
  undo::push_end(*ctx.object);
  /* A new layer starts with zero displacement, so the combined surface is unchanged. */
  /* Auto-enable REC so sculpting immediately records into the freshly added layer; the layer
   * itself already starts enabled at influence 1.0 (see #bke::sculpt_layers::add). */
  if (SculptSession *ss = session_of(*ctx.object)) {
    ss->layers.rec_active = true;
  }
  layers_ui_notify(C, *ctx.object);
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
  /* Derive the mesh base from the still-consistent pre-change state. */
  session_state_ensure(*ctx.object);
  undo::push_begin(*CTX_data_scene(C), *ctx.object, op);
  /* Capture every payload while the list is still intact so each records the neighbour it really
   * followed; undo re-inserts them in capture order and rebuilds the original stack. Capturing and
   * removing one at a time would make each layer record a stale neighbour instead (see
   * #SCULPT_OT_layer_merge_selected for the same trap). Capturing does not unlink, so removal is a
   * second pass. */
  Vector<undo::SculptLayerUndoPayload> removed;
  for (SculptLayer *layer : targets) {
    removed.append(undo::sculpt_layer_payload_capture(*ctx.mesh, layer->base));
  }
  /* No explicit #active_set afterwards: #bke::sculpt_layers::remove hands the active marker to a
   * neighbour, and when that neighbour is itself a target its own removal hands it on again, so
   * the marker walks off the removed run on its own. */
  for (SculptLayer *layer : targets) {
    bke::sculpt_layers::remove(*ctx.mesh, *layer);
  }
  undo::push_sculpt_layer_list_change(*ctx.object, std::move(removed), {}, false);
  commit_layers_change(*ctx.depsgraph, *ctx.object);
  undo::push_end(*ctx.object);
  layers_ui_notify(C, *ctx.object);
  return OPERATOR_FINISHED;
}

void SCULPT_OT_layer_remove(wmOperatorType *ot)
{
  ot->name = "Remove Sculpt Layer";
  ot->idname = "SCULPT_OT_layer_remove";
  ot->description = "Remove the selected sculpt layers and their contribution";
  ot->exec = layer_remove_exec;
  ot->poll = layers_poll;
  /* No #OPTYPE_UNDO: the operator pushes its own sculpt undo step; a global push would insert a
   * memfile step that does not compose with the stroke SCULPT steps (see #SCULPT_OT_layer_solo_base). */
  ot->flag = OPTYPE_REGISTER;
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
     * which is a reparent this operator does not do. */
    return OPERATOR_FINISHED;
  }
  const int prev_uid_from = node_prev_uid(node);
  /* Up: land where the neighbour sat, i.e. after whatever the neighbour followed (null = the head
   * of the folder). Down: land directly after the neighbour. */
  SculptLayerTreeNode *after = (dir == 0) ? neighbour->prev : neighbour;
  bke::sculpt_layers::node_move_into(*ctx.mesh, node, parent, after);

  bke::sculpt_layers::active_set(*ctx.mesh, layer);
  undo::push_begin(*CTX_data_scene(C), *ctx.object, op);
  /* Move Up/Down never leaves the layer's folder, so both group fields carry the same uid. */
  const int parent_uid = parent.base.uid;
  Vector<undo::ReparentMove> moves = {
      {node.uid, prev_uid_from, node_prev_uid(node), parent_uid, parent_uid}};
  undo::push_sculpt_layer_reparent(*ctx.object, std::move(moves));
  undo::push_end(*ctx.object);
  /* Layer order does not affect the additive combination; UI refresh only. */
  layers_ui_notify(C, *ctx.object);
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

static wmOperatorStatus layer_move_to_exec(bContext *C, wmOperator *op)
{
  OpContext ctx;
  if (!op_context_get(C, op, ctx)) {
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

  session_state_ensure(*ctx.object);
  undo::push_begin(*CTX_data_scene(C), *ctx.object, op);
  undo::push_sculpt_layer_reparent(*ctx.object, std::move(moves));
  /* A move across a folder boundary changes what is visible (the destination folder may be
   * disabled), so unlike the Этап 1 reorder this is not a UI-only refresh. */
  group_cascade_resync_with_undo(*ctx.object, mesh);
  commit_layers_change(*ctx.depsgraph, *ctx.object);
  undo::push_end(*ctx.object);
  layers_ui_notify(C, *ctx.object);
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

static wmOperatorStatus layer_duplicate_exec(bContext *C, wmOperator *op)
{
  OpContext ctx;
  if (!op_context_get(C, op, ctx)) {
    return OPERATOR_CANCELLED;
  }
  SculptLayer *src = bke::sculpt_layers::active_get(*ctx.mesh);
  if (!src) {
    return OPERATOR_CANCELLED;
  }
  session_state_ensure(*ctx.object);
  undo::push_begin(*CTX_data_scene(C), *ctx.object, op);
  SculptLayer *copy = bke::sculpt_layers::duplicate(*ctx.mesh, *src);
  undo::push_sculpt_layer_list_change(*ctx.object, {}, Vector<int>({copy->base.uid}), false);
  /* The duplicated contribution doubles up in the combined result. */
  commit_layers_change(*ctx.depsgraph, *ctx.object);
  undo::push_end(*ctx.object);
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
  session_state_ensure(*ctx.object);
  undo::push_begin(*CTX_data_scene(C), *ctx.object, op);
  /* Snapshot the surviving layer's pre-merge metadata and data for undo. */
  undo::push_sculpt_layer_data(*ctx.object, *below);

  /* Merging with the grid domain requires both buffers at the canonical size (the layers are
   * validated to the top level, so a missing buffer just means zero displacement). */
  const int n = ctx.grids ? std::max(active->totelem, below->totelem) : element_count(*ctx.object);
  MutableSpan<float3> below_data = bke::sculpt_layers::data_ensure(*below, n);
  if (ctx.grids && active->data && below->level != active->level) {
    below->level = active->level;
  }
  const float active_eff = effective(*active);
  const float below_eff = effective(*below);
  if (active->data) {
    const Span<float3> active_data(static_cast<const float3 *>(active->data), active->totelem);
    const int64_t count = std::min<int64_t>(below_data.size(), active_data.size());
    for (int64_t i = 0; i < count; i++) {
      below_data[i] = below_data[i] * below_eff + active_data[i] * active_eff;
    }
  }
  else {
    for (float3 &v : below_data) {
      v *= below_eff;
    }
  }
  below->influence = 1.0f;
  below->base.flag |= SCULPT_LAYER_ENABLED;

  Vector<undo::SculptLayerUndoPayload> removed;
  removed.append(undo::sculpt_layer_payload_capture(*ctx.mesh, active->base));
  bke::sculpt_layers::remove(*ctx.mesh, *active);
  bke::sculpt_layers::active_set(*ctx.mesh, below);
  undo::push_sculpt_layer_list_change(*ctx.object, std::move(removed), {}, false);
  undo::push_end(*ctx.object);

  /* `below * below_eff + active * active_eff` at influence 1 equals the previous combination
   * exactly, so the combined surface (and the live positions) are unchanged. */
  layers_ui_notify(C, *ctx.object);
  return OPERATOR_FINISHED;
}

void SCULPT_OT_layer_merge_down(wmOperatorType *ot)
{
  ot->name = "Merge Sculpt Layer Down";
  ot->idname = "SCULPT_OT_layer_merge_down";
  ot->description = "Merge the active sculpt layer into the one below it";
  ot->exec = layer_merge_down_exec;
  ot->poll = layers_poll;
  /* No #OPTYPE_UNDO: the operator pushes its own sculpt undo step; a global push would insert a
   * memfile step that does not compose with the stroke SCULPT steps (see #SCULPT_OT_layer_solo_base). */
  ot->flag = OPTYPE_REGISTER;
}

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

  session_state_ensure(*ctx.object);
  undo::push_begin(*CTX_data_scene(C), *ctx.object, op);
  /* Snapshot the surviving layer's pre-merge metadata and data for undo. */
  undo::push_sculpt_layer_data(*ctx.object, *active);

  /* Merging with the grid domain requires every buffer at the canonical size (the layers are
   * validated to the top level, so a missing buffer just means zero displacement). */
  int n = ctx.grids ? active->totelem : element_count(*ctx.object);
  if (ctx.grids) {
    for (const SculptLayer *source : sources) {
      n = std::max(n, source->totelem);
    }
  }
  MutableSpan<float3> active_data = bke::sculpt_layers::data_ensure(*active, n);
  const float active_eff = effective(*active);
  for (float3 &v : active_data) {
    v *= active_eff;
  }
  for (const SculptLayer *source : sources) {
    if (ctx.grids && source->data && active->level != source->level) {
      active->level = source->level;
    }
    if (!source->data) {
      continue;
    }
    const float source_eff = effective(*source);
    const Span<float3> source_data(static_cast<const float3 *>(source->data), source->totelem);
    const int64_t count = std::min<int64_t>(active_data.size(), source_data.size());
    for (int64_t i = 0; i < count; i++) {
      active_data[i] += source_data[i] * source_eff;
    }
  }
  active->influence = 1.0f;
  active->base.flag |= SCULPT_LAYER_ENABLED;

  /* Every payload is captured while the list is still intact, so each records the neighbour it
   * really followed; undo then re-inserts them in capture order and rebuilds the original stack.
   * Capturing and removing one at a time would make each layer record a stale neighbour instead
   * (see #SCULPT_OT_layer_bake for the same trap). Capturing does not unlink, so removal is a
   * second pass. */
  Vector<undo::SculptLayerUndoPayload> removed;
  for (SculptLayer *source : sources) {
    removed.append(undo::sculpt_layer_payload_capture(*ctx.mesh, source->base));
  }
  for (SculptLayer *source : sources) {
    bke::sculpt_layers::remove(*ctx.mesh, *source);
  }
  bke::sculpt_layers::active_set(*ctx.mesh, active);
  undo::push_sculpt_layer_list_change(*ctx.object, std::move(removed), {}, false);
  undo::push_end(*ctx.object);

  /* `active * active_eff + Σ(source * source_eff)` at influence 1 equals the previous combination
   * exactly, so the combined surface (and the live positions) are unchanged. */
  layers_ui_notify(C, *ctx.object);
  return OPERATOR_FINISHED;
}

void SCULPT_OT_layer_merge_selected(wmOperatorType *ot)
{
  ot->name = "Merge Selected Sculpt Layers";
  ot->idname = "SCULPT_OT_layer_merge_selected";
  ot->description = "Merge the selected sculpt layers into the active one";
  ot->exec = layer_merge_selected_exec;
  ot->poll = layers_poll;
  /* No #OPTYPE_UNDO: the operator pushes its own sculpt undo step; a global push would insert a
   * memfile step that does not compose with the stroke SCULPT steps (see #SCULPT_OT_layer_solo_base). */
  ot->flag = OPTYPE_REGISTER;
}

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
  undo::push_begin(*CTX_data_scene(C), *ctx.object, op);
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
    baked.append(undo::sculpt_layer_payload_capture(*ctx.mesh, layer->base));
  }
  for (SculptLayer *layer : all) {
    bke::sculpt_layers::remove(*ctx.mesh, *layer);
  }
  undo::push_sculpt_layer_list_change(*ctx.object, std::move(baked), {}, true);
  if (bake_key) {
    undo::push_sculpt_layer_bake_shape_key(*ctx.object, bake_key->uid);
  }
  /* The combined surface is unchanged; the runtime mesh base now equals the live positions. */
  invalidate_runtime(*ctx.object);
  session_state_ensure(*ctx.object);
  undo::push_end(*ctx.object);
  if (!ctx.grids && ctx.mesh->key != nullptr) {
    /* The key data changed and the session's #deform_cos is a snapshot of the pre-bake surface:
     * re-evaluate so the display is rebuilt from the new keys. */
    DEG_id_tag_update(&ctx.mesh->id, ID_RECALC_GEOMETRY);
  }
  layers_ui_notify(C, *ctx.object);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus layer_bake_invoke(bContext *C, wmOperator *op, const wmEvent * /*event*/)
{
  OpContext ctx;
  if (!op_context_get(C, op, ctx)) {
    return OPERATOR_CANCELLED;
  }
  /* State-aware confirmation matching what #layer_bake_exec actually does with the layers:
   * - Multires: folded into the displacement (permanent).
   * - Mesh without shape keys: folded into the base geometry (permanent, the layers are gone).
   * - Mesh with a relative key: turned into one new dial-able shape key (non-destructive).
   * - Mesh with an absolute key: folded into every existing key block (no dial). */
  const char *message;
  if (ctx.grids) {
    message = IFACE_("All sculpt layers will be baked into the multires displacement.");
  }
  else if (ctx.mesh->key == nullptr) {
    message = IFACE_(
        "All sculpt layers will be permanently baked into the base geometry and removed.");
  }
  else if (ctx.mesh->key->type == KEY_RELATIVE) {
    message = IFACE_(
        "This mesh has shape keys, so all sculpt layers will be baked into a new dial-able shape "
        "key.");
  }
  else {
    message = IFACE_(
        "This mesh has shape keys, so all sculpt layers will be baked into the existing shape "
        "keys.");
  }
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
}

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
   * The bootstrap below (and its #created_key undo machinery) runs only when there is no key yet. */
  if (ctx.mesh->key != nullptr) {
    return WM_operator_name_call(
        C, "SCULPT_OT_layer_bake", wm::OpCallContext::ExecDefault, nullptr, nullptr);
  }

  const short pre_bake_shapenr = ctx.object->shapenr;
  undo::push_begin(*CTX_data_scene(C), *ctx.object, op);

  /* Bootstrap the Key: mirrors #insert_meshkey (object.cc). #BKE_key_add's ID_ME case already
   * strips the layer contribution out of `vert_positions` (see
   * #bke::sculpt_layers::strip_vert_layers_from_positions), so `basis` below is seeded with the
   * un-layered original shape. */
  Main *bmain = CTX_data_main(C);
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
    baked_layers.append(undo::sculpt_layer_payload_capture(*ctx.mesh, layer->base));
  }
  for (SculptLayer *layer : all) {
    bke::sculpt_layers::remove(*ctx.mesh, *layer);
  }
  undo::push_sculpt_layer_list_change(*ctx.object, std::move(baked_layers), {}, true);
  undo::push_sculpt_layer_bake_to_shape_key(*ctx.object, pre_bake_shapenr);

  /* Select the usable key as active (matching the pattern #object_shape_key_add uses,
   * object_shapekey.cc): the baked block when it has data, the Basis otherwise (degenerate
   * case — see the design spec's "Degenerate case" section). */
  KeyBlock *active_key = (baked && baked->data) ? baked : basis;
  ctx.object->shapenr = BLI_findindex(&key->block, active_key) + 1;

  /* The combined surface is unchanged; the runtime mesh base now equals the live positions
   * composed through the new key instead of the (now absent) layer list. */
  invalidate_runtime(*ctx.object);
  session_state_ensure(*ctx.object);
  undo::push_end(*ctx.object);
  /* The key data changed and the session's deform snapshot is a pre-bake surface: re-evaluate
   * so the display is rebuilt from the new keys (unconditional here, unlike #layer_bake_exec's
   * equivalent tag, because this operator's `ctx.mesh->key` is always non-null by this point). */
  DEG_id_tag_update(&ctx.mesh->id, ID_RECALC_GEOMETRY);
  layers_ui_notify(C, *ctx.object);
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
}

/* "Bake All Layers" choice of #SCULPT_PT_layer_editmode_confirm (see
 * #OBJECT_OT_editmode_toggle's sculpt-layers warning): bake, then re-enter the Edit Mode toggle
 * with the warning bypassed so the mode switch actually proceeds. The warning can fire from any
 * mode (not just Sculpt Mode), but #SCULPT_OT_layer_bake needs a live sculpt session, so enter
 * Sculpt Mode first if the object is not already in it; #OBJECT_OT_editmode_toggle's own
 * mode-compat handling below exits Sculpt Mode again on the way into Edit Mode. */
static wmOperatorStatus layer_bake_and_editmode_enter_exec(bContext *C, wmOperator * /*op*/)
{
  const Object *object = CTX_data_active_object(C);
  if (object && !(object->mode & OB_MODE_SCULPT)) {
    WM_operator_name_call(
        C, "SCULPT_OT_sculptmode_toggle", wm::OpCallContext::ExecDefault, nullptr, nullptr);
  }

  WM_operator_name_call(
      C, "SCULPT_OT_layer_bake", wm::OpCallContext::ExecDefault, nullptr, nullptr);

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
}

static wmOperatorStatus layer_clear_exec(bContext *C, wmOperator *op)
{
  OpContext ctx;
  if (!op_context_get(C, op, ctx)) {
    return OPERATOR_CANCELLED;
  }
  SculptLayer *layer = bke::sculpt_layers::active_get(*ctx.mesh);
  if (!layer) {
    return OPERATOR_CANCELLED;
  }
  session_state_ensure(*ctx.object);
  undo::push_begin(*CTX_data_scene(C), *ctx.object, op);
  undo::push_sculpt_layer_data(*ctx.object, *layer);
  bke::sculpt_layers::data_clear(*layer);
  commit_layers_change(*ctx.depsgraph, *ctx.object);
  undo::push_end(*ctx.object);
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
  SculptLayer *layer = bke::sculpt_layers::active_get(*ctx.mesh);
  if (!layer || !layer->data) {
    return OPERATOR_CANCELLED;
  }
  session_state_ensure(*ctx.object);
  undo::push_begin(*CTX_data_scene(C), *ctx.object, op);
  undo::push_sculpt_layer_data(*ctx.object, *layer);
  for (float3 &v : bke::sculpt_layers::data_get(*layer)) {
    v = -v;
  }
  commit_layers_change(*ctx.depsgraph, *ctx.object);
  undo::push_end(*ctx.object);
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

  session_state_ensure(*ctx.object);
  undo::push_begin(*CTX_data_scene(C), *ctx.object, op);
  if (action == ValidateAction::Remove) {
    /* Captured in list order while the list is still intact, then removed, so undo restores each
     * layer next to the neighbour it really had (see #layer_bake_exec). */
    Vector<undo::SculptLayerUndoPayload> removed;
    for (SculptLayer *layer : stale) {
      removed.append(undo::sculpt_layer_payload_capture(*ctx.mesh, layer->base));
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
      resized.append(undo::sculpt_layer_payload_capture(*ctx.mesh, layer->base));
      /* Re-fit to the live count of the layer's *own* domain (not the object's current sculpt
       * domain, which says nothing about a vertex layer sitting under a multires modifier).
       * #data_ensure allocates zeroed, which is exactly the wanted result: an empty but usable
       * layer on the new topology rather than a buffer that cannot be indexed. */
      bke::sculpt_layers::data_ensure(*layer,
                                      bke::sculpt_layers::element_count(*ctx.mesh, *layer));
    }
    undo::push_sculpt_layer_data_resize(*ctx.object, std::move(resized));
  }
  /* A stale layer contributed nothing to the surface, so neither action moves any vertex; the
   * commit is still needed to rebuild the runtime base and the display from the new layer set. */
  commit_layers_change(*ctx.depsgraph, *ctx.object);
  undo::push_end(*ctx.object);
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

static wmOperatorStatus layer_mask_isolate_exec(bContext *C, wmOperator *op)
{
  OpContext ctx;
  if (!op_context_get(C, op, ctx)) {
    return OPERATOR_CANCELLED;
  }
  SculptLayer *layer = bke::sculpt_layers::active_get(*ctx.mesh);
  if (!layer || !layer->data) {
    return OPERATOR_CANCELLED;
  }

  Array<float> mask;
  if (!gather_layer_mask(*ctx.object, *layer, mask)) {
    BKE_report(op->reports, RPT_ERROR, "No mask painted");
    return OPERATOR_CANCELLED;
  }

  const bool invert = RNA_boolean_get(op->ptr, "invert");
  const bool clear_mask = RNA_boolean_get(op->ptr, "clear_mask");

  session_state_ensure(*ctx.object);
  undo::push_begin(*CTX_data_scene(C), *ctx.object, op);
  undo::push_sculpt_layer_data(*ctx.object, *layer);
  MutableSpan<float3> data = bke::sculpt_layers::data_get(*layer);
  for (const int64_t i : data.index_range()) {
    const float factor = invert ? 1.0f - mask[i] : mask[i];
    data[i] *= factor;
  }
  commit_layers_change(*ctx.depsgraph, *ctx.object);
  undo::push_end(*ctx.object);

  if (clear_mask) {
    /* Reuse the existing flood-fill operator instead of re-deriving the per-domain mask-clear
     * logic (grid dirty flags, node mask-changed tagging, multires-modified marking are already
     * handled there). Runs as its own undo step, same as other layer operators that delegate to
     * a sibling operator (see #layer_bake_and_editmode_enter_exec calling SCULPT_OT_layer_bake).
     */
    PointerRNA props = WM_operator_properties_create("PAINT_OT_mask_flood_fill");
    RNA_enum_set_identifier(C, &props, "mode", "VALUE");
    RNA_float_set(&props, "value", 0.0f);
    WM_operator_name_call(
        C, "PAINT_OT_mask_flood_fill", wm::OpCallContext::ExecDefault, &props, nullptr);
    WM_operator_properties_free(&props);
  }

  layers_ui_notify(C, *ctx.object);
  return OPERATOR_FINISHED;
}

void SCULPT_OT_layer_mask_isolate(wmOperatorType *ot)
{
  ot->name = "Isolate Layer by Mask";
  ot->idname = "SCULPT_OT_layer_mask_isolate";
  ot->description =
      "Keep only the masked part of the active sculpt layer's displacement, zeroing the rest";
  ot->exec = layer_mask_isolate_exec;
  ot->poll = layers_poll;
  ot->flag = OPTYPE_REGISTER;

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
  SculptLayer *layer = bke::sculpt_layers::active_get(*ctx.mesh);
  if (!layer) {
    return OPERATOR_CANCELLED;
  }
  const float value = RNA_float_get(op->ptr, "influence");
  session_state_ensure(*ctx.object);
  undo::push_begin(*CTX_data_scene(C), *ctx.object, op);
  undo::push_sculpt_layer_metadata(*ctx.object, layer->base);
  layer->influence = value;
  commit_layers_change(*ctx.depsgraph, *ctx.object);
  undo::push_end(*ctx.object);
  layers_ui_notify(C, *ctx.object);
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
         * refresh below and once more on release. */
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

static wmOperatorStatus layer_toggle_visibility_exec(bContext *C, wmOperator *op)
{
  OpContext ctx;
  if (!op_context_get(C, op, ctx)) {
    return OPERATOR_CANCELLED;
  }
  SculptLayer *layer = bke::sculpt_layers::active_get(*ctx.mesh);
  if (!layer) {
    return OPERATOR_CANCELLED;
  }
  session_state_ensure(*ctx.object);
  undo::push_begin(*CTX_data_scene(C), *ctx.object, op);
  undo::push_sculpt_layer_metadata(*ctx.object, layer->base);
  layer->base.flag ^= SCULPT_LAYER_ENABLED;
  commit_layers_change(*ctx.depsgraph, *ctx.object);
  undo::push_end(*ctx.object);
  layers_ui_notify(C, *ctx.object);
  return OPERATOR_FINISHED;
}

void SCULPT_OT_layer_toggle_visibility(wmOperatorType *ot)
{
  ot->name = "Toggle Sculpt Layer Visibility";
  ot->idname = "SCULPT_OT_layer_toggle_visibility";
  ot->description = "Toggle whether the active sculpt layer contributes to the result";
  ot->exec = layer_toggle_visibility_exec;
  ot->poll = layers_poll;
  ot->flag = OPTYPE_REGISTER;
}

/* Solo Base: sculpting the base while layers are visible bakes the layer residual into the base
 * wherever a surface-dependent brush (smooth / grab / flatten) reshapes the composed surface —
 * the flush must store `surface - layers` to keep the result intact. Isolating the base removes
 * the layers from the surface the brush sees, so base edits stay clean. The enabled flags of the
 * hidden layers are preserved via #SCULPT_LAYER_SOLO_HIDDEN and restored on the second toggle. */
static wmOperatorStatus layer_solo_base_exec(bContext *C, wmOperator *op)
{
  OpContext ctx;
  if (!op_context_get(C, op, ctx)) {
    return OPERATOR_CANCELLED;
  }
  Mesh &mesh = *ctx.mesh;

  /* Solo is active when any layer carries the marker (single authority in BKE). */
  const bool solo_active = bke::sculpt_layers::solo_active(mesh);

  /* Layers the toggle changes: the enabled ones when activating, the marked ones when ending. */
  Vector<SculptLayer *> affected;
  for (SculptLayer *layer : bke::sculpt_layers::layers(mesh)) {
    const int test_flag = solo_active ? SCULPT_LAYER_SOLO_HIDDEN : SCULPT_LAYER_ENABLED;
    if (layer->base.flag & test_flag) {
      affected.append(layer);
    }
  }
  if (affected.is_empty()) {
    return OPERATOR_CANCELLED;
  }

  /* Derive the mesh-domain runtime base from the still-consistent pre-change state (pending
   * multires base edits were already consumed by #op_context_get). */
  session_state_ensure(*ctx.object);

  undo::push_begin(*CTX_data_scene(C), *ctx.object, op);
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
  undo::push_end(*ctx.object);
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

static wmOperatorStatus layer_group_toggle_visibility_exec(bContext *C, wmOperator *op)
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

  /* Derive the mesh-domain runtime base from the still-consistent pre-change state (pending multires
   * base edits were already consumed by #op_context_get), as the Solo Base toggle does. */
  session_state_ensure(*ctx.object);

  undo::push_begin(*CTX_data_scene(C), *ctx.object, op);
  undo::push_sculpt_layer_metadata(*ctx.object, group->base);
  group->base.flag ^= SCULPT_LAYER_GROUP_ENABLED;
  group_cascade_resync_with_undo(*ctx.object, *ctx.mesh);
  commit_layers_change(*ctx.depsgraph, *ctx.object);
  undo::push_end(*ctx.object);
  layers_ui_notify(C, *ctx.object);
  return OPERATOR_FINISHED;
}

void SCULPT_OT_layer_group_toggle_visibility(wmOperatorType *ot)
{
  ot->name = "Toggle Sculpt Layer Group Visibility";
  ot->idname = "SCULPT_OT_layer_group_toggle_visibility";
  ot->description = "Toggle visibility of a sculpt layer group and everything inside it";
  ot->exec = layer_group_toggle_visibility_exec;
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
              "Unique id of the sculpt layer group to toggle",
              0,
              INT_MAX);
}

static wmOperatorStatus layer_group_add_exec(bContext *C, wmOperator *op)
{
  OpContext ctx;
  if (!op_context_get(C, op, ctx)) {
    return OPERATOR_CANCELLED;
  }
  Mesh &mesh = *ctx.mesh;

  /* Wrap the current selection, if any: the new folder takes the place of the first selected row
   * (so it appears where the user was looking) and everything selected moves into it.
   *
   * The gather skips whatever sits inside a selected folder, which is what makes the wrap safe as
   * well as right: the first wrapped node is then never nested in another wrapped folder, so the
   * new folder — a sibling of that first node — is never inside one either, and no move below can
   * detach a subtree. */
  Vector<SculptLayerTreeNode *> wrapped;
  selected_nodes_gather(*bke::sculpt_layers::root_group(mesh), wrapped);

  /* The new folder is a sibling of what it wraps, so it lands in that node's parent, in that
   * node's own slot; with nothing selected it is appended to the root. Both are read before
   * anything moves. */
  SculptLayerGroup *parent = wrapped.is_empty() ? bke::sculpt_layers::root_group(mesh) :
                                                  wrapped.first()->parent;
  SculptLayerTreeNode *slot = wrapped.is_empty() ? nullptr : wrapped.first()->prev;

  session_state_ensure(*ctx.object);
  undo::push_begin(*CTX_data_scene(C), *ctx.object, op);

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
  undo::push_end(*ctx.object);
  layers_ui_notify(C, *ctx.object);
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

  session_state_ensure(*ctx.object);
  undo::push_begin(*CTX_data_scene(C), *ctx.object, op);

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
  removed.append(undo::sculpt_layer_payload_capture(mesh, group->base));
  bke::sculpt_layers::group_remove(mesh, *group);
  undo::push_sculpt_layer_group_list_change(*ctx.object, std::move(removed), {});

  group_cascade_resync_with_undo(*ctx.object, mesh);
  commit_layers_change(*ctx.depsgraph, *ctx.object);
  undo::push_end(*ctx.object);
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
    removed_groups.append(undo::sculpt_layer_payload_capture(mesh, folder->base));
  }
  for (int64_t i = folders.size() - 1; i >= 0; i--) {
    bke::sculpt_layers::group_remove(mesh, *folders[i]);
  }
  undo::push_sculpt_layer_group_list_change(object, std::move(removed_groups), {});
}

static wmOperatorStatus layer_group_merge_exec(bContext *C, wmOperator *op)
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

  SculptLayer *survivor = subtree_layers.first();

  session_state_ensure(*ctx.object);
  undo::push_begin(*CTX_data_scene(C), *ctx.object, op);

  /* Accumulate `data[i] * effective` of every subtree layer into the survivor at influence 1. The
   * sum equals the folder's prior combined contribution, so the visible surface is unchanged. Buffer
   * sizing mirrors #layer_merge_selected_exec: grid buffers all sit at the canonical (top) level. */
  int n = ctx.grids ? survivor->totelem : element_count(*ctx.object);
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

  const float survivor_eff = effective(*survivor);
  for (float3 &v : survivor_data) {
    v *= survivor_eff;
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
    const Span<float3> layer_data(static_cast<const float3 *>(layer->data), layer->totelem);
    const int64_t count = std::min<int64_t>(survivor_data.size(), layer_data.size());
    for (int64_t i = 0; i < count; i++) {
      survivor_data[i] += layer_data[i] * layer_eff;
    }
  }
  survivor->influence = 1.0f;
  survivor->base.flag |= SCULPT_LAYER_ENABLED;

  /* Capture the merged-away layers at their *original* positions, before the lift below moves them,
   * so undo re-inserts them where they really sat. Capturing transfers each buffer to the payload;
   * the merge already read it above, so this only hands over ownership. The survivor is excluded — it
   * stays as the merged result. */
  Vector<undo::SculptLayerUndoPayload> removed_layers;
  for (SculptLayer *layer : subtree_layers) {
    if (layer == survivor) {
      continue;
    }
    removed_layers.append(undo::sculpt_layer_payload_capture(mesh, layer->base));
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
  undo::push_end(*ctx.object);
  /* `Σ(data · effective)` at influence 1 equals the folder's prior combined contribution, so the
   * combined surface (and the live positions) are unchanged. */
  layers_ui_notify(C, *ctx.object);
  return OPERATOR_FINISHED;
}

void SCULPT_OT_layer_group_merge(wmOperatorType *ot)
{
  ot->name = "Merge Sculpt Layer Group";
  ot->idname = "SCULPT_OT_layer_group_merge";
  ot->description =
      "Merge every sculpt layer inside a folder into a single layer named after the folder, "
      "removing the folder and any nested folders";
  ot->exec = layer_group_merge_exec;
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
              "Unique id of the sculpt layer group to merge",
              0,
              INT_MAX);
}

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

  /* Derive the mesh base from the still-consistent pre-change state: dropping the layers changes the
   * combined surface, and the commit below recomputes it from that base. */
  session_state_ensure(*ctx.object);
  undo::push_begin(*CTX_data_scene(C), *ctx.object, op);

  /* Capture every layer at its *original* position before the lift below moves it, so undo re-inserts
   * it where it really sat (with its data buffer). */
  Vector<undo::SculptLayerUndoPayload> removed_layers;
  removed_layers.reserve(subtree_layers.size());
  for (SculptLayer *layer : subtree_layers) {
    removed_layers.append(undo::sculpt_layer_payload_capture(mesh, layer->base));
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
  undo::push_end(*ctx.object);
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

static wmOperatorStatus layer_select_exec(bContext *C, wmOperator *op)
{
  OpContext ctx;
  if (!op_context_get(C, op, ctx)) {
    return OPERATOR_CANCELLED;
  }
  const int uid = RNA_int_get(op->ptr, "uid");
  /* The kind-checked cast is what refuses a folder's uid — one counter spans both kinds now — and,
   * with it, uid 0: #node_find_by_uid resolves 0 to the root folder, which is not a layer. That is
   * a different convention from #Mesh::sculpt_layers_active_uid below, where 0 means "no active
   * layer"; the two must not be resolved through each other. */
  if (bke::sculpt_layers::node_as_layer(bke::sculpt_layers::node_find_by_uid(*ctx.mesh, uid)) ==
      nullptr)
  {
    BKE_report(op->reports, RPT_ERROR, "No sculpt layer with that id");
    return OPERATOR_CANCELLED;
  }
  const int uid_from = ctx.mesh->sculpt_layers_active_uid;
  if (uid == uid_from) {
    return OPERATOR_FINISHED;
  }
  /* Selection must ride on a sculpt undo step: without one, #OPTYPE_UNDO used to push a plain
   * memfile step, and undoing across it between two stroke SCULPT steps corrupted the delta-based
   * sculpt undo state. */
  undo::push_begin(*CTX_data_scene(C), *ctx.object, op);
  undo::push_sculpt_layer_active(*ctx.object, uid_from, uid);
  ctx.mesh->sculpt_layers_active_uid = uid;
  undo::push_end(*ctx.object);
  layers_ui_notify(C, *ctx.object);
  return OPERATOR_FINISHED;
}

void SCULPT_OT_layer_select(wmOperatorType *ot)
{
  ot->name = "Select Sculpt Layer";
  ot->idname = "SCULPT_OT_layer_select";
  ot->description = "Set the active sculpt layer";
  ot->exec = layer_select_exec;
  ot->poll = layers_poll;
  /* No #OPTYPE_UNDO: the operator pushes its own sculpt undo step; a global push would insert a
   * memfile step that does not compose with the stroke SCULPT steps (see #SCULPT_OT_layer_solo_base). */
  ot->flag = OPTYPE_REGISTER;
  /* Identified by uid rather than by position: an operator's arguments outlive the call (they are
   * kept for redo), by which point a position may name a different layer. */
  RNA_def_int(
      ot->srna, "uid", 0, 0, INT_MAX, "Layer ID", "Unique id of the layer to make active", 0, INT_MAX);
}

static wmOperatorStatus layer_toggle_rec_exec(bContext *C, wmOperator *op)
{
  OpContext ctx;
  if (!op_context_get(C, op, ctx)) {
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
  /* Derive the runtime base from the still-consistent pre-change state, then capture pre-change
   * layer metadata so Ctrl+Z can revert the enabled/influence normalization. */
  session_state_ensure(*ctx.object);
  if (layer) {
    undo::push_begin(*CTX_data_scene(C), *ctx.object, op);
    undo::push_sculpt_layer_metadata(*ctx.object, layer->base);
  }
  ss->layers.rec_active = !ss->layers.rec_active;
  if (ss->layers.rec_active && layer) {
    const float old_eff = effective(*layer);
    layer->base.flag |= SCULPT_LAYER_ENABLED;
    layer->influence = 1.0f;
    if (effective(*layer) != old_eff) {
      /* REC pins the layer to enabled + influence 1.0; bring the positions in sync so the first
       * recorded stroke starts from the surface the user actually sees. */
      commit_layers_change(*ctx.depsgraph, *ctx.object);
    }
  }
  if (layer) {
    undo::push_end(*ctx.object);
  }
  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, ctx.object);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, &ctx.mesh->id);
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
