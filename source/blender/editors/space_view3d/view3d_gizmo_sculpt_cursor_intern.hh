/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spview3d
 * Helpers shared by the sculpt cursor gizmo and its viewport buttons group.
 */

#pragma once

struct Object;
struct RegionView3D;
struct Scene;

namespace blender::ed::view3d {

/** World matrix of the sculpt cursor of \a ob. */
void sculpt_cursor_world_matrix_get(const Scene &scene, const Object &ob, float r_mat[4][4]);

/**
 * Factor applied to every cursor gizmo handle's `scale_basis`: the user size, and in world size
 * mode also the compensation for the view distance.
 * \param co: World location of the gizmo.
 */
float gizmo_size_factor_get(const Scene &scene,
                            const Object &ob,
                            const RegionView3D &rv3d,
                            const float co[3]);

}  // namespace blender::ed::view3d
