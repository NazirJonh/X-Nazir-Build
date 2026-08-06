/* SPDX-FileCopyrightText: 2008 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spview3d
 */

#include "AS_asset_representation.hh"

#include "BKE_asset.hh"
#include "BKE_context.hh"
#include "BKE_global.hh"
#include "BKE_idprop.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_lib_remap.hh"
#include "BKE_library.hh"
#include "BKE_object.hh"

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_map.hh"
#include "BLI_math_matrix.h"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.h"

#include "BLT_translation.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"

#include "DNA_ID.h"
#include "DNA_collection_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"

#include "ED_object.hh"
#include "ED_outliner.hh"
#include "ED_screen.hh"
#include "ED_undo.hh"
#include "ED_view3d.hh"

#include "UI_interface_c.hh"
#include "UI_resources.hh"

#include "RNA_access.hh"

#include "WM_api.hh"
#include "WM_types.hh"
#include "wm_event_types.hh"

#include <memory>

#include "view3d_intern.hh" /* own include */
#include "view3d_sculpt_drop_preview.hh"

namespace blender {

static bool view3d_drop_in_main_region_poll(bContext *C, const wmEvent *event)
{
  ScrArea *area = CTX_wm_area(C);
  return ED_region_overlap_isect_any_xy(area, event->xy) == false;
}

static ID_Type view3d_drop_id_in_main_region_poll_get_id_type(bContext *C,
                                                              wmDrag *drag,
                                                              const wmEvent *event)
{
  const ScrArea *area = CTX_wm_area(C);

  if (ED_region_overlap_isect_any_xy(area, event->xy)) {
    return ID_Type(0);
  }
  if (!view3d_drop_in_main_region_poll(C, event)) {
    return ID_Type(0);
  }

  ID *local_id = WM_drag_get_local_ID(drag, 0);
  if (local_id) {
    return GS(local_id->name);
  }

  wmDragAsset *asset_drag = WM_drag_get_asset_data(drag, 0);
  if (asset_drag) {
    return asset_drag->asset->get_id_type();
  }

  return ID_Type(0);
}

static bool view3d_drop_id_in_main_region_poll(bContext *C,
                                               wmDrag *drag,
                                               const wmEvent *event,
                                               ID_Type id_type)
{
  if (!view3d_drop_in_main_region_poll(C, event)) {
    return false;
  }

  return WM_drag_is_ID_type(drag, id_type);
}

static V3DSnapCursorState *view3d_drop_snap_init(wmDropBox *drop)
{
  V3DSnapCursorState *state = static_cast<V3DSnapCursorState *>(drop->draw_data);
  if (state) {
    return state;
  }

  state = ED_view3d_cursor_snap_state_create();
  drop->draw_data = state;
  state->draw_plane = true;
  return state;
}

static void view3d_drop_snap_exit(wmDropBox *drop, wmDrag * /*drag*/)
{
  V3DSnapCursorState *state = static_cast<V3DSnapCursorState *>(drop->draw_data);
  if (state) {
    ED_view3d_cursor_snap_state_free(state);
    drop->draw_data = nullptr;
  }
}

/* -------------------------------------------------------------------- */
/** \name Sculpt-Mode Drop State (scale / rotate during hover)
 * \{ */

/**
 * Per-drop interactive state for the sculpt-mode dropboxes.
 * Stored in #wmDropBox::draw_data (replaces the raw #V3DSnapCursorState pointer used by the
 * Object-Mode boxes so that we can carry extra interaction state alongside the snap cursor).
 */
using SculptDropData = ed::view3d::sculpt_drop_preview::SculptDropData;

/**
 * Placement-option toggles (J/F/L/M, see #view3d_sculpt_on_event_while_hover) carry over from one
 * drop to the next within the running session: kept here rather than in #SculptDropData, which is
 * freed and recreated for every new drag.
 */
struct SculptDropFlags {
  bool join_to_active = true;
  bool replace_face_sets = false;
  bool linked = true;
  bool apply_mask = false;
};
static SculptDropFlags sculpt_drop_flags;

/** Trivial: #WM_paint_cursor_activate already gates by space/region type, and the cursor's
 * lifetime is scoped to this specific drop (activated in on_enter, ended in on_exit). */
static bool sculpt_drop_preview_cursor_poll(bContext * /*C*/)
{
  return true;
}

static SculptDropData *sculpt_drop_data_create(wmDropBox *drop)
{
  SculptDropData *data = MEM_new<SculptDropData>(__func__);
  data->snap_state = ED_view3d_cursor_snap_state_create();
  data->snap_state->draw_plane = true;
  copy_v3_fl(data->base_box_dims, 1.0f);
  data->scale_factor = 1.0f;
  data->is_scaling = false;
  data->scale_start_mval_x = 0;
  data->scale_start_factor = 1.0f;
  data->rotation_angle = 0.0f;
  data->is_rotating = false;
  data->rotate_start_mval_x = 0;
  data->rotate_start_angle = 0.0f;
  data->join_to_active = sculpt_drop_flags.join_to_active;
  data->replace_face_sets = sculpt_drop_flags.replace_face_sets;
  data->linked = sculpt_drop_flags.linked;
  data->apply_mask = sculpt_drop_flags.apply_mask;
  data->preview_cursor = WM_paint_cursor_activate(
      SPACE_VIEW3D,
      RGN_TYPE_WINDOW,
      sculpt_drop_preview_cursor_poll,
      ed::view3d::sculpt_drop_preview::preview_draw_paint_cursor,
      data);
  drop->draw_data = data;
  return data;
}

static void sculpt_drop_data_free(wmDropBox *drop)
{
  SculptDropData *data = static_cast<SculptDropData *>(drop->draw_data);
  if (data) {
    if (data->preview_cursor) {
      WM_paint_cursor_end(data->preview_cursor);
      data->preview_cursor = nullptr;
    }
    ed::view3d::sculpt_drop_preview::preview_runtime_clear(data->preview);
    data->preview.active_job.reset();
    ED_view3d_cursor_snap_state_free(data->snap_state);
    MEM_delete(data);
    drop->draw_data = nullptr;
  }
}

/** Poll-time context for sculpt preview (on_enter lacks #bContext; set in poll, read on enter). */
struct SculptDropPreviewEnterContext {
  Main *real_main = nullptr;
  uint winid = 0;
  Depsgraph *depsgraph = nullptr;
};

/**
 * Keyed by the #wmDrag this poll/enter/exit sequence belongs to (stable for the drag's lifetime),
 * so concurrent drags hovering different windows/dropboxes each get their own captured context
 * instead of clobbering a single process-wide slot.
 */
static Map<const wmDrag *, SculptDropPreviewEnterContext> sculpt_drop_preview_enter_ctx_map;

/** Process-wide; bumped on each new preview start and on sculpt drop exit. */
static uint64_t sculpt_drop_preview_generation_counter = 0;

static void sculpt_drop_preview_capture_enter_context(bContext *C, const wmDrag *drag)
{
  SculptDropPreviewEnterContext ctx;
  ctx.real_main = CTX_data_main(C);
  ctx.depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  if (wmWindow *win = CTX_wm_window(C)) {
    ctx.winid = win->winid;
  }
  sculpt_drop_preview_enter_ctx_map.add_overwrite(drag, ctx);
}

static SculptDropPreviewEnterContext sculpt_drop_preview_enter_ctx_lookup(const wmDrag *drag)
{
  if (const SculptDropPreviewEnterContext *ctx = sculpt_drop_preview_enter_ctx_map.lookup_ptr(
          drag))
  {
    return *ctx;
  }
  return {};
}

/** Starts async external preview load (on-disk assets only; skips local and online-only). */
static std::shared_ptr<ed::view3d::sculpt_drop_preview::PreviewJobState>
sculpt_drop_external_preview_job_start(wmDrag *drag,
                                       const SculptDropPreviewEnterContext &enter_ctx)
{
  if (drag->type != WM_DRAG_ASSET) {
    return nullptr;
  }

  wmDragAsset *asset_drag = WM_drag_get_asset_data(drag, 0);
  if (!asset_drag || !asset_drag->asset) {
    return nullptr;
  }

  const asset_system::AssetRepresentation *asset = asset_drag->asset;
  const bool is_local = asset->is_local_id();
  const bool is_online = asset->is_online_only();
  if (is_local || is_online) {
    return nullptr;
  }

  Main *real_main = enter_ctx.real_main;
  if (real_main == nullptr) {
    return nullptr;
  }
  wmWindowManager *wm = static_cast<wmWindowManager *>(real_main->wm.first);
  if (!wm) {
    return nullptr;
  }

  auto state = std::make_shared<ed::view3d::sculpt_drop_preview::PreviewJobState>();
  state->blend_filepath = asset->full_library_path();
  state->idcode = short(asset->get_id_type());
  state->idname = asset->get_name();
  state->is_collection = state->idcode == ID_GR;
  state->real_main = real_main;
  state->winid = enter_ctx.winid;
  state->job_owner = drag;
  state->generation = ++sculpt_drop_preview_generation_counter;
  state->accepting.store(true);
  state->ready.store(false);
  state->load_error.store(false);

  wmWindow *win = nullptr;
  for (wmWindow &w : wm->windows) {
    if (w.winid == state->winid) {
      win = &w;
      break;
    }
  }

  ed::view3d::sculpt_drop_preview::preview_job_start(*wm, win, state);
  return state;
}

static void view3d_sculpt_ob_drop_on_enter(wmDropBox *drop, wmDrag *drag)
{
  if (WM_drag_asset_will_import_linked(drag)) {
    return;
  }

  const SculptDropPreviewEnterContext enter_ctx = sculpt_drop_preview_enter_ctx_lookup(drag);

  SculptDropData *data = sculpt_drop_data_create(drop);
  data->preview.redraw_winid = enter_ctx.winid;
  V3DSnapCursorState *state = data->snap_state;

  float dimensions[3] = {0.0f};
  if (drag->type == WM_DRAG_ID) {
    Object *ob = id_cast<Object *>(WM_drag_get_local_ID(drag, ID_OB));
    if (ob) {
      BKE_object_dimensions_eval_cached_get(ob, dimensions);
    }
  }
  else {
    AssetMetaData *meta_data = WM_drag_get_asset_meta_data(drag, ID_OB);
    if (meta_data) {
      IDProperty *dimensions_prop = BKE_asset_metadata_idprop_find(meta_data, "dimensions");
      if (dimensions_prop) {
        copy_v3_v3(dimensions, IDP_array_float_get(dimensions_prop));
      }
    }
  }

  if (!is_zero_v3(dimensions)) {
    mul_v3_v3fl(state->box_dimensions, dimensions, 0.5f);
    copy_v3_v3(data->base_box_dims, state->box_dimensions);
    ui::theme::get_color_4ubv(TH_GIZMO_PRIMARY, state->color_box);
    state->draw_box = true;
  }

  if (std::shared_ptr<ed::view3d::sculpt_drop_preview::PreviewJobState> active_job =
          sculpt_drop_external_preview_job_start(drag, enter_ctx))
  {
    data->preview.active_job = std::move(active_job);
    data->preview.generation = data->preview.active_job->generation;
  }
  else if (drag->type == WM_DRAG_ID) {
    if (Object *ob = id_cast<Object *>(WM_drag_get_local_ID(drag, ID_OB))) {
      if (Depsgraph *depsgraph = enter_ctx.depsgraph) {
        ed::view3d::sculpt_drop_preview::preview_build_local_object(
            data->preview, *depsgraph, *ob);
      }
    }
  }
}

static void view3d_sculpt_collection_drop_on_enter(wmDropBox *drop, wmDrag *drag)
{
  if (WM_drag_asset_will_import_linked(drag)) {
    return;
  }

  const SculptDropPreviewEnterContext enter_ctx = sculpt_drop_preview_enter_ctx_lookup(drag);

  SculptDropData *data = sculpt_drop_data_create(drop);
  data->preview.redraw_winid = enter_ctx.winid;
  V3DSnapCursorState *state = data->snap_state;

  /* Accumulate the largest extents across all direct mesh objects in the collection. */
  float bbox_extents[3] = {0.0f};
  bool has_dims = false;

  if (drag->type == WM_DRAG_ID) {
    Collection *collection = id_cast<Collection *>(WM_drag_get_local_ID(drag, ID_GR));
    if (collection) {
      for (CollectionObject *cob = static_cast<CollectionObject *>(collection->gobject.first); cob;
           cob = cob->next)
      {
        if (!cob->ob || cob->ob->type != OB_MESH) {
          continue;
        }
        float dims[3] = {0.0f};
        BKE_object_dimensions_eval_cached_get(cob->ob, dims);
        if (!is_zero_v3(dims)) {
          has_dims = true;
          for (int i = 0; i < 3; i++) {
            bbox_extents[i] = max_ff(bbox_extents[i], dims[i]);
          }
        }
      }
    }
  }
  else {
    AssetMetaData *meta_data = WM_drag_get_asset_meta_data(drag, ID_GR);
    if (meta_data) {
      IDProperty *dimensions_prop = BKE_asset_metadata_idprop_find(meta_data, "dimensions");
      if (dimensions_prop) {
        copy_v3_v3(bbox_extents, IDP_array_float_get(dimensions_prop));
        has_dims = !is_zero_v3(bbox_extents);
      }
    }
  }

  if (has_dims) {
    mul_v3_v3fl(state->box_dimensions, bbox_extents, 0.5f);
    copy_v3_v3(data->base_box_dims, state->box_dimensions);
    ui::theme::get_color_4ubv(TH_GIZMO_PRIMARY, state->color_box);
    state->draw_box = true;
  }

  if (std::shared_ptr<ed::view3d::sculpt_drop_preview::PreviewJobState> active_job =
          sculpt_drop_external_preview_job_start(drag, enter_ctx))
  {
    data->preview.active_job = std::move(active_job);
    data->preview.generation = data->preview.active_job->generation;
  }
  else if (drag->type == WM_DRAG_ID) {
    if (Collection *collection = id_cast<Collection *>(WM_drag_get_local_ID(drag, ID_GR))) {
      if (Depsgraph *depsgraph = enter_ctx.depsgraph) {
        ed::view3d::sculpt_drop_preview::preview_build_local_collection(
            data->preview, *depsgraph, *collection);
      }
    }
  }
}

static void view3d_sculpt_drop_on_exit(wmDropBox *drop, wmDrag *drag)
{
  sculpt_drop_preview_generation_counter++;

  Main *bmain = nullptr;

  if (SculptDropData *data = static_cast<SculptDropData *>(drop->draw_data)) {
    if (data->preview.active_job) {
      data->preview.active_job->accepting.store(false);
      bmain = data->preview.active_job->real_main;
    }
  }

  if (bmain == nullptr) {
    if (const SculptDropPreviewEnterContext *ctx = sculpt_drop_preview_enter_ctx_map.lookup_ptr(
            drag))
    {
      bmain = ctx->real_main;
    }
  }
  if (bmain == nullptr) {
    bmain = G_MAIN;
  }
  if (wmWindowManager *wm = static_cast<wmWindowManager *>(bmain->wm.first)) {
    ed::view3d::sculpt_drop_preview::preview_job_request_stop(*wm, drag);
  }
  sculpt_drop_preview_enter_ctx_map.remove(drag);
  sculpt_drop_data_free(drop);
}

/**
 * Handles keyboard transforms and placement-option toggles during sculpt-mode asset hover.
 * Called for every event while the sculpt drop-box is active (poll passes).
 *
 * W/S: scale down/up by 5% per key press.
 * E/R: rotate left/right by 5 degrees per key press around the surface normal.
 * J: toggle Join to Active.
 * F: toggle Replace Face Sets (only relevant while Join to Active is on).
 * L: toggle Linked (only relevant for local assets placed as a separate object).
 * M: toggle Mask — when on (default), pre-existing geometry on the active mesh is masked so only
 *    the dropped-in geometry stays sculptable; when off, the active mesh's existing mask is left
 *    untouched entirely.
 * ESC: resets scale and rotation to their defaults (placement-option toggles are left as-is).
 *
 * Placement options are decided here, before the drop runs, rather than via the operator's redo
 * panel afterward: by the time #copy hands them to #SCULPT_OT_mesh_asset_drop, the operator only
 * ever runs #wmOperatorType.exec once, so there is no dependency on being able to re-run exec()
 * with a `session_uid`/`collection_uid` that may no longer resolve after the first run already
 * joined, duplicated, or deleted the referenced object/collection.
 */
static void view3d_sculpt_on_event_while_hover(bContext *C,
                                               wmDropBox &dropbox,
                                               const wmEvent *event)
{
  SculptDropData *data = static_cast<SculptDropData *>(dropbox.draw_data);
  if (!data) {
    return;
  }
  WorkspaceStatus status(C);
  status.item(IFACE_("Scale Down"), ui::icon_from_event_type(EVT_WKEY, KM_PRESS));
  status.item(IFACE_("Scale Up"), ui::icon_from_event_type(EVT_SKEY, KM_PRESS));
  status.item(IFACE_("Rotate Left"), ui::icon_from_event_type(EVT_EKEY, KM_PRESS));
  status.item(IFACE_("Rotate Right"), ui::icon_from_event_type(EVT_RKEY, KM_PRESS));
  status.item_bool(IFACE_("Join to Active"),
                   data->join_to_active,
                   ui::icon_from_event_type(EVT_JKEY, KM_PRESS));
  if (data->join_to_active) {
    status.item_bool(IFACE_("Replace Face Sets"),
                     data->replace_face_sets,
                     ui::icon_from_event_type(EVT_FKEY, KM_PRESS));
  }
  else {
    status.item_bool(IFACE_("Linked"), data->linked, ui::icon_from_event_type(EVT_LKEY, KM_PRESS));
  }
  status.item_bool(IFACE_("Mask"), data->apply_mask, ui::icon_from_event_type(EVT_MKEY, KM_PRESS));
  ed::view3d::sculpt_drop_preview::preview_try_pull(data->preview);
  V3DSnapCursorState *state = data->snap_state;

  switch (event->type) {
    case EVT_WKEY:
      if (event->val == KM_PRESS) {
        data->scale_factor = max_ff(0.01f, data->scale_factor * 0.95f);
        mul_v3_v3fl(state->box_dimensions, data->base_box_dims, data->scale_factor);
        ED_region_tag_redraw(CTX_wm_region(C));
      }
      break;
    case EVT_SKEY: {
      if (event->val == KM_PRESS) {
        data->scale_factor = max_ff(0.01f, data->scale_factor * 1.05f);
        mul_v3_v3fl(state->box_dimensions, data->base_box_dims, data->scale_factor);
        ED_region_tag_redraw(CTX_wm_region(C));
      }
      break;
    }
    case EVT_EKEY:
      if (event->val == KM_PRESS) {
        data->rotation_angle -= float(M_PI) / 36.0f;
        ED_region_tag_redraw(CTX_wm_region(C));
      }
      break;
    case EVT_RKEY: {
      if (event->val == KM_PRESS) {
        data->rotation_angle += float(M_PI) / 36.0f;
        ED_region_tag_redraw(CTX_wm_region(C));
      }
      break;
    }
    case EVT_ESCKEY: {
      if (event->val == KM_PRESS) {
        data->is_scaling = false;
        data->is_rotating = false;
        data->scale_factor = 1.0f;
        data->rotation_angle = 0.0f;
        copy_v3_v3(state->box_dimensions, data->base_box_dims);
        ED_region_tag_redraw(CTX_wm_region(C));
      }
      break;
    }
    case EVT_JKEY: {
      if (event->val == KM_PRESS) {
        data->join_to_active = !data->join_to_active;
        sculpt_drop_flags.join_to_active = data->join_to_active;
      }
      break;
    }
    case EVT_FKEY: {
      if (event->val == KM_PRESS && data->join_to_active) {
        data->replace_face_sets = !data->replace_face_sets;
        sculpt_drop_flags.replace_face_sets = data->replace_face_sets;
      }
      break;
    }
    case EVT_LKEY: {
      if (event->val == KM_PRESS && !data->join_to_active) {
        data->linked = !data->linked;
        sculpt_drop_flags.linked = data->linked;
      }
      break;
    }
    case EVT_MKEY: {
      if (event->val == KM_PRESS) {
        data->apply_mask = !data->apply_mask;
        sculpt_drop_flags.apply_mask = data->apply_mask;
      }
      break;
    }
    default:
      break;
  }
}

static bool view3d_sculpt_on_event_while_hover_handled(bContext * /*C*/,
                                                       wmDropBox &dropbox,
                                                       const wmEvent *event)
{
  const SculptDropData *data = static_cast<const SculptDropData *>(dropbox.draw_data);
  if (!data) {
    return false;
  }

  /* Consume transform and placement-option keys so Sculpt keymaps cannot treat them as brush or
   * tool shortcuts. */
  if (ELEM(event->type,
           EVT_WKEY,
           EVT_SKEY,
           EVT_EKEY,
           EVT_RKEY,
           EVT_JKEY,
           EVT_FKEY,
           EVT_LKEY,
           EVT_MKEY))
  {
    return true;
  }
  return false;
}

/** \} */

static void view3d_ob_drop_on_enter(wmDropBox *drop, wmDrag *drag)
{
  /* Don't use the snap cursor when linking the object. Object transform isn't editable then and
   * would be reset on reload. */
  if (WM_drag_asset_will_import_linked(drag)) {
    return;
  }

  V3DSnapCursorState *state = view3d_drop_snap_init(drop);

  float dimensions[3] = {0.0f};
  if (drag->type == WM_DRAG_ID) {
    Object *ob = id_cast<Object *>(WM_drag_get_local_ID(drag, ID_OB));
    BKE_object_dimensions_eval_cached_get(ob, dimensions);
  }
  else {
    AssetMetaData *meta_data = WM_drag_get_asset_meta_data(drag, ID_OB);
    IDProperty *dimensions_prop = BKE_asset_metadata_idprop_find(meta_data, "dimensions");
    if (dimensions_prop) {
      copy_v3_v3(dimensions, IDP_array_float_get(dimensions_prop));
    }
  }

  if (!is_zero_v3(dimensions)) {
    mul_v3_v3fl(state->box_dimensions, dimensions, 0.5f);
    ui::theme::get_color_4ubv(TH_GIZMO_PRIMARY, state->color_box);
    state->draw_box = true;
  }
}

static bool view3d_ob_drop_poll(bContext *C, wmDrag *drag, const wmEvent *event)
{
  return view3d_drop_id_in_main_region_poll(C, drag, event, ID_OB);
}
static bool view3d_ob_drop_poll_external_asset(bContext *C, wmDrag *drag, const wmEvent *event)
{
  if (!view3d_ob_drop_poll(C, drag, event) || (drag->type != WM_DRAG_ASSET)) {
    return false;
  }
  /* In Sculpt Mode with a mesh, the dedicated #SCULPT_OT_mesh_asset_drop handles Object drops. */
  const Object *ob = CTX_data_active_object(C);
  if (ob && ob->type == OB_MESH && ob->mode == OB_MODE_SCULPT) {
    return false;
  }
  return true;
}

/**
 * \note the term local here refers to not being an external asset,
 * poll will succeed for linked library objects.
 */
static bool view3d_ob_drop_poll_local_id(bContext *C, wmDrag *drag, const wmEvent *event)
{
  if (!view3d_ob_drop_poll(C, drag, event) || (drag->type != WM_DRAG_ID)) {
    return false;
  }
  /* In Sculpt Mode with a mesh, the dedicated #SCULPT_OT_mesh_asset_drop handles Object drops. */
  const Object *ob = CTX_data_active_object(C);
  if (ob && ob->type == OB_MESH && ob->mode == OB_MODE_SCULPT) {
    return false;
  }
  return true;
}

static bool view3d_collection_drop_poll(bContext *C, wmDrag *drag, const wmEvent *event)
{
  return view3d_drop_id_in_main_region_poll(C, drag, event, ID_GR);
}

static bool view3d_collection_drop_poll_local_id(bContext *C, wmDrag *drag, const wmEvent *event)
{
  if (!view3d_collection_drop_poll(C, drag, event) || (drag->type != WM_DRAG_ID)) {
    return false;
  }
  /* In Sculpt Mode, collections are handled by the dedicated sculpt box. */
  const Object *ob = CTX_data_active_object(C);
  if (ob && ob->type == OB_MESH && ob->mode == OB_MODE_SCULPT) {
    return false;
  }
  return true;
}

static bool view3d_collection_drop_poll_external_asset(bContext *C,
                                                       wmDrag *drag,
                                                       const wmEvent *event)
{
  if (!view3d_collection_drop_poll(C, drag, event) || (drag->type != WM_DRAG_ASSET)) {
    return false;
  }
  /* In Sculpt Mode, collections are handled by the dedicated sculpt box. */
  const Object *ob = CTX_data_active_object(C);
  if (ob && ob->type == OB_MESH && ob->mode == OB_MODE_SCULPT) {
    return false;
  }
  return true;
}

static bool view3d_mat_drop_poll(bContext *C, wmDrag *drag, const wmEvent *event)
{
  if (!view3d_drop_id_in_main_region_poll(C, drag, event, ID_MA)) {
    return false;
  }

  Object *ob = ED_view3d_give_object_under_cursor(C, event->mval);

  return (ob && ID_IS_EDITABLE(&ob->id) && !ID_IS_OVERRIDE_LIBRARY(&ob->id));
}

static std::string view3d_mat_drop_tooltip(bContext *C,
                                           wmDrag *drag,
                                           const int xy[2],
                                           wmDropBox * /*drop*/)
{
  const std::string name = WM_drag_get_item_name(drag);
  ARegion *region = CTX_wm_region(C);
  const int mval[2] = {
      xy[0] - region->winrct.xmin,
      xy[1] - region->winrct.ymin,
  };
  return ed::object::drop_named_material_tooltip(C, name, mval);
}

static bool view3d_world_drop_poll(bContext *C, wmDrag *drag, const wmEvent *event)
{
  return view3d_drop_id_in_main_region_poll(C, drag, event, ID_WO);
}

static bool view3d_object_data_drop_poll(bContext *C, wmDrag *drag, const wmEvent *event)
{
  ID_Type id_type = view3d_drop_id_in_main_region_poll_get_id_type(C, drag, event);
  if (id_type && OB_DATA_SUPPORT_ID(id_type)) {
    return true;
  }
  return false;
}

static std::string view3d_object_data_drop_tooltip(bContext * /*C*/,
                                                   wmDrag * /*drag*/,
                                                   const int /*xy*/[2],
                                                   wmDropBox * /*drop*/)
{
  return TIP_("Create object instance from object-data");
}

static bool view3d_ima_drop_poll(bContext *C, wmDrag *drag, const wmEvent *event)
{
  if (ED_region_overlap_isect_any_xy(CTX_wm_area(C), event->xy)) {
    return false;
  }
  return WM_drag_is_ID_type(drag, ID_IM);
}

static bool view3d_ima_bg_is_camera_view(bContext *C)
{
  RegionView3D *rv3d = CTX_wm_region_view3d(C);
  if (rv3d && (rv3d->persp == RV3D_CAMOB)) {
    View3D *v3d = CTX_wm_view3d(C);
    if (v3d && v3d->camera && v3d->camera->type == OB_CAMERA) {
      return true;
    }
  }
  return false;
}

static bool view3d_ima_bg_drop_poll(bContext *C, wmDrag *drag, const wmEvent *event)
{
  if (!view3d_ima_drop_poll(C, drag, event)) {
    return false;
  }

  if (ED_view3d_is_object_under_cursor(C, event->mval)) {
    return false;
  }

  return view3d_ima_bg_is_camera_view(C);
}

static bool view3d_ima_empty_drop_poll(bContext *C, wmDrag *drag, const wmEvent *event)
{
  if (!view3d_ima_drop_poll(C, drag, event)) {
    return false;
  }

  Object *ob = ED_view3d_give_object_under_cursor(C, event->mval);

  if (ob == nullptr) {
    return true;
  }

  if (ob->type == OB_EMPTY && ob->empty_drawtype == OB_EMPTY_IMAGE) {
    return true;
  }

  return false;
}

static bool view3d_geometry_nodes_drop_poll(bContext *C, wmDrag *drag, const wmEvent *event)
{
  if (!view3d_drop_id_in_main_region_poll(C, drag, event, ID_NT)) {
    return false;
  }

  if (drag->type == WM_DRAG_ID) {
    const bNodeTree *node_tree = reinterpret_cast<const bNodeTree *>(
        WM_drag_get_local_ID(drag, ID_NT));
    if (!node_tree) {
      return false;
    }
    return node_tree->type == NTREE_GEOMETRY;
  }

  if (drag->type == WM_DRAG_ASSET) {
    const wmDragAsset *asset_data = WM_drag_get_asset_data(drag, ID_NT);
    if (!asset_data) {
      return false;
    }
    const AssetMetaData *metadata = &asset_data->asset->get_metadata();
    const IDProperty *tree_type = BKE_asset_metadata_idprop_find(metadata, "type");
    if (!tree_type || IDP_int_get(tree_type) != NTREE_GEOMETRY) {
      return false;
    }
    if (wmDropBox *drop_box = drag->drop_state.active_dropbox) {
      const uint32_t uid = RNA_int_get(drop_box->ptr, "session_uid");
      const bNodeTree *node_tree = reinterpret_cast<const bNodeTree *>(
          BKE_libblock_find_session_uid(CTX_data_main(C), ID_NT, uid));
      if (node_tree) {
        return node_tree->type == NTREE_GEOMETRY;
      }
    }
  }
  return true;
}

static std::string view3d_geometry_nodes_drop_tooltip(bContext *C,
                                                      wmDrag * /*drag*/,
                                                      const int xy[2],
                                                      wmDropBox *drop)
{
  ARegion *region = CTX_wm_region(C);
  int mval[2] = {xy[0] - region->winrct.xmin, xy[1] - region->winrct.ymin};
  return ed::object::drop_geometry_nodes_tooltip(C, drop->ptr, mval);
}

static void view3d_ob_drop_matrix_from_snap(V3DSnapCursorState *snap_state,
                                            Object *ob,
                                            float obmat_final[4][4])
{
  V3DSnapCursorData *snap_data = ED_view3d_cursor_snap_data_get();
  BLI_assert(snap_state->draw_box || snap_state->draw_plane);
  UNUSED_VARS_NDEBUG(snap_state);
  copy_m4_m3(obmat_final, snap_data->plane_omat);
  copy_v3_v3(obmat_final[3], snap_data->loc);

  float scale[3];
  mat4_to_size(scale, ob->object_to_world().ptr());
  rescale_m4(obmat_final, scale);

  if (const std::optional<Bounds<float3>> bb = BKE_object_boundbox_get(ob)) {
    float3 offset = math::midpoint(bb->min, bb->max);
    offset[2] = bb->min[2];
    mul_mat3_m4_v3(obmat_final, offset);
    sub_v3_v3(obmat_final[3], offset);
  }
}

static void view3d_ob_drop_copy_local_id(bContext * /*C*/, wmDrag *drag, wmDropBox *drop)
{
  ID *id = WM_drag_get_local_ID(drag, ID_OB);

  RNA_int_set(drop->ptr, "session_uid", id->session_uid);
  /* Don't duplicate ID's which were just imported. Only do that for existing, local IDs. */
  BLI_assert(drag->type != WM_DRAG_ASSET);

  V3DSnapCursorState *snap_state = ED_view3d_cursor_snap_state_active_get();
  float obmat_final[4][4];

  view3d_ob_drop_matrix_from_snap(snap_state, id_cast<Object *>(id), obmat_final);

  RNA_float_set_array(drop->ptr, "matrix", &obmat_final[0][0]);
}

/**
 * Make all selected objects and the collections (if \a localize_collections is set) local so
 * objects can be transformed. The object data and other dependencies are kept linked/packed.
 *
 * This is a bit of a minimum effort solution. Ideally there would be a version of
 * #BKE_lib_id_make_local() that allows passing multiple IDs to make local.
 */
static void make_selected_objects_local(Main &bmain,
                                        Scene &scene,
                                        ViewLayer &view_layer,
                                        View3D &v3d,
                                        const bool localize_parent_collections)
{
  Set<Collection *> collections_to_localize;

  if (localize_parent_collections) {
    /* Collect the collections containing the selected objects. */
    for (Collection &collection : bmain.collections) {
      if (ID_IS_LINKED(&collection) &&
          /* This #BKE_view_layer_has_collection() check requires the view layer to be synced.
           * That's why we first collect the collections before doing any changes. Otherwise we'd
           * have to sync the view layer after every "make local" operation. */
          BKE_view_layer_has_collection(&view_layer, &collection))
      {
        FOREACH_SELECTED_OBJECT_BEGIN (&view_layer, &v3d, ob_selected) {
          if (BKE_collection_has_object(&collection, ob_selected)) {
            collections_to_localize.add(&collection);
            break;
          }
        }
        FOREACH_SELECTED_OBJECT_END;
      }
    }
  }

  Vector<ID *> localized_ids;

  /* Make IDs local. When an ID gets made local, pointers to it will be remapped to the new local
   * version. However, this doesn't work if the pointer is owned by linked data too.
   *
   * E.g. when multiple objects are linked/packed together, they may point to each other. An object
   * that is being made local may be pointed to by another object that hasn't been made local yet.
   * So this pointer cannot be remapped yet either.
   *
   * That's why we do another remapping pass below, over all IDs that were made local. This catches
   * remaining old pointers.
   */
  {
    const auto make_id_local = [&bmain, &localized_ids](ID *id) {
      BKE_lib_id_make_local(
          &bmain, id, LIB_ID_MAKELOCAL_ASSET_DATA_CLEAR | LIB_ID_MAKELOCAL_LIBOVERRIDE_CLEAR);
      if (id->newid) {
        BLI_assert(ID_IS_LINKED(id));
        BLI_assert(!ID_IS_LINKED(id->newid));

        localized_ids.append(id);
      }
    };

    for (Collection *collection : collections_to_localize) {
      make_id_local(&collection->id);
    }
    BKE_view_layer_synced_ensure(bmain, &scene, &view_layer);

    FOREACH_SELECTED_OBJECT_BEGIN (&view_layer, &v3d, ob_selected) {
      make_id_local(&ob_selected->id);
    }
    FOREACH_SELECTED_OBJECT_END;
  }

  /* Make sure all remaining pointers from the linked IDs are replaced by the new one. */
  {
    bke::id::IDRemapper remapper;
    for (ID *id : localized_ids) {
      remapper.add(id, id->newid);
    }
    BKE_libblock_remap_multiple(&bmain, remapper, ID_REMAP_SKIP_INDIRECT_USAGE);
  }
}

/* Mostly the same logic as #view3d_collection_drop_copy_external_asset(), just different enough to
 * make sharing code a bit difficult. */
static void view3d_ob_drop_copy_external_asset(bContext *C, wmDrag *drag, wmDropBox *drop)
{
  /* NOTE(@ideasman42): Selection is handled here, de-selecting objects before append,
   * using auto-select to ensure the new objects are selected.
   * This is done so #OBJECT_OT_transform_to_mouse (which runs after this drop handler)
   * can use the context setup here to place the objects. */
  BLI_assert(drag->type == WM_DRAG_ASSET);

  wmDragAsset *asset_drag = WM_drag_get_asset_data(drag, 0);
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);

