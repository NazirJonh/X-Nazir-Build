/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editors
 */

#pragma once

struct bContext;
struct bNode;
struct Object;

namespace blender {

struct wmOperatorType;
struct wmKeyConfig;

/* -------------------------------------------------------------------- */
/** \name Mode Entry/Exit
 * \{ */

/**
 * Poll function for GN Selection Mode operators.
 * Checks if the active object has a Geometry Nodes modifier.
 */
bool ED_gn_selection_mode_poll(const bContext *C);

/**
 * Enter GN Selection Mode for the given node.
 * If node is NULL, finds the first 3D View Selection node in the modifier's node tree.
 *
 * \return true if mode was entered successfully.
 */
bool ED_gn_selection_mode_enter(bContext *C, bNode *node);

/**
 * Exit GN Selection Mode.
 *
 * \param confirm: If true, save selection to node storage. If false, discard changes.
 */
void ED_gn_selection_mode_exit(bContext *C, bool confirm);

/**
 * Check if GN Selection Mode is active for the given object.
 */
bool ED_gn_selection_mode_active(const Object *ob);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Operators
 * \{ */

/**
 * Select element under cursor in GN Selection Mode.
 */
void GN_OT_select(wmOperatorType *ot);

/**
 * Confirm selection and exit GN Selection Mode.
 */
void GN_OT_selection_confirm(wmOperatorType *ot);

/**
 * Cancel selection and exit GN Selection Mode.
 */
void GN_OT_selection_cancel(wmOperatorType *ot);

/**
 * Set object to GN Selection Mode (from object mode).
 */
void OBJECT_OT_gn_selection_mode_set(wmOperatorType *ot);

/**
 * Enter GN Selection Mode from node editor.
 */
void NODE_OT_gn_selection_enter(wmOperatorType *ot);

/**
 * Clear selection in 3D View Selection node.
 */
void NODE_OT_gn_selection_clear(wmOperatorType *ot);

/**
 * Switch selection domain in GN Selection Mode (Vertex/Edge/Face).
 */
void GN_OT_select_mode(wmOperatorType *ot);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Keymap
 * \{ */

/**
 * Register GN Selection Mode keymap.
 */
void view3d_keymap_gn_selection(wmKeyConfig *keyconf);

/** \} */

}  // namespace blender
