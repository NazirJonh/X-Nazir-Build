/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Internal constants and helpers for the Sculpt Lattice Tool.
 * Single source of truth for thresholds shared between pick, deform, overlay.
 *
 * See .My_Docs_July_2026/Sculpt-Mode/Lattice-Tool/Plan_1/03_data_model.md
 */

#pragma once

#include "BLI_math_vector_types.hh"

struct LatticeDeformData;
struct Object;

namespace blender::ed::sculpt_paint::lattice {

/* The mask epsilon lives in the public header as #SCULPT_LATTICE_MASK_EPS_DEFAULT
 * (single source of truth); do not redefine it here. */

/* Screen-space Manhattan distance threshold (px) for picking a BPoint. */
constexpr float PAINT_LATTICE_POINT_PICK_THRESHOLD = 40.0f;

/* Minimum lattice resolution per axis (ADR-8). */
constexpr int SCULPT_LATTICE_MIN_RESOLUTION = 2;

/**
 * (Re)builds lattice deform data from the live #Lattice.def control points.
 * Clears stale evaluated displists so #BKE_lattice_deform_data_create does not
 * read outdated cage geometry from the draw cache.
 */
LatticeDeformData *sculpt_lattice_deform_data_rebuild(Object *lat_ob, Object *mesh_ob);

}  // namespace blender::ed::sculpt_paint::lattice