  BKE_view_layer_base_deselect_all(*bmain, scene, view_layer);

  ID *id = WM_drag_asset_id_import(C, asset_drag, FILE_AUTOSELECT);
  if (!id) {
    return;
  }

  /* TODO(sergey): Only update relations for the current scene. */
  DEG_relations_tag_update(bmain);
  WM_event_add_notifier(C, NC_SCENE | ND_LAYER_CONTENT, scene);

  BKE_view_layer_synced_ensure(*bmain, scene, view_layer);
  Base *base = BKE_view_layer_base_find(view_layer, id_cast<Object *>(id));
  if (base != nullptr) {
    BKE_view_layer_base_select_and_set_active(view_layer, base);
    WM_main_add_notifier(NC_SCENE | ND_OB_ACTIVE, scene);
  }
  DEG_id_tag_update(&scene->id, ID_RECALC_SELECT);

  /* Make objects local so they can be transformed. */
  if (WM_drag_asset_will_import_packed(drag)) {
    make_selected_objects_local(*bmain, *scene, *view_layer, *CTX_wm_view3d(C), false);

    /* Making the IDs local might result in a new, copied ID. */
    if (id->newid) {
      id = id->newid;
    }
  }

  ED_outliner_select_sync_from_object_tag(C);

  /* Do after making local, since that changes the session UID. */
  RNA_int_set(drop->ptr, "session_uid", id->session_uid);

