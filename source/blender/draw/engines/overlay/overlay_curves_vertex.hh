/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup overlay
 */

#pragma once

#include "DNA_object_types.h"

#include "overlay_curves_paint_base.hh"

namespace blender::draw::overlay {

/**
 * Vertex Paint overlay for Curves.
 * Visualizes the `vertex_color` point attribute in the viewport.
 */
class CurvesVertexPaint : public CurvesPaintOverlayBase {
 public:
  CurvesVertexPaint() : CurvesPaintOverlayBase("CurvesVertexPaint") {}

 protected:
  eObjectMode paint_object_mode() const final
  {
    return OB_MODE_VERTEX_CURVES;
  }
  bool is_paint_mode(const Object *object, const State &state) final;
  void setup_passes(Resources &res, const State &state) final;
  gpu::Batch *geometry_batch_get(Object *object) final;
};

}  // namespace blender::draw::overlay
