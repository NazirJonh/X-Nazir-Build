/* SPDX-FileCopyrightText: 2026 Modifications by Nazir
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Operators for the sculpt history (see #BKE_sculpt_history.hh): go to a step, scrub the
 * timeline, delete the later steps. Going to a step is one undo step, like any sculpt edit, and
 * isn't recorded in the history itself: undoing it puts the mesh back in the step it left (see
 * #undo::history_jump_begin).
 */

#include <algorithm>
#include <cfloat>
#include <climits>
#include <cmath>

#include <fmt/format.h>

#include "MEM_guardedalloc.h"

#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_windowmanager_types.h"

#include "BLT_translation.hh"

#include "BKE_context.hh"
#include "BKE_mesh.hh"
#include "BKE_multires.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_report.hh"
#include "BKE_sculpt_history.hh"

#include "DEG_depsgraph.hh"

#include "ED_screen.hh"
#include "ED_sculpt.hh"

#include "UI_interface_icons.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_api.hh"
#include "WM_types.hh"
#include "wm_event_types.hh"

#include "sculpt_intern.hh"
#include "sculpt_undo.hh"

namespace blender::ed::sculpt_paint {

static SculptHistory *active_history(bContext *C)
{
  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr || ob->type != OB_MESH || ob->sculpt_history == nullptr ||
      ob->sculpt_history->steps_num == 0)
  {
    return nullptr;
  }
  return ob->sculpt_history;
}

static bool history_poll(bContext *C)
{
  if (!sculpt_mode_poll(C) || active_history(C) == nullptr) {
    return false;
  }
  const Object &ob = *CTX_data_active_object(C);
  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(ob);
  if (pbvh && pbvh->type() == bke::pbvh::Type::BMesh) {
    CTX_wm_operator_poll_msg_set(C, "The sculpt history doesn't work with Dyntopo");
    return false;
  }
  return true;
}

/**
 * Puts the mesh (or its Multires grids) in the state of step \a index and updates the sculpt
 * session for it. False when the step can't be shown now (another subdivision level).
 *
 * \a op is optional: an undo-driven restore (#undo::history_apply_cursor) has no operator to
 * report the topology replacement through, and only warns about it when going to a step by hand.
 */
