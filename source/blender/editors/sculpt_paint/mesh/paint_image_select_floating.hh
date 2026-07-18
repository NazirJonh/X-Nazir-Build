/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * State and helpers shared by the three floating-fragment tools (move / transform / warp).
 *
 * Each tool keeps a heap-allocated state object alive on #SpaceImage_Runtime::paint_select for as
 * long as a fragment is floating. The three state structs used to repeat the same six members and
 * the same lifetime plumbing (draw-handle removal, cursor restore, "is a fragment floating in this
 * space" predicates); that shared part now lives in #PaintSelectFloatingSession, which the three
 * derive from.
 *
 * The base is deliberately non-polymorphic: every state object is created with #MEM_new and
 * destroyed with #MEM_delete through its *concrete* type, so no virtual destructor is needed and
 * none must be added.
 *
 * NOTE: This header is included from #paint_image_select_intern.hh, which is what consumers
 * should include.
 */

#pragma once

#include "DNA_image_types.h"

struct ARegion;
struct ARegionType;
struct ImBuf;
struct SpaceImage;
struct bContext;
struct wmKeyConfig;
struct wmKeyMap;
struct wmOperatorType;

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Shared floating session
 * \{ */

/**
 * The part of a floating tool's state that is identical across move / transform / warp.
 *
 * \note Members are public and keep their original names so the derived tools' existing
 * `state->owner_sima` style access is unchanged.
 */
struct PaintSelectFloatingSession {
  /** The Image Editor whose runtime owns this state. Never null once the session is live. */
  SpaceImage *owner_sima = nullptr;
  /** Region *type* the preview draw callback is registered on (callbacks are per type). */
  ARegionType *owner_region_type = nullptr;
  /** Image user pinned to the session's reference tile. */
  ImageUser iuser = {};
  /** Handle returned by #ED_region_draw_cb_activate, removed on every exit path. */
  void *draw_handle = nullptr;
  /** True while an image undo step opened at lift time is still open. */
  bool undo_begun = false;
  /** True only while a mouse drag gesture is in progress (a modal handler is running). */
  bool is_dragging = false;
};

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

/** One bit per floating tool, for #image_select_floating_sessions_end. */
enum {
  IMAGE_SELECT_FLOATING_TOOL_MOVE = (1 << 0),
  IMAGE_SELECT_FLOATING_TOOL_TRANSFORM = (1 << 1),
  IMAGE_SELECT_FLOATING_TOOL_GRADIENT = (1 << 2),
  IMAGE_SELECT_FLOATING_TOOL_WARP = (1 << 3),
};

/**
 * End the floating sessions of \a sima named in \a tools, leaving the others untouched.
 *
 * #PaintSelectSession keeps a separate slot per tool, so more than one session can be live at
 * once -- which is never wanted. A lifted session holds an image undo step *open*, and the next
 * tool's #ED_image_undo_push_begin frees that still-open step from under it (see the
 * `ustack->step_init` branch of #BKE_undosys_step_push_init_with_type), leaving the older session
 * with `undo_begun == true` pointing at freed memory. Every entry point that is about to open an
 * undo step must therefore first end every session it is not itself taking over.
 *
 * Per tool the teardown follows that tool's own "the user moved on" path: move / transform / warp
 * have lifted pixels off the canvas, so their session is *committed* -- baked, undo step closed --
 * exactly as #image_select_transform_invoke already does for a floating move, which keeps the
 * user's in-progress edit instead of silently throwing it away. A gradient session is only an
 * unconfirmed preview over tile backups and holds no undo step of its own, so it is *restored and
 * discarded*, matching its own re-invoke and cancel paths.
 *
 * \note Must not be called on the move -> transform hand-over path
 * (#image_select_move_convert_to_transform), which deliberately passes one and the same open undo
 * step from the move session to the transform session.
 */
void image_select_floating_sessions_end(bContext *C, SpaceImage *sima, int tools);

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
