/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * The refresh half of the Curve Patch object-source module: the row re-stamp entry points the BKE
 * source scan reaches, and the passive rebuild a row without a live session is stamped from.
 */

#include "paint_curve_patch_layer.hh"

#include <algorithm>
#include <optional>
#include <utility>

#include "BLI_index_range.hh"
#include "BLI_listbase_iterator.hh"
#include "BLI_math_vector.hh"
#include "BLI_span.hh"
#include "BLI_utildefines.h"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "DNA_ID.h"
#include "DNA_material_types.h"
#include "DNA_object_types.h"
#include "DNA_paint_vector_types.h"
#include "DNA_scene_types.h"

#include "BKE_brush.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_curve_patch_object.hh"
#include "BKE_paint_types.hh"
#include "BKE_paint_vector.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "ED_curve_patch.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "mesh/sculpt_intern.hh"

#include "paint_curve_patch_apply.hh"
#include "paint_curve_patch_document.hh"
#include "paint_curve_patch_layer_object_source_intern.hh"
#include "paint_curve_patch_session.hh"
#include "paint_image_curve_patch.hh"

namespace blender::ed::sculpt_paint {
/** The live 3D layer session the object source refresh addresses, or null: the session edits
 * \a row when it was entered from it (#curve_patch_layer_attach_row) and still lives on its
 * owner's sculpt session. Sessions are editor-owned state on objects, so the search walks the
 * database's objects of \a bmain -- the main the refresh scanned, not a global: a refresh can run
 * against another file's main (a background job). The refresh fires rarely (curve edits), not per
 * frame. */
static CurvePatchSession *curve_patch_layer_object_source_session_find(
    Main &bmain, const Material &ma, const MaterialPaintLayer &row)
{
  for (Object &object : bmain.objects) {
    if (object.runtime == nullptr || object.runtime->sculpt_session == nullptr) {
      continue;
    }
    CurvePatchSession *session = object.runtime->sculpt_session->curve_patch_session;
    if (session == nullptr || !session->is_layer_effect()) {
      continue;
    }
    if (session->layer_material != &ma || !BLI_uuid_equal(session->layer_row_marker, row.marker)) {
      continue;
    }
    return session;
  }
  return nullptr;
}

/** The first object of \a bmain that uses \a ma: the painted object a row without a session maps
 * the object-backed curves against, and the one the detach falls back to. \a r_stamp_object, when
 * given, receives the first mesh user with a UV map.
 *
 * A mesh user with a UV map qualifies as a stamp host regardless of its mode: what the host
 * otherwise requires -- a Sculpt session, a Paint BVH, free stroke slots -- the stamp itself
 * provides or swaps for the duration of its synchronous run (#curve_patch_apply_with_status), so
 * a host the user never sculpted on and one whose Curve Patch session is open right now both
 * restamp instead of leaving the row waiting. Both outputs are null when no user qualifies. */
static Object *curve_patch_layer_material_user_find(Main &bmain,
                                                    const Material &ma,
                                                    Object **r_stamp_object)
{
  Object *first_user = nullptr;
  if (r_stamp_object != nullptr) {
    *r_stamp_object = nullptr;
  }
  for (Object &candidate : bmain.objects) {
    bool uses_ma = false;
    for (const int slot : IndexRange(candidate.totcol)) {
      if (BKE_object_material_get(&candidate, short(slot + 1)) == &ma) {
        uses_ma = true;
        break;
      }
    }
    if (!uses_ma) {
      continue;
    }
    if (first_user == nullptr) {
      first_user = &candidate;
    }
    const bool want_stamp = r_stamp_object != nullptr && *r_stamp_object == nullptr;
    if (want_stamp && candidate.type == OB_MESH && curve_patch_layer_object_has_uv(candidate)) {
      *r_stamp_object = &candidate;
    }
    if (first_user != nullptr && (r_stamp_object == nullptr || *r_stamp_object != nullptr)) {
      break;
    }
  }
  return first_user;
}

/* The stored curve of every object-backed item follows its object: the fallback the file and the
 * container readers use while the object is gone. Read through the evaluated copies, the same
 * shape the refresh just hashed.
 *
 * Writing the evaluated result into the item's own data is deliberate, not a mistake of the
 * post-evaluation refresh: the item has to keep the shape the graph resolved even where no
 * session is around to re-read it. */
static void curve_patch_layer_object_stored_geometry_refresh(PaintVector &pv,
                                                             const Object &painted_object,
                                                             const Depsgraph *depsgraph)
{
  for (const int item_i : IndexRange(pv.items_num)) {
    PaintVectorItem &item = pv.items[item_i];
    if (item.type != PAINT_VECTOR_ITEM_CURVE_PATCH || item.curve_object == nullptr) {
      continue;
    }
    bke::CurvesGeometry curve;
    if (curve_patch_item_object_curve(item, painted_object, depsgraph, curve)) {
      item.curve_patch_geometry.wrap() = std::move(curve);
    }
  }
}

/* What the passive stamp reads from the scene it runs in: the tool settings whose paint state the
 * stamp borrows, and the row's snapshot brush it stamps with. Null members mean the stamp cannot
 * run (see #curve_patch_layer_object_stamp_context_get). */
struct CurvePatchPassiveStampContext {
  Scene *scene = nullptr;
  Sculpt *sd = nullptr;
  Brush *snapshot = nullptr;
};

static std::optional<CurvePatchPassiveStampContext> curve_patch_layer_object_stamp_context_get(
    const Depsgraph &depsgraph, const MaterialPaintLayer &row)
{
  CurvePatchPassiveStampContext context;
  context.scene = DEG_get_input_scene(&depsgraph);
  context.sd = (context.scene != nullptr && context.scene->toolsettings != nullptr) ?
                   context.scene->toolsettings->sculpt :
                   nullptr;
  if (context.sd == nullptr) {
    return std::nullopt;
  }
  context.snapshot = row.curve_patch->brush;
  if (context.snapshot == nullptr || !bke::brush::supports_curve_patch(*context.snapshot) ||
      ID_IS_LINKED(&context.snapshot->id))
  {
    /* The stamp has no tool to run (the same refusals a session entry reports, silent here: the
     * refresh is a background-scale path and the user is not waiting on it). */
    return std::nullopt;
  }
  if (context.snapshot->material_paint == nullptr) {
    BKE_brush_material_paint_ensure(context.snapshot);
  }
  if (!BKE_paint_can_use_brush(&context.sd->paint, context.snapshot)) {
    context.snapshot->ob_mode |= OB_MODE_SCULPT;
  }
  return context;
}

/* Restores the paint targets this file aims at the rebuilt row before a passive stamp and puts
 * the user's picks back afterwards. Construction is the only place the save happens, so a later
 * early return cannot leave the picks moved. */
struct PassiveStampScope {
  Object &stamp_object;
  Material &ma;
  PaintModeSettings &paint_mode;
  Sculpt &sd;
  const short actcol;
  const bUUID active_row;
  const ePaintCanvasSource canvas_source;
  const int8_t layer_target_mode;
  Brush *const brush;
  const float overlap_factor;

