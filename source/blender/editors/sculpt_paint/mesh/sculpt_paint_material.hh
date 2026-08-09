/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#pragma once

#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

#include "DNA_brush_types.h"
#include "DNA_scene_types.h"

namespace blender {

struct Depsgraph;
struct Sculpt;
struct Object;
struct PaintModeSettings;
struct Scene;
class IndexMask;

namespace ed::sculpt_paint::material {

/**
 * Whether \a ob can be painted by Poly Paint at all.
 *
 * Material paint stores its channels in mesh point attributes, which only exist for a
 * #bke::pbvh::Type::Mesh session: Multires evaluates into grids and Dyntopo into a #BMesh, neither
 * of which the paint or the undo path can address. Callers must check this before creating
 * attributes or pushing an undo step, not only before painting: preparing state for an object that
 * can never be painted leaves attributes behind and pushes undo nodes with no stored data.
 *
 * Deliberately phrased in terms of original data (modifiers and Dyntopo flag) rather than the
 * #bke::pbvh::Tree, because the tree is not necessarily built yet when a stroke starts.
 */
bool paint_supported_on_object(const Scene &scene, Object &ob);

/**
 * Poly Paint: paints every enabled material channel from the active brush's
 * #BrushMaterialPaint in one stroke.
 * The Base Color channel goes into the mesh color attribute, the scalar channels into their
 * float point attributes. The brush supplies the stroke parameters (blend mode, falloff).
 *
 * The target attributes must already exist; they are created once per stroke by
 * #brush_stroke_init so that creation is part of the stroke's undo step.
 */
void do_paint_material_brush(const Depsgraph &depsgraph,
                             const Sculpt &sd,
                             Object &ob,
                             const IndexMask &node_mask,
                             const PaintModeSettings &paint_mode_settings);

/**
 * The point float attribute names written by the enabled scalar channels, without duplicates.
 * Shared by the brush and by the undo push so both agree on exactly which attributes a stroke
 * touches.
 */
Vector<StringRef, PAINT_MATERIAL_CHANNEL_NUM> enabled_scalar_attribute_names(
    const BrushMaterialPaint &brush_paint, const PaintModeSettings &mode_settings);

}  // namespace ed::sculpt_paint::material

}  // namespace blender