  /* Make sure the depsgraph is evaluated so the new object's transforms are up-to-date.
   * The evaluated #Object::object_to_world() will be copied back to the original object
   * and used below. */
  CTX_data_ensure_evaluated_depsgraph(C);

  V3DSnapCursorState *snap_state = static_cast<V3DSnapCursorState *>(drop->draw_data);
  if (snap_state) {
    float obmat_final[4][4];

    view3d_ob_drop_matrix_from_snap(snap_state, id_cast<Object *>(id), obmat_final);

    RNA_float_set_array(drop->ptr, "matrix", &obmat_final[0][0]);
  }
}

/**
 * Build a placement matrix for a single object dropped in sculpt mode.
 *
 * Extends the standard #view3d_ob_drop_matrix_from_snap with user-driven uniform scale and
 * rotation around the surface normal acquired during the hover phase.
 */
static void sculpt_drop_build_ob_matrix(const SculptDropData *data,
                                        const Object *ob,
                                        float r_mat[4][4])
{
  const V3DSnapCursorData *snap_data = ED_view3d_cursor_snap_data_get();

  ed::view3d::sculpt_drop_preview::PreviewPlacement place;
  ed::view3d::sculpt_drop_preview::fill_placement_from_object(*ob, place);
  ed::view3d::sculpt_drop_preview::build_single_object_matrix(data->rotation_angle,
                                                              data->scale_factor,
                                                              place,
                                                              snap_data->plane_omat,
                                                              snap_data->loc,
                                                              r_mat);
}