static bool apply_step(bContext *C, Object &ob, int index, wmOperator *op = nullptr)
{
  using bke::sculpt_history::ApplyResult;
  bke::sculpt_history::ScopedPause pause(ob);
  SculptHistory &history = *ob.sculpt_history;
  index = std::clamp(index, 0, history.steps_num - 1);
  Depsgraph &depsgraph = *CTX_data_ensure_evaluated_depsgraph(C);
  SculptSession *ss = ob.runtime->sculpt_session;
  if (ss && ss->subdiv_ccg && ss->subdiv_ccg->masks.is_empty() &&
      bke::sculpt_history::step_has_mask(history, index))
  {
    /* The grids get a mask layer to hold the step's mask. */
    if (MultiresModifierData *mmd = BKE_sculpt_multires_active(CTX_data_scene(C), &ob)) {
      BKE_sculpt_mask_layers_ensure(&depsgraph, CTX_data_main(C), &ob, mmd);
    }
  }

  const ApplyResult result = bke::sculpt_history::apply_step(ob, index);
  if (result == ApplyResult::Failed) {
    return false;
  }
  if (result == ApplyResult::Topology && op != nullptr) {
    BKE_report(op->reports,
               RPT_WARNING,
               "The mesh was replaced with the step's topology; attributes that do not fit it "
               "were not kept");
  }
  Mesh &mesh = *id_cast<Mesh *>(ob.data);
  bke::pbvh::Tree *pbvh = bke::object::pbvh_get(ob);
  ss = ob.runtime->sculpt_session;
  /* With deform modifiers or shape keys, the sculpted positions aren't the mesh's own. */
  const bool in_place = result == ApplyResult::Positions && pbvh && ss &&
                        (pbvh->type() == bke::pbvh::Type::Grids ||
                         (!ss->deform_modifiers_active && ss->shapekey_active == nullptr));
  if (in_place) {
    IndexMaskMemory memory;
    const IndexMask node_mask = bke::pbvh::all_leaf_nodes(*pbvh, memory);
    if (pbvh->type() == bke::pbvh::Type::Grids) {
      bke::pbvh::update_mask_grids(*ss->subdiv_ccg, node_mask, *pbvh);
      multires_mark_as_modified(&depsgraph, &ob, MULTIRES_COORDS_MODIFIED);
    }
    else {
      /* Sculpting reads (and its undo steps keep) the evaluated positions, which are separate
       * from the mesh's own after the file is loaded: they must be the step's too. */
      MutableSpan<float3> positions_eval = bke::pbvh::vert_positions_eval_for_write(depsgraph,
                                                                                    ob);
      const Span<float3> positions = mesh.vert_positions();
      if (positions_eval.data() != positions.data()) {
        positions_eval.copy_from(positions);
      }
      bke::pbvh::update_mask_mesh(mesh, node_mask, *pbvh);
    }
    pbvh->tag_positions_changed(node_mask);
    pbvh->tag_masks_changed(node_mask);
    pbvh->update_bounds(depsgraph, ob);
    /* The explicit object, not the active one: undo also restores the other objects in Sculpt
     * Mode. */
    ViewContext vc = ED_view3d_viewcontext_init(C, CTX_data_depsgraph_pointer(C));
    const wmWindowManager &wm = *CTX_wm_manager(C);
    flush_update_step(vc, ob, UpdateType::Position);
    flush_update_done(vc, wm, ob, UpdateType::Position);
    flush_update_step(vc, ob, UpdateType::Mask);
    flush_update_done(vc, wm, ob, UpdateType::Mask);
    return true;
  }
  BKE_sculptsession_free_pbvh(ob);
  BKE_mesh_batch_cache_dirty_tag(&mesh, BKE_MESH_BATCH_DIRTY_ALL);
  DEG_id_tag_update(&ob.id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, &mesh);
  return true;
}

static void report_other_level(wmOperator *op)
{
  BKE_report(op->reports,
             RPT_WARNING,
             "That history step was sculpted at another Multires level (or without Multires)");
}

/** Starts the undo step of a jump on the timeline. */
static void jump_begin(bContext *C, Object &ob, const wmOperator *op)
{
  /* The undo step needs the sculpt session's PBVH. */
  bke::object::pbvh_ensure(*CTX_data_ensure_evaluated_depsgraph(C), ob);
  undo::history_jump_begin(*CTX_data_scene(C), ob, op);
}

namespace undo {

void history_apply_cursor(bContext *C, Object &ob, const int cursor)
{
  if (ob.sculpt_history == nullptr || ob.type != OB_MESH || !(ob.mode & OB_MODE_SCULPT)) {
    return;
  }
  const int index = bke::sculpt_history::find_cursor(*ob.sculpt_history, cursor);
  if (index != -1) {
    apply_step(C, ob, index);
    WM_main_add_notifier(NC_OBJECT | ND_DRAW, &ob);
  }
}

}  // namespace undo

/* -------------------------------------------------------------------- */
/** \name Go to Step
 * \{ */

static wmOperatorStatus history_goto_exec(bContext *C, wmOperator *op)
{
  Object &ob = *CTX_data_active_object(C);
  SculptHistory &history = *ob.sculpt_history;
  /* A non-zero offset is relative to the step that is current when the operator runs, so a
   * button drawn before the previous click was handled can't go to a stale index. */
  const int offset = RNA_int_get(op->ptr, "offset");
  const int target = offset != 0 ? history.current + offset : RNA_int_get(op->ptr, "index");
  const int index = std::clamp(target, 0, history.steps_num - 1);
  if (index == history.current) {
    return OPERATOR_CANCELLED;
  }
  jump_begin(C, ob, op);
  const bool applied = apply_step(C, ob, index, op);
  undo::history_jump_end(ob);
  if (!applied) {
    report_other_level(op);
  }
  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, &ob);
  return OPERATOR_FINISHED;
}

