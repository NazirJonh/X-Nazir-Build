/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Mirror symmetry for Image Paint geometry fill.
 *
 * Both entry points reduce to the same input: a hit position in object space. The 3D
 * viewport gets it from its ray-cast, the 2D Image Editor recovers it from the clicked
 * UV. Mirroring that point and locating the nearest face yields extra seed faces, which
 * the caller feeds into the normal expand step — so expand, tile bucketing and the
 * rasterizer stay unaware of symmetry.
 */

#pragma once

#include "BLI_math_vector_types.hh"
#include "BLI_vector.hh"


namespace blender {

struct BMesh;
struct Object;

/**
 * Mirror \a hit_position across every enabled axis combination and append the face
 * closest to each mirrored point.
 *
 * \param symmetry_flags: bit mask of ME_SYMMETRY_X / _Y / _Z, as stored in Mesh::symmetry.
 * \param hit_position: the hit point in the object space of \a ob.
 * \param r_seed_faces: in-out. Arrives holding the original seed faces; mirrored faces are
 * appended, already-present indices are not duplicated.
 *
 * A candidate is rejected when the mirrored point is farther from the located face than
 * a tolerance relative to the mesh size. Without that test an asymmetric model would
 * silently fill an arbitrary face on the far side.
 */
void image_paint_symmetry_mirror_faces(Object *ob,
                                       BMesh *bm,
                                       const float3 &hit_position,
                                       int symmetry_flags,
                                       Vector<int> &r_seed_faces);

/**
 * The UV the active UV map carries at the mesh point nearest \a position (object space).
 *
 * This is the bridge from a 3D point -- the sculpt 3D cursor, a viewport hit, a snap -- to the UV
 * space a row's mapping lives in: the triangle holding the point is located, \a position is
 * projected onto it, and the triangle's corner UVs are interpolated by the projected barycentric
 * weights. \return false when there is no mesh, no active UV map, or the point is farther from the
 * surface than a small fraction of the mesh's own size (2% of its bounding-box diagonal): a mesh
 * with nothing under the point reads no UV rather than the UV of whichever triangle is nearest.
 *
 * \param corner_tri_hint: the corner triangle already known to hold \a position -- a ray-cast
 * hit's index, which a caller holding one passes so the search is skipped -- or -1 to locate it.
 *
 * Object Mode only: the Mesh data is read, not a BMesh being edited. A caller in Edit Mode should
 * use the edit-mesh route and pass object-space coordinates from it.
 */
bool image_paint_symmetry_uv_at_position(Object *ob,
                                         const float3 &position,
                                         float r_uv[2],
                                         int corner_tri_hint = -1);

}  // namespace blender