/**
 * Sculpt Mode counterpart of #view3d_ob_drop_poll_external_asset.
 *
 * #view3d_ob_drop_poll_external_asset explicitly returns false in Sculpt Mode so that the
 * EVT_DROP handler (which only checks the dropbox poll, not the operator poll) skips the Object
 * Mode box and falls through to this one.
 */
static bool view3d_sculpt_mesh_asset_drop_poll(bContext *C, wmDrag *drag, const wmEvent *event)
{
  if (!view3d_drop_id_in_main_region_poll(C, drag, event, ID_OB)) {
    return false;
  }
  const Object *ob = CTX_data_active_object(C);
  if (!ob || ob->type != OB_MESH || ob->mode != OB_MODE_SCULPT) {
    return false;
  }
  sculpt_drop_preview_capture_enter_context(C, drag);
  return true;
}

/**
 * Import (or locate) the dropped asset and hand it to #SCULPT_OT_mesh_asset_drop via
 * `session_uid`.
 *
 * Handles both external assets (#WM_DRAG_ASSET — imported, joined, then removed) and local
 * data-block assets already present in the file (#WM_DRAG_ID — joined but kept in the scene).
 */
static void view3d_sculpt_mesh_asset_drop_copy(bContext *C, wmDrag *drag, wmDropBox *drop)
{
  SculptDropData *data = static_cast<SculptDropData *>(drop->draw_data);

  /* Placement options were decided interactively during hover (J/F/L keys, see
   * #view3d_sculpt_on_event_while_hover); hand them to the operator now instead of leaving them
   * at their RNA defaults, since exec() only ever runs once (see the file-level comment in
   * sculpt_asset_drop.cc for why re-running it via the redo panel isn't supported). */
  if (data) {
    RNA_boolean_set(drop->ptr, "join_to_active", data->join_to_active);
    RNA_boolean_set(drop->ptr, "replace_face_sets", data->replace_face_sets);
    RNA_boolean_set(drop->ptr, "linked", data->linked);
    RNA_boolean_set(drop->ptr, "apply_mask", data->apply_mask);
  }
  ED_workspace_status_text(C, nullptr);

  auto apply_snap_matrix = [&](const Object *ob) {
    if (!data) {
      return;
    }
    float obmat_final[4][4];
    sculpt_drop_build_ob_matrix(data, ob, obmat_final);
    RNA_float_set_array(drop->ptr, "matrix", &obmat_final[0][0]);
  };

  if (drag->type == WM_DRAG_ID) {
    /* Local asset: the Object is already in the file — no import needed. */
    ID *id = WM_drag_get_local_ID(drag, ID_OB);
    if (!id) {
      return;
    }
    RNA_int_set(drop->ptr, "session_uid", int(id->session_uid));
    /* Signal exec to keep the source object in the scene after joining. */
    RNA_boolean_set(drop->ptr, "keep_source", true);
    apply_snap_matrix(id_cast<Object *>(id));
    return;
  }

  /* WM_DRAG_ASSET path: external asset that needs importing. */
  BLI_assert(drag->type == WM_DRAG_ASSET);
  Main *bmain = CTX_data_main(C);
  wmDragAsset *asset_drag = WM_drag_get_asset_data(drag, 0);

  ID *id = WM_drag_asset_id_import(C, asset_drag, FILE_AUTOSELECT);
  if (!id) {
    return;
  }
  DEG_relations_tag_update(bmain);

  /* The operator needs local mesh data to read and join. */
  if (WM_drag_asset_will_import_packed(drag)) {
    BKE_lib_id_make_local(
        bmain, id, LIB_ID_MAKELOCAL_ASSET_DATA_CLEAR | LIB_ID_MAKELOCAL_LIBOVERRIDE_CLEAR);
    if (id->newid) {
      id = id->newid;
    }
  }

  /* Do after making local, since that changes the session UID. */
  RNA_int_set(drop->ptr, "session_uid", int(id->session_uid));
  RNA_boolean_set(drop->ptr, "keep_source", false);

  /* Make sure the imported object's evaluated transform is available for the snap matrix. */
  CTX_data_ensure_evaluated_depsgraph(C);

  apply_snap_matrix(id_cast<Object *>(id));
}

