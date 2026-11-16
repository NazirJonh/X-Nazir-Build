/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup edsculpt
 *
 * The geometric half of a Curve Patch Space switch: re-expressing curve coordinates between the
 * object space of a mesh and the canvas UV space of its active UV map. Free of any context or
 * material, so it can be reasoned about (and tested) on a mesh and curves alone.
 */

#include "BLI_span.hh"

namespace blender {

struct Mesh;

namespace bke {
class CurvesGeometry;
}

namespace ed::sculpt_paint {

/**
 * Convert the point and handle positions of every curve in \a curves between the object space of
 * \a mesh and the canvas UV space (x/y carry the UV, z is 0) of its active UV map: object space to
 * UV with \a to_uv, UV back to object space otherwise.
 *
 * All or nothing: a point that has no counterpart (an object-space point on a mesh without
 * triangles, a UV point outside every UV island) makes the whole call fail without touching any
 * curve, so a switch never leaves coordinates of two spaces mixed in one container. Auto handles
 * are recomputed and the positions tagged changed on success.
 *
 * \return False when the mesh cannot convert (no UV map, no triangles) or any point has no
 * counterpart; \a r_failed_points_num then holds the number of points that missed (0 for a mesh
 * that cannot convert at all).
 */
bool curve_patch_space_convert(const Mesh &mesh,
                               Span<bke::CurvesGeometry *> curves,
                               bool to_uv,
                               int &r_failed_points_num);

}  // namespace ed::sculpt_paint

}  // namespace blender
