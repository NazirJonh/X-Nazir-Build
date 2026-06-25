/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Sculpt Lattice Tool: operators and lifecycle.
 *
 * State machine (see 02_ux_state_machine.md):
 *   ToolSelected --first LMB--> Placing (ensure_session) --> Editing
 *   Editing --LMB on BPoint--> DragPoint --LMB release--> Editing  (per-drag undo step)
 *   Editing --Enter--> Confirmed (apply-once, session_free)
 *   Editing --Esc-->  Cancelled (restore entry_positions, session_free)
 *
 * ADR-12: lazy init — selection of the tool in toolbar does NOT call an operator.
 */

#include "sculpt_lattice.hh"
#include "sculpt_lattice_intern.hh"

#include <cstdio> /* TEMP DEBUG: live-preview diagnostics, remove once resolved. */

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
#include "BLI_utildefines.hh"
#include "BLI_vector.hh"

#include "BLT_translation.hh"

#include "DNA_lattice_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

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

/** BKE_object_add_for_data selects the new object; keep the sculpt mesh active. */
static void sculpt_lattice_mesh_set_active(Main &bmain, Scene &scene, ViewLayer &view_layer, Object &ob_mesh)
{
  BKE_view_layer_synced_ensure(bmain, &scene, &view_layer);
  Base *mesh_base = BKE_view_layer_base_find(&view_layer, &ob_mesh);
  if (mesh_base) {
    BKE_view_layer_base_select_and_set_active(&view_layer, mesh_base);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Temp OB_LATTICE lifecycle
 * \{ */

/* Creates a temp OB_LATTICE fitted to the unmasked bbox (+margin) in object-space.
 * Mirrors object_add.cc `lattice_add_to_selected_exec` but does not add modifiers. */
static Object *sculpt_lattice_create_temp_object(Main &bmain,
                                                  Scene &scene,
                                                  ViewLayer &view_layer,
                                                  Object &ob_mesh,
                                                  const int3 resolution)
{
  BKE_view_layer_synced_ensure(bmain, &scene, &view_layer);

  Lattice *lt = BKE_lattice_add(&bmain, "SculptLatticeTemp");
  Object *lat_ob = BKE_object_add_for_data(&bmain,
                                           &scene,
                                           &view_layer,
                                           ObjectType(OB_LATTICE),
                                           "SculptLatticeTemp",
                                           &lt->id,
                                           true);

  /* Resolution. */
  BKE_lattice_resize(lt,
                     max_ii(SCULPT_LATTICE_MIN_RESOLUTION, resolution.x),
                     max_ii(SCULPT_LATTICE_MIN_RESOLUTION, resolution.y),
                     max_ii(SCULPT_LATTICE_MIN_RESOLUTION, resolution.z),
                     lat_ob);

  /* Interpolation: KEY_LINEAR by default (ADR-5). */
  lt->typeu = lt->typev = lt->typew = KEY_LINEAR;

  /* Match the mesh object orientation (object-aligned, ADR-4). */
  float3x3 orientation;
  copy_m3_m4(orientation.ptr(), ob_mesh.object_to_world().ptr());
  normalize_m3(orientation.ptr());
  BKE_object_mat3_to_rot(lat_ob, orientation.ptr(), false);

  /* Center the cage at the mesh origin; scale set per-bounds in ensure_session. */
  zero_v3(lat_ob->loc);
  copy_v3_fl(lat_ob->scale, 1.0f);

  /* Do not set OB_HIDE_VIEWPORT: would suppress the `Lattices` overlay (Q5).
   * Draw the cage in front so it stays visible over the mesh. */
  lat_ob->dtx |= OB_DRAW_IN_FRONT;
  lat_ob->visibility_flag |= OB_HIDE_SELECT;

  sculpt_lattice_mesh_set_active(bmain, scene, view_layer, ob_mesh);

  DEG_id_tag_update(&lat_ob->id, ID_RECALC_GEOMETRY | ID_RECALC_TRANSFORM);
  DEG_relations_tag_update(&bmain);

  return lat_ob;
}

/* Fits the temp lattice (loc/scale in object-space of ob_mesh) to the given bbox + margin.
 * The mesh's object_to_world is identity in sculpt (we work in mesh-local), so the lattice
 * object is placed directly in mesh-local coords via matching transform. */
static void sculpt_lattice_fit_temp_to_bounds(Object &lat_ob,
                                              Object &ob_mesh,
                                              const Bounds<float3> &bounds,
                                              const float margin)
{
  const float3 center_local = bounds.center();
  const float3 size = bounds.size();

  /* Move the lattice to mesh local space: place at center, scale by size. */
  /* The mesh object_to_world must be applied to the lattice placement. */
  float3 center_world = center_local;
  mul_m4_v3(ob_mesh.object_to_world().ptr(), center_world);

  copy_v3_v3(lat_ob.loc, center_world);

  /* Lattice resolution grid is in unit cube [-0.5, 0.5]^3 (after BKE_lattice_resize), so the
   * lattice object scale must equal the bbox size (+ margin) to enclose the region. */
  const float3 scaled = size + float3(margin * 2.0f);
  copy_v3_v3(lat_ob.scale, scaled);

  /* Prevent invalid / zero scale. */
  for (int i = 0; i < 3; i++) {
    if (!isfinite(lat_ob.scale[i]) || lat_ob.scale[i] <= FLT_EPSILON) {
      lat_ob.scale[i] = 1.0f;
    }
  }

  /* Refresh the runtime transform from the loc/scale just written. The depsgraph only updates
   * the *evaluated* copy's matrix; every reader on the original object (affected region, pick,
   * deform) uses #Object::object_to_world(), which would otherwise stay at the stale
   * creation-time identity until the first #sculpt_lattice_deform_data_rebuild — producing an
   * empty affected region and no live preview. Keep this in sync with that rebuild. */
  BKE_object_to_mat4(&lat_ob, lat_ob.runtime->object_to_world.ptr());

  DEG_id_tag_update(&lat_ob.id, ID_RECALC_GEOMETRY | ID_RECALC_TRANSFORM);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Session lifecycle
 * \{ */

bool sculpt_lattice_ensure_session(bContext *C)
{
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  Object *ob_mesh = CTX_data_active_object(C);

  if (!ob_mesh || ob_mesh->type != OB_MESH || !(ob_mesh->mode & OB_MODE_SCULPT)) {
    return false;
  }

  SculptSession *ss = ob_mesh->runtime->sculpt_session;
  if (!ss) {
    return false;
  }

  if (ss->lattice_tool_state) {
    /* Already initialised. */
    return true;
  }

  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  BKE_sculpt_update_object_for_edit(depsgraph, ob_mesh, false);

  /* MVP supports only the Mesh PBVH (multires / dynamic topology: phase 3). Refuse early with a
   * clear message instead of failing later in #sculpt_lattice_compute_deform_bounds with a
   * misleading "all vertices masked" report. */
  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(*ob_mesh);
  if (!pbvh || pbvh->type() != bke::pbvh::Type::Mesh) {
    BKE_report(CTX_wm_reports(C),
               RPT_ERROR,
               "Sculpt Lattice tool supports only mesh sculpt data, not multiresolution or "
               "dynamic topology");
    return false;
  }

  LatticeToolData *state = MEM_new<LatticeToolData>(__func__);
  state->resolution = int3(3, 3, 3);
  state->strength = 1.0f;
  state->margin = 0.1f;
  state->mask_eps = SCULPT_LATTICE_MASK_EPS_DEFAULT;

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
      state->interpolation = short(RNA_enum_get(&props_ptr, "interpolation"));
      state->keep_as_modifier = RNA_boolean_get(&props_ptr, "keep_as_modifier");
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
    MEM_delete(state);
    BKE_reportf(
        CTX_wm_reports(C), RPT_ERROR, "All vertices are masked; nothing to deform");
    return false;
  }

  /* Create temp OB_LATTICE. */
  Object *lat_ob = sculpt_lattice_create_temp_object(
      *bmain, *scene, *view_layer, *ob_mesh, state->resolution);
  sculpt_lattice_fit_temp_to_bounds(*lat_ob, *ob_mesh, *bounds, state->margin);

  /* Apply the interpolation type chosen in the tool settings. #create_temp_object initialises
   * the cage to KEY_LINEAR (ADR-5 default); honour a user switch to B-Spline here. The RNA enum
   * stores raw #KeyInterpolationType values (KEY_LINEAR = 0, KEY_BSPLINE = 2). */
  if (Lattice *lt = id_cast<Lattice *>(lat_ob->data)) {
    lt->typeu = lt->typev = lt->typew = char(state->interpolation);
  }

  state->lattice_ob = lat_ob;
  state->phase = Phase::Editing;

  /* Snapshot entry positions for Cancel (Esc). */
  const Span<float3> positions = bke::pbvh::vert_positions_eval(*depsgraph, *ob_mesh);
  state->entry_positions.reinitialize(positions.size());
  state->entry_positions.as_mutable_span().copy_from(positions);

  ss->lattice_tool_state = state;

  /* Build the affected region ONCE, against the original (undeformed) positions. The lattice cage
   * is the single deformation accumulator: every drag mutates the cage's control points (which are
   * never reset), and the mesh is always recomputed as `lattice(rest)` from these original rest
   * positions. Re-snapshotting `rest_coords` per drag against the already-deformed mesh would
   * double-apply every prior drag (the cage still encodes it), which is the runaway the user saw. */
  sculpt_lattice_build_affected_region(*depsgraph, *ob_mesh, *state);

  DEG_id_tag_update(&ob_mesh->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, ob_mesh);
  return true;
}

void sculpt_lattice_session_free(Main *bmain, Scene *scene, Object *ob_mesh)
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
    if (bmain && scene) {
      /* Full removal: removes Base from view_layer + collections + Object + data. */
      object::base_free_and_unlink(bmain, scene, state->lattice_ob);
    }
    /* When scene is nullptr (BKE_sculptsession_free / Main teardown on undo) the temp
     * OB_LATTICE is freed with the rest of Main — do not BKE_id_free_us here: the pointer
     * may already be stale or partially destroyed (see log_hu.md). */
    state->lattice_ob = nullptr;
    if (bmain && scene) {
      DEG_relations_tag_update(bmain);
    }
  }

  ss->lattice_slide_active = false;

  MEM_delete(state);
  ss->lattice_tool_state = nullptr;

  if (bmain && scene) {
    DEG_id_tag_update(&ob_mesh->id, ID_RECALC_GEOMETRY);
  }
}

/* Callback registered into #BKE_sculpt_lattice_state_free_cb. Called from
 * #BKE_sculptsession_free (mode-exit / object deletion / undo Main teardown).
 * Searches G_MAIN for a scene that contains `ob` so that #base_free_and_unlink can properly
 * remove the temp OB_LATTICE from the scene graph.  When no scene is found (e.g. `ob` is from
 * old_bmain during undo teardown and is not in G_MAIN's scene list) the lattice object is freed
 * together with old_bmain — skipping #base_free_and_unlink is safe in that path. */
static void sculpt_lattice_state_free_cb(Object *ob)
{
  if (!ob || !ob->runtime->sculpt_session || !ob->runtime->sculpt_session->lattice_tool_state) {
    return;
  }
  Scene *scene = nullptr;
  for (Scene &sc : G_MAIN->scenes) {
    if (BKE_scene_object_find(*G_MAIN, &sc, ob)) {
      scene = &sc;
      break;
    }
  }
  sculpt_lattice_session_free(G_MAIN, scene, ob);
}

void sculpt_lattice_register()
{
  static bool registered = false;
  if (registered) {
    return;
  }
  registered = true;
  BKE_sculpt_lattice_state_free_cb = sculpt_lattice_state_free_cb;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Screen-space pick
 *
 * Find the BPoint closest to the mouse by Manhattan distance (ROKO metric).
 * Returns -1 if none within PAINT_LATTICE_POINT_PICK_THRESHOLD pixels.
 * \{ */

static int sculpt_lattice_pick_bpoint(const Depsgraph &depsgraph,
                                      const ARegion &region,
                                      const float2 mval,
                                      Object &lat_ob)
{
  Object *lat_eval = DEG_get_evaluated(&depsgraph, &lat_ob);
  if (!lat_eval) {
    lat_eval = &lat_ob;
  }
  const Lattice *lt = BKE_object_get_lattice(lat_eval);
  if (!lt) {
    return -1;
  }

  int best_index = -1;
  float best_dist = PAINT_LATTICE_POINT_PICK_THRESHOLD;

  const int pntsu = lt->pntsu;
  const int pntsv = lt->pntsv;
  const int pntsw = lt->pntsw;
  const float4x4 lat_to_world = lat_eval->object_to_world();

  int idx = 0;
  for (int w = 0; w < pntsw; w++) {
    for (int v = 0; v < pntsv; v++) {
      for (int u = 0; u < pntsu; u++, idx++) {
        const float3 world = math::transform_point(lat_to_world, float3(lt->def[idx].vec));
        float2 screen;
        ED_view3d_project_v2(&region, world, screen);
        const float dist = math::distance_manhattan(screen, mval);
        if (dist < best_dist) {
          best_dist = dist;
          best_index = idx;
        }
      }
    }
  }

  return best_index;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Live resolution change
 * \{ */

/* Defined further down; forward-declared so the resolution update can flush the preview. */
static void sculpt_lattice_preview_flush(bContext *C, Object &ob_mesh);

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
  if (!state || !state->lattice_ob || state->drag_active) {
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
  BKE_sculpt_update_object_for_edit(depsgraph, ob_mesh, false);

  /* Re-grid the cage. With `lt_ob` given, #BKE_lattice_resize reinitialises the control points to
   * the unit grid while approximately preserving the current deformed shape. */
  BKE_lattice_resize(lt, res.x, res.y, res.z, state->lattice_ob);
  state->resolution = res;

  /* #BKE_lattice_resize forces the interpolation to KEY_LINEAR while reinitialising; restore it. */
  lt->typeu = lt->typev = lt->typew = char(state->interpolation);

  /* Rebuild the deform context against the new cage geometry. */
  if (state->deform_data) {
    BKE_lattice_deform_data_destroy(state->deform_data);
    state->deform_data = nullptr;
  }
  state->deform_data = sculpt_lattice_deform_data_rebuild(state->lattice_ob, ob_mesh);

  /* Re-apply so the mesh follows the re-gridded cage immediately (a no-op when undeformed). */
  sculpt_lattice_deform_apply(*depsgraph, *ob_mesh, *state);

  sculpt_lattice_preview_flush(C, *ob_mesh);
  DEG_id_tag_update(&state->lattice_ob->id, ID_RECALC_GEOMETRY);
  ED_region_tag_redraw(CTX_wm_region(C));
}

/* RNA update for #SCULPT_OT_lattice_tool resolution properties (tool settings). */
static void sculpt_lattice_resolution_update(bContext *C, PointerRNA * /*ptr*/, PropertyRNA * /*prop*/)
{
  sculpt_lattice_apply_resolution_change(C);
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

  RNA_def_float(ot->srna, "strength", 1.0f, 0.0f, 1.0f, "Strength", "Deformation strength", 0.0f, 1.0f);
  RNA_def_float(ot->srna,
                "margin",
                0.1f,
                0.0f,
                10.0f,
                "Margin",
                "Extra cage margin around the deform region (in mesh units)",
                0.0f,
                2.0f);
  /* The resolution properties re-grid the live cage interactively (add/remove control points)
   * via #sculpt_lattice_resolution_update; see #sculpt_lattice_apply_resolution_change. */
  PropertyRNA *prop;
  prop = RNA_def_int(
      ot->srna, "resolution_u", 3, SCULPT_LATTICE_MIN_RESOLUTION, 64, "Resolution U", "", SCULPT_LATTICE_MIN_RESOLUTION, 32);
  RNA_def_property_update_runtime_with_context_and_property(prop, sculpt_lattice_resolution_update);
  prop = RNA_def_int(
      ot->srna, "resolution_v", 3, SCULPT_LATTICE_MIN_RESOLUTION, 64, "Resolution V", "", SCULPT_LATTICE_MIN_RESOLUTION, 32);
  RNA_def_property_update_runtime_with_context_and_property(prop, sculpt_lattice_resolution_update);
  prop = RNA_def_int(
      ot->srna, "resolution_w", 3, SCULPT_LATTICE_MIN_RESOLUTION, 64, "Resolution W", "", SCULPT_LATTICE_MIN_RESOLUTION, 32);
  RNA_def_property_update_runtime_with_context_and_property(prop, sculpt_lattice_resolution_update);
  RNA_def_float(ot->srna,
                "mask_eps",
                SCULPT_LATTICE_MASK_EPS_DEFAULT,
                0.0f,
                1.0f,
                "Mask Epsilon",
                "Treat vertices with (1 - mask) below this as protected",
                0.0f,
                0.01f);

  static EnumPropertyItem interpolation_items[] = {
      {0, "LINEAR", 0, "Linear", "Linear interpolation (recommended)"},
      {2, "BSPLINE", 0, "B-Spline", "Smoother B-spline interpolation"},
      {0, nullptr, 0, nullptr, nullptr},
  };
  RNA_def_enum(ot->srna,
               "interpolation",
               interpolation_items,
               0,
               "Interpolation",
               "Lattice interpolation type");
  RNA_def_boolean(ot->srna, "keep_as_modifier", false, "Keep as Modifier", "On confirm, keep the lattice as a modifier (phase 3)");
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

  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  ARegion *region = CTX_wm_region(C);
  const float2 mval = float2(float(event->mval[0]), float(event->mval[1]));

  const int best = sculpt_lattice_pick_bpoint(*depsgraph, *region, mval, *state->lattice_ob);
  printf("[lattice] pick: best_bpoint=%d (mval=%d,%d)\n", best, event->mval[0], event->mval[1]);
  fflush(stdout);
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

/** Push deformed positions to the 3D viewport (same path as Mesh Filter / Transform). */
static void sculpt_lattice_preview_flush(bContext *C, Object &ob_mesh)
{
  /* TEMP DEBUG: which draw path is taken, and is the position tag reachable? */
  const RegionView3D *rv3d = CTX_wm_region_view3d(C);
  const SculptSession *ss = ob_mesh.runtime->sculpt_session;
  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(ob_mesh);
  printf("[lattice] flush: use_pbvh_draw=%d shapekey=%d deform_mods=%d pbvh_type=%d active_is_mesh=%d\n",
         int(BKE_sculptsession_use_pbvh_draw(&ob_mesh, rv3d)),
         ss ? int(ss->shapekey_active != nullptr) : -1,
         ss ? int(ss->deform_modifiers_active) : -1,
         pbvh ? int(pbvh->type()) : -1,
         int(CTX_data_active_object(C) == &ob_mesh));
  fflush(stdout);

  /* DIAGNOSTIC / STOPGAP: force a full geometry recalc so the deformation becomes visible
   * during the drag (same effect as the confirm step). This proves the drawn PBVH is detached
   * from `mesh.position` writes — the proper fix is to stop adding the cage as a scene object,
   * which triggers the depsgraph relations rebuild that detaches it. Slower than the fast PBVH
   * path; will be removed once the scene-less cage lands. */
  DEG_id_tag_update(&ob_mesh.id, ID_RECALC_GEOMETRY);

  flush_update_step(C, UpdateType::Position);
}

static wmOperatorStatus sculpt_lattice_slide_invoke(bContext *C,
                                                    wmOperator *op,
                                                    const wmEvent * /*event*/)
{
  Object *ob_mesh = CTX_data_active_object(C);
  SculptSession *ss = ob_mesh->runtime->sculpt_session;
  LatticeToolData *state = ss ? ss->lattice_tool_state : nullptr;
  if (!state || !state->lattice_ob || state->pending_drag_index < 0) {
    return OPERATOR_CANCELLED;
  }

  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  BKE_sculpt_update_object_for_edit(depsgraph, ob_mesh, false);

  Scene *scene = CTX_data_scene(C);

  /* NOTE: the affected region (rest_coords / verts / mask) is built ONCE in
   * #sculpt_lattice_ensure_session against the original positions and is NOT rebuilt here. The
   * cage is the single deformation accumulator; rebuilding rest against the deformed mesh each
   * drag would double-apply prior drags. Only the deform context (cage geometry) is rebuilt. */

  /* (Re)build deform context. */
  if (state->deform_data) {
    BKE_lattice_deform_data_destroy(state->deform_data);
  }
  state->deform_data = sculpt_lattice_deform_data_rebuild(state->lattice_ob, ob_mesh);

  printf("[lattice] slide_invoke: affected_verts=%d deform_data=%s drag_index=%d\n",
         int(state->current.verts.size()),
         state->deform_data ? "ok" : "NULL",
         state->pending_drag_index);
  fflush(stdout);

  /* Per-drag undo step (like Mesh Filter). */
  undo::push_begin(*scene, *ob_mesh, op);

  /* Register position undo nodes BEFORE any deformation: #PositionDeformData::deform writes
   * coordinates directly and does not push undo itself, so without this the per-drag undo step
   * would be empty and Ctrl+Z could not revert the deformation. Mirrors Mesh Filter's
   * `cache_init` (sculpt_filter_mesh.cc). Pushing all leaf nodes is correct and matches the
   * cancel path; it can be narrowed to the affected nodes later if profiling requires it. */
  {
    bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(*ob_mesh);
    if (pbvh.type() == bke::pbvh::Type::Mesh) {
      IndexMaskMemory memory;
      const IndexMask node_mask = bke::pbvh::all_leaf_nodes(pbvh, memory);
      undo::push_nodes(*depsgraph, *ob_mesh, node_mask, undo::Type::Position);
    }
  }

  state->drag_active = true;
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
  BKE_lattice_batch_cache_dirty_tag(lt, BKE_LATTICE_BATCH_DIRTY_ALL);
  /* IMPORTANT: do NOT tag the lattice #ID_RECALC_GEOMETRY here. The deform reads the live
   * #Lattice.def directly (see #sculpt_lattice_deform_data_rebuild), so it does not need the
   * evaluated cage. Tagging a foreign object dirty during the modal forces a depsgraph
   * re-evaluation before the next redraw, which discards the fast PBVH draw-buffer refresh that
   * #flush_update_step just performed on the mesh — the deform runs (verified non-zero) but the
   * viewport never shows it. The cage's own redraw is deferred to drag end / confirm. */

  /* Rebuild deform context (cage changed). */
  if (state.deform_data) {
    BKE_lattice_deform_data_destroy(state.deform_data);
  }
  state.deform_data = sculpt_lattice_deform_data_rebuild(lat_ob, &ob_mesh);

  BKE_sculpt_update_object_for_edit(depsgraph, &ob_mesh, false);
  sculpt_lattice_deform_apply(*depsgraph, ob_mesh, state);
  sculpt_lattice_preview_flush(C, ob_mesh);
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
      printf("[lattice] modal MOUSEMOVE\n");
      fflush(stdout);
      sculpt_lattice_slide_update_point(C, *ob_mesh, *state, event);
      return OPERATOR_RUNNING_MODAL;
    }
    case LEFTMOUSE: {
      if (event->val == KM_RELEASE) {
        /* End drag: one undo step per drag (ADR-7). RMB does NOT end the drag. */
        undo::push_end(*ob_mesh);
        state->drag_active = false;
        state->pending_drag_index = -1;
        ss->lattice_slide_active = false;
        /* Drag finished: now it is safe to re-evaluate the cage once so its drawn (evaluated)
         * shape catches up with the edited control points. This single eval per drag does not
         * affect the live mesh preview (which already happened during MOUSEMOVE). */
        if (state->lattice_ob) {
          DEG_id_tag_update(&state->lattice_ob->id, ID_RECALC_GEOMETRY);
        }
        flush_update_done(C, *ob_mesh, UpdateType::Position);
        ED_region_tag_redraw(CTX_wm_region(C));
        return OPERATOR_FINISHED;
      }
      return OPERATOR_RUNNING_MODAL;
    }
    case EVT_ESCKEY: {
      /* Roll back the current drag only: restore from rest_coords snapshot. */
      Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
      if (!depsgraph) {
        return OPERATOR_RUNNING_MODAL;
      }
      AffectedRegion &ar = state->current;
      if (!ar.verts.is_empty()) {
        const PositionDeformData position_data(*depsgraph, *ob_mesh);
        Vector<float3> translations(ar.verts.size());
        for (const int i : ar.verts.index_range()) {
          /* Move from the last position this tool wrote back to rest (see
           * #AffectedRegion::current_coords); `eval` is unreliable under the re-eval stopgap. */
          translations[i] = ar.rest_coords[i] - ar.current_coords[i];
          ar.current_coords[i] = ar.rest_coords[i];
        }
        position_data.deform(translations, ar.verts);
      }
      BKE_sculpt_update_object_for_edit(depsgraph, ob_mesh, false);
      flush_update_step(C, UpdateType::Position);
      undo::push_end(*ob_mesh);
      state->drag_active = false;
      state->pending_drag_index = -1;
      ss->lattice_slide_active = false;
      flush_update_done(C, *ob_mesh, UpdateType::Position);
      ED_region_tag_redraw(CTX_wm_region(C));
      return OPERATOR_CANCELLED;
    }
    default:
      return OPERATOR_RUNNING_MODAL;
  }
}

void SCULPT_OT_lattice_slide(wmOperatorType *ot)
{
  ot->name = "Lattice Slide";
  ot->idname = "SCULPT_OT_lattice_slide";
  ot->description = "Drag a picked lattice control point to deform the mesh";

  ot->invoke = sculpt_lattice_slide_invoke;
  ot->modal = sculpt_lattice_slide_modal;
  ot->poll = sculpt_lattice_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING | OPTYPE_GRAB_CURSOR_X |
             OPTYPE_DEPENDS_ON_CURSOR;
}

/* --- SCULPT_OT_lattice_confirm (Enter) ------------------------------ */

static wmOperatorStatus sculpt_lattice_confirm_exec(bContext *C, wmOperator * /*op*/)
{
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  Object *ob_mesh = CTX_data_active_object(C);
  if (!ob_mesh || !ob_mesh->runtime->sculpt_session ||
      !ob_mesh->runtime->sculpt_session->lattice_tool_state)
  {
    return OPERATOR_CANCELLED;
  }

  /* Apply-once: positions are already in the mesh. Drop the session + temp obj. */
  sculpt_lattice_session_free(bmain, scene, ob_mesh);
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

static wmOperatorStatus sculpt_lattice_cancel_exec(bContext *C, wmOperator * /*op*/)
{
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  Object *ob_mesh = CTX_data_active_object(C);
  if (!ob_mesh || !ob_mesh->runtime->sculpt_session ||
      !ob_mesh->runtime->sculpt_session->lattice_tool_state)
  {
    return OPERATOR_CANCELLED;
  }
  LatticeToolData *state = ob_mesh->runtime->sculpt_session->lattice_tool_state;

  /* Restore all positions from the entry snapshot. */
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  MutableSpan<float3> positions = bke::pbvh::vert_positions_eval_for_write(*depsgraph, *ob_mesh);
  if (!state->entry_positions.is_empty() && positions.size() == state->entry_positions.size()) {
    positions.copy_from(state->entry_positions);
    bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(*ob_mesh);
    IndexMaskMemory memory;
    const IndexMask all = bke::pbvh::all_leaf_nodes(pbvh, memory);
    pbvh.tag_positions_changed(all);
  }
  BKE_sculpt_update_object_for_edit(depsgraph, ob_mesh, false);

  sculpt_lattice_session_free(bmain, scene, ob_mesh);
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