/* -------------------------------------------------------------------- */
/** \name Sculpt-Mode Collection Drop
 * \{ */

/** Accept ID_GR drags over a sculpt-mode mesh object. */
static bool view3d_sculpt_collection_drop_poll(bContext *C, wmDrag *drag, const wmEvent *event)
{
  if (!view3d_drop_id_in_main_region_poll(C, drag, event, ID_GR)) {
    return false;
  }
  const Object *ob = CTX_data_active_object(C);
  if (!ob || ob->type != OB_MESH || ob->mode != OB_MODE_SCULPT) {
    return false;
  }
  sculpt_drop_preview_capture_enter_context(C, drag);
  return true;
}

/**
 * Import (or locate) the dropped collection and pass its session UID to
 * #SCULPT_OT_mesh_asset_drop via the `collection_uid` property.  Builds the placement matrix the
 * same way the object path does but without any per-object scale offset.
 */
static void view3d_sculpt_collection_drop_copy(bContext *C, wmDrag *drag, wmDropBox *drop)
{
  SculptDropData *data = static_cast<SculptDropData *>(drop->draw_data);
  Main *bmain = CTX_data_main(C);

  /* Placement options were decided interactively during hover (J/F keys, see
   * #view3d_sculpt_on_event_while_hover); hand them to the operator now since exec() only ever
   * runs once. Linked doesn't apply to collections (there is no single carrier object to link). */
  if (data) {
    RNA_boolean_set(drop->ptr, "join_to_active", data->join_to_active);
    RNA_boolean_set(drop->ptr, "replace_face_sets", data->replace_face_sets);
    RNA_boolean_set(drop->ptr, "apply_mask", data->apply_mask);
  }
  ED_workspace_status_text(C, nullptr);

  Collection *collection = nullptr;
  bool keep_source = false;

  if (drag->type == WM_DRAG_ID) {
    collection = id_cast<Collection *>(WM_drag_get_local_ID(drag, ID_GR));
    keep_source = true;
  }
  else {
    BLI_assert(drag->type == WM_DRAG_ASSET);
    wmDragAsset *asset_drag = WM_drag_get_asset_data(drag, 0);
    ID *id = WM_drag_asset_id_import(C, asset_drag, 0);
    if (!id || GS(id->name) != ID_GR) {
      return;
    }
    DEG_relations_tag_update(bmain);
    if (WM_drag_asset_will_import_packed(drag)) {
      BKE_lib_id_make_local(
          bmain, id, LIB_ID_MAKELOCAL_ASSET_DATA_CLEAR | LIB_ID_MAKELOCAL_LIBOVERRIDE_CLEAR);
      if (id->newid) {
        id = id->newid;
      }
    }
    collection = id_cast<Collection *>(id);
    keep_source = false;
  }

  if (!collection) {
    return;
  }

  RNA_int_set(drop->ptr, "collection_uid", int(collection->id.session_uid));
  RNA_boolean_set(drop->ptr, "keep_source", keep_source);

  CTX_data_ensure_evaluated_depsgraph(C);

  /* Build the snap placement matrix for the collection as a whole.
   * The exec path will offset individual objects relative to their shared center. */
  if (data) {
    const V3DSnapCursorData *snap_cdata = ED_view3d_cursor_snap_data_get();
    if (snap_cdata) {
      float mat[4][4];
      ed::view3d::sculpt_drop_preview::build_collection_snap_matrix(
          data->rotation_angle, data->scale_factor, snap_cdata->plane_omat, snap_cdata->loc, mat);
      RNA_float_set_array(drop->ptr, "matrix", &mat[0][0]);
    }
  }
}

