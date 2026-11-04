/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * #SCULPT_OT_layer_mirror_copy — copy the active sculpt layer's displacement onto the mirrored side
 * of the enabled symmetry plane(s).
 *
 * The operator has two entry points over one core (`sculpt_layer_mirror_map.cc`): the modal path
 * (the menu item) clicks the source side in a 3D viewport, previews the result on the composed
 * surface, and confirms or restores it; #exec does the same write in one step from the
 * `source_point` property, which is what makes the operation reachable from Python and from a tool
 * that has no cursor.
 *
 * The mapping is computed once, when the source side is known, and cached: `Replace`/`Add`, the
 * face-set limit and the mask limit only change how the cached pairs are *written*, so no key press
 * repeats the spatial search. That is also why the mapping covers the whole source side, empty
 * elements included — `Replace` has to be able to zero a target whose source carries no
 * displacement.
 *
 * v1 limitations, all deliberate:
 * - Radial symmetry is not transferred: an enabled axis with `Mesh::radial_symmetry[axis] > 1` is a
 *   refusal.
 * - No sync-group fan-out (see #mirror_gate): the preview and its undo step are per-object geometry,
 *   not the per-layer arithmetic #layer_data_fanout_targets fans out, so a grouped layer refuses
 *   rather than leaving the members silently divergent.
 * - Of several sculpted objects only the active one is mirrored, with a warning.
 * - The grid (multires) mapping always works in the layer's own top-level layout
 *   (#SculptLayer::level), even when the live CCG sits at a lower sculpt level, and takes its tangent
 *   frames from #BKE_multires_sculpt_layer_grid_frames_get — a pure limit-surface evaluation, because
 *   the reshape encode path would re-write #CD_MDISPS, which this operator must not do.
 * - Grid seam duplicates (a coarse vertex or edge point held by every grid around it) are one source
 *   and several targets: each copy of a target gets its own pair, with its own grid's tangent frame, so
 *   no copy is left at its old value to tear the surface along the grid edge.
 */

#include <algorithm>
#include <climits>
#include <optional>
#include <variant>

#include "MEM_guardedalloc.h"

#include "BLI_array.hh"
/* `DNA_listBase.h` only forward-declares the iterator the range-for loops below run on, so the
 * definition has to be pulled in explicitly here. */
#include "BLI_listbase_iterator.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_rect.h"
#include "BLI_span.hh"
#include "BLI_string.h"
#include "BLI_vector.hh"

#include "BLT_translation.hh"

#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BKE_attribute.hh"
#include "BKE_ccg.hh"
#include "BKE_context.hh"
#include "BKE_main.hh"
#include "BKE_mesh.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_report.hh"
#include "BKE_screen.hh"
#include "BKE_sculpt_layers.hh"
#include "BKE_subdiv_ccg.hh"

#include "DEG_depsgraph.hh"

#include "ED_screen.hh"
#include "ED_view3d.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "UI_resources.hh"

#include "WM_api.hh"
#include "WM_keymap.hh"
#include "WM_types.hh"

#include "sculpt_intern.hh"
#include "sculpt_layer_mirror.hh"
#include "sculpt_layer_mirror_map.hh"

#include "sculpt_undo.hh"

namespace blender::ed::sculpt_paint::layers {

/**
 * Values #SCULPT_OT_layer_mirror_copy receives through #EVT_MODAL_MAP. Keyboard actions only: the
 * click that picks the source side has to know which viewport it landed in, so mouse events stay in
 * the operator's modal handler instead of the map. Declared outside `mirror_copy` because the
 * keymap registration below, which sits outside it, builds the map's items from these values.
 */
enum MirrorCopyModalAction {
  MIRROR_MODAL_CONFIRM = 0,
  MIRROR_MODAL_CANCEL = 1,
  MIRROR_MODAL_TOGGLE_MODE = 2,
  MIRROR_MODAL_TOGGLE_LIMIT = 3,
  MIRROR_MODAL_TOGGLE_MASK = 4,
};

namespace mirror_copy {

static const EnumPropertyItem mode_items[] = {
    {int(Mode::Replace),
     "REPLACE",
     0,
     "Replace",
     "Overwrite the mirrored side with a copy of the source side, empty areas of it included"},
    {int(Mode::Add), "ADD", 0, "Add", "Add the mirrored displacement to the target side"},
    {0, nullptr, 0, nullptr, nullptr},
};

static const EnumPropertyItem limit_items[] = {
    {int(Limit::All), "ALL", 0, "Whole Side", "Mirror the whole side of the mesh"},
    {int(Limit::FaceSet),
     "FACE_SET",
     0,
     "Face Set",
     "Mirror only the face set under the source point"},
    {0, nullptr, 0, nullptr, nullptr},
};

/* -------------------------------------------------------------------- */
/** \name Session state
 * \{ */

/**
 * The modal session's state. Every pointer in it is re-resolved before the next use (see
 * #mirror_state_refresh): a preview can span an undo, a layer removal or a mode switch.
 */
struct MirrorCopyState {
  enum class Phase {
    /** Waiting for the click that picks the source side. */
    WaitClick,
    /** Mirrored deltas are written into the layer; confirm or cancel pending. */
    Preview,
  };

  Phase phase = Phase::WaitClick;
  Object *object = nullptr;
  SculptLayer *layer = nullptr;
  /** #SculptLayerTreeNode::uid of #MirrorCopyState::layer, the identity it is re-resolved by. */
  int layer_uid = 0;
  /** True when the session took the mesh's active layer (`layer_uid` was left at 0), so switching the
   * active layer mid-preview ends it; a session a script pointed at a layer by uid keeps working
   * whichever layer the UI has selected. */
  bool follows_active_layer = true;
  Depsgraph *depsgraph = nullptr;
  /** The operator's settings, cached so no event has to read RNA again. The keys write through to the
   * properties as well, so the operator's own state stays what a caller asked for. */
  MirrorParams params;
  /** The element the click hit, or -1 for a caller that names #MirrorParams::source_point itself. */
  int64_t click_elem = -1;
  /** The mapping of the whole source side, computed once and re-written by every key press. */
  Vector<MirrorPair> pairs;
  /** The layer data as it was before the first write; every preview re-applies from it. */
  Array<float3> saved_data;
  /** Targets the last write reached, for the status line and the report. */
  int written = 0;
};

/** The layer the session was opened on, resolved by uid, or null when the tree no longer has it. */
static SculptLayer *layer_of_uid(Mesh &mesh, const int uid)
{
  return bke::sculpt_layers::node_as_layer(bke::sculpt_layers::node_find_by_uid(mesh, uid));
}

/** Read the operator's properties into the cached settings — the only place they are read. */
static void params_from_operator(wmOperator *op, MirrorParams &r_params)
{
  r_params.mode = Mode(RNA_enum_get(op->ptr, "mode"));
  r_params.limit = Limit(RNA_enum_get(op->ptr, "limit"));
  r_params.use_mask = RNA_boolean_get(op->ptr, "use_mask_limit");
  /* Left at zero when the caller did not name a point: #exec refuses an unset point instead of
   * mirroring from the origin, and the modal path decides the side from the click instead (see
   * #mirror_mapping_build). */
  if (RNA_struct_property_is_set(op->ptr, "source_point")) {
    float values[3];
    RNA_float_get_array(op->ptr, "source_point", values);
    r_params.source_point = float3(values[0], values[1], values[2]);
  }
}

/** Write the settings back, so a modal session that toggled them reports what it actually used. */
static void params_to_operator(wmOperator *op, const MirrorParams &params)
{
  RNA_enum_set(op->ptr, "mode", int(params.mode));
  RNA_enum_set(op->ptr, "limit", int(params.limit));
  RNA_boolean_set(op->ptr, "use_mask_limit", params.use_mask);
}

/** Record the clicked position in the property, which is what #exec would need to redo the write. */
static void source_point_to_operator(wmOperator *op, const float3 &point)
{
  const float values[3] = {point.x, point.y, point.z};
  RNA_float_set_array(op->ptr, "source_point", values);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Viewport binding
 * \{ */

/**
 * The window region of a 3D viewport that \a xy falls in, or null when it falls in none. With \a xy
 * null (a key press, whose position means nothing) the screen's first viewport is returned.
 *
 * The viewport the session is bound to wins over every other one: the modal handler sets the
 * operator's context to its own area, so with several viewports on screen a click can never re-aim
 * the session at a viewport it was not started in, and a click on that area's header or sidebar
 * belongs to no viewport at all. The screen scan below only answers for a context outside any
 * viewport — the Properties editor button the operator was invoked from, whose area is not a 3D
 * viewport — and picks the first viewport of the screen.
 *
 * The point is taken in window coordinates on purpose: the operator is invoked from a button in the
 * Properties editor, so #wmEvent::mval belongs to that editor's region rather than to the view the
 * session is aimed at.
 */
static bool view3d_region_at(const bContext *C,
                             const int *xy,
                             ScrArea **r_area,
                             ARegion **r_region)
{
  if (const ScrArea *bound_area = CTX_wm_area(C);
      bound_area != nullptr && bound_area->spacetype == SPACE_VIEW3D)
  {
    if (ARegion *region = BKE_area_find_region_type(bound_area, RGN_TYPE_WINDOW)) {
      if (xy == nullptr || BLI_rcti_isect_pt_v(&region->winrct, xy)) {
        *r_area = CTX_wm_area(C);
        *r_region = region;
        return true;
      }
      return false;
    }
  }
  bScreen *screen = CTX_wm_screen(C);
  if (screen == nullptr) {
    return false;
  }
  for (ScrArea &area : screen->areabase) {
    if (area.spacetype != SPACE_VIEW3D) {
      continue;
    }
    ARegion *region = BKE_area_find_region_type(&area, RGN_TYPE_WINDOW);
    if (region == nullptr) {
      continue;
    }
    if (xy == nullptr || BLI_rcti_isect_pt_v(&region->winrct, xy)) {
      *r_area = &area;
      *r_region = region;
      return true;
    }
  }
  return false;
}

/** Tag every viewport of the screen: the preview changed the composed surface they all draw. */
static void view3d_redraw(const bContext *C)
{
  bScreen *screen = CTX_wm_screen(C);
  if (screen == nullptr) {
    return;
  }
  for (ScrArea &area : screen->areabase) {
    if (area.spacetype != SPACE_VIEW3D) {
      continue;
    }
    ARegion *region = BKE_area_find_region_type(&area, RGN_TYPE_WINDOW);
    if (region != nullptr) {
      ED_region_tag_redraw(region);
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Gating and mapping
 * \{ */

/* The copy tolerance is keyed to the domain's own element spacing (see #mirror_element_spacing), a
 * factor over it rather than an absolute length: loose enough to absorb the limit-surface rounding
 * of the mirrored position, tight enough not to pair two elements the mesh does not actually
 * correspond between. */
static constexpr float tolerance_spacing_factor = 1.5f;
/* The plane test is a fraction of the tolerance: a point this close to a symmetry plane counts as
 * lying on it, so plane-adjacent sources still find their plane-adjacent targets. */
static constexpr float plane_eps_divisor = 100.0f;

/** The number of objects currently in Sculpt Mode, for the multi-object warning in #mirror_gate. */
static int sculpt_mode_object_num(const Main &bmain)
{
  int num = 0;
  for (const Object &ob : bmain.objects) {
    if ((ob.mode & OB_MODE_SCULPT) != 0) {
      num++;
      if (num > 1) {
        break;
      }
    }
  }
  return num;
}

/**
 * Every refusal, in one place, for both entry points: the checks that decide whether this layer can be
 * mirrored at all must not differ between the click path and #exec. On success the object, the layer,
 * the depsgraph and the geometry-dependent half of #MirrorParams are filled in.
 */
static bool mirror_gate(bContext *C, wmOperator *op, MirrorCopyState &r_state)
{
  OpContext ctx;
  if (!op_context_get(C, op, ctx)) {
    return false;
  }
  if (mask_edit_refuse_active_session(op, *ctx.object)) {
    return false;
  }
  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(*ctx.object);
  if (pbvh == nullptr ||
      (pbvh->type() != bke::pbvh::Type::Mesh && pbvh->type() != bke::pbvh::Type::Grids))
  {
    BKE_report(op->reports, RPT_ERROR, "Mirror Sculpt Layer is not available for this object");
    return false;
  }

  /* `layer_uid` lets a script name any layer of the mesh; the UI leaves it at 0, which means the active
   * one, and the session then follows that identity rather than a pointer (see
   * #mirror_state_refresh). */
  const int wanted_uid = RNA_int_get(op->ptr, "layer_uid");
  Mesh &mesh = mesh_of(*ctx.object);
  SculptLayer *layer = (wanted_uid != 0) ? layer_of_uid(mesh, wanted_uid) :
                                           bke::sculpt_layers::active_get(mesh);
  if (layer == nullptr) {
    BKE_report(op->reports,
               RPT_ERROR,
               wanted_uid != 0 ? "No sculpt layer with this uid" : "No active sculpt layer");
    return false;
  }
  if (layer->domain != domain_for(*ctx.object)) {
    BKE_report(op->reports,
               RPT_ERROR,
               "The sculpt layer's domain does not match the object's sculpt domain");
    return false;
  }
  if (layer->base.flag & SCULPT_LAYER_LOCKED) {
    BKE_report(op->reports, RPT_ERROR, "The sculpt layer is locked");
    return false;
  }
  if (bke::sculpt_layers::is_stale(mesh, *layer)) {
    BKE_report(op->reports,
               RPT_ERROR,
               "The sculpt layer's data is stale (the topology changed after it was sculpted)");
    return false;
  }
  if (layer->data == nullptr) {
    BKE_report(op->reports, RPT_INFO, "The sculpt layer has no displacement to mirror");
    return false;
  }

  const ePaintSymmetryFlags symm = mesh_symmetry_xyz_get(*ctx.object);
  if (symm == PAINT_SYMM_NONE) {
    BKE_report(op->reports, RPT_ERROR, "Enable mesh symmetry (X, Y or Z) first");
    return false;
  }
  for (const int axis : {0, 1, 2}) {
    if ((symm & ePaintSymmetryFlags(1 << axis)) && mesh.radial_symmetry[axis] > 1) {
      BKE_report(op->reports,
                 RPT_ERROR,
                 "Mirror Sculpt Layer does not support radial symmetry (use a symmetry count of 1)");
      return false;
    }
  }

  /* A sync group's members would have to be mapped, previewed and undone as one step each, which is
   * the fan-out #layer_data_fanout_targets resolves for an edit that is pure per-layer arithmetic. A
   * mirror is per-object geometry: the sides of two members do not correspond, so copying one object's
   * side onto another's would silently produce a different shape. Refuse instead. */
  if (layer->base.sync_uid != 0) {
    BKE_report(op->reports,
               RPT_ERROR,
               "Mirror Sculpt Layer does not support layers in a sync group (unlink the group to "
               "mirror one object at a time)");
    return false;
  }

  /* Counted over all of `bmain` rather than through #sculpt_mode_objects: #exec has no viewport to
   * build a #ViewContext from, and the count only feeds a warning. */
  if (sculpt_mode_object_num(*CTX_data_main(C)) > 1) {
    BKE_report(op->reports,
               RPT_WARNING,
               "Multiple objects are in sculpt mode: only the active object is mirrored");
  }

  const bool grids = ctx.grids;
  if (grids && layer->level <= 0) {
    /* The grid mapping is indexed by the layer's own subdivision level; without one there is no layout
     * to address. */
    BKE_report(op->reports, RPT_ERROR, "The grid layer has no subdivision level");
    return false;
  }
  if (grids && session_of(*ctx.object)->subdiv_ccg->level != layer->level) {
    /* The grid mapping addresses the CCG with the layer's own layout, which only coincides with it at
     * the layer's top level. A coarser sculpt level has fewer CCG points than layer elements, and the
     * finer ones have no composed position to search by. */
    BKE_report(op->reports,
               RPT_ERROR,
               "Set the multires sculpt level to the highest level to mirror this layer");
    return false;
  }
  const int top_grid_size = grids ? CCG_grid_size(layer->level) : 0;

  r_state.object = ctx.object;
  r_state.layer = layer;
  r_state.layer_uid = layer->base.uid;
  r_state.follows_active_layer = (wanted_uid == 0);
  r_state.depsgraph = ctx.depsgraph;
  params_from_operator(op, r_state.params);
  r_state.params.grids = grids;
  r_state.params.symm = symm;
  r_state.params.tolerance = std::max(
      mirror_element_spacing(mesh, top_grid_size) * tolerance_spacing_factor, 1e-5f);
  r_state.params.plane_eps = r_state.params.tolerance / plane_eps_divisor;
  return true;
}

/**
 * Re-resolve the pointers the session holds. Between two events the user can undo, remove the layer,
 * select another one or leave Sculpt Mode, and the next event writes through all of them.
 */
static bool mirror_state_refresh(bContext *C, MirrorCopyState &state)
{
  Object *object = CTX_data_active_object(C);
  if (object == nullptr || object != state.object || (object->mode & OB_MODE_SCULPT) == 0 ||
      session_of(*object) == nullptr)
  {
    return false;
  }
  Mesh &mesh = mesh_of(*object);
  SculptLayer *layer = layer_of_uid(mesh, state.layer_uid);
  if (layer == nullptr || layer != state.layer || layer->data == nullptr ||
      (layer->base.flag & SCULPT_LAYER_LOCKED) != 0 || layer->domain != domain_for(*object) ||
      bke::sculpt_layers::is_stale(mesh, *layer) ||
      (state.follows_active_layer && bke::sculpt_layers::active_get(mesh) != layer) ||
      (state.params.grids && (session_of(*object)->subdiv_ccg == nullptr ||
                              session_of(*object)->subdiv_ccg->level != layer->level)))
  {
    return false;
  }
  state.depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Writing the preview
 * \{ */

/** Put the layer back exactly as the snapshot held it and recompose. */
static void mirror_restore(MirrorCopyState &state)
{
  /* NOTE: The preview maintains an independent copy in #MirrorCopyState::saved_data in addition to
   * the undo step opened with #push_sculpt_layer_data. Reading back from the active sculpt-undo node
   * could avoid duplicating the layer's displacement, but would couple the interactive preview to
   * the undo stack layout. */
  MutableSpan<float3> data = bke::sculpt_layers::data_get(*state.layer);
  if (data.size() == state.saved_data.size()) {
    data.copy_from(state.saved_data);
  }
  commit_layers_change(*state.depsgraph, *state.object);
}

/**
 * Re-apply the cached mapping over the snapshot with the settings the state currently holds and publish
 * it through the canonical commit path, so the viewport shows exactly what a confirm would keep.
 */
static void mirror_preview_write(bContext *C, MirrorCopyState &state)
{
  MutableSpan<float3> data = bke::sculpt_layers::data_get(*state.layer);
  if (data.is_empty() || data.size() != state.saved_data.size()) {
    return;
  }
  data.copy_from(state.saved_data);
  state.written = mirror_mapping_apply(state.params, state.pairs, data);
  commit_layers_change(*state.depsgraph, *state.object);
  view3d_redraw(C);
  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, &state.object->id);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Status line
 * \{ */

/**
 * The hint for the whole session. The keys come from the operator's modal keymap, so what the status
 * line shows is what the user actually bound.
 */
static void mirror_status(bContext *C, const wmOperator *op, const MirrorCopyState &state)
{
  WorkspaceStatus status(C);
  const char *mode_next = (state.params.mode == Mode::Replace) ? IFACE_("Mode: Add") :
                                                                 IFACE_("Mode: Replace");
  const char *limit_next = (state.params.limit == Limit::All) ? IFACE_("Limit: Face Set") :
                                                                IFACE_("Limit: Whole Side");
  const char *mask_next = state.params.use_mask ? IFACE_("Mask: Off") : IFACE_("Mask: On");

  /* The map is the only source of the keys (as for the mesh filter and the expand maps), so the
   * status line shows the user's own bindings rather than a second, hard-coded set. */
  status.opmodal(IFACE_("Confirm"), op->type, MIRROR_MODAL_CONFIRM);
  status.opmodal(IFACE_("Cancel"), op->type, MIRROR_MODAL_CANCEL);
  status.opmodal(mode_next, op->type, MIRROR_MODAL_TOGGLE_MODE);
  status.opmodal(limit_next, op->type, MIRROR_MODAL_TOGGLE_LIMIT);
  status.opmodal(mask_next, op->type, MIRROR_MODAL_TOGGLE_MASK);

  if (state.phase == MirrorCopyState::Phase::WaitClick) {
    status.item(IFACE_("Click the source side"), ICON_MOUSE_LMB);
  }
  else {
    char detail[128];
    BLI_snprintf(detail, sizeof(detail), IFACE_("Mirrored %d target(s)"), state.written);
    status.item(detail, ICON_NONE);
    status.item(IFACE_("Click or Enter to confirm"), ICON_NONE);
  }
}

/** One key press: keep the properties in step with the settings, re-write, re-hint. */
static void mirror_repreview(bContext *C, wmOperator *op, MirrorCopyState &state)
{
  params_to_operator(op, state.params);
  if (state.phase == MirrorCopyState::Phase::Preview) {
    mirror_preview_write(C, state);
  }
  mirror_status(C, op, state);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Ending the session
 * \{ */

/** Both exit paths drop the status line, the cursor and the state here. */
static void mirror_session_end(bContext *C, wmOperator *op)
{
  ED_workspace_status_text(C, nullptr);
  if (wmWindow *win = CTX_wm_window(C)) {
    WM_cursor_modal_restore(win);
  }
  /* #op->customdata is a `void *`, which this `MEM_delete` refuses by static_assert. */
  MEM_delete(static_cast<MirrorCopyState *>(op->customdata));
  op->customdata = nullptr;
}

/** Confirm or cancel the preview. The caller has just validated the session. */
static wmOperatorStatus mirror_finish(bContext *C, wmOperator *op, const bool cancel)
{
  MirrorCopyState &state = *static_cast<MirrorCopyState *>(op->customdata);
  if (state.phase == MirrorCopyState::Phase::Preview) {
    if (cancel) {
      mirror_restore(state);
      /* The snapshot and the preview both belong to the discarded step: nothing on the layer moved, so
       * no undo entry may survive the cancel. */
      undo::discard_init_step();
    }
    else {
      Object *objects[] = {state.object};
      undo::finish_multi_object(C, {objects, 1}, UpdateType::Position);
      layers_ui_notify(C, *state.object);
    }
  }
  mirror_session_end(C, op);
  return cancel ? OPERATOR_CANCELLED : OPERATOR_FINISHED;
}

/**
 * End after #mirror_state_refresh failed, when the object or the layer the preview was written into may
 * be gone: the undo bracket the click opened still has to be closed, and the snapshot is put back only
 * while the layer it came from can still be resolved. A surface that is already being recomposed by the
 * mode switch or the removal is not this session's to commit.
 */
static wmOperatorStatus mirror_abort(bContext *C, wmOperator *op)
{
  MirrorCopyState &state = *static_cast<MirrorCopyState *>(op->customdata);
  if (state.phase == MirrorCopyState::Phase::Preview) {
    Object *object = CTX_data_active_object(C);
    if (object == state.object && (object->mode & OB_MODE_SCULPT) != 0 &&
        session_of(*object) != nullptr)
    {
      SculptLayer *layer = layer_of_uid(mesh_of(*object), state.layer_uid);
      if (layer != nullptr) {
        state.layer = layer;
        state.depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
        mirror_restore(state);
      }
    }
    undo::discard_init_step();
  }
  BKE_report(op->reports,
             RPT_WARNING,
             "Mirror Sculpt Layer Data: the object or the layer changed, the session ended");
  mirror_session_end(C, op);
  return OPERATOR_CANCELLED;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Clicking the source side
 * \{ */

/**
 * The click that picks the source side: locate the element under the pointer in the viewport it landed
 * in, map its side, open the undo bracket and show the result. Nothing is written before the mapping
 * succeeds, so a click that mirrors nothing leaves no step behind.
 */
static void mirror_click(bContext *C,
                         wmOperator *op,
                         MirrorCopyState &state,
                         ScrArea &area,
                         ARegion &region,
                         const int *xy)
{
  ViewContext vc = ED_view3d_viewcontext_init(C, state.depsgraph);
  vc.region = &region;
  vc.v3d = static_cast<View3D *>(area.spacedata.first);
  vc.rv3d = static_cast<RegionView3D *>(region.regiondata);

  /* Recomputed from the window position rather than taken from #wmEvent::mval, so the ray cannot be
   * aimed by a value WM derived for a different region. */
  const float2 mval(float(xy[0] - region.winrct.xmin), float(xy[1] - region.winrct.ymin));
  const std::optional<ActiveElementInfo> info = active_element_info_get(vc, mval);
  if (!info || !std::holds_alternative<int>(info->vert)) {
    BKE_report(op->reports, RPT_INFO, "Click on the mesh to pick the source side");
    return;
  }

  /* The element that was hit decides the source side; its base position is resolved by the mapping,
   * and recorded in the property once it is known. */
  const int elem = std::get<int>(info->vert);
  state.click_elem = elem;

  /* The face set under the click, read off the hit: a grid belongs to one face, and the ray's face
   * index comes with a vertex hit. Remembered whatever the current limit, so the limit key can filter
   * the cached mapping without a new click. */
  state.params.face_set_id = -1;
  int face = info->active_face_idx;
  if (state.params.grids) {
    face = (info->active_grid_idx >= 0) ?
               session_of(*state.object)->subdiv_ccg->grid_to_face_map[info->active_grid_idx] :
               -1;
  }
  if (face >= 0) {
    const bke::AttributeAccessor attributes = mesh_of(*state.object).attributes();
    if (attributes.contains(".sculpt_face_set")) {
      const VArraySpan<int> face_set = *attributes.lookup<int>(".sculpt_face_set",
                                                              bke::AttrDomain::Face);
      if (face < face_set.size()) {
        state.params.face_set_id = face_set[face];
      }
    }
  }

  if (!mirror_mapping_build(
          *state.object, *state.layer, state.params, state.click_elem, state.pairs))
  {
    BKE_report(op->reports,
               RPT_INFO,
               "Nothing mirrored: no source displacement on that side, or no matching elements on the "
               "mirrored side (within tolerance)");
    return;
  }
  source_point_to_operator(op, state.params.source_point);
  if (state.params.limit == Limit::FaceSet && state.params.face_set_id < 0) {
    BKE_report(op->reports, RPT_WARNING, "The mesh has no face sets: mirroring the whole side");
  }

  /* The order #layer_invert_exec uses: capture the base, open the bracket, snapshot the layer, then
   * mutate. #commit_layers_change asserts the base is already captured. */
  session_state_ensure(*state.object);
  undo::push_begin(*CTX_data_scene(C), *state.object, op);
  undo::push_sculpt_layer_data(*state.object, *state.layer);
  /* NOTE: the layer data is now held twice (the undo snapshot and #MirrorCopyState::saved_data).
   * Deliberate: the preview re-applies the mapping from an independent copy after every key press,
   * while the undo snapshot has to stay exactly what the step will restore; reading the snapshot
   * back from the undo node on every key press would couple the preview to the undo system's
   * internals. */
  state.saved_data = Array<float3>(bke::sculpt_layers::data_get(*state.layer).as_span());

  mirror_preview_write(C, state);
  state.phase = MirrorCopyState::Phase::Preview;
  BKE_reportf(op->reports,
              RPT_INFO,
              "Preview: mirrored %d target(s). Enter or a second click to confirm, Esc to cancel",
              state.written);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Operators
 * \{ */

static wmOperatorStatus mirror_invoke(bContext *C, wmOperator *op, const wmEvent * /*event*/)
{
  MirrorCopyState *state = MEM_new<MirrorCopyState>(__func__);
  if (!mirror_gate(C, op, *state)) {
    MEM_delete(state);
    return OPERATOR_CANCELLED;
  }
  ScrArea *area = nullptr;
  ARegion *region = nullptr;
  if (!view3d_region_at(C, nullptr, &area, &region)) {
    BKE_report(op->reports, RPT_ERROR, "No 3D viewport to click in");
    MEM_delete(state);
    return OPERATOR_CANCELLED;
  }
  op->customdata = state;

  /* Bind the session to the viewport instead of to the region the button lives in, so the operator's
   * context resolves the view it is aimed at. Events still arrive from the whole window, which is why
   * every mouse event is tested against the viewport's own rect. */
  WM_event_add_modal_handler_ex(CTX_wm_window(C), area, region, op);
  WM_cursor_modal_set(CTX_wm_window(C), WM_CURSOR_CROSS);
  mirror_status(C, op, *state);
  return OPERATOR_RUNNING_MODAL;
}

/* One body per toggle, so the modal map's cases and nothing else write the settings, and a future
 * key or entry point reuses exactly what a key press does. */
static void mirror_toggle_mode(bContext *C, wmOperator *op, MirrorCopyState &state)
{
  state.params.mode = (state.params.mode == Mode::Replace) ? Mode::Add : Mode::Replace;
  mirror_repreview(C, op, state);
}

static void mirror_toggle_limit(bContext *C, wmOperator *op, MirrorCopyState &state)
{
  state.params.limit = (state.params.limit == Limit::All) ? Limit::FaceSet : Limit::All;
  mirror_repreview(C, op, state);
}

static void mirror_toggle_mask(bContext *C, wmOperator *op, MirrorCopyState &state)
{
  state.params.use_mask = !state.params.use_mask;
  mirror_repreview(C, op, state);
}

/**
 * The session's event filter.
 *
 * Only the events the session acts on are consumed; everything else returns a bare
 * #OPERATOR_PASS_THROUGH, which is what lets the view still be orbited and zoomed under a preview.
 * #OPERATOR_RUNNING_MODAL must not be ORed onto that: #wm_handler_operator_call maps the pair
 * `OPERATOR_PASS_THROUGH | OPERATOR_RUNNING_MODAL` to `WM_HANDLER_BREAK | WM_HANDLER_MODAL`, which stops
 * the event from ever reaching the viewport handlers — the combination reads as the natural way to say
 * "still modal, but pass it on" and does not work. The handler stays registered either way, because
 * removal is gated on #OPERATOR_CANCELLED / #OPERATOR_FINISHED alone.
 */
static wmOperatorStatus mirror_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  MirrorCopyState &state = *static_cast<MirrorCopyState *>(op->customdata);
  if (!mirror_state_refresh(C, state)) {
    return mirror_abort(C, op);
  }

  switch (event->type) {
    case EVT_MODAL_MAP: {
      /* Bound keys arrive here, already claimed by the map. The map is the only source of the
       * bindings, so a rebound keyconfig changes the behavior instead of fighting a second copy of
       * it in the raw events. */
      switch (event->val) {
        case MIRROR_MODAL_CONFIRM:
          if (state.phase == MirrorCopyState::Phase::Preview) {
            return mirror_finish(C, op, false);
          }
          break;
        case MIRROR_MODAL_CANCEL:
          return mirror_finish(C, op, true);
        case MIRROR_MODAL_TOGGLE_MODE:
          mirror_toggle_mode(C, op, state);
          break;
        case MIRROR_MODAL_TOGGLE_LIMIT:
          mirror_toggle_limit(C, op, state);
          break;
        case MIRROR_MODAL_TOGGLE_MASK:
          mirror_toggle_mask(C, op, state);
          break;
      }
      return OPERATOR_RUNNING_MODAL;
    }
    case LEFTMOUSE:
    case RIGHTMOUSE: {
      ScrArea *area = nullptr;
      ARegion *region = nullptr;
      if (!view3d_region_at(C, event->xy, &area, &region)) {
        /* A click on a panel, a header or another editor is not the session's to take: confirming or
         * picking from it would act on a view the user never aimed at. */
        return OPERATOR_PASS_THROUGH;
      }
      if (event->type == RIGHTMOUSE) {
        if (event->val == KM_PRESS) {
          return mirror_finish(C, op, true);
        }
        return OPERATOR_RUNNING_MODAL;
      }
      if (event->val != KM_PRESS) {
        return OPERATOR_RUNNING_MODAL;
      }
      if (state.phase == MirrorCopyState::Phase::WaitClick) {
        mirror_click(C, op, state, *area, *region, event->xy);
        mirror_status(C, op, state);
        return OPERATOR_RUNNING_MODAL;
      }
      return mirror_finish(C, op, false);
    }
    case EVT_ZKEY:
      /* Only the undo and redo presses are taken: they would close the preview's still-open undo
       * step from under the session, which would leave #MirrorCopyState::saved_data restoring over a
       * surface that has already moved. A plain Z — the viewport shading pie and whatever else the
       * user bound it to — passes through. */
      if (event->val == KM_PRESS && (event->modifier & KM_CTRL) != 0) {
        BKE_report(op->reports,
                   RPT_INFO,
                   "Confirm (Enter) or cancel (Esc) the mirror preview before undoing");
        return OPERATOR_RUNNING_MODAL;
      }
      return OPERATOR_PASS_THROUGH;
    default:
      break;
  }
  /* Navigation — middle mouse, wheel, trackpad, plain moves, view keys — is not the session's. */
  return OPERATOR_PASS_THROUGH;
}

/**
 * One non-interactive mirror: map the source side of `source_point` and write it, in a single undo step
 * and with no preview to confirm. The same core the modal path runs, so a script and a click cannot
 * disagree about what a given set of properties means.
 */
static wmOperatorStatus mirror_exec(bContext *C, wmOperator *op)
{
  MirrorCopyState state;
  if (!mirror_gate(C, op, state)) {
    return OPERATOR_CANCELLED;
  }
  if (!RNA_struct_property_is_set(op->ptr, "source_point")) {
    BKE_report(op->reports,
               RPT_ERROR,
               "Mirror Sculpt Layer Data needs a source point on the side to copy from");
    return OPERATOR_CANCELLED;
  }
  if (!mirror_mapping_build(
          *state.object, *state.layer, state.params, state.click_elem, state.pairs))
  {
    BKE_report(op->reports, RPT_INFO, "Nothing mirrored: no matching elements on the mirrored side");
    return OPERATOR_CANCELLED;
  }
  if (state.params.limit == Limit::FaceSet && state.params.face_set_id < 0) {
    BKE_report(op->reports, RPT_WARNING, "The mesh has no face sets: mirroring the whole side");
  }

  session_state_ensure(*state.object);
  undo::push_begin(*CTX_data_scene(C), *state.object, op);
  undo::push_sculpt_layer_data(*state.object, *state.layer);
  MutableSpan<float3> data = bke::sculpt_layers::data_get(*state.layer);
  const int written = mirror_mapping_apply(state.params, state.pairs, data);
  commit_layers_change(*state.depsgraph, *state.object);

  Object *objects[] = {state.object};
  undo::finish_multi_object(C, {objects, 1}, UpdateType::Position);
  layers_ui_notify(C, *state.object);

  BKE_reportf(op->reports, RPT_INFO, "Mirrored %d target(s) of the sculpt layer", written);
  return OPERATOR_FINISHED;
}

static void mirror_cancel(bContext *C, wmOperator *op)
{
  if (op->customdata == nullptr) {
    return;
  }
  /* The cancel can arrive without a preceding modal event (a workspace switch, a window close), so the
   * session has to be validated here too before the restore touches anything. */
  MirrorCopyState &state = *static_cast<MirrorCopyState *>(op->customdata);
  if (mirror_state_refresh(C, state)) {
    mirror_finish(C, op, true);
  }
  else {
    mirror_abort(C, op);
  }
}

/** \} */

}  // namespace mirror_copy

void mirror_copy_modal_keymap(wmKeyConfig *keyconf)
{
  static const EnumPropertyItem modal_items[] = {
      {MIRROR_MODAL_CONFIRM, "CONFIRM", 0, "Confirm", "Keep the mirrored displacement"},
      {MIRROR_MODAL_CANCEL, "CANCEL", 0, "Cancel", "Restore the layer as it was before the click"},
      {MIRROR_MODAL_TOGGLE_MODE,
       "TOGGLE_MODE",
       0,
       "Toggle Mode",
       "Switch between replacing and adding to the mirrored side"},
      {MIRROR_MODAL_TOGGLE_LIMIT,
       "TOGGLE_LIMIT",
       0,
       "Toggle Limit",
       "Switch between the whole side and the face set under the click"},
      {MIRROR_MODAL_TOGGLE_MASK,
       "TOGGLE_MASK",
       0,
       "Toggle Mask Limit",
       "Scale the copy by the weight mask of its source on or off"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const char *name = "Sculpt Layer Mirror Modal Map";
  wmKeyMap *keymap = WM_modalkeymap_find(keyconf, name);
  if (keymap && keymap->modal_items) {
    WM_modalkeymap_assign(keymap, "SCULPT_OT_layer_mirror_copy");
    return;
  }
  /* Bindings live in `blender_default.py`, so a key-config merge cannot empty the map and the user
   * can rebind the keys. A preset that does not list the map leaves its keys unbound, exactly as for
   * the mesh filter and the expand modal maps. */
  keymap = WM_modalkeymap_ensure(keyconf, name, modal_items);
  WM_modalkeymap_assign(keymap, "SCULPT_OT_layer_mirror_copy");
}

void SCULPT_OT_layer_mirror_copy(wmOperatorType *ot)
{
  ot->name = "Mirror Sculpt Layer Data";
  ot->idname = "SCULPT_OT_layer_mirror_copy";
  ot->description =
      "Copy the sculpt layer's displacement to the mirrored side of the symmetry plane(s): the modal "
      "operator clicks the source side and previews it, Enter confirms and Esc restores";
  ot->invoke = mirror_copy::mirror_invoke;
  ot->modal = mirror_copy::mirror_modal;
  ot->exec = mirror_copy::mirror_exec;
  ot->cancel = mirror_copy::mirror_cancel;
  ot->poll = layers_poll;
  /* No #OPTYPE_UNDO: the layer data is bracketed by the sculpt undo system instead (see
   * #mirror_click and #mirror_exec, which close the step with #undo::finish_multi_object or drop it with
   * #undo::discard_init_step). An operator undo push on top of that step would make one mirror need two
   * Ctrl+Z presses, which is why the other sculpt layer operators leave this flag off too. */
  ot->flag = OPTYPE_REGISTER | OPTYPE_BLOCKING;

  RNA_def_enum(ot->srna,
               "mode",
               mirror_copy::mode_items,
               int(mirror_copy::Mode::Replace),
               "Mode",
               "How the mirrored displacement lands on the target side");
  RNA_def_enum(ot->srna,
               "limit",
               mirror_copy::limit_items,
               int(mirror_copy::Limit::All),
               "Limit",
               "Which part of the source side is mirrored");
  RNA_def_boolean(ot->srna,
                  "use_mask_limit",
                  false,
                  "Limit by Mask",
                  "Scale the mirrored displacement by the source element's weight mask; in Replace mode "
                  "a fully masked-out source zeroes its target");
  PropertyRNA *uid_prop = RNA_def_int(ot->srna,
                                      "layer_uid",
                                      0,
                                      0,
                                      INT_MAX,
                                      "Layer",
                                      "Sculpt layer to mirror, by uid (0 means the active one)",
                                      0,
                                      INT_MAX);
  /* Not remembered across calls: a value left over from a previous Python call or a redo would
   * silently mirror a layer the next UI invocation never asked for (the UI means the active one by
   * leaving it at 0). */
  RNA_def_property_flag(uid_prop, PROP_SKIP_SAVE);
  PropertyRNA *source_prop = RNA_def_float_vector(ot->srna,
                                                  "source_point",
                                                  3,
                                                  nullptr,
                                                  -1.0e6f,
                                                  1.0e6f,
                                                  "Source Point",
                                                  "Object-space position on the side to copy from; its "
                                                  "sign per enabled symmetry axis picks that side",
                                                  -1.0e6f,
                                                  1.0e6f);
  RNA_def_property_subtype(source_prop, PROP_TRANSLATION);
  /* Same as `layer_uid`: the modal path records the clicked point itself, and #exec must see a
   * point the caller actually named rather than one left over from the previous call. */
  RNA_def_property_flag(source_prop, PROP_SKIP_SAVE);
}

}  // namespace blender::ed::sculpt_paint::layers