  PassiveStampScope(Object &stamp_object,
                    Material &ma,
                    PaintModeSettings &paint_mode,
                    Sculpt &sd,
                    Brush &snapshot,
                    const int slot,
                    const bUUID &row_marker,
                    const bool mask_role)
      : stamp_object(stamp_object),
        ma(ma),
        paint_mode(paint_mode),
        sd(sd),
        actcol(stamp_object.actcol),
        active_row(ma.active_layer_marker),
        canvas_source(paint_mode.canvas_source),
        layer_target_mode(paint_mode.layer_target_mode),
        brush(sd.paint.brush),
        overlap_factor(sd.paint.runtime->overlap_factor)
  {
    stamp_object.actcol = slot;
    /* Unconditional, nil included: a material with no active row before the stamp must come back
     * with none, not with the row this stamp aimed at. */
    BKE_paint_layers_active_set(ma, row_marker);
    paint_mode.canvas_source = PAINT_CANVAS_SOURCE_MATERIAL;
    paint_mode.layer_target_mode = mask_role ? PAINT_LAYER_TARGET_MASK : PAINT_LAYER_TARGET_CONTENT;
    /* A plain pointer swap: the apply and the symmetry machinery read the active brush, and the
     * row's snapshot is the tool this stamp runs. The script path runs the same machinery on
     * whatever brush is active, so nothing beyond the pointer is needed. */
    sd.paint.brush = &snapshot;
  }
  ~PassiveStampScope()
  {
    /* Reverse order, so every pick is back where it was before the next one moves. */
    sd.paint.runtime->overlap_factor = overlap_factor;
    sd.paint.brush = brush;
    paint_mode.canvas_source = canvas_source;
    paint_mode.layer_target_mode = layer_target_mode;
    BKE_paint_layers_active_set(ma, active_row);
    stamp_object.actcol = actcol;
  }
};

/* Reset the maps the stamp is about to write: they hold the previous render of these very curves,
 * and a moved curve would otherwise leave the old result under it. The same target resolution a
 * session entry resets through. */
static bool curve_patch_layer_object_maps_reset(Object &stamp_object,
                                                Material &ma,
                                                MaterialPaintLayer &row,
                                                PaintModeSettings &paint_mode,
                                                Sculpt &sd,
                                                Brush &snapshot)
{
  bool maps_reset = false;
  for (const paint::image::ImagePaintTarget &target : paint::image::init_image_paint_targets(
           stamp_object, paint_mode, &snapshot, sd.paint.visible_material_channels))
  {
    if (!target.is_material_channel) {
      continue;
    }
    maps_reset |= curve_patch_layer_row_map_reset(ma, row, int(target.channel));
  }
  return maps_reset;
}

/**
 * The passive rebuild the source refresh drives for a row without a live session (the hook's
 * other branch): the row's stored curves follow the objects, and its maps are re-stamped from
 * them once, so a commit of a foreign session or an external edit of a shared curve object
 * reaches every effect that reads it without entering its tool. Read through the same converters
 * the session entry publishes with, so the stamp is exactly what the next session would stamp.
 *
 * The re-stamp runs through the headless one-shot apply on the first qualifying material user,
 * with the paint targets temporarily aimed at \a row (active slot, active row, Material canvas,
 * snapshot brush) and everything put back afterwards. It records no undo step (the commit's own
 * "Curve Patch Layer Edit" memfile step covers the state the rebuild lands, and a refresh runs
 * outside every undo transaction). No object is written and no expected hash is registered: the
 * rebuild cannot echo back through the refresh.
 *
 * Returns a status instead of returning silently: the refresh hook keeps a row whose re-stamp is
 * merely #CurvePatchLayerRestampStatus::Blocked pending (a later refresh or the end of the
 * session that occupies the host retries it), and drops a row whose refusal is
 * #CurvePatchLayerRestampStatus::Unsupported (a real change of the curves arms it again).
 */
static CurvePatchLayerRestampStatus curve_patch_layer_object_source_rebuild_passive(
    Main &bmain, Material &ma, MaterialPaintLayer &row, const Depsgraph *depsgraph)
{
  MaterialPaintLayerCurvePatch &payload = *row.curve_patch;
  if (payload.vectors == nullptr || !BKE_paint_layers_curve_patch_reads_object(&payload)) {
    return CurvePatchLayerRestampStatus::Unsupported;
  }
  PaintVector &pv = *payload.vectors;

  Object *stamp_object = nullptr;
  Object *painted = curve_patch_layer_material_user_find(bmain, ma, &stamp_object);
  if (painted == nullptr) {
    /* No object to map the objects' splines through: the items' stored geometry is the only
     * shape there is, the same answer the detach falls back to. */
    return CurvePatchLayerRestampStatus::Unsupported;
  }

  curve_patch_layer_object_stored_geometry_refresh(pv, *painted, depsgraph);

  if (stamp_object == nullptr || depsgraph == nullptr) {
    /* No mesh user with a UV map to stamp through, or no graph to read the evaluated curves
     * with: the bake the refresh requested recomposes the row's current maps, and the next
     * session entry re-stamps from the objects. Neither recovers on its own, so the row stops
     * asking (a curve change re-arms it through the refresh's own CHANGED verdict). */
    return CurvePatchLayerRestampStatus::Unsupported;
  }
  const std::optional<CurvePatchPassiveStampContext> context =
      curve_patch_layer_object_stamp_context_get(*depsgraph, row);
  if (!context) {
    return CurvePatchLayerRestampStatus::Unsupported;
  }
  Scene *scene = context->scene;
  Sculpt *sd = context->sd;
  Brush *snapshot = context->snapshot;

  CurvePatchDocument doc;
  curve_patch_publish_document(payload, *stamp_object, depsgraph, doc);
  const bool any_visible = std::any_of(
      doc.patches.begin(), doc.patches.end(), [](const CurvePatchItem &item) {
        return !item.params.hidden && item.control_curve.points_num() >= 2;
      });
  if (!any_visible) {
    return CurvePatchLayerRestampStatus::Unsupported;
  }
  /* The apply takes the curves and their frozen parameters as two parallel spans: copies, since
   * the document's patches own both. */
  Vector<bke::CurvesGeometry> curves;
  Vector<bke::CurvePatchParams> params;
  curves.reserve(doc.patches.size());
  params.reserve(doc.patches.size());
  for (const CurvePatchItem &patch : doc.patches) {
    curves.append(patch.control_curve);
    params.append(patch.params);
  }

  /* The one-shot stamp resolves its canvases through the object's active material slot and the
   * material's active row, exactly like a session entry: aim both at the rebuilt row. The scope
   * puts the user's picks back on every return; constructed after the last refusal, so it never
   * has to undo a save that never happened. */
  const int slot = BKE_object_material_slot_find_index(stamp_object, &ma);
  if (slot == 0) {
    return CurvePatchLayerRestampStatus::Unsupported;
  }
  PaintModeSettings &paint_mode = scene->toolsettings->paint_mode;
  PassiveStampScope stamp_scope(*stamp_object,
                                 ma,
                                 paint_mode,
                                 *sd,
                                 *snapshot,
                                 slot,
                                 row.marker,
                                 row.role == MA_PAINT_LAYER_ROLE_MASK_ITEM);

  const bool maps_reset = curve_patch_layer_object_maps_reset(
      *stamp_object, ma, row, paint_mode, *sd, *snapshot);

  CurvePatchApplyInput input;
  input.stamp_textures_disabled = row.role == MA_PAINT_LAYER_ROLE_MASK_ITEM;
  const CurvePatchApplyStatus stamp_status = curve_patch_apply_with_status(
      *scene,
      *depsgraph,
      *stamp_object,
      *sd,
      paint_mode,
      curves,
      params,
      CurvePatchEffectType::Image,
      input,
      /*reports=*/nullptr,
      /*record_undo=*/false);
  if (stamp_status != CurvePatchApplyStatus::Done && maps_reset) {
    /* The stamp refused after the reset: the maps are empty where they held the old shape. The
     * bake the refresh requested recomposes them, and the next session entry re-stamps
     * everything -- the same failure state a session entry leaves when its start fails after the
     * reset. */
    BKE_paint_layers_bake_request(row);
  }
  if (stamp_status == CurvePatchApplyStatus::Done) {
    /* No bContext reaches the refresh: the images the stamp wrote tag themselves dirty and the
     * material the refresh tagged carries the shading, so the viewports redraw through the ID
     * tags. This only brings the UI panels up to date with it (the same call the 2D session's
     * exit makes without a context). */
    WM_main_add_notifier(NC_MATERIAL | ND_SHADING, &ma.id);
  }

  /* The apply's own refusals keep the temporary/permanent split the pending bookkeeping needs:
   * a busy host is temporary (its session will end), everything else here is not. */
  return (stamp_status == CurvePatchApplyStatus::Blocked) ? CurvePatchLayerRestampStatus::Blocked :
         (stamp_status == CurvePatchApplyStatus::Done) ? CurvePatchLayerRestampStatus::Done :
                                                         CurvePatchLayerRestampStatus::Unsupported;
}

}  // namespace blender::ed::sculpt_paint

