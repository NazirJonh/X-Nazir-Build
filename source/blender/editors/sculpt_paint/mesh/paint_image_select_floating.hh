/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * State and helpers shared by the four floating tools (move / transform / gradient / warp).
 *
 * Each tool keeps a heap-allocated state object alive on #SpaceImage_Runtime::paint_select for as
 * long as its edit is floating. The state structs used to repeat the same members and the same
 * lifetime plumbing (draw-handle removal, cursor restore, "is something floating in this space"
 * predicates); that shared part lives in #PaintSelectFloatingSession, which all four derive from.
 *
 * #PaintSelectSession holds exactly one of them at a time, so the tag on the base
 * (#PaintSelectFloatingSession::tool) is what identifies which tool the live session belongs to.
 * The accessors below are the only sanctioned way in and out of that slot.
 *
 * The base is deliberately non-polymorphic: every state object is created with #MEM_new and
 * destroyed with #MEM_delete through its *concrete* type, so no virtual destructor is needed and
 * none must be added. #image_select_floating_session_free bridges the gap by dispatching on the
 * tag to the owning tool.
 *
 * NOTE: This header is included from #paint_image_select_intern.hh, which is what consumers
 * should include.
 */

#pragma once

#include "BLI_assert.h"

#include "DNA_image_types.h"
/* #SpaceImage must be complete: the session accessors below reach through `sima->runtime`. */
#include "DNA_space_types.h"

/* #PaintSelectSession, the single slot the accessors below read and write. This header is the only
 * place the slot is touched directly; it forward-declares #PaintSelectFloatingSession and includes
 * nothing from this module, so the dependency stays one-way. */
#include "../../space_image/image_runtime.hh"

struct ARegion;
struct ARegionType;
struct ImBuf;
struct bContext;
struct wmKeyConfig;
struct wmKeyMap;
struct wmOperatorType;

namespace blender {

/* The per-tool states derive from #PaintSelectFloatingSession and are defined in their own
 * translation units. Declared here because the runtime slot used to declare them. */
struct ImageSelectMoveState;
struct ImageSelectTransformState;
struct ImageSelectGradientState;
struct ImageSelectWarpState;

/* -------------------------------------------------------------------- */
/** \name Shared floating session
 * \{ */

/**
 * Which tool a live #PaintSelectFloatingSession belongs to.
 *
 * The tag exists because #PaintSelectSession has one untyped slot: it is the only way back from
 * the stored base pointer to the concrete state, and it is what lets a tool tell "the session in
 * this editor is mine" from "someone else's session is live and has to be ended first".
 */
enum class PaintSelectTool {
  Move,
  Transform,
  Gradient,
  Warp,
};

/**
 * The part of a floating tool's state that is identical across move / transform / gradient / warp.
 *
 * \note Members are public and keep their original names so the derived tools' existing
 * `state->owner_sima` style access is unchanged.
 */
struct PaintSelectFloatingSession {
  /**
   * Which tool owns this session. Const and constructor-initialized: every derived state passes
   * its own #tool_type, so a session cannot come into existence untagged or be re-tagged later.
   *
   * \note The const-ness deletes copy-assignment on every derived state. That is intentional and
   * safe: all four are heap objects reached only through a pointer (#MEM_new / #MEM_delete), never
   * assigned, copied or stored in a container by value.
   */
  const PaintSelectTool tool;
  /** The Image Editor whose runtime owns this state. Never null once the session is live. */
  SpaceImage *owner_sima = nullptr;
  /** Region *type* the preview draw callback is registered on (callbacks are per type). */
  ARegionType *owner_region_type = nullptr;
  /** Image user pinned to the session's reference tile. */
  ImageUser iuser = {};
  /** Handle returned by #ED_region_draw_cb_activate, removed on every exit path. */
  void *draw_handle = nullptr;
  /**
   * True while an image undo step opened at lift time is still open.
   *
   * Stays false for the whole life of a gradient session: the gradient paints over per-tile
   * backups and only opens (and immediately closes) a step when it is confirmed, so it never owns
   * an open step the way the lifted-fragment tools do.
   */
  bool undo_begun = false;
  /** True only while a mouse drag gesture is in progress (a modal handler is running). */
  bool is_dragging = false;

  explicit PaintSelectFloatingSession(const PaintSelectTool tool) : tool(tool) {}
};

/** The live floating session of \a sima whatever tool it belongs to, or null when there is none. */
inline PaintSelectFloatingSession *image_select_session_active(const SpaceImage *sima)
{
  if (!sima || !sima->runtime) {
    return nullptr;
  }
  return sima->runtime->paint_select.active;
}

/**
 * The live floating session of \a sima as \a T, or null when the session belongs to another tool
 * (or there is none). This is the only sanctioned way down from the untyped slot to a concrete
 * state: the tag is checked before the cast, so a tool can never read another tool's memory
 * through its own type.
 *
 * \note \a T must be complete here, which in practice means each tool instantiates this in its own
 * translation unit. Callers that only need "is some session live" use
 * #image_select_session_active; callers in other translation units use that tool's own
 * `image_select_<tool>_state_get`.
 */
template<typename T> T *image_select_session_get(const SpaceImage *sima)
{
  PaintSelectFloatingSession *session = image_select_session_active(sima);
  if (!session || session->tool != T::tool_type) {
    return nullptr;
  }
  return static_cast<T *>(session);
}

/**
 * Install \a session as the live session of \a sima.
 *
 * The slot must be empty: a tool that lifts pixels has to end whatever was floating before (see
 * #image_select_floating_sessions_end) rather than overwrite it, which would leak the old state
 * and leave its draw callback pointing at freed memory.
 */
inline void image_select_session_set(SpaceImage *sima, PaintSelectFloatingSession *session)
{
  BLI_assert(sima && sima->runtime);
  BLI_assert(sima->runtime->paint_select.active == nullptr);
  sima->runtime->paint_select.active = session;
}

/**
 * Empty the slot without freeing anything. The caller owns the pointer it took out of it and is
 * responsible for tearing it down.
 */
inline void image_select_session_clear(SpaceImage *sima)
{
  if (sima && sima->runtime) {
    sima->runtime->paint_select.active = nullptr;
  }
}

/**
 * Free \a session through its concrete type, without a #bContext.
 *
 * For teardown paths that only have to release memory and GPU/callback resources -- editor close,
 * file load -- where committing or restoring canvas pixels is neither possible nor wanted. Use
 * #image_select_floating_sessions_end wherever a context exists.
 */
void image_select_floating_session_free(PaintSelectFloatingSession *session);

/**
 * True when \a state is the live floating session of \a sima.
 *
 * The state pointer already lives on `sima->runtime->paint_select`, so `owner_sima` can only ever
 * be `sima`; the check is kept as a cheap consistency assertion of that invariant and gives the
 * three tools one shared spelling of the predicate.
 */
bool image_select_floating_state_owns(const PaintSelectFloatingSession *state,
                                      const SpaceImage *sima);

/** Remove the preview draw callback and clear the handle. Safe to call more than once. */
void image_select_floating_draw_handle_clear(PaintSelectFloatingSession &session);

/**
 * Leave an active drag gesture.
 *
 * Clears #PaintSelectFloatingSession::is_dragging and restores the window cursor. Every modal
 * exit path must go through this: the drag sets #WM_CURSOR_NSEW_SCROLL, and an exit that skips
 * the restore leaves that cursor applied for the rest of the session. \a session may be null when
 * the state has already been freed.
 */
void image_select_floating_drag_end(bContext *C, PaintSelectFloatingSession *session);

/**
 * True when the runtime selection mask \a mask can be indexed with bounds derived from \a ibuf.
 *
 * Masks are allocated per tile at the tile's resolution, but the pixel loops derive their bounds
 * from the tile's #ImBuf. If an image was resized or reloaded at a different resolution after the
 * mask was built, the two disagree and writing through the mask runs past its allocation. Callers
 * must skip the tile entirely (not half-write it) when this returns false.
 */
bool image_select_mask_matches(const ImBuf *mask, int width, int height);

/** Convenience overload taking the dimensions from the tile buffer \a ibuf itself. */
bool image_select_mask_matches(const ImBuf *mask, const ImBuf *ibuf);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Cross-tool session teardown
 * \{ */

/**
 * End \a sima's floating session unless it already belongs to \a taking_over.
 *
 * A lifted session holds an image undo step *open*, and the next tool's
 * #ED_image_undo_push_begin frees that still-open step from under it (see the `ustack->step_init`
 * branch of #BKE_undosys_step_push_init_with_type). Every entry point that is about to open an undo
 * step must therefore first end the session it is taking the editor over from.
 *
 * The teardown follows the ending tool's own "the user moved on" path: move / transform / warp have
 * lifted pixels off the canvas, so their session is *committed* -- baked, undo step closed --
 * exactly as #image_select_transform_invoke already does for a floating move, which keeps the
 * user's in-progress edit instead of silently throwing it away. A gradient session is only an
 * unconfirmed preview over tile backups and holds no undo step of its own, so it is *restored and
 * discarded*, matching its own re-invoke and cancel paths.
 *
 * \param taking_over: the tool that is about to claim the editor. Its own session is left alone,
 * because a tool re-invoking over itself handles its previous session in its own way (re-drag,
 * re-anchor, or an explicit restore) rather than through a takeover.
 *
 * \note Must not be called on the move -> transform hand-over path
 * (#image_select_move_convert_to_transform), which deliberately passes one and the same open undo
 * step from the move session to the transform session.
 */
void image_select_floating_sessions_end(bContext *C, SpaceImage *sima, PaintSelectTool taking_over);

/**
 * End \a sima's floating session whichever tool it belongs to.
 *
 * For entry points that are not a floating tool themselves -- the mask gesture operators, Select
 * All / None / Invert -- and simply need the canvas settled before they open an undo step of their
 * own.
 */
void image_select_floating_sessions_end_all(bContext *C, SpaceImage *sima);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shared modal keymap and status bar
 * \{ */

/** Item ids of the "Image Paint Selection Floating Modal" keymap. */
enum {
  IMAGE_SELECT_FLOATING_MODAL_CONFIRM = 1,
  IMAGE_SELECT_FLOATING_MODAL_CANCEL = 2,
  IMAGE_SELECT_FLOATING_MODAL_UNDO_STEP = 3,
};

/**
 * Create (once) the modal keymap shared by the floating-selection operators and assign it to all
 * of them. The three tools bind the same three actions to the same events, so one keymap keeps
 * them configurable from a single place in the Keymap editor.
 */
wmKeyMap *image_select_floating_modal_keymap(wmKeyConfig *keyconf);

/**
 * Describe the available modal actions in the status bar. \a ot is the operator whose modal
 * keymap the shortcuts are read from, so the text follows the user's own bindings.
 */
void image_select_floating_status_set(bContext *C, const wmOperatorType *ot, bool has_undo_step);

/** Clear the status bar text set by #image_select_floating_status_set. */
void image_select_floating_status_clear(bContext *C);

/** \} */

}  /* namespace blender */
