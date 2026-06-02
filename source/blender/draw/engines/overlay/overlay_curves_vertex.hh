/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup overlay
 */

#pragma once

#include "DNA_object_types.h"

#include "overlay_base.hh"

namespace blender::draw::overlay {

/**
 * Vertex Paint overlay for Curves.
 * Visualizes the `vertex_color` point attribute in the viewport.
 */
class CurvesVertexPaint : Overlay {
 private:
  PassMain curves_vertex_ps_ = {"CurvesVertexPaint"};
  PassMain::Sub *color_ps_ = nullptr;
  PassMain::Sub *color_fake_shading_ps_ = nullptr;

  bool enabled_ = false;
  bool use_fake_shading_ = false;

 public:
  void begin_sync(Resources &res, const State &state) final;
  void object_sync(Manager &manager,
                   const ObjectRef &ob_ref,
                   Resources &res,
                   const State &state) final;
  void draw(Framebuffer &fb, Manager &manager, View &view) final;
  void draw_on_render(gpu::FrameBuffer *fb, Manager &manager, View &view) final;

 private:
  bool is_curves_vertex_paint_mode(const Object *object, const State &state);
  void curves_vertex_sync(Manager &manager, const ObjectRef &ob_ref, const State &state);
};

}  // namespace blender::draw::overlay
