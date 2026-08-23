/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup edsculpt
 *
 * What Curve Patch editing needs to know about the target it acts on, and nothing else.
 *
 * Two interfaces, because the two callers need different amounts. #CurvePatchHost is what an
 * action body (`paint_curve_patch_actions.hh`) needs; #CurvePatchEditorHost adds what the modal
 * editor (`paint_curve_patch_editor.hh`) needs on top -- session lifetime, the live-brush
 * watchdog, the context menu. A target can implement the first without the second, which is how
 * 3D Sculpt Mode shares the action bodies while still running its own modal.
 *
 * One implementation per target: `SculptCurvePatchHost` (`paint_curve_patch_edit.cc`) and
 * `ImageCurvePatchHost` (`paint_image_curve_patch_edit.cc`).
 */

#include "BKE_curves.hh"

#include "paint_curve_patch_document.hh"

struct ReportList;

namespace blender {
struct bContext;
}

namespace blender::ed::sculpt_paint {

class CurvePatchHost {
 public:
  virtual ~CurvePatchHost() = default;

  /** The target-independent state the action mutates. */
  virtual CurvePatchDocument &document() = 0;

  /** The control curve the user edits.
   *
   * NOT always `document().active_item().control_curve`: the flat canvas edits a canonical UV
   * curve and keeps a pixel-space copy of it on the item, so only the owner knows which of the two
   * an action means. Unifying the two into one curve is the editor's job, not an action's. */
  virtual bke::CurvesGeometry &curve() = 0;

  /** Push what the current curve produces onto the target and redraw. Called once per event while
   * a drag is in flight, so it must be cheap enough for that -- both targets restore and re-stamp
   * only the patch's own footprint. Records no undo step.
   *
   * "Cheap" is not just about cost: this runs BETWEEN interactions, so it must leave the target in
   * whatever mid-interaction state its live preview needs. Anything that belongs to the END of an
   * interaction goes in #interaction_end instead -- see the note there. */
  virtual void restamp(bContext &C) = 0;

  /**
   * The interaction is over: settle whatever #restamp deliberately left mid-flight.
   *
   * Split from #restamp because the two are not the same operation, and collapsing them breaks the
   * live preview. Sculpt Mode's re-stamp ends in `flush_update_step()`, which sets `RV3D_PAINTING`
   * to arm the fast PBVH redraw path; the finished-stroke counterpart `flush_update_done()` clears
   * that flag again. Issuing the finished-stroke flush after every re-stamp therefore tears the
   * fast path down before the viewport ever draws through it, and the mesh stops updating until
   * the drag ends.
   */
  virtual void interaction_end(bContext & /*C*/) {}

  /** Record the current state on the target's own session-local history, where it has one. A drag
   * calls this once on release, never per event: a step per #MOUSEMOVE would fill the history with
   * intermediate positions nobody wants to step through. */
  virtual void undo_push() {}

  /** The tail of a complete action: one history step, the re-stamp, then the settle. Deliberately
   * not virtual -- an override could only get the ordering wrong. */
  void after_curve_change(bContext &C)
  {
    this->undo_push();
    this->restamp(C);
    this->interaction_end(C);
  }

  /**
   * Make `point_index` the one the user is acting on.
   *
   * `sel_bits` names which part of it was picked, in the packed convention of
   * #paintcurve_geom_set_selection (0x01 left handle, 0x02 the point, 0x04 right handle). Targets
   * whose overlay colors the picked handle mirror it into the curve's selection attribute; targets
   * that read the index alone ignore it. Pass -1 to clear.
   */
  virtual void active_point_set(const int point_index, uint8_t /*sel_bits*/)
  {
    this->document().active_point = point_index;
  }

  /**
   * The curves a click may land on, in creation order. #curve() is always one of them.
   *
   * A target that stamps several patches at once offers all of their control curves, so that a
   * click picks the closest point across the lot rather than only the active patch's. The default
   * offers the single edited curve, which is what a one-patch target wants.
   */
  virtual void pickable_curves(Vector<bke::CurvesGeometry *> &r_curves)
  {
    r_curves.clear();
    r_curves.append(&this->curve());
  }

  /** Make the `index`-th entry of #pickable_curves the one #curve() returns. No-op where there is
   * only ever one. */
  virtual void pickable_curve_activate(int /*index*/) {}

  /** Re-issue the modal status bar text, for the actions whose label depends on curve state
   * (Close / Open Curve). No-op where the target shows no status. */
  virtual void status_refresh(bContext & /*C*/) {}

  /** Where an action reports a refusal the user must see, or null to stay silent. */
  virtual ReportList *reports()
  {
    return nullptr;
  }
};

/** The extra a target must answer for the shared modal editor to drive it. */
class CurvePatchEditorHost : public CurvePatchHost {
 public:
  /** Apply the patch and end the session. The editor calls this and then reports `Finished`; it
   * never touches session lifetime itself. */
  virtual void commit(bContext &C) = 0;

  /** Discard the patch and end the session. */
  virtual void cancel(bContext &C) = 0;

  /** Notice a brush edit made from the UI while the modal runs and re-stamp. Returns true when a
   * change was found -- which may have ended the session, so the editor re-checks #is_alive. */
  virtual bool sync_live_brush(bContext & /*C*/)
  {
    return false;
  }

  /** False once the session this host points at is gone or its editing context went invalid. */
  virtual bool is_alive(bContext &C) = 0;

  /** Redraw what shows the curve, without re-stamping. For the events that change only what is
   * picked or highlighted -- a press that starts a drag, a release that ends one. */
  virtual void redraw(bContext &C) = 0;

  /** Open the right-click menu over the active point. */
  virtual void context_menu_open(bContext &C) = 0;

  /* Stepping the session-local history is deliberately NOT part of this interface. It used to be,
   * as a pair of virtuals the editing core never called: 3D Sculpt Mode binds undo/redo to its own
   * modal keymap and handles both before an event reaches the core, and the flat canvas has no
   * session history to step at all -- its document snapshots `CurvePatchItem::control_curve`,
   * which on that target is the derived pixel-space copy, while the curve the user edits is the
   * session's canonical UV one (see #ImageCurvePatchSession). Giving the canvas real session undo
   * means unifying those two curves first; until then a declared-but-unreachable interface only
   * hides that the feature is missing. */
};

}  // namespace blender::ed::sculpt_paint
