/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 * \brief Functions for weight painting on curves.
 */

#include "BLI_span.hh"

struct Object;
struct MDeformVert;

namespace blender::bke::curves {

/* Weight assignment modes */
constexpr int WEIGHT_REPLACE = 1;
constexpr int WEIGHT_ADD = 2;
constexpr int WEIGHT_SUBTRACT = 3;

/**
 * Ensure that the curves object has deform vertices allocated.
 * This function creates the deform vertices array if it doesn't exist.
 */
void ensure_deform_verts(Object *ob);

/**
 * Get the weight of a specific vertex in a vertex group.
 * \param ob: The curves object.
 * \param vertex_index: Index of the vertex.
 * \param def_nr: Index of the vertex group.
 * \return Weight value (0.0 to 1.0) or -1.0 if not found.
 */
float get_vertex_group_weight(const Object *ob, int vertex_index, int def_nr);

/**
 * Set the weight of a specific vertex in a vertex group.
 * \param ob: The curves object.
 * \param vertex_index: Index of the vertex.
 * \param def_nr: Index of the vertex group.
 * \param weight: Weight value to set (0.0 to 1.0).
 * \param assign_mode: How to assign the weight (WEIGHT_REPLACE, WEIGHT_ADD, WEIGHT_SUBTRACT).
 */
void set_vertex_group_weight(Object *ob, int vertex_index, int def_nr, float weight, int assign_mode);

/**
 * Normalize weights for a specific vertex across all vertex groups.
 * \param ob: The curves object.
 * \param vertex_index: Index of the vertex to normalize.
 * \param lock_active: Whether to lock the active vertex group during normalization.
 * \param auto_normalize: Whether to perform automatic normalization.
 */
void normalize_point_weights(Object *ob, int vertex_index, bool lock_active, bool auto_normalize);

/**
 * Remove a vertex from a specific vertex group.
 * \param ob: The curves object.
 * \param vertex_index: Index of the vertex.
 * \param def_nr: Index of the vertex group.
 */
void remove_vertex_from_group(Object *ob, int vertex_index, int def_nr);

/**
 * Get the total number of vertices in the curves object.
 * \param ob: The curves object.
 * \return Number of vertices.
 */
int get_curves_vertex_count(const Object *ob);

/**
 * Check if the curves object has deform vertices.
 * \param ob: The curves object.
 * \return True if deform vertices exist.
 */
bool has_deform_verts(const Object *ob);

}  // namespace blender::bke::curves