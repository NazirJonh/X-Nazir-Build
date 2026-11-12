/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

struct ImBuf;

namespace blender {

struct Mesh;
struct Object;
struct PaintMaterialCompositeMapping;

/**
 * The mesh a projected row bakes through: the evaluated mesh when \a object is an evaluated object,
 * so the bake sees the modifiers' UV and positions the 3D viewport shows, else the original data.
 * Null when \a object is not a mesh object or has no mesh.
 */
const Mesh *composite_projection_mesh(const Object *object);

/** A premultiplied scene-linear transparent buffer of \a width by \a height, or null. */
ImBuf *composite_alloc_transparent(int width, int height);

/**
 * Bake \a src into the painted object's UV space through the projection of \a mapping. Null when
 * the mapping names no object, mesh or render UV map.
 */
ImBuf *composite_resample_projected(const ImBuf *src,
                                    int ref_width,
                                    int ref_height,
                                    const PaintMaterialCompositeMapping &mapping);

}  // namespace blender
