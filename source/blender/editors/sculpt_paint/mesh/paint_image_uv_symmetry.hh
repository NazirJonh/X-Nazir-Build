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

struct BMesh;
struct Object;

namespace blender {

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

}  // namespace blender
