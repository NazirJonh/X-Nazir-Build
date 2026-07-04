/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#pragma once

#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

#include "DNA_scene_types.h"

namespace blender {

struct Depsgraph;
struct Sculpt;
struct Object;
struct PaintModeSettings;
class IndexMask;

namespace ed::sculpt_paint::material {

/**
 * Poly Paint: paints every enabled material channel from \a paint_mode_settings in one stroke.
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
    const PaintModeSettings &settings);

}  // namespace ed::sculpt_paint::material

}  // namespace blender
