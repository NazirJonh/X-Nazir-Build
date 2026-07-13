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
 * `base + sum_over_enabled_layers(layer.data[i] * layer.influence)`.
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
#include <type_traits>

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
#include "BLI_task.hh"
#include "BLI_vector.hh"

#include "BLT_translation.hh"

#include "BKE_context.hh"
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

/* Sculpt layers are duplicated with C-style allocators (#MEM_dupalloc / #BLI_duplicatelist) and
 * their data buffers with #MEM_dupalloc_void, while the nodes are freed with #MEM_delete. This is
 * only valid as long as #SculptLayer stays trivially destructible. */
static_assert(std::is_trivially_destructible_v<SculptLayer>,
              "SculptLayer must remain trivially destructible (see allocation notes in "
              "sculpt_layers.cc / BKE_sculpt_layers.hh)");

/* -------------------------------------------------------------------------------------------------
 * Helpers
 */

static Mesh &mesh_of(Object &object)
{
  return *id_cast<Mesh *>(object.data);
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
  ss->layers.base_view_node_bounds = {};
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
  for (const SculptLayer &layer : mesh.sculpt_layers) {
    if (layer.flag & SCULPT_LAYER_ENABLED) {
      return true;
    }
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
  for (SculptLayer &layer : mesh.sculpt_layers) {
    if (layer.domain != SCULPT_LAYER_DOMAIN_GRID || layer.data == nullptr) {
      continue;
    }
    if (layer.totelem == expected && layer.level == totlvl) {
      continue;
    }
    MEM_delete_void(layer.data);
    layer.data = nullptr;
    layer.totelem = 0;
    layer.level = short(totlvl);
    if (!warned) {
      CLOG_WARN(&LOG,
                "Grid sculpt layer data reset: stored level/topology no longer matches the mesh.");
      warned = true;
    }
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
  bke::sculpt_layers::derive_base_mesh(positions, mesh.sculpt_layers, ss->layers.mesh_base);
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
  bke::sculpt_layers::combine_layers_mesh(ss->layers.mesh_base, mesh.sculpt_layers, positions);
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
      ss->layers.mesh_base.as_span(), mesh.sculpt_layers, expected);
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
    for (const SculptLayer &layer : mesh.sculpt_layers) {
      printf("  layer[%d] uid=%d domain=%d enabled=%d influence=%.4f totelem=%d\n",
             layer_index++,
             layer.uid,
             int(layer.domain),
             int((layer.flag & SCULPT_LAYER_ENABLED) != 0),
             double(layer.influence),
             layer.totelem);
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

/* Bounds of each leaf node's own elements in base-view space, `position[i] - base_view[i]` (the DC
 * is added at test time, see #base_view_extend_node_mask). Computed once per stroke, right after the
 * base view itself; non-leaf entries stay empty and never intersect anything. */
static void base_view_node_bounds_ensure(const Depsgraph &depsgraph, Object &object)
{
  SculptSession *ss = session_of(object);
  ss->layers.base_view_node_bounds = {};

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
    /* The evaluated positions, not #Mesh::vert_positions: with a shape key (or a deform modifier)
     * the mesh holds the untouched basis, while the brush — and therefore the mask this bounds test
     * feeds — works on the deformed surface. Without a deform the two are the same array. */
    const Span<float3> positions = bke::pbvh::vert_positions_eval(depsgraph, object);
    if (positions.size() != base_view.size()) {
      return;
    }
    const Span<bke::pbvh::MeshNode> nodes = pbvh->nodes<bke::pbvh::MeshNode>();
    threading::parallel_for(leaf_indices.index_range(), 8, [&](const IndexRange range) {
      for (const int index : range) {
        const int node_index = leaf_indices[index];
        Bounds<float3> bounds = negative_bounds();
        for (const int vert : nodes[node_index].verts()) {
          math::min_max(positions[vert] - base_view[vert], bounds.min, bounds.max);
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
            math::min_max(positions[i] - base_view[i], bounds.min, bounds.max);
          }
        }
        node_bounds[node_index] = bounds;
      }
    });
  }
  else {
    return;
  }

  ss->layers.base_view_node_bounds = std::move(node_bounds);
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
  const Span<Bounds<float3>> node_bounds = ss->layers.base_view_node_bounds;
  if (node_bounds.is_empty()) {
    return node_mask;
  }

  /* The brush measures its falloff on the base view, `position[i] - (base_view[i] - dc)`, so an
   * element is reached when its base-view position lies within the radius of the cursor — that is,
   * when `position[i] - base_view[i]` lies within the radius of `location - dc`. Gather every node
   * whose base-view bounds satisfy that, on top of the nodes the composed-space search already
   * found. The result is a superset, so the elements the gather adds simply receive their (possibly
   * zero) factor like any other; what matters is that no element with a NON-zero factor is left in
   * an ungathered node, which is what carved the stroke along node borders. */
  const float3 center = ss->cache->location_symm - ss->layers.base_view_dc;
  const float radius_sq = radius * radius;
  const IndexMask reached = IndexMask::from_predicate(
      IndexMask(node_bounds.index_range()), memory, [&](const int64_t i) {
        const Bounds<float3> &bounds = node_bounds[i];
        if (bounds.min.x > bounds.max.x) {
          /* Empty: a non-leaf node. */
          return false;
        }
        const float3 nearest = math::clamp(center, bounds.min, bounds.max);
        return math::distance_squared(nearest, center) < radius_sq;
      });
  return IndexMask::from_union(node_mask, reached, memory);
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
  bke::sculpt_layers::apply_vert_layers(mesh.sculpt_layers, view);
  return view;
}

/* Only base edits build a base view; a recorded stroke works on the composed surface (see
 * #stroke_record_begin), so there is no layer to exclude from the sum. */
static void base_view_ensure(const Depsgraph &depsgraph, Object &object)
{
  SculptSession *ss = session_of(object);
  ss->layers.base_view = {};
  ss->layers.base_view_dc = float3(0.0f);
  ss->layers.base_view_node_bounds = {};

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
  for (const SculptLayer &layer : mesh.sculpt_layers) {
    if (layer.domain == domain && (layer.flag & SCULPT_LAYER_ENABLED) &&
        layer.influence != 0.0f && layer.data != nullptr)
    {
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
    for (const SculptLayer &layer : mesh.sculpt_layers) {
      if (layer.domain != SCULPT_LAYER_DOMAIN_GRID || !(layer.flag & SCULPT_LAYER_ENABLED) ||
          layer.influence == 0.0f || layer.data == nullptr)
      {
        continue;
      }
      if (!BKE_multires_sculpt_layer_object_contribution(mesh, subdiv_ccg, layer, contrib)) {
        /* A partial sum would make the brush inputs inconsistent; fall back to plain behavior. */
        return;
      }
      const float influence = layer.influence;
      threading::parallel_for(total.index_range(), 8192, [&](const IndexRange range) {
        for (const int64_t i : range) {
          total[i] += contrib[i] * influence;
        }
      });
    }
    ss->layers.base_view = std::move(total);
  }
  base_view_node_bounds_ensure(depsgraph, object);
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
  if (!ss->layers.rec_active || bke::sculpt_layers::solo_active(mesh)) {
    /* REC off, or Solo Base is isolating the base for direct sculpting (nothing to record into
     * while layers are hidden): the stroke edits the base. Compute the base view so brush inputs
     * (falloff, area normal, smoothing / plane targets) can be evaluated against the un-layered
     * base; see the Base view section above. */
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

  SculptLayer *layer = bke::sculpt_layers::active_get(mesh);
  if (!layer || layer->domain != domain_for(object) || (layer->flag & SCULPT_LAYER_LOCKED)) {
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
  ss->layers.base_view_node_bounds = {};

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
  ss->layers.base_view_node_bounds = {};

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
  ss->layers.base_view_node_bounds = {};

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
  undo::push_sculpt_layer_list_change(*ctx.object, {}, Vector<int>({layer->uid}), false);
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
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static wmOperatorStatus layer_remove_exec(bContext *C, wmOperator *op)
{
  OpContext ctx;
  if (!op_context_get(C, op, ctx)) {
    return OPERATOR_CANCELLED;
  }
  SculptLayer *layer = bke::sculpt_layers::active_get(*ctx.mesh);
  if (!layer) {
    return OPERATOR_CANCELLED;
  }
  /* Derive the mesh base from the still-consistent pre-change state. */
  session_state_ensure(*ctx.object);
  undo::push_begin(*CTX_data_scene(C), *ctx.object, op);
  Vector<undo::SculptLayerUndoPayload> removed;
  removed.append(undo::sculpt_layer_payload_capture(*ctx.mesh, *layer));
  bke::sculpt_layers::remove(*ctx.mesh, *layer);
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
  ot->description = "Remove the active sculpt layer and its contribution";
  ot->exec = layer_remove_exec;
  ot->poll = layers_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
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
  const int dir = RNA_enum_get(op->ptr, "direction");
  const int step = (dir == 0) ? -1 : 1;
  const int index_from = bke::sculpt_layers::index_of(*ctx.mesh, *layer);
  if (BLI_listbase_link_move(&ctx.mesh->sculpt_layers, layer, step)) {
    bke::sculpt_layers::active_set(*ctx.mesh, layer);
    undo::push_begin(*CTX_data_scene(C), *ctx.object, op);
    undo::push_sculpt_layer_move(
        *ctx.object, *layer, index_from, bke::sculpt_layers::index_of(*ctx.mesh, *layer));
    undo::push_end(*ctx.object);
    /* Layer order does not affect the additive combination; UI refresh only. */
    layers_ui_notify(C, *ctx.object);
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
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  RNA_def_enum(ot->srna, "direction", direction_items, 0, "Direction", "");
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
  undo::push_sculpt_layer_list_change(*ctx.object, {}, Vector<int>({copy->uid}), false);
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
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static wmOperatorStatus layer_merge_down_exec(bContext *C, wmOperator *op)
{
  OpContext ctx;
  if (!op_context_get(C, op, ctx)) {
    return OPERATOR_CANCELLED;
  }
  SculptLayer *active = bke::sculpt_layers::active_get(*ctx.mesh);
  if (!active || !active->next) {
    BKE_report(op->reports, RPT_ERROR, "No layer below to merge into");
    return OPERATOR_CANCELLED;
  }
  SculptLayer *below = active->next;
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
  below->flag |= SCULPT_LAYER_ENABLED;

  Vector<undo::SculptLayerUndoPayload> removed;
  removed.append(undo::sculpt_layer_payload_capture(*ctx.mesh, *active));
  bke::sculpt_layers::active_set(*ctx.mesh, below);
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
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
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
  Vector<undo::SculptLayerUndoPayload> baked;
  while (SculptLayer *layer = static_cast<SculptLayer *>(ctx.mesh->sculpt_layers.first)) {
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
    baked.append(undo::sculpt_layer_payload_capture(*ctx.mesh, *layer));
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

void SCULPT_OT_layer_bake(wmOperatorType *ot)
{
  ot->name = "Bake Sculpt Layers";
  ot->idname = "SCULPT_OT_layer_bake";
  ot->description = "Apply all sculpt layers permanently to the base geometry and remove them";
  ot->exec = layer_bake_exec;
  /* Irreversible-feeling from the UI (drops every layer into the base), so confirm before running. */
  ot->invoke = WM_operator_confirm;
  ot->poll = layers_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
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
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
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
  undo::push_sculpt_layer_metadata(*ctx.object, *layer);
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
    const bool enabled = (layer.flag & SCULPT_LAYER_ENABLED) != 0;
    const float target_eff = enabled ? target : 0.0f;
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
  if (!layer || layer->domain != domain_for(object) || (layer->flag & SCULPT_LAYER_LOCKED)) {
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
                         bke::object::pbvh_get(object)->begin_influence_drag(layer->uid);
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
           layer->uid,
           int((layer->flag & SCULPT_LAYER_ENABLED) != 0),
           int(layer->data != nullptr),
           layer->totelem);

  /* Capture the pre-drag layer metadata so Ctrl+Z reverts the influence change. */
  undo::push_begin(*CTX_data_scene(C), object, op);
  undo::push_sculpt_layer_metadata(object, *layer);

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
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING;
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
  undo::push_sculpt_layer_metadata(*ctx.object, *layer);
  layer->flag ^= SCULPT_LAYER_ENABLED;
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
  for (SculptLayer &layer : mesh.sculpt_layers) {
    const int test_flag = solo_active ? SCULPT_LAYER_SOLO_HIDDEN : SCULPT_LAYER_ENABLED;
    if (layer.flag & test_flag) {
      affected.append(&layer);
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
    uids.append(layer->uid);
    flags.append(layer->flag);
  }
  undo::push_sculpt_layer_solo(*ctx.object, std::move(uids), std::move(flags));

  for (SculptLayer *layer : affected) {
    if (solo_active) {
      layer->flag |= SCULPT_LAYER_ENABLED;
      layer->flag &= ~SCULPT_LAYER_SOLO_HIDDEN;
    }
    else {
      layer->flag &= ~SCULPT_LAYER_ENABLED;
      layer->flag |= SCULPT_LAYER_SOLO_HIDDEN;
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

static wmOperatorStatus layer_select_exec(bContext *C, wmOperator *op)
{
  OpContext ctx;
  if (!op_context_get(C, op, ctx)) {
    return OPERATOR_CANCELLED;
  }
  const int index = RNA_int_get(op->ptr, "index");
  const int count = BLI_listbase_count(&ctx.mesh->sculpt_layers);
  if (index < 0 || index >= count) {
    return OPERATOR_CANCELLED;
  }
  ctx.mesh->sculpt_layers_active_index = index;
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
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  RNA_def_int(ot->srna, "index", 0, 0, INT_MAX, "Index", "Index of the layer to make active", 0, INT_MAX);
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
  /* Derive the runtime base from the still-consistent pre-change state, then capture pre-change
   * layer metadata so Ctrl+Z can revert the enabled/influence normalization. */
  session_state_ensure(*ctx.object);
  if (layer) {
    undo::push_begin(*CTX_data_scene(C), *ctx.object, op);
    undo::push_sculpt_layer_metadata(*ctx.object, *layer);
  }
  ss->layers.rec_active = !ss->layers.rec_active;
  if (ss->layers.rec_active && layer) {
    const float old_eff = effective(*layer);
    layer->flag |= SCULPT_LAYER_ENABLED;
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
