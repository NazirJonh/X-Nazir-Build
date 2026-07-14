/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#pragma once

#include "BLI_index_mask_fwd.hh"
#include "BLI_span.hh"

namespace blender {

struct bContext;
struct Depsgraph;
struct Object;
struct wmOperatorType;
namespace bke::pbvh {
class Node;
}

namespace ed::sculpt_paint::hide {
void sync_all_from_faces(Object &object);
void mesh_show_all(const Depsgraph &depsgraph, Object &object, const IndexMask &node_mask);
void grids_show_all(Depsgraph &depsgraph, Object &object, const IndexMask &node_mask);
/**
 * Tag every object whose visibility changed, so the 3D viewport picks the change up immediately.
 *
 * #flush_update_done only tags the 3D regions for redraw; it deliberately does not tag the object
 * itself, because during a brush stroke #flush_update_step already did so on every step. One-shot
 * visibility operators have no step phase, so without this the viewport keeps drawing the previous
 * state until something else (a cursor update, a view rotation) happens to force a refresh.
 */
void tag_update_visibility(const bContext &C, Span<Object *> objects);

void PAINT_OT_hide_show_masked(wmOperatorType *ot);
void PAINT_OT_hide_show_all(wmOperatorType *ot);
void PAINT_OT_hide_show(wmOperatorType *ot);
void PAINT_OT_hide_show_lasso_gesture(wmOperatorType *ot);
void PAINT_OT_hide_show_line_gesture(wmOperatorType *ot);
void PAINT_OT_hide_show_polyline_gesture(wmOperatorType *ot);

void PAINT_OT_visibility_invert(wmOperatorType *ot);
void PAINT_OT_visibility_filter(wmOperatorType *ot);
}  // namespace ed::sculpt_paint::hide

}  // namespace blender
