/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#pragma once

#include "DNA_scene_types.h"

#include "BLI_generic_span.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_offset_indices.hh"
#include "BLI_span.hh"

namespace blender {

struct Depsgraph;
struct Mesh;
struct Object;
namespace bke {
enum class AttrDomain : int8_t;
struct GAttributeReader;
struct GSpanAttributeWriter;
namespace pbvh {
class Node;
}
}  // namespace bke

namespace ed::sculpt_paint::color {

/* Swaps colors at each element in indices with values in colors. */
void swap_gathered_colors(Span<int> indices,
                          GMutableSpan color_attribute,
                          MutableSpan<float4> r_colors);

/* Stores colors from the elements in indices into colors. */
void gather_colors(GSpan color_attribute, Span<int> indices, MutableSpan<float4> r_colors);

/* Like gather_colors but handles loop->vert conversion */
void gather_colors_vert(OffsetIndices<int> faces,
                        Span<int> corner_verts,
                        GroupedSpan<int> vert_to_face_map,
                        GSpan color_attribute,
                        bke::AttrDomain color_domain,
                        Span<int> verts,
                        MutableSpan<float4> r_colors);

void color_vert_set(OffsetIndices<int> faces,
                    Span<int> corner_verts,
                    GroupedSpan<int> vert_to_face_map,
                    bke::AttrDomain color_domain,
                    int vert,
                    const float4 &color,
                    GMutableSpan color_attribute);
float4 color_vert_get(OffsetIndices<int> faces,
                      Span<int> corner_verts,
                      GroupedSpan<int> vert_to_face_map,
                      GSpan color_attribute,
                      bke::AttrDomain color_domain,
                      int vert);

bke::GAttributeReader active_color_attribute(const Mesh &mesh);
bke::GSpanAttributeWriter active_color_attribute_for_write(Mesh &mesh);

/**
 * Align every mesh in \a other_meshes to one shared color channel. The reference channel is the
 * active color attribute of \a active_mesh (name, domain, type). Per mesh: an existing attribute
 * with the reference name that is a valid color attribute is made active as-is (no data
 * conversion); a missing attribute is created with the reference name/domain/type and made
 * active; a name collision with a non-color attribute silently skips that mesh (painting will
 * skip it too). Does nothing when \a active_mesh has no valid active color attribute.
 */
void ensure_shared_color_attributes(Mesh &active_mesh, Span<Mesh *> other_meshes);

/**
 * Object-level wrapper: first ensures the active object itself has a valid active color
 * attribute (creating the default "Color" Point/ColorFloat channel via
 * #BKE_sculpt_color_layer_create_if_needed when it has none), then aligns every mesh object in
 * \a objects to that channel. Non-mesh objects are skipped.
 */
void ensure_shared_color_attributes(Object &active_object, Span<Object *> objects);

void do_paint_brush(const Depsgraph &depsgraph,
                    PaintModeSettings &paint_mode_settings,
                    const Sculpt &sd,
                    Object &ob,
                    const IndexMask &node_mask,
                    const IndexMask &texnode_mask);
void do_smear_brush(const Depsgraph &depsgraph,
                    const Sculpt &sd,
                    Object &ob,
                    const IndexMask &node_mask);
void do_blur_brush(const Depsgraph &depsgraph,
                   const Sculpt &sd,
                   Object &ob,
                   const IndexMask &node_mask);
}  // namespace ed::sculpt_paint::color

}  // namespace blender