namespace blender {

/* Keeps #CurvePatchSession::layer_object_sync_suspended raised for the duration of a scope, so a
 * return added inside it cannot leave the write-back suppressed for the rest of the session. */
struct ScopedLayerObjectSyncSuspended {
  ed::sculpt_paint::CurvePatchSession &session;
  explicit ScopedLayerObjectSyncSuspended(ed::sculpt_paint::CurvePatchSession &session)
      : session(session)
  {
    session.layer_object_sync_suspended = true;
  }
  ~ScopedLayerObjectSyncSuspended()
  {
    session.layer_object_sync_suspended = false;
  }
};

/**
 * The curve source object of \a row changed (the BKE source refresh detected it): a live session
 * of the row re-reads the object's curves and re-stamps the row's maps, so the effect follows the
 * object the way it follows its own tool. A row without a live session is rebuilt passively: its
 * stored curves follow the objects and its maps are re-stamped from them once (see
 * #curve_patch_layer_object_source_rebuild_passive), so a foreign session's commit or an external
 * edit reaches it without entering its tool. The refresh has invalidated the row's bake already;
 * the re-stamp here is what puts the new shape into the maps the bake recomposes.
 *
 * The rebuild is refused while a control point is being dragged (the drag's own commit writes the
 * document back, and a rebuild under it would tear the state apart); the refresh holds the row
 * until the release anyway (#BKE_paint_layers_curve_patch_object_drag_in_flight_set). Registered
 * with #BKE_paint_layers_curve_patch_object_changed_cb_set once, at the editors' startup
 * (#ED_curve_patch_layer_object_source_hook_register).
 *
 * Returns a status in every exit: the refresh hook keeps a #CurvePatchLayerRestampStatus::Blocked
 * row pending (a later refresh or the session end retries it) and clears a Done or Unsupported
 * one. \a force skips the drag gate -- the caller that just re-pointed a curve at an object has
 * priority over the point the user is dragging.
 *
 * The stored session context (#CurvePatchSession::view_context) drives the re-stamp: the modal
 * editor re-syncs it from the current context on every event
 * (`curve_patch_sync_view_context`), and the refresh runs on the main thread from a window
 * update, so the pointers are the live editor's -- the same stored-context use the session's own
 * flush and override helpers make.
 */
CurvePatchLayerRestampStatus ED_curve_patch_layer_row_restamp(Main &bmain,
                                                              Material &ma,
                                                              MaterialPaintLayer &row,
                                                              const Depsgraph *depsgraph,
                                                              const bool force)
{
  MaterialPaintLayer *live = BKE_paint_layers_find(ma, row.marker);
  if (live == nullptr || live != &row || live->curve_patch != row.curve_patch) {
    /* The row was removed or re-pointed between the hash scan and this call: nothing is left to
     * re-stamp, and a retry would not change that. */
    return CurvePatchLayerRestampStatus::Unsupported;
  }
  ed::sculpt_paint::CurvePatchSession *session =
      ed::sculpt_paint::curve_patch_layer_object_source_session_find(bmain, ma, row);
  if (session == nullptr || session->owner == nullptr ||
      session->owner->runtime->sculpt_session == nullptr)
  {
    /* No live session on the row: the passive rebuild. Runs on the main thread after a scene
     * update, outside every undo transaction and every session, so the one-shot stamp it drives
     * cannot collide with the user's work. */
    return ed::sculpt_paint::curve_patch_layer_object_source_rebuild_passive(
        bmain, ma, row, depsgraph);
  }
  if (!force && session->doc.active_point != -1) {
    /* A drag is in flight; its commit writes the shape back and re-stamps. Temporary: the row
     * stays pending and the next refresh (the release tags the material) retries. */
    return CurvePatchLayerRestampStatus::Blocked;
  }
  if (bke::object::pbvh_get(*session->owner) == nullptr) {
    /* The live session's own host lost its Paint BVH: the re-stamp cannot run through it now,
     * but the session is still live and a later refresh may find the BVH rebuilt. */
    return CurvePatchLayerRestampStatus::Blocked;
  }

  /* Re-publish the document from the object the refresh just hashed (the evaluated copy, so the
   * session shows exactly what the stamp reads), reset the maps the session owns and re-stamp --
   * the same sequence a session entry runs after its publish. */
  if (depsgraph == nullptr) {
    depsgraph = session->view_context.depsgraph;
  }
  ed::sculpt_paint::curve_patch_publish_document(
      *row.curve_patch, *session->owner, depsgraph, session->doc);
  SculptSession &ss = *session->owner->runtime->sculpt_session;
  if (ss.cache != nullptr) {
    for (const ed::sculpt_paint::paint::image::ImagePaintTarget &target :
         ss.cache->image_paint_targets)
    {
      if (!target.is_material_channel) {
        continue;
      }
      ed::sculpt_paint::curve_patch_layer_row_map_reset(ma, row, int(target.channel));
    }
  }
  if (session->view_context.C != nullptr) {
    /* The curves were just read from the objects: writing them back would only churn the objects
     * (and echo through the refresh). */
    ScopedLayerObjectSyncSuspended sync_suspended{*session};
    ed::sculpt_paint::curve_patch_restore_and_restamp(*session->view_context.C, *session);
    WM_event_add_notifier(session->view_context.C, NC_MATERIAL | ND_SHADING, &ma);
    return CurvePatchLayerRestampStatus::Done;
  }
  /* The session is live but holds no context to re-stamp through: the modal re-syncs the stored
   * context on its next event, so a later refresh can still get through. */
  return CurvePatchLayerRestampStatus::Blocked;
}

CurvePatchObjectSetResult ED_curve_patch_item_object_assign(Main &bmain,
                                                            PaintVector &pv,
                                                            const int item_index,
                                                            Object *object,
                                                            const int spline)
{
  if (item_index < 0 || item_index >= pv.items_num) {
    return CurvePatchObjectSetResult::NotCurvePatch;
  }
  const PaintVectorItem &item = pv.items[item_index];
  const bool clearing = object == nullptr && item.curve_object != nullptr;
  const CurvePatchObjectSetResult result = BKE_paint_layers_curve_patch_item_object_validate(
      bmain, pv, item_index, object, spline);
  if (result != CurvePatchObjectSetResult::Ok) {
    return result;
  }
  /* Releasing the curve keeps the shape it has now. Done after validation, so a refused
   * assignment cannot leave the old object detached from the curve. */
  if (clearing) {
    ED_curve_patch_layer_item_object_detach(bmain, pv, item_index);
  }
  const CurvePatchObjectSetResult set_result = BKE_paint_layers_curve_patch_item_object_set(
      bmain, pv, item_index, object, spline);
  if (set_result == CurvePatchObjectSetResult::Ok) {
    /* The live session adopts the picked curve instead of overwriting it with its own shape. */
    ED_curve_patch_layer_item_object_assigned(bmain, pv);
  }
  return set_result;
}

/** The editors hook the source refresh reaches: registered once at the editors' startup (see
 * #ED_curve_patch_layer_object_source_hook_register), since the rebuild it drives needs the
 * editor layer, which BKE must not link. */
static void curve_patch_layer_object_source_changed_cb(Main &bmain,
                                                       Material &ma,
                                                       MaterialPaintLayer &row,
                                                       const Depsgraph *depsgraph,
                                                       void * /*user_data*/)
{
  const CurvePatchLayerRestampStatus status = ED_curve_patch_layer_row_restamp(
      bmain, ma, row, depsgraph);
  /* The pending marker follows the executor's verdict: #Blocked keeps the row asking (the next
   * refresh, or the end of the session that holds the stamp host, retries it), #Done and
   * #Unsupported clear it -- a permanent refusal must not spin the retry on every update, and a
   * real change of the curves re-arms the row through the refresh's own CHANGED verdict. */
  BKE_paint_layers_curve_patch_object_restamp_pending_set(
      ma, row, status == CurvePatchLayerRestampStatus::Blocked);
}

void ED_curve_patch_layer_item_object_assigned(Main &bmain, PaintVector &pv)
{
  const CurvePatchVectorOwner owner = BKE_paint_layers_curve_patch_row_find_by_vector(bmain, pv);
  if (owner.row != nullptr) {
    /* Forced: the user just picked the curve, so a point being selected is no reason to keep the
     * previous shape in the document. */
    ED_curve_patch_layer_row_restamp(bmain, *owner.material, *owner.row, nullptr, true);
  }
}

void ED_curve_patch_layer_item_object_detach(Main &bmain, PaintVector &pv, const int item_index)
{
  if (item_index < 0 || item_index >= pv.items_num) {
    return;
  }
  PaintVectorItem &item = pv.items[item_index];
  if (item.type != PAINT_VECTOR_ITEM_CURVE_PATCH || item.curve_object == nullptr) {
    return;
  }
  const CurvePatchVectorOwner owner = BKE_paint_layers_curve_patch_row_find_by_vector(bmain, pv);
  if (owner.row == nullptr) {
    return;
  }
  Material *ma = owner.material;
  MaterialPaintLayer *row = owner.row;

  /* A live session already holds the shape in the owner's space: take it from there. */
  if (ed::sculpt_paint::CurvePatchSession *session =
          ed::sculpt_paint::curve_patch_layer_object_source_session_find(bmain, *ma, *row))
  {
    for (const ed::sculpt_paint::CurvePatchItem &patch : session->doc.patches) {
      if (patch.source_curve_index == item_index) {
        item.curve_patch_geometry.wrap() = patch.control_curve;
        return;
      }
    }
  }

  /* Otherwise the object's spline, mapped into the space of an object that uses the material (the
   * space the container's curves live in). */
  Object *painted = ed::sculpt_paint::curve_patch_layer_material_user_find(bmain, *ma, nullptr);
  if (painted == nullptr) {
    return;
  }
  bke::CurvesGeometry curve;
  if (ed::sculpt_paint::curve_patch_item_object_curve(item, *painted, nullptr, curve)) {
    item.curve_patch_geometry.wrap() = std::move(curve);
  }
}

void ED_curve_patch_layer_object_source_hook_register()
{
  /* Idempotent: the startup runs once, and a re-registration would only reset the same pair. */
  BKE_paint_layers_curve_patch_object_changed_cb_set(curve_patch_layer_object_source_changed_cb,
                                                     nullptr);
}

}  // namespace blender