void SCULPT_OT_sculpt_history_goto(wmOperatorType *ot)
{
  ot->name = "Go to History Step";
  ot->idname = "SCULPT_OT_sculpt_history_goto";
  ot->description = "Bring the mesh back to a step of its sculpt history";

  ot->exec = history_goto_exec;
  ot->poll = history_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_int(ot->srna, "index", 0, 0, INT_MAX, "Index", "Step to go to", 0, 1000);
  PropertyRNA *prop = RNA_def_int(ot->srna,
                                  "offset",
                                  0,
                                  INT_MIN,
                                  INT_MAX,
                                  "Offset",
                                  "Steps to move from the current one (overrides the index "
                                  "when not zero)",
                                  -1,
                                  1);
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Scrub
 * \{ */

struct ScrubData {
  int start_index;
};

static int scrub_index(const wmOperator &op, const SculptHistory &history, const wmEvent &event)
{
  /* The timeline runs from `start` to `end` along x, or y for a bar docked to a side. */
  const float start = RNA_float_get(op.ptr, "start");
  const float end = RNA_float_get(op.ptr, "end");
  const bool vertical = RNA_boolean_get(op.ptr, "vertical");
  if (history.steps_num < 2 || start == end) {
    return history.current;
  }
  const float pos = float(vertical ? event.mval[1] : event.mval[0]);
  const float t = std::clamp((pos - start) / (end - start), 0.0f, 1.0f);
  return int(std::round(t * float(history.steps_num - 1)));
}

static void scrub_status(bContext *C, const SculptHistory &history)
{
  const SculptHistoryStep &step = history.steps[history.current];
  const std::string text = fmt::format(fmt::runtime(IFACE_("History: step {} of {} ({})")),
                                       history.current + 1,
                                       history.steps_num,
                                       step.name);
  ED_area_status_text(CTX_wm_area(C), text.c_str());
}

static void scrub_end(bContext *C, wmOperator *op)
{
  Object &ob = *CTX_data_active_object(C);
  undo::history_jump_end(ob);
  ED_area_status_text(CTX_wm_area(C), nullptr);
  MEM_delete(static_cast<ScrubData *>(op->customdata));
  op->customdata = nullptr;
}

static wmOperatorStatus history_scrub_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  Object &ob = *CTX_data_active_object(C);
  SculptHistory &history = *ob.sculpt_history;

  ScrubData *data = MEM_new<ScrubData>(__func__);
  data->start_index = history.current;
  op->customdata = data;

  jump_begin(C, ob, op);
  const int index = scrub_index(*op, history, *event);
  if (index != history.current && !apply_step(C, ob, index, op)) {
    report_other_level(op);
  }
  scrub_status(C, history);
  WM_event_add_modal_handler(C, op);
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus history_scrub_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  Object &ob = *CTX_data_active_object(C);
  SculptHistory &history = *ob.sculpt_history;
  switch (event->type) {
    case MOUSEMOVE: {
      const int index = scrub_index(*op, history, *event);
      if (index != history.current && apply_step(C, ob, index, op)) {
        scrub_status(C, history);
        WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, &ob);
      }
      return OPERATOR_RUNNING_MODAL;
    }
    case LEFTMOUSE:
      if (event->val == KM_RELEASE) {
        scrub_end(C, op);
        return OPERATOR_FINISHED;
      }
      return OPERATOR_RUNNING_MODAL;
    case EVT_ESCKEY:
    case RIGHTMOUSE: {
      const ScrubData &data = *static_cast<ScrubData *>(op->customdata);
      if (data.start_index != history.current) {
        apply_step(C, ob, data.start_index, op);
      }
      scrub_end(C, op);
      WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, &ob);
      return OPERATOR_CANCELLED;
    }
    default:
      return OPERATOR_RUNNING_MODAL;
  }
}

