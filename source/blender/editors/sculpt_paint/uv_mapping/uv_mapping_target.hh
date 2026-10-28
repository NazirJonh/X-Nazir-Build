/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * The editor-facing target of the UV mapping gizmo: what row is being edited and how its
 * mapping is read and written.
 *
 * #PaintLayerRowMappingTarget is the first implementation: a Fill layer, a Fill correction or a
 * mask item of the active object's layered material. It resolves the same row the Mapping panel
 * edits (see `LAYER_MATERIAL_PT_mapping::_shown_row`) and only while the Image Editor shows that
 * material's image. A future feature plugs a target of its own into the gizmo without touching
 * the gizmo's gestures or drawing.
 *
 * The target stores the material and the row's stable marker rather than a pointer: every read
 * and write finds the row again, so reordering, an undo or a row removal cannot leave it
 * dangling. Writes are value-only: they never rebuild the layer graph, which is what makes them
 * safe to call on every drag step.
 */

#pragma once

#include "DNA_uuid_types.h"

#include "uv_mapping_math.hh"

namespace blender {
struct bContext;
struct Material;
struct MaterialPaintLayer;
}  // namespace blender

namespace blender::ed::sculpt_paint::uv_mapping {

/**
 * The row the Mapping panel shows: the shown mask item, else the active row (its base mask while
 * the brush targets masks). The C++ mirror of `LAYER_MATERIAL_PT_mapping::_shown_row`; the caller
 * resolves the material. Null when there is none.
 */
MaterialPaintLayer *mapping_shown_row_get(const bContext &C, Material &ma);

class PaintLayerRowMappingTarget {
 public:
  /**
   * Resolve the row the Mapping panel shows and check that the Image Editor edits the same
   * material. Returns false when there is nothing to edit right now: no row, no mapping, or one
   * the user switched off. A Fill answers yes before it has a map, its mapping being a property
   * of the row.
   */
  bool resolve(const bContext &C);

  /** Read the row's mapping. False when the row is gone or no longer carries a mapping. */
  bool read(UvMappingXform &r_xform) const;

  /** Write the row's mapping through the value-only BKE setters. */
  bool write(const UvMappingXform &xform);

  /** Whether the row's scale axes move together. */
  bool scale_locked() const;

  /** Lock or unlock the row's scale axes (the Mapping panel's Lock toggle). */
  bool set_scale_locked(bool locked);

  const char *undo_name() const;

  Material *material() const
  {
    return material_;
  }
  const bUUID &marker() const
  {
    return marker_;
  }

 private:
  MaterialPaintLayer *row() const;

  Material *material_ = nullptr;
  bUUID marker_ = {};
};

}  // namespace blender::ed::sculpt_paint::uv_mapping
