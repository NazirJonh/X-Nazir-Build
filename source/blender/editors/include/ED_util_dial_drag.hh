/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <cstdint>

#include "DNA_windowmanager_enums.h"

namespace blender {

struct bContext;
struct wmEvent;
struct wmOperator;

namespace ed {

/**
 * Describes one angle that a modal "turn it with the cursor" operator drags. Must outlive the
 * operator (use a `static const`), as the drag keeps a pointer to it.
 */
struct DialDragParams {
  /** Reads the angle (radians) being dragged. Null when the target is not available. */
  float (*get_angle)(bContext *C);
  /**
   * Writes the angle (radians). Called on every mouse move, so it must not send notifiers; the
   * drag tags the region for redraw and sends #notifier when it ends.
   */
  void (*set_angle)(bContext *C, float angle);
  /** Untranslated label for the header text, shown as "Label: 12.0°". */
  const char *header_label;
  /**
   * Turn of the angle per unit of cursor bearing around the press point; with #horizontal, the
   * turn in radians for a cursor travel of the whole region width.
   */
  float cursor_gain;
  /** Notifier sent once when the drag ends, whether it was confirmed or cancelled. */
  uint32_t notifier;
  /**
   * Drive the angle by the cursor's horizontal travel from the press point instead of its bearing
   * around it, so the result does not depend on where in the region the drag started.
   */
  bool horizontal = false;
};

/** Angle step the Ctrl key snaps a #DialDragParams drag to, in degrees. */
constexpr float DIAL_DRAG_SNAP_DEGREES = 15.0f;

/**
 * Operator callbacks for a dial drag. Use from `invoke`, `modal` and `cancel`; the drag ends when
 * the button that started it is released (confirm) or Escape is pressed (restore the start angle).
 */
wmOperatorStatus dial_drag_invoke(bContext *C,
                                  wmOperator *op,
                                  const wmEvent *event,
                                  const DialDragParams &params);
wmOperatorStatus dial_drag_modal(bContext *C, wmOperator *op, const wmEvent *event);
void dial_drag_cancel(bContext *C, wmOperator *op);

}  // namespace ed
}  // namespace blender