/** \} */

static void view3d_collection_drop_on_enter(wmDropBox *drop, wmDrag *drag)
{
  if (WM_drag_asset_will_import_linked(drag)) {
    const wmDragAsset *asset_drag = WM_drag_get_asset_data(drag, 0);
    /* Linked collections cannot be transformed except when using instancing. Don't enable
     * snapping. */
    if (!asset_drag->import_settings.use_instance_collections) {
      return;
    }
  }

  view3d_drop_snap_init(drop);
}

static void view3d_collection_drop_matrix_from_snap(V3DSnapCursorState *snap_state,
                                                    float r_loc[3],
                                                    float r_rot[3])
{
  V3DSnapCursorData *snap_data = ED_view3d_cursor_snap_data_get();
  BLI_assert(snap_state->draw_box || snap_state->draw_plane);
  UNUSED_VARS_NDEBUG(snap_state);

  mat3_normalized_to_eul(r_rot, snap_data->plane_omat);
  copy_v3_v3(r_loc, snap_data->loc);
}

static void view3d_collection_drop_copy_local_id(bContext * /*C*/, wmDrag *drag, wmDropBox *drop)
{
  ID *id = WM_drag_get_local_ID(drag, ID_GR);
  RNA_int_set(drop->ptr, "session_uid", int(id->session_uid));

  V3DSnapCursorState *snap_state = ED_view3d_cursor_snap_state_active_get();

  float loc[3], rot[3];
  view3d_collection_drop_matrix_from_snap(snap_state, loc, rot);
  RNA_float_set_array(drop->ptr, "location", loc);
  RNA_float_set_array(drop->ptr, "rotation", rot);
}

