/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_span.hh"

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

/**
 * A hash of \a mesh's positions, topology, render UV, material indices and corner normals. The
 * heavy content hash is cached per mesh and re-run only when a cheap fingerprint (array pointers,
 * counts, a strided sample) moves or the cached value is a second old, so calling it every frame
 * costs a few thousand reads.
 */
uint64_t composite_mesh_content_hash(const Mesh &mesh);

/** A premultiplied scene-linear transparent buffer of \a width by \a height, or null. */
ImBuf *composite_alloc_transparent(int width, int height);

/**
 * Bake \a src into the painted object's UV space through the projection of \a mapping. Null when
 * the mapping names no object, mesh or render UV map.
 *
 * \a input_steps are the row's transform effects (Transform Mapping, Mirror), in the effects list's
 * order. They act on the map coordinate the projection computes, before the row's own UV adjust;
 * a clipping wrap on a step makes the texel transparent outside that coordinate's unit square. The
 * Cube mode runs them in the local [0, 1] space of the tap's own atlas cell.
 */
ImBuf *composite_resample_projected(const ImBuf *src,
                                    int ref_width,
                                    int ref_height,
                                    const PaintMaterialCompositeMapping &mapping,
                                    Span<PaintMaterialCompositeMapping> input_steps);

}  // namespace blender
