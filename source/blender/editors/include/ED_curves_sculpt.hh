/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editors
 */

#pragma once

#include <cstdint>

#include "BLI_span.hh"

struct bContext;

namespace blender {

struct Curves;

void ED_operatortypes_sculpt_curves();

/**
 * Restart the mode-transfer flash on every Curves object the current multi-object edit scope
 * covers, so that changing the scope reads as "this is what you are editing now".
 */
void ED_curves_sculpt_flash_edit_scope(bContext *C);

/** Whether the brush influence highlight currently publishes per-point weights for
 * \a curves_orig (a deforming sculpt curves stroke is running). Used by the draw module to skip
 * the influence vertex buffer entirely when the feature is not in use. */
bool ED_curves_sculpt_has_influence(const Curves &curves_orig);

/** Whether the influence record for \a curves_orig comes from the hover preview rather than a
 * running stroke (see #sculpt_influence_viz.cc). */
bool ED_curves_sculpt_influence_is_hover(const Curves &curves_orig);

/**
 * Per-point brush influence published by the deforming sculpt curves brushes, normalized to
 * 0..1 (see #sculpt_influence_viz.cc). Empty when the highlight is inactive.
 */
Span<float> ED_curves_sculpt_get_influence(const Curves &curves_orig);

/**
 * Monotonic version of the influence record of \a curves_orig (0 when never published). The draw
 * cache compares it to detect hover/stroke-preview changes without re-evaluating the curves
 * geometry (see #DRW_curves_batch_cache_validate).
 */
uint32_t ED_curves_sculpt_influence_version(const Curves &curves_orig);

}  // namespace blender