/* Mostly the same logic as #view3d_ob_drop_copy_external_asset(), just different enough to make
 * sharing code a bit difficult. */
static void view3d_collection_drop_copy_external_asset(bContext *C, wmDrag *drag, wmDropBox *drop)
{
  BLI_assert(drag->type == WM_DRAG_ASSET);

  wmDragAsset *asset_drag = WM_drag_get_asset_data(drag, 0);
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);

  BKE_view_layer_base_deselect_all(*bmain, scene, view_layer);

  const bool use_instance_collections = asset_drag->import_settings.use_instance_collections;
  /* Temporarily disable instancing for the import, the drop operator handles that. */
  asset_drag->import_settings.use_instance_collections = false;

  ID *id = WM_drag_asset_id_import(C, asset_drag, FILE_AUTOSELECT);
  if (!id) {
    return;
  }
  Collection *collection = id_cast<Collection *>(id);

  /* Reset temporary override. */
  asset_drag->import_settings.use_instance_collections = use_instance_collections;

  /* TODO(sergey): Only update relations for the current scene. */
  DEG_relations_tag_update(bmain);
  WM_event_add_notifier(C, NC_SCENE | ND_LAYER_CONTENT, scene);

  /* Make an object active, just use the first one in the collection. */
  CollectionObject *cobject = static_cast<CollectionObject *>(collection->gobject.first);
  BKE_view_layer_synced_ensure(*bmain, scene, view_layer);
  Base *base = cobject ? BKE_view_layer_base_find(view_layer, cobject->ob) : nullptr;
  if (base) {
    BLI_assert((base->flag & BASE_SELECTABLE) && (base->flag & BASE_ENABLED_VIEWPORT));
    BKE_view_layer_base_select_and_set_active(view_layer, base);
    WM_main_add_notifier(NC_SCENE | ND_OB_ACTIVE, scene);
  }
  DEG_id_tag_update(&scene->id, ID_RECALC_SELECT);

  /* Make objects local so they can be transformed. */
  if (WM_drag_asset_will_import_packed(drag) && !use_instance_collections) {
    make_selected_objects_local(*bmain, *scene, *view_layer, *CTX_wm_view3d(C), true);

    /* Making the IDs local might result in a new, copied ID. */
    collection = id_cast<Collection *>(id->newid ? id->newid : id);
    id = &collection->id;
  }

  ED_outliner_select_sync_from_object_tag(C);

  /* Do after making local, since that changes the session UID. */
  RNA_int_set(drop->ptr, "session_uid", int(id->session_uid));
  RNA_boolean_set(drop->ptr, "use_instance", asset_drag->import_settings.use_instance_collections);

  V3DSnapCursorState *snap_state = static_cast<V3DSnapCursorState *>(drop->draw_data);
  if (snap_state) {
    float loc[3], rot[3];
    view3d_collection_drop_matrix_from_snap(snap_state, loc, rot);
    RNA_float_set_array(drop->ptr, "location", loc);
    RNA_float_set_array(drop->ptr, "rotation", rot);
  }

  /* XXX Without an undo push here, there will be a crash when the user modifies operator
   * properties. The stuff we do in these drop callbacks just isn't safe over undo/redo. */
  ED_undo_push(C, "Drop Collection");
}