void SCULPT_OT_sculpt_history_scrub(wmOperatorType *ot)
{
  ot->name = "Scrub History";
  ot->idname = "SCULPT_OT_sculpt_history_scrub";
  ot->description = "Drag along the timeline to go through the sculpt history";

  ot->invoke = history_scrub_invoke;
  ot->modal = history_scrub_modal;
  ot->poll = history_poll;

  /* The undo step is pushed by hand, when the drag ends. */
  ot->flag = OPTYPE_BLOCKING;

  PropertyRNA *prop = RNA_def_float(ot->srna,
                                    "start",
                                    0.0f,
                                    -FLT_MAX,
                                    FLT_MAX,
                                    "Start",
                                    "Where the first step is on the timeline (region pixels)",
                                    0,
                                    1);
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
  prop = RNA_def_float(ot->srna,
                       "end",
                       1.0f,
                       -FLT_MAX,
                       FLT_MAX,
                       "End",
                       "Where the last step is on the timeline (region pixels)",
                       0,
                       1);
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
  prop = RNA_def_boolean(
      ot->srna, "vertical", false, "Vertical", "The timeline runs along y (docked to a side)");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Delete
 * \{ */

static bool history_delete_poll(bContext *C)
{
  const Object *ob = CTX_data_active_object(C);
  return ob && ob->sculpt_history;
}

static wmOperatorStatus history_delete_exec(bContext *C, wmOperator * /*op*/)
{
  Object &ob = *CTX_data_active_object(C);
  bke::sculpt_history::remove(ob);
  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, &ob);
  WM_file_tag_modified();
  return OPERATOR_FINISHED;
}

static wmOperatorStatus history_delete_invoke(bContext *C,
                                              wmOperator *op,
                                              const wmEvent * /*event*/)
{
  const Object &ob = *CTX_data_active_object(C);
  const std::string message = fmt::format(
      fmt::runtime(IFACE_("Are you sure you want to delete history for {}?")), ob.id.name + 2);
  return WM_operator_confirm_ex(
      C, op, IFACE_("Delete History"), message.c_str(), IFACE_("Confirm"), ui::AlertIcon::Warning);
}

void SCULPT_OT_sculpt_history_delete(wmOperatorType *ot)
{
  ot->name = "Delete Sculpt History";
  ot->idname = "SCULPT_OT_sculpt_history_delete";
  ot->description = "Delete the object's sculpt history (the mesh stays as it is)";

  ot->invoke = history_delete_invoke;
  ot->exec = history_delete_exec;
  ot->poll = history_delete_poll;

  ot->flag = OPTYPE_REGISTER;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Delete the Later Steps
 * \{ */

static wmOperatorStatus history_truncate_exec(bContext *C, wmOperator * /*op*/)
{
  Object &ob = *CTX_data_active_object(C);
  SculptHistory &history = *ob.sculpt_history;
  if (history.current >= history.steps_num - 1) {
    return OPERATOR_CANCELLED;
  }
  const int current = bke::sculpt_history::cursor_get(ob);
  bke::sculpt_history::truncate_after_current(history);
  undo::history_steps_deleted(ob, current);
  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, &ob);
  WM_file_tag_modified();
  return OPERATOR_FINISHED;
}

void SCULPT_OT_sculpt_history_truncate(wmOperatorType *ot)
{
  ot->name = "Delete Later History Steps";
  ot->idname = "SCULPT_OT_sculpt_history_truncate";
  ot->description =
      "Delete the sculpt history steps after the current one, to sculpt on from this step";

  ot->exec = history_truncate_exec;
  ot->poll = history_poll;

  ot->flag = OPTYPE_REGISTER;
}

/** \} */

}  // namespace blender::ed::sculpt_paint
