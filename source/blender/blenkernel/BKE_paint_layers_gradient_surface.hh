/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * Per-object UV-to-object-space surfaces for the 2D Image Editor preview of Object/Generated
 * Gradient rows.
 *
 * A Gradient row's shader reads a 3D coordinate. In the Image Editor there is only a texel, so the
 * 2D preview has to know which object-space point that texel's UV maps to. One surface holds that
 * mapping for one object and one UV map, and is cached because rasterizing it is O(UV area) and
 * must not happen on every redraw.
 *
 * The builder is deliberately separate from #BKE_paint_layers_composite_image_layers: the latter
 * only samples the #PaintMaterialCompositeSurface, so a caller that already has one (a test, a
 * future asynchronous builder) needs no object at all.
 *
 * Known limitations: only the 0..1 UV square is rasterized (no UDIM tiles), and overlapping UV
 * islands are resolved deterministically by the last triangle that covers a texel. The surface is
 * a preview aid, not a bake.
 */

#include <cstdint>
#include <memory>

#include "BLI_math_vector_types.hh"
#include "BLI_string_ref.hh"

#include "BKE_paint_material_composite_surface.hh"

namespace blender {

struct Mesh;
struct Object;

/**
 * The cached UV-space surface of \a mesh as deformed by \a object, or null when the mesh has no
 * \a uv_map_name corner UV layer.
 *
 * \param mesh: evaluated mesh; its triangles and positions are what the shader sees.
 * \param size: requested square resolution, clamped to 256..1024. The surface carries both the
 *             evaluated object-space position (for Object rows) and, when the mesh has a
 *             #CD_ORCO layer, the shader's generated coordinate input; the texture-space bounding
 *             box is the fallback for an undeformed mesh.
 *
 * Repeated calls with the same object, UV map, size and geometry revision return the same shared
 * surface. The caller may hold it while the cache replaces its own entry.
 */
std::shared_ptr<const PaintMaterialCompositeSurface>
BKE_paint_layers_gradient_surface_get(const Object &object,
                                      const Mesh &mesh,
                                      const char *uv_map_name,
                                      int size);

/**
 * The UV map the generated shader reads as `Texture Coordinate.UV`: the render-active/default map,
 * not the editable active one. Empty when the mesh has no UV map.
 */
StringRefNull BKE_paint_layers_gradient_uv_map_name(const Mesh &mesh);

/**
 * The texture-space box the shader's `Generated` coordinate is normalized by: its center and half
 * extent. A flat axis gets one unit of extent. Falls back to the bounds of \a object (nullable)
 * when the mesh's texture space is empty.
 *
 * \return false when neither gives a usable box.
 */
bool BKE_paint_layers_gradient_generated_box_get(const Object *object,
                                                 const Mesh &mesh,
                                                 float3 &r_location,
                                                 float3 &r_size);

/** An object-space \a position as the shader's `Generated` coordinate (0..1 over the box). */
bool BKE_paint_layers_gradient_generated_from_object(const Object *object,
                                                     const Mesh &mesh,
                                                     const float3 &position,
                                                     float3 &r_generated);

/** The inverse of #BKE_paint_layers_gradient_generated_from_object. */
bool BKE_paint_layers_gradient_generated_to_object(const Object *object,
                                                   const Mesh &mesh,
                                                   const float3 &generated,
                                                   float3 &r_position);

/** Drop every cached surface. For teardown, file load and a wholesale geometry change. */
void BKE_paint_layers_gradient_surface_cache_clear();

/** Drop the cached surfaces of one object, identified by #ID.session_uid. */
void BKE_paint_layers_gradient_surface_cache_clear_for_object(uint32_t object_session_uid);

}  // namespace blender
