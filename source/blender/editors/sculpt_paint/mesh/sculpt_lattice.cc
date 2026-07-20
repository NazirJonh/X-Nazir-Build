/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Sculpt Lattice Tool: operators and lifecycle.
 *
 * State machine (see 02_ux_state_machine.md):
 *   ToolSelected --first LMB--> Placement (ensure_session)
 *   Placement --LMB drag--> defines the cage box on a view/surface plane (Shift square, Alt
 *     center, Space move, then ray-depth; F flips depth); G/R/S transform it; F fits it to the
 *     mesh when the box modal is not running
 *   Placement --C--> Deform (re-snapshot affected region against the current mesh)
 *   Deform --C--> Placement (bake accumulated deform, reset cage to neutral)
 *   Deform --LMB on BPoint--> DragPoint --LMB release--> Deform  (per-drag undo step)
 *   (any phase) --Enter--> Confirmed (apply-once, session_free)
 *   (any phase, no sub-modal active) --Esc--> Cancelled (restore entry_positions, session_free)
 *   Box-define and Slide handle Esc themselves while their own sub-modal is running
 *
 * ADR-12: lazy init — selection of the tool in toolbar does NOT call an operator.
 */

#include "sculpt_lattice.hh"
#include "sculpt_lattice_intern.hh"

#include "MEM_guardedalloc.h"

#include "BLI_bounds.hh"
#include "BLI_bounds_types.hh"
#include "BLI_listbase_iterator.hh"
#include "BLI_math_base.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "BLT_translation.hh"

#include "DNA_lattice_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_theme_types.h" /* For #UI_SCALE_FAC. */
#include "DNA_userdef_types.h"

#include "BKE_attribute.hh"
#include "BKE_context.hh"
#include "BKE_global.hh"
#include "BKE_lattice.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_mesh.hh"
#include "BKE_object.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_report.hh"
#include "BKE_scene.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"
#include "DEG_depsgraph_query.hh"

#include "WM_api.hh"
#include "WM_toolsystem.hh"
#include "WM_types.hh"

#include "ED_object.hh"
#include "ED_screen.hh"
#include "ED_sculpt.hh"
#include "ED_sculpt_lattice.hh"
#include "ED_view3d.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_prototypes.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "sculpt_intern.hh"
#include "sculpt_undo.hh"