static void view3d_id_drop_copy(bContext *C, wmDrag *drag, wmDropBox *drop)
{
  ID *id = WM_drag_get_local_ID_or_import_from_asset(C, drag, 0);
  if (id) {
    WM_operator_properties_id_lookup_set_from_id(drop->ptr, id);
  }
}

static void view3d_geometry_nodes_drop_copy(bContext *C, wmDrag *drag, wmDropBox *drop)
{
  view3d_id_drop_copy(C, drag, drop);
  RNA_boolean_set(drop->ptr, "show_datablock_in_modifier", (drag->type != WM_DRAG_ASSET));
}

static void view3d_id_drop_copy_with_type(bContext *C, wmDrag *drag, wmDropBox *drop)
{
  ID *id = WM_drag_get_local_ID_or_import_from_asset(C, drag, 0);
  wmDragAsset *asset_drag = WM_drag_get_asset_data(drag, 0);

  std::optional<ID_Type> idtype = std::nullopt;
  if (asset_drag) {
    idtype = asset_drag->asset->get_id_type();
  }
  else if (id) {
    idtype = GS(id->name);
  }

  if (idtype) {
    RNA_enum_set(drop->ptr, "type", *idtype);
  }
  if (id) {
    WM_operator_properties_id_lookup_set_from_id(drop->ptr, id);
  }
}

static void view3d_id_path_drop_copy(bContext *C, wmDrag *drag, wmDropBox *drop)
{
  ID *id = WM_drag_get_local_ID_or_import_from_asset(C, drag, 0);

  if (id) {
    WM_operator_properties_id_lookup_set_from_id(drop->ptr, id);
    RNA_struct_property_unset(drop->ptr, "filepath");
    return;
  }
}

void view3d_dropboxes()
{
  ListBaseT<wmDropBox> *lb = WM_dropboxmap_find("View3D", SPACE_VIEW3D, RGN_TYPE_WINDOW);

  wmDropBox *drop;
  drop = WM_dropbox_add(lb,
                        "OBJECT_OT_add_named",
                        view3d_ob_drop_poll_local_id,
                        view3d_ob_drop_copy_local_id,
                        WM_drag_free_imported_drag_ID,
                        nullptr);

  drop->draw_droptip = WM_drag_draw_item_name_fn;
  drop->on_enter = view3d_ob_drop_on_enter;
  drop->on_exit = view3d_drop_snap_exit;

  drop = WM_dropbox_add(lb,
                        "OBJECT_OT_transform_to_mouse",
                        view3d_ob_drop_poll_external_asset,
                        view3d_ob_drop_copy_external_asset,
                        WM_drag_free_imported_drag_ID,
                        nullptr);

  drop->draw_droptip = WM_drag_draw_item_name_fn;
  drop->on_enter = view3d_ob_drop_on_enter;
  drop->on_exit = view3d_drop_snap_exit;

  /* Sculpt Mode mesh-object drop — handles both WM_DRAG_ID and WM_DRAG_ASSET for ID_OB.
   * Supports S (scale) and R (rotate around normal) hotkeys during hover. */
  drop = WM_dropbox_add(lb,
                        "SCULPT_OT_mesh_asset_drop",
                        view3d_sculpt_mesh_asset_drop_poll,
                        view3d_sculpt_mesh_asset_drop_copy,
                        WM_drag_free_imported_drag_ID,
                        nullptr);
  drop->draw_droptip = WM_drag_draw_item_name_fn;
  drop->on_enter = view3d_sculpt_ob_drop_on_enter;
  drop->on_exit = view3d_sculpt_drop_on_exit;
  drop->on_event_while_hover = view3d_sculpt_on_event_while_hover;
  drop->on_event_while_hover_handled = view3d_sculpt_on_event_while_hover_handled;

  /* Sculpt Mode collection drop — joins all mesh objects in the collection into the sculpt mesh.
   */
  drop = WM_dropbox_add(lb,
                        "SCULPT_OT_mesh_asset_drop",
                        view3d_sculpt_collection_drop_poll,
                        view3d_sculpt_collection_drop_copy,
                        WM_drag_free_imported_drag_ID,
                        nullptr);
  drop->draw_droptip = WM_drag_draw_item_name_fn;
  drop->on_enter = view3d_sculpt_collection_drop_on_enter;
  drop->on_exit = view3d_sculpt_drop_on_exit;
  drop->on_event_while_hover = view3d_sculpt_on_event_while_hover;
  drop->on_event_while_hover_handled = view3d_sculpt_on_event_while_hover_handled;

  drop = WM_dropbox_add(lb,
                        "OBJECT_OT_collection_external_asset_drop",
                        view3d_collection_drop_poll_external_asset,
                        view3d_collection_drop_copy_external_asset,
                        WM_drag_free_imported_drag_ID,
                        nullptr);
  drop->draw_droptip = WM_drag_draw_item_name_fn;
  drop->on_enter = view3d_collection_drop_on_enter;
  drop->on_exit = view3d_drop_snap_exit;
  drop = WM_dropbox_add(lb,
                        "OBJECT_OT_collection_instance_add",
                        view3d_collection_drop_poll_local_id,
                        view3d_collection_drop_copy_local_id,
                        WM_drag_free_imported_drag_ID,
                        nullptr);
  drop->draw_droptip = WM_drag_draw_item_name_fn;
  drop->on_enter = view3d_collection_drop_on_enter;
  drop->on_exit = view3d_drop_snap_exit;

  WM_dropbox_add(lb,
                 "OBJECT_OT_drop_named_material",
                 view3d_mat_drop_poll,
                 view3d_id_drop_copy,
                 WM_drag_free_imported_drag_ID,
                 view3d_mat_drop_tooltip);
  WM_dropbox_add(lb,
                 "OBJECT_OT_drop_geometry_nodes",
                 view3d_geometry_nodes_drop_poll,
                 view3d_geometry_nodes_drop_copy,
                 WM_drag_free_imported_drag_ID,
                 view3d_geometry_nodes_drop_tooltip);
  WM_dropbox_add(lb,
                 "VIEW3D_OT_camera_background_image_add",
                 view3d_ima_bg_drop_poll,
                 view3d_id_path_drop_copy,
                 WM_drag_free_imported_drag_ID,
                 nullptr);
  WM_dropbox_add(lb,
                 "OBJECT_OT_empty_image_add",
                 view3d_ima_empty_drop_poll,
                 view3d_id_path_drop_copy,
                 WM_drag_free_imported_drag_ID,
                 nullptr);
  WM_dropbox_add(lb,
                 "OBJECT_OT_data_instance_add",
                 view3d_object_data_drop_poll,
                 view3d_id_drop_copy_with_type,
                 WM_drag_free_imported_drag_ID,
                 view3d_object_data_drop_tooltip);
  WM_dropbox_add(lb,
                 "VIEW3D_OT_drop_world",
                 view3d_world_drop_poll,
                 view3d_id_drop_copy,
                 WM_drag_free_imported_drag_ID,
                 nullptr);
}

}  // namespace blender