namespace blender::ed::sculpt_paint::lattice {

/* -------------------------------------------------------------------- */
/** \name Poll
 * \{ */

bool sculpt_lattice_tool_active_poll(bContext *C)
{
  Object *ob = CTX_data_active_object(C);
  if (!ob || !(ob->mode & OB_MODE_SCULPT)) {
    return false;
  }
  if (ob->type != OB_MESH) {
    return false;
  }
  const bToolRef *tref = WM_toolsystem_ref_from_context(C);
  return tref != nullptr && STREQ(tref->idname, "builtin.sculpt_lattice");
}

static bool sculpt_lattice_poll(bContext *C)
{
  return sculpt_lattice_tool_active_poll(C);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name PBVH access
 * \{ */

bke::pbvh::Tree *sculpt_lattice_pbvh_ensure(Depsgraph &depsgraph, Object &ob_mesh)
{
  BKE_sculpt_update_object_for_edit(&depsgraph, &ob_mesh, false);
  return sculpt_lattice_pbvh_find(ob_mesh);
}

bke::pbvh::Tree *sculpt_lattice_pbvh_find(Object &ob_mesh)
{
  bke::pbvh::Tree *pbvh = bke::object::pbvh_get(ob_mesh);
  if (pbvh == nullptr || pbvh->type() != bke::pbvh::Type::Mesh) {
    return nullptr;
  }
  return pbvh;
}

const bke::pbvh::Tree *sculpt_lattice_pbvh_find(const Object &ob_mesh)
{
  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(ob_mesh);
  if (pbvh == nullptr || pbvh->type() != bke::pbvh::Type::Mesh) {
    return nullptr;
  }
  return pbvh;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Temp OB_LATTICE lifecycle
 * \{ */

/* Creates a temp OB_LATTICE fitted to the unmasked bbox (+margin) in object-space.
 *
 * ADR-15: the lattice + object are standalone no-main IDs. They never enter #Main, a scene or a
 * view-layer, so they cannot trigger the memfile-undo serialization that caused the previous undo
 * crash, and they do not clutter the scene graph. The object exists purely for the deform math
 * (#BKE_lattice_deform_data_create) and for #object_to_world(); the cage visual is drawn by the
 * #SculptLatticeCage overlay reading the live #Lattice.def. */
static Object *sculpt_lattice_create_temp_object(const int3 resolution)
{
  Lattice *lt = static_cast<Lattice *>(BKE_id_new_nomain(ID_LT, "SculptLatticeTemp"));
  Object *lat_ob = BKE_object_add_only_object(nullptr, ObjectType(OB_LATTICE), "SculptLatticeTemp");
  lat_ob->data = &lt->id;

  /* The transform integration drives the cage through #TransDataExtension::quat, mirroring
   * #TransConvertType_Sculpt. #BKE_object_add_only_object leaves Euler mode, which would make that
   * path write into an unused field. #BKE_object_mat3_to_rot honors this setting, so the fit
   * helper keeps working unchanged. */
  lat_ob->rotmode = ROT_MODE_QUAT;

  /* Resolution. */
  BKE_lattice_resize(lt,
                     max_ii(SCULPT_LATTICE_MIN_RESOLUTION, resolution.x),
                     max_ii(SCULPT_LATTICE_MIN_RESOLUTION, resolution.y),
                     max_ii(SCULPT_LATTICE_MIN_RESOLUTION, resolution.z),
                     lat_ob);

  /* Interpolation: KEY_LINEAR by default (ADR-5). */
  lt->typeu = lt->typev = lt->typew = KEY_LINEAR;

  /* Placement (loc / rot / scale) is set by #sculpt_lattice_fit_temp_to_bounds. */
  return lat_ob;
}

/**
 * Places the temp lattice so that its unit cell encloses \a bounds (given in object-space of
 * \a ob_mesh) plus \a margin, object-aligned with the mesh (ADR-4).
 *
 * The cage is a parent-less no-main object, so its loc/rot/scale *are* its world transform, while
 * `bounds` is mesh-local. All three components must therefore go through the mesh transform:
 * location through the full matrix, orientation from its normalized basis, and size scaled by the
 * mesh's own object scale. Deriving only the location from it — and leaving the size in mesh-local
 * units — makes the cage mismatch the mesh for any object with a non-applied scale, pushing
 * boundary vertices outside the lattice grid where they clamp to the outermost control points.
 */
void sculpt_lattice_fit_temp_to_bounds(Object &lat_ob,
                                       const Object &ob_mesh,
                                       const Bounds<float3> &bounds,
                                       const float margin)
{
  const float4x4 &mesh_to_world = ob_mesh.object_to_world();

  const float3 center_world = math::transform_point(mesh_to_world, bounds.center());
  copy_v3_v3(lat_ob.loc, center_world);

  float3x3 orientation = float3x3(mesh_to_world);
  normalize_m3(orientation.ptr());
  BKE_object_mat3_to_rot(&lat_ob, orientation.ptr(), false);

  /* The control-point grid spans the unit cube [-0.5, 0.5]^3 (see #BKE_lattice_resize), so the
   * object scale is the world-space extent of the region. The margin is documented in mesh units,
   * hence it is added before the mesh scale is applied. */
  const float3 mesh_scale = math::to_scale(mesh_to_world);
  float3 extent = (bounds.size() + 2.0f * margin) * mesh_scale;

  /* A flat region (plane, single vertex) or a degenerate object scale would collapse an axis and
   * make the deform matrix singular. */
  for (const int axis : IndexRange(3)) {
    if (!isfinite(extent[axis]) || extent[axis] <= FLT_EPSILON) {
      extent[axis] = 1.0f;
    }
  }
  copy_v3_v3(lat_ob.scale, extent);

  /* Refresh the runtime transform from the loc/rot/scale just written. The depsgraph only updates
   * the *evaluated* copy's matrix; every reader on the original object (affected region, pick,
   * deform) uses #Object::object_to_world(), which would otherwise stay at the stale
   * creation-time identity until the first #sculpt_lattice_deform_data_rebuild — producing an
   * empty affected region and no live preview. Keep this in sync with that rebuild.
   *
   * ADR-15: the lattice object is no-main and not in any depsgraph, so there is nothing to tag
   * dirty here — #object_to_world() is the single source of truth for every reader. */
  BKE_object_to_mat4(&lat_ob, lat_ob.runtime->object_to_world.ptr());
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Session lifecycle
 * \{ */

bool sculpt_lattice_ensure_session(bContext *C)
{
  Object *ob_mesh = CTX_data_active_object(C);

  if (!ob_mesh || ob_mesh->type != OB_MESH || !(ob_mesh->mode & OB_MODE_SCULPT)) {
    return false;
  }

  SculptSession *ss = ob_mesh->runtime->sculpt_session;
  if (!ss) {
    return false;
  }

  if (ss->lattice_tool_state) {
    /* Already initialized. */
    return true;
  }

  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  if (!depsgraph) {
    return false;
  }

  /* MVP supports only the Mesh PBVH (multires / dynamic topology: phase 3). Refuse early with a
   * clear message instead of failing later in #sculpt_lattice_compute_deform_bounds with a
   * misleading "all vertices masked" report. */
  if (!sculpt_lattice_pbvh_ensure(*depsgraph, *ob_mesh)) {
    BKE_report(CTX_wm_reports(C),
               RPT_ERROR,
               "Sculpt Lattice tool supports only mesh sculpt data, not multiresolution or "
               "dynamic topology");
    return false;
  }

  /* The member initializers hold the defaults (see #SCULPT_LATTICE_STRENGTH_DEFAULT and friends,
   * shared with the RNA property definitions); only the tool settings override them below. */
  LatticeToolData *state = MEM_new<LatticeToolData>(__func__);

  /* Apply RNA from the active tool (if any). */
  bToolRef *tref = WM_toolsystem_ref_from_context(C);
  if (tref) {
    wmOperatorType *ot = WM_operatortype_find("SCULPT_OT_lattice_tool", false);
    PointerRNA props_ptr;
    if (ot && WM_toolsystem_ref_properties_get_from_operator(tref, ot, &props_ptr)) {
      state->strength = RNA_float_get(&props_ptr, "strength");
      state->margin = RNA_float_get(&props_ptr, "margin");
      state->resolution = int3(RNA_int_get(&props_ptr, "resolution_u"),
                               RNA_int_get(&props_ptr, "resolution_v"),
                               RNA_int_get(&props_ptr, "resolution_w"));
      state->mask_eps = RNA_float_get(&props_ptr, "mask_eps");
      state->interpolation = KeyInterpolationType(RNA_enum_get(&props_ptr, "interpolation"));
      /* NOTE: do NOT free `props_ptr`. #WM_toolsystem_ref_properties_get_from_operator returns a
       * discrete pointer onto the tool-ref's *own* stored IDProperty (no copy is made), so
       * #WM_operator_properties_free would destroy the persisted tool settings — after the first
       * session every subsequent read would miss the group and fall back to RNA defaults
       * (interpolation -> KEY_LINEAR, etc.). Other readers (wm_gizmo.cc,
       * view3d_gizmo_preselect_type.cc) likewise never free it. */
    }
  }

  /* Clamp resolution. */
  state->resolution = int3(max_ii(SCULPT_LATTICE_MIN_RESOLUTION, state->resolution.x),
                           max_ii(SCULPT_LATTICE_MIN_RESOLUTION, state->resolution.y),
                           max_ii(SCULPT_LATTICE_MIN_RESOLUTION, state->resolution.z));

  /* Compute unmasked bbox (variant A). */
  std::optional<Bounds<float3>> bounds;
  if (!sculpt_lattice_compute_deform_bounds(*depsgraph, *ob_mesh, state->mask_eps, bounds)) {
    /* Everything is masked. The session still starts, in the placement phase, so the cage can be
     * positioned over an area whose mask the user is about to lift. Entering the deform phase is
     * what refuses in this state, not creating the session. */
    const Span<float3> all_positions = bke::pbvh::vert_positions_eval(*depsgraph, *ob_mesh);
    bounds = bounds::min_max(all_positions);
    if (!bounds.has_value()) {
      MEM_delete(state);
      BKE_report(CTX_wm_reports(C), RPT_ERROR, "Mesh has no vertices");
      return false;
    }
    BKE_report(CTX_wm_reports(C),
               RPT_WARNING,
               "All vertices are masked; the cage was fitted to the whole mesh");
  }

  /* Create temp OB_LATTICE (no-main standalone, ADR-15). */
  Object *lat_ob = sculpt_lattice_create_temp_object(state->resolution);
  sculpt_lattice_fit_temp_to_bounds(*lat_ob, *ob_mesh, *bounds, state->margin);

  /* Apply the interpolation type chosen in the tool settings. #create_temp_object initializes the
   * cage to #KEY_LINEAR (ADR-5 default); honor a user switch to B-Spline here. */
  if (Lattice *lt = id_cast<Lattice *>(lat_ob->data)) {
    lt->typeu = lt->typev = lt->typew = char(state->interpolation);
  }

  state->lattice_ob = lat_ob;
  state->phase = Phase::Placement;

  /* Snapshot entry positions for Cancel (Esc). */
  const Span<float3> positions = bke::pbvh::vert_positions_eval(*depsgraph, *ob_mesh);
  state->entry_positions.reinitialize(positions.size());
  state->entry_positions.as_mutable_span().copy_from(positions);

  ss->lattice_tool_state = state;

  /* Build the affected region against the current mesh positions. This is one of the two points
   * where #sculpt_lattice_build_affected_region runs (see its doc comment): session start, here,
   * and every #Phase::Placement -> #Phase::Deform transition in #sculpt_lattice_enter_deform.
   * Within a #Phase::Deform stretch the lattice cage is the sole deformation accumulator: every
   * drag mutates its control points, and the mesh is always recomputed as `lattice(rest)` from
   * these rest positions, so re-snapshotting `rest_coords` between drags would double-apply every
   * prior drag (the cage still encodes it), which is the runaway the user saw. */
  sculpt_lattice_build_affected_region(*depsgraph, *ob_mesh, *state);

  DEG_id_tag_update(&ob_mesh->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, ob_mesh);
  return true;
}

void sculpt_lattice_session_free(Object *ob_mesh)
{
  if (!ob_mesh || !ob_mesh->runtime->sculpt_session) {
    return;
  }
  SculptSession *ss = ob_mesh->runtime->sculpt_session;
  LatticeToolData *state = ss->lattice_tool_state;
  if (!state) {
    return;
  }

  if (state->deform_data) {
    BKE_lattice_deform_data_destroy(state->deform_data);
    state->deform_data = nullptr;
  }

  if (state->lattice_ob) {
    /* ADR-15: the cage is a standalone no-main Object + Lattice. Free both directly; they are not
     * in #Main, a scene or a view-layer, so there is no Base to unlink and nothing was serialized
     * to memfile-undo (this removes the root cause of the previous undo crash). Detach the data
     * pointer first so freeing the object does not touch the separately-owned Lattice. */
    Lattice *lt = id_cast<Lattice *>(state->lattice_ob->data);
    state->lattice_ob->data = nullptr;
    BKE_id_free(nullptr, state->lattice_ob);
    if (lt) {
      BKE_id_free(nullptr, lt);
    }
    state->lattice_ob = nullptr;
  }

  ss->lattice_slide_active = false;

  MEM_delete(state);
  ss->lattice_tool_state = nullptr;
}

void sculpt_lattice_register()
{
  /* #BKE_sculptsession_free covers mode-exit, object deletion and undo Main teardown. Unlike
   * #filter::Cache or #expand::Cache — which only live for the duration of one blocking modal
   * operator and are always released by it — the lattice session spans several operator
   * invocations (ADR-12), so it can still be live at any of those points. Routing the cleanup
   * through this hook makes #sculpt_lattice_session_free reachable from blenkernel, which cannot
   * call into editors directly. */
  BKE_sculpt_lattice_state_free_cb = sculpt_lattice_session_free;
}

bool placement_active(bContext *C, const Object &ob)
{
  if (C == nullptr || !sculpt_lattice_tool_active_poll(C)) {
    return false;
  }
  if (ob.runtime == nullptr || ob.runtime->sculpt_session == nullptr) {
    return false;
  }
  const LatticeToolData *state = ob.runtime->sculpt_session->lattice_tool_state;
  return state != nullptr && state->phase == Phase::Placement && state->lattice_ob != nullptr;
}

Object *cage_object(const Object &ob)
{
  if (ob.runtime == nullptr || ob.runtime->sculpt_session == nullptr) {
    return nullptr;
  }
  const LatticeToolData *state = ob.runtime->sculpt_session->lattice_tool_state;
  return state != nullptr ? state->lattice_ob : nullptr;
}

void undo_push_begin(const Scene &scene, Object &ob, const char *name)
{
  undo::push_begin_ex(scene, ob, name);
}

void undo_push_end(Object &ob)
{
  undo::push_end(ob);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Screen-space pick
 *
 * Find the BPoint closest to the mouse by Manhattan distance (ROKO metric).
 * Returns -1 if none within PAINT_LATTICE_POINT_PICK_THRESHOLD pixels.
 * \{ */

static int sculpt_lattice_pick_bpoint(const ARegion &region, const float2 mval, Object &lat_ob)
{
  /* ADR-15: the cage is a no-main object that is in no depsgraph, so there is no evaluated copy to
   * look up — #DEG_get_evaluated would hand back this very object. Read the original directly, the
   * same way the overlay and the deform do. */
  const Lattice *lt = BKE_object_get_lattice(&lat_ob);
  if (!lt) {
    return -1;
  }

  int best_index = -1;
  /* Keep the pick radius constant in physical size regardless of display scaling. */
  float best_dist = PAINT_LATTICE_POINT_PICK_THRESHOLD * UI_SCALE_FAC;

  const int point_num = lt->pntsu * lt->pntsv * lt->pntsw;
  const float4x4 lat_to_world = lat_ob.object_to_world();

  for (const int i : IndexRange(point_num)) {
    const float3 world = math::transform_point(lat_to_world, float3(lt->def[i].vec));
    float2 screen;
    ED_view3d_project_v2(&region, world, screen);
    const float dist = math::distance_manhattan(screen, mval);
    if (dist < best_dist) {
      best_dist = dist;
      best_index = i;
    }
  }

  return best_index;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Live resolution change
 * \{ */

/* Defined further down; forward-declared so the resolution update can flush the preview. */
static void sculpt_lattice_preview_flush(bContext *C);

/**
 * Re-applies the cage to the mesh from a tool-settings change, wrapped in its own undo step.
 *
 * The RNA update callbacks below run outside any operator, so nothing opens an undo step for them.
 * #sculpt_lattice_deform_apply writes coordinates straight through #PositionDeformData and pushes
 * no undo itself, so without this the vertices it moves are invisible to the undo system: the top
 * of the stack would keep describing the mesh from before the change, and the next Ctrl+Z would
 * restore that rather than what is on screen. Mirrors the per-drag step #SCULPT_OT_lattice_slide
 * opens for exactly the same reason.
 *
 * Only meaningful in #Phase::Deform — in #Phase::Placement the cage is neutral and moves nothing,
 * so callers skip it there rather than pushing an empty step per slider tick.
 */
static void sculpt_lattice_reapply_with_undo(bContext *C,
                                             Depsgraph &depsgraph,
                                             Object &ob_mesh,
                                             LatticeToolData &state,
                                             const char *undo_name)
{
  const Scene *scene = CTX_data_scene(C);
  bke::pbvh::Tree *pbvh = sculpt_lattice_pbvh_ensure(depsgraph, ob_mesh);
  if (scene == nullptr || pbvh == nullptr) {
    return;
  }

  undo::push_begin_ex(*scene, ob_mesh, undo_name);

  /* Register the position nodes BEFORE the deformation, like #sculpt_lattice_slide_invoke. Pushing
   * all leaf nodes matches the cancel path; it can be narrowed to the affected nodes later if
   * profiling requires it. */
  IndexMaskMemory memory;
  const IndexMask node_mask = bke::pbvh::all_leaf_nodes(*pbvh, memory);
  undo::push_nodes(depsgraph, ob_mesh, node_mask, undo::Type::Position);

  sculpt_lattice_deform_apply(depsgraph, ob_mesh, state);

  undo::push_end(ob_mesh);
}

/**
 * Re-grid the live cage when the user edits the resolution in the tool settings, so control points
 * are added/removed interactively without restarting the session. No-op when there is no active
 * session (the new resolution is then picked up by #sculpt_lattice_ensure_session) or while a slide
 * drag is in progress.
 *
 * The affected region (verts / rest_coords / mask) is intentionally NOT rebuilt: the cage bbox is
 * unchanged by a re-grid, so the same vertices are affected, and #AffectedRegion::current_coords
 * must keep tracking the actual mesh positions (rebuilding it would reset the tracker to rest and
 * double-apply the existing deformation on the next #sculpt_lattice_deform_apply).
 */
static void sculpt_lattice_apply_resolution_change(bContext *C)
{
  Object *ob_mesh = CTX_data_active_object(C);
  if (!ob_mesh || ob_mesh->type != OB_MESH || !(ob_mesh->mode & OB_MODE_SCULPT) ||
      !ob_mesh->runtime->sculpt_session)
  {
    return;
  }
  SculptSession *ss = ob_mesh->runtime->sculpt_session;
  LatticeToolData *state = ss->lattice_tool_state;
  /* Re-gridding under a running slide would pull the cage out from under the drag. */
  if (!state || !state->lattice_ob || ss->lattice_slide_active) {
    return;
  }

  bToolRef *tref = WM_toolsystem_ref_from_context(C);
  if (!tref) {
    return;
  }
  wmOperatorType *ot = WM_operatortype_find("SCULPT_OT_lattice_tool", false);
  PointerRNA props_ptr;
  if (!ot || !WM_toolsystem_ref_properties_get_from_operator(tref, ot, &props_ptr)) {
    return;
  }
  const int3 res = int3(max_ii(SCULPT_LATTICE_MIN_RESOLUTION, RNA_int_get(&props_ptr, "resolution_u")),
                        max_ii(SCULPT_LATTICE_MIN_RESOLUTION, RNA_int_get(&props_ptr, "resolution_v")),
                        max_ii(SCULPT_LATTICE_MIN_RESOLUTION, RNA_int_get(&props_ptr, "resolution_w")));

  Lattice *lt = id_cast<Lattice *>(state->lattice_ob->data);
  if (lt->pntsu == res.x && lt->pntsv == res.y && lt->pntsw == res.z) {
    return;
  }

  /* Use the already-evaluated depsgraph rather than forcing a synchronous re-eval: this runs from
   * inside a UI property update, and the deform reads the live #Lattice.def directly. */
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  if (!depsgraph) {
    return;
  }

  /* Re-grid the cage. With `lt_ob` given, #BKE_lattice_resize reinitializes the control points to
   * the unit grid while approximately preserving the current deformed shape. */
  BKE_lattice_resize(lt, res.x, res.y, res.z, state->lattice_ob);
  state->resolution = res;

  /* #BKE_lattice_resize forces the interpolation to KEY_LINEAR while reinitializing; restore it. */
  lt->typeu = lt->typev = lt->typew = char(state->interpolation);

  /* Rebuild the deform context against the new cage geometry. */
  if (state->deform_data) {
    BKE_lattice_deform_data_destroy(state->deform_data);
    state->deform_data = nullptr;
  }
  state->deform_data = sculpt_lattice_deform_data_rebuild(state->lattice_ob, ob_mesh);

  /* Re-apply so the mesh follows the re-gridded cage immediately. In the placement phase the cage
   * is neutral and this would be a no-op; skip it explicitly rather than leaning on that. */
  if (state->phase == Phase::Deform) {
    sculpt_lattice_reapply_with_undo(C, *depsgraph, *ob_mesh, *state, "Lattice Resolution");
  }

  sculpt_lattice_preview_flush(C);
  /* ADR-15: no-main cage is not in any depsgraph; the #SculptLatticeCage overlay reads the live
   * #Lattice.def each redraw, so a redraw tag alone is enough. */
  ED_region_tag_redraw(CTX_wm_region(C));
}

/* RNA update for #SCULPT_OT_lattice_tool resolution properties (tool settings). */
static void sculpt_lattice_resolution_update(bContext *C, PointerRNA * /*ptr*/, PropertyRNA * /*prop*/)
{
  sculpt_lattice_apply_resolution_change(C);
}

/**
 * Re-applies the live cage interpolation when the user edits it in the tool settings, so switching
 * Linear <-> B-Spline previews immediately instead of only taking effect on the next session.
 *
 * Unlike a resolution change, this needs neither #BKE_lattice_resize nor a deform-context rebuild:
 * #LatticeDeformData keeps a pointer to the cage's own #Lattice (not a copy), and
 * #BKE_lattice_deform_data_eval_co reads `typeu/typev/typew` from it on every call. Writing the new
 * type onto that same struct is therefore enough for the next #sculpt_lattice_deform_apply to pick
 * it up.
 */
static void sculpt_lattice_apply_interpolation_change(bContext *C)
{
  Object *ob_mesh = CTX_data_active_object(C);
  if (!ob_mesh || ob_mesh->type != OB_MESH || !(ob_mesh->mode & OB_MODE_SCULPT) ||
      !ob_mesh->runtime->sculpt_session)
  {
    return;
  }
  SculptSession *ss = ob_mesh->runtime->sculpt_session;
  LatticeToolData *state = ss->lattice_tool_state;
  /* Changing the interpolation mid-drag would re-map the vertices under the cursor. */
  if (!state || !state->lattice_ob || ss->lattice_slide_active) {
    return;
  }

  bToolRef *tref = WM_toolsystem_ref_from_context(C);
  if (!tref) {
    return;
  }
  wmOperatorType *ot = WM_operatortype_find("SCULPT_OT_lattice_tool", false);
  PointerRNA props_ptr;
  if (!ot || !WM_toolsystem_ref_properties_get_from_operator(tref, ot, &props_ptr)) {
    return;
  }
  const KeyInterpolationType interpolation = KeyInterpolationType(
      RNA_enum_get(&props_ptr, "interpolation"));
  if (interpolation == state->interpolation) {
    return;
  }
  state->interpolation = interpolation;

  Lattice *lt = id_cast<Lattice *>(state->lattice_ob->data);
  lt->typeu = lt->typev = lt->typew = char(interpolation);

  /* Re-apply so the mesh follows the new interpolation immediately. In the placement phase the
   * cage is neutral and this would be a no-op; skip it explicitly rather than leaning on that. */
  if (state->phase == Phase::Deform) {
    Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
    if (!depsgraph) {
      return;
    }
    sculpt_lattice_reapply_with_undo(C, *depsgraph, *ob_mesh, *state, "Lattice Interpolation");
  }

  sculpt_lattice_preview_flush(C);
  /* ADR-15: no-main cage is not in any depsgraph; the #SculptLatticeCage overlay reads the live
   * #Lattice.def each redraw, so a redraw tag alone is enough. */
  ED_region_tag_redraw(CTX_wm_region(C));
}

/* RNA update for #SCULPT_OT_lattice_tool interpolation property (tool settings). */
static void sculpt_lattice_interpolation_update(bContext *C,
                                                 PointerRNA * /*ptr*/,
                                                 PropertyRNA * /*prop*/)
{
  sculpt_lattice_apply_interpolation_change(C);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Operators
 * \{ */

/* --- SCULPT_OT_lattice_tool (RNA container) ------------------------- */

static wmOperatorStatus sculpt_lattice_tool_exec(bContext *C, wmOperator * /*op*/)
{
  /* Not invoked on tool select (ADR-12); kept for repeat-last / debug. */
  if (!sculpt_lattice_ensure_session(C)) {
    return OPERATOR_CANCELLED;
  }
  return OPERATOR_FINISHED;
}

static void sculpt_lattice_tool_ui(bContext * /*C*/, wmOperator *op)
{
  ui::Layout &layout = *op->layout;
  layout.prop(op->ptr, "strength", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(op->ptr, "margin", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(op->ptr, "resolution_u", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(op->ptr, "resolution_v", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(op->ptr, "resolution_w", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(op->ptr, "interpolation", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(op->ptr, "mask_eps", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

void SCULPT_OT_lattice_tool(wmOperatorType *ot)
{
  ot->name = "Lattice Tool";
  ot->idname = "SCULPT_OT_lattice_tool";
  ot->description = "Deform an unmasked region of a mesh with a lattice cage";

  ot->exec = sculpt_lattice_tool_exec;
  ot->poll = sculpt_lattice_poll;
  ot->ui = sculpt_lattice_tool_ui;

  ot->flag = OPTYPE_REGISTER;

  RNA_def_float(ot->srna,
                "strength",
                SCULPT_LATTICE_STRENGTH_DEFAULT,
                0.0f,
                1.0f,
                "Strength",
                "Deformation strength",
                0.0f,
                1.0f);
  RNA_def_float(ot->srna,
                "margin",
                SCULPT_LATTICE_MARGIN_DEFAULT,
                0.0f,
                10.0f,
                "Margin",
                "Extra cage margin around the deform region (in mesh units)",
                0.0f,
                2.0f);
  /* The resolution properties re-grid the live cage interactively (add/remove control points)
   * via #sculpt_lattice_resolution_update; see #sculpt_lattice_apply_resolution_change. */
  const char *resolution_names[] = {"resolution_u", "resolution_v", "resolution_w"};
  const char *resolution_labels[] = {"Resolution U", "Resolution V", "Resolution W"};
  for (const int axis : IndexRange(3)) {
    PropertyRNA *prop = RNA_def_int(ot->srna,
                                    resolution_names[axis],
                                    SCULPT_LATTICE_RESOLUTION_DEFAULT,
                                    SCULPT_LATTICE_MIN_RESOLUTION,
                                    SCULPT_LATTICE_MAX_RESOLUTION,
                                    resolution_labels[axis],
                                    "Number of cage control points along this axis",
                                    SCULPT_LATTICE_MIN_RESOLUTION,
                                    32);
    RNA_def_property_update_runtime_with_context_and_property(prop,
                                                             sculpt_lattice_resolution_update);
  }
  RNA_def_float(ot->srna,
                "mask_eps",
                SCULPT_LATTICE_MASK_EPS_DEFAULT,
                0.0f,
                1.0f,
                "Mask Epsilon",
                "Treat vertices with (1 - mask) below this as protected",
                0.0f,
                0.01f);

  /* Values are raw #KeyInterpolationType, written straight into #Lattice.typeu/typev/typew. */
  static const EnumPropertyItem interpolation_items[] = {
      {KEY_LINEAR, "LINEAR", 0, "Linear", "Linear interpolation (recommended)"},
      {KEY_BSPLINE, "BSPLINE", 0, "B-Spline", "Smoother B-spline interpolation"},
      {0, nullptr, 0, nullptr, nullptr},
  };
  PropertyRNA *interpolation_prop = RNA_def_enum(ot->srna,
                                                 "interpolation",
                                                 interpolation_items,
                                                 KEY_LINEAR,
                                                 "Interpolation",
                                                 "Lattice interpolation type");
  RNA_def_property_update_runtime_with_context_and_property(interpolation_prop,
                                                             sculpt_lattice_interpolation_update);
}

/* --- SCULPT_OT_lattice_pick ----------------------------------------- */

static wmOperatorStatus sculpt_lattice_pick_invoke(bContext *C,
                                                   wmOperator * /*op*/,
                                                   const wmEvent *event)
{
  if (!sculpt_lattice_ensure_session(C)) {
    return OPERATOR_CANCELLED;
  }
  Object *ob_mesh = CTX_data_active_object(C);
  if (!ob_mesh || ob_mesh->type != OB_MESH || !ob_mesh->runtime) {
    return OPERATOR_CANCELLED;
  }
  SculptSession *ss = ob_mesh->runtime->sculpt_session;
  if (!ss) {
    return OPERATOR_CANCELLED;
  }
  LatticeToolData *state = ss->lattice_tool_state;
  if (!state || !state->lattice_ob) {
    return OPERATOR_CANCELLED;
  }

  if (state->phase == Phase::Placement) {
    /* In the placement phase LMB draws a new cage instead of picking a control point. */
    return WM_operator_name_call(
        C, "SCULPT_OT_lattice_box_define", wm::OpCallContext::InvokeDefault, nullptr, event);
  }

  ARegion *region = CTX_wm_region(C);
  if (!region) {
    return OPERATOR_CANCELLED;
  }
  const float2 mval = float2(float(event->mval[0]), float(event->mval[1]));

  /* The pick only projects the cage's own control points; it touches neither the mesh nor the
   * depsgraph, so no evaluation is forced here. */
  const int best = sculpt_lattice_pick_bpoint(*region, mval, *state->lattice_ob);
  if (best < 0) {
    return OPERATOR_PASS_THROUGH;
  }

  state->pending_drag_index = best;

  /* MVP: directly invoke the slide (ADR Q3). Phase 2: PASS_THROUGH chain. */
  return WM_operator_name_call(
      C, "SCULPT_OT_lattice_slide", wm::OpCallContext::InvokeDefault, nullptr, event);
}

void SCULPT_OT_lattice_pick(wmOperatorType *ot)
{
  ot->name = "Lattice Pick";
  ot->idname = "SCULPT_OT_lattice_pick";
  ot->description = "Pick a lattice control point and start dragging it";

  ot->invoke = sculpt_lattice_pick_invoke;
  ot->poll = sculpt_lattice_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_DEPENDS_ON_CURSOR;
}

/* --- SCULPT_OT_lattice_slide (modal) -------------------------------- */

/**
 * Push deformed positions to the 3D viewport (same path as Mesh Filter / Transform).
 *
 * #sculpt_lattice_deform_apply writes through #PositionDeformData and tags the affected PBVH
 * nodes, so the fast draw-buffer refresh is sufficient — a full #ID_RECALC_GEOMETRY here would
 * re-evaluate the whole modifier stack on every mouse-move.
 */
static void sculpt_lattice_preview_flush(bContext *C)
{
  flush_update_step(C, UpdateType::Position);
}

static wmOperatorStatus sculpt_lattice_slide_invoke(bContext *C,
                                                    wmOperator *op,
                                                    const wmEvent * /*event*/)
{
  Object *ob_mesh = CTX_data_active_object(C);
  SculptSession *ss = ob_mesh ? ob_mesh->runtime->sculpt_session : nullptr;
  LatticeToolData *state = ss ? ss->lattice_tool_state : nullptr;
  if (!state || !state->lattice_ob || state->pending_drag_index < 0) {
    return OPERATOR_CANCELLED;
  }

  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  Scene *scene = CTX_data_scene(C);
  if (!depsgraph || !scene) {
    return OPERATOR_CANCELLED;
  }
  bke::pbvh::Tree *pbvh = sculpt_lattice_pbvh_ensure(*depsgraph, *ob_mesh);
  if (!pbvh) {
    return OPERATOR_CANCELLED;
  }

  /* NOTE: the affected region (rest_coords / verts / mask) is built ONCE in
   * #sculpt_lattice_ensure_session against the original positions and is NOT rebuilt here. The
   * cage is the single deformation accumulator; rebuilding rest against the deformed mesh each
   * drag would double-apply prior drags. Only the deform context (cage geometry) is rebuilt. */

  /* The session outlives individual operators (ADR-12), so the mesh may have been changed by
   * something else since the previous drag. Re-seed the incremental tracker, and drop the session
   * outright if the vertex count no longer matches — `verts` and `rest_coords` index into it. */
  if (!sculpt_lattice_sync_tracker_to_mesh(*depsgraph, *ob_mesh, *state)) {
    BKE_report(CTX_wm_reports(C),
               RPT_WARNING,
               "Mesh topology changed; the lattice session has been reset");
    sculpt_lattice_session_free(ob_mesh);
    ED_region_tag_redraw(CTX_wm_region(C));
    return OPERATOR_CANCELLED;
  }

  /* (Re)build deform context. */
  if (state->deform_data) {
    BKE_lattice_deform_data_destroy(state->deform_data);
  }
  state->deform_data = sculpt_lattice_deform_data_rebuild(state->lattice_ob, ob_mesh);

  /* Per-drag undo step (like Mesh Filter). */
  undo::push_begin(*scene, *ob_mesh, op);

  /* Register position undo nodes BEFORE any deformation: #PositionDeformData::deform writes
   * coordinates directly and does not push undo itself, so without this the per-drag undo step
   * would be empty and Ctrl+Z could not revert the deformation. Mirrors Mesh Filter's
   * `cache_init` (sculpt_filter_mesh.cc). Pushing all leaf nodes is correct and matches the
   * cancel path; it can be narrowed to the affected nodes later if profiling requires it. */
  {
    IndexMaskMemory memory;
    const IndexMask node_mask = bke::pbvh::all_leaf_nodes(*pbvh, memory);
    undo::push_nodes(*depsgraph, *ob_mesh, node_mask, undo::Type::Position);
  }

  /* Snapshot the control point so the drag can be rolled back (see
   * #sculpt_lattice_slide_drag_cancel). */
  const Lattice *lt = id_cast<const Lattice *>(state->lattice_ob->data);
  state->drag_start_point = float3(lt->def[state->pending_drag_index].vec);

  ss->lattice_slide_active = true;

  WM_event_add_modal_handler(C, op);
  ED_region_tag_redraw(CTX_wm_region(C));

  return OPERATOR_RUNNING_MODAL;
}

static void sculpt_lattice_slide_update_point(bContext *C,
                                              Object &ob_mesh,
                                              LatticeToolData &state,
                                              const wmEvent *event)
{
  if (!state.lattice_ob || state.pending_drag_index < 0) {
    return;
  }
  ARegion *region = CTX_wm_region(C);
  RegionView3D *rv3d = CTX_wm_region_view3d(C);
  /* Use the already-evaluated depsgraph (like Mesh Filter's modal), NOT
   * #CTX_data_ensure_evaluated_depsgraph: the latter forces a full synchronous graph
   * re-evaluation every MOUSEMOVE. That mid-modal re-eval thrashes the sculpt mesh / PBVH and
   * breaks the live preview. The deform reads the live #Lattice.def directly (see
   * #sculpt_lattice_deform_data_rebuild), so it does not need the evaluated cage at all during
   * the drag; the cage is re-evaluated once on drag release instead. */
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  if (!region || !rv3d || !depsgraph) {
    return;
  }

  Object *lat_ob = state.lattice_ob;
  Lattice *lt = id_cast<Lattice *>(lat_ob->data);

  /* The BPoint under the cursor — move it on a view-aligned plane through its
   * original world position (simpler than ROKO; ADR: drag plane = view-aligned). */
  BPoint &bp = lt->def[state.pending_drag_index];
  const float3 bp_world_orig = math::transform_point(lat_ob->object_to_world(), float3(bp.vec));

  float2 mval_fl(float(event->mval[0]), float(event->mval[1]));
  float3 new_world;
  ED_view3d_win_to_3d(CTX_wm_view3d(C), region, bp_world_orig, mval_fl, new_world);

  /* Convert world back to lattice data-space (edit original ID, not evaluated copy). */
  float4x4 world_to_lat;
  invert_m4_m4(world_to_lat.ptr(), lat_ob->object_to_world().ptr());
  const float3 new_local = math::transform_point(world_to_lat, new_world);

  copy_v3_v3(bp.vec, new_local);
  /* ADR-15: the cage is a no-main object drawn by the #SculptLatticeCage overlay, which reads the
   * live #Lattice.def every redraw. The standard lattice batch-cache is unused for it, and there
   * is no evaluated copy to tag dirty — the #ED_region_tag_redraw from #sculpt_lattice_preview_flush
   * is what makes the new control-point position appear. (Tagging a depsgraph object dirty here
   * would also discard the fast PBVH draw-buffer refresh #flush_update_step just did on the mesh.) */

  /* Rebuild deform context (cage changed). */
  if (state.deform_data) {
    BKE_lattice_deform_data_destroy(state.deform_data);
  }
  state.deform_data = sculpt_lattice_deform_data_rebuild(lat_ob, &ob_mesh);

  if (!sculpt_lattice_pbvh_ensure(*depsgraph, ob_mesh)) {
    return;
  }
  sculpt_lattice_deform_apply(*depsgraph, ob_mesh, state);
  sculpt_lattice_preview_flush(C);
}

/**
 * Closes the per-drag undo step and clears the in-progress state.
 *
 * #SculptSession::lattice_slide_active suppresses the PBVH free in
 * #BKE_sculpt_update_object_before_eval, so leaving it set would keep that suppression alive for
 * the rest of the sculpt session. Every exit from the modal — release, Esc, and the window
 * manager's forced #wmOperatorType::cancel — must come through here.
 */
static void sculpt_lattice_slide_drag_end(bContext *C, Object &ob_mesh, LatticeToolData &state)
{
  undo::push_end(ob_mesh);
  state.pending_drag_index = -1;
  if (SculptSession *ss = ob_mesh.runtime->sculpt_session) {
    ss->lattice_slide_active = false;
  }
  /* ADR-15: nothing to re-evaluate for the no-main cage — the #SculptLatticeCage overlay
   * already follows the live control points each redraw. */
  flush_update_done(C, ob_mesh, UpdateType::Position);
  ED_region_tag_redraw(CTX_wm_region(C));
}

/**
 * Rolls the current drag back: returns the affected vertices to their rest positions AND the
 * dragged control point to where it was when the drag started.
 *
 * Both halves are required. The cage is the absolute accumulator for the whole session, so a
 * control point left at its cancelled position would be re-applied by the next drag even though
 * the mesh was restored here.
 */
static void sculpt_lattice_slide_drag_cancel(bContext *C, Object &ob_mesh, LatticeToolData &state)
{
  if (state.lattice_ob && state.pending_drag_index >= 0) {
    Lattice *lt = id_cast<Lattice *>(state.lattice_ob->data);
    copy_v3_v3(lt->def[state.pending_drag_index].vec, state.drag_start_point);
  }

  if (Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C)) {
    /* Ahead of the restore, not after it: any geometry re-evaluation since this modal started may
     * have freed the PBVH that #PositionDeformData and #sculpt_lattice_tag_affected_nodes read.
     * This is the same order #sculpt_lattice_slide_update_point uses for the same reason. */
    AffectedRegion &ar = state.current;
    if (sculpt_lattice_pbvh_ensure(*depsgraph, ob_mesh) && !ar.verts.is_empty()) {
      const PositionDeformData position_data(*depsgraph, ob_mesh);
      Vector<float3> translations(ar.verts.size());
      for (const int i : ar.verts.index_range()) {
        /* Move from the last position this tool wrote back to rest (see
         * #AffectedRegion::current_coords). */
        translations[i] = ar.rest_coords[i] - ar.current_coords[i];
        ar.current_coords[i] = ar.rest_coords[i];
      }
      position_data.deform(translations, ar.verts);
      sculpt_lattice_tag_affected_nodes(*depsgraph, ob_mesh, ar);
    }
    flush_update_step(C, UpdateType::Position);
  }

  sculpt_lattice_slide_drag_end(C, ob_mesh, state);
}

static wmOperatorStatus sculpt_lattice_slide_modal(bContext *C,
                                                   wmOperator * /*op*/,
                                                   const wmEvent *event)
{
  Object *ob_mesh = CTX_data_active_object(C);
  SculptSession *ss = ob_mesh ? ob_mesh->runtime->sculpt_session : nullptr;
  LatticeToolData *state = ss ? ss->lattice_tool_state : nullptr;
  if (!state) {
    return OPERATOR_CANCELLED;
  }

  switch (event->type) {
    case MOUSEMOVE: {
      sculpt_lattice_slide_update_point(C, *ob_mesh, *state, event);
      return OPERATOR_RUNNING_MODAL;
    }
    case LEFTMOUSE: {
      if (event->val == KM_RELEASE) {
        /* End drag: one undo step per drag (ADR-7). RMB does NOT end the drag. */
        sculpt_lattice_slide_drag_end(C, *ob_mesh, *state);
        sculpt_lattice_status_idle(C);
        return OPERATOR_FINISHED;
      }
      return OPERATOR_RUNNING_MODAL;
    }
    case EVT_ESCKEY: {
      sculpt_lattice_slide_drag_cancel(C, *ob_mesh, *state);
      sculpt_lattice_status_idle(C);
      return OPERATOR_CANCELLED;
    }
    default:
      return OPERATOR_RUNNING_MODAL;
  }
}

/* Forced termination by the window manager (file load, area change, another operator taking over
 * the modal handlers). Without this the drag would keep #SculptSession::lattice_slide_active set
 * and leave the per-drag undo step open. */
static void sculpt_lattice_slide_cancel(bContext *C, wmOperator * /*op*/)
{
  Object *ob_mesh = CTX_data_active_object(C);
  SculptSession *ss = ob_mesh ? ob_mesh->runtime->sculpt_session : nullptr;
  LatticeToolData *state = ss ? ss->lattice_tool_state : nullptr;
  if (!state || !ss->lattice_slide_active) {
    return;
  }
  sculpt_lattice_slide_drag_cancel(C, *ob_mesh, *state);
}

void SCULPT_OT_lattice_slide(wmOperatorType *ot)
{
  ot->name = "Lattice Slide";
  ot->idname = "SCULPT_OT_lattice_slide";
  ot->description = "Drag a picked lattice control point to deform the mesh";

  ot->invoke = sculpt_lattice_slide_invoke;
  ot->modal = sculpt_lattice_slide_modal;
  ot->cancel = sculpt_lattice_slide_cancel;
  ot->poll = sculpt_lattice_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING | OPTYPE_GRAB_CURSOR_X |
             OPTYPE_DEPENDS_ON_CURSOR;
}

/* --- SCULPT_OT_lattice_confirm (Enter) ------------------------------ */

static wmOperatorStatus sculpt_lattice_confirm_exec(bContext *C, wmOperator * /*op*/)
{
  Object *ob_mesh = CTX_data_active_object(C);
  if (!ob_mesh || !ob_mesh->runtime->sculpt_session ||
      !ob_mesh->runtime->sculpt_session->lattice_tool_state)
  {
    return OPERATOR_CANCELLED;
  }

  sculpt_lattice_session_free(ob_mesh);
  ED_workspace_status_text(C, nullptr);
  ED_region_tag_redraw(CTX_wm_region(C));
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, ob_mesh);
  return OPERATOR_FINISHED;
}

void SCULPT_OT_lattice_confirm(wmOperatorType *ot)
{
  ot->name = "Lattice Confirm";
  ot->idname = "SCULPT_OT_lattice_confirm";
  ot->description = "Apply the lattice deformation and finish the session";

  ot->exec = sculpt_lattice_confirm_exec;
  ot->poll = sculpt_lattice_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/* --- SCULPT_OT_lattice_cancel (Esc) --------------------------------- */

static wmOperatorStatus sculpt_lattice_cancel_exec(bContext *C, wmOperator *op)
{
  Object *ob_mesh = CTX_data_active_object(C);
  if (!ob_mesh || !ob_mesh->runtime->sculpt_session ||
      !ob_mesh->runtime->sculpt_session->lattice_tool_state)
  {
    return OPERATOR_CANCELLED;
  }
  LatticeToolData *state = ob_mesh->runtime->sculpt_session->lattice_tool_state;

  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  Scene *scene = CTX_data_scene(C);
  bke::pbvh::Tree *pbvh = (depsgraph && scene) ? sculpt_lattice_pbvh_ensure(*depsgraph, *ob_mesh) :
                                                 nullptr;

  /* A missing PBVH means there is nothing left to restore into, but the session must still go —
   * dropping it is the whole point of this operator, and its operators are the only way to reach
   * it. */
  if (pbvh) {
    /* Restore every position from the entry snapshot as its own undo step. Each drag already
     * pushed one, so writing the positions outside the undo system would leave the top of the
     * stack describing a mid-session state that is no longer on screen, and the next Ctrl+Z would
     * restore that instead of the pre-session mesh. */
    undo::push_begin(*scene, *ob_mesh, op);

    IndexMaskMemory memory;
    const IndexMask all = bke::pbvh::all_leaf_nodes(*pbvh, memory);
    undo::push_nodes(*depsgraph, *ob_mesh, all, undo::Type::Position);

    MutableSpan<float3> positions = bke::pbvh::vert_positions_eval_for_write(*depsgraph, *ob_mesh);
    if (!state->entry_positions.is_empty() && positions.size() == state->entry_positions.size()) {
      positions.copy_from(state->entry_positions);
      pbvh->tag_positions_changed(all);
    }

    undo::push_end(*ob_mesh);
  }

  sculpt_lattice_session_free(ob_mesh);
  ED_workspace_status_text(C, nullptr);
  ED_region_tag_redraw(CTX_wm_region(C));
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, ob_mesh);
  return OPERATOR_FINISHED;
}

void SCULPT_OT_lattice_cancel(wmOperatorType *ot)
{
  ot->name = "Lattice Cancel";
  ot->idname = "SCULPT_OT_lattice_cancel";
  ot->description = "Cancel the lattice session and restore the mesh";

  ot->exec = sculpt_lattice_cancel_exec;
  ot->poll = sculpt_lattice_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

}  // namespace blender::ed::sculpt_paint::lattice
