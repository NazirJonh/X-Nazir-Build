/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup overlay
 */

#pragma once

#include "BKE_paint.hh"
#include "BKE_curves.hh"

#include "DNA_curves_types.h"
#include "DNA_object_types.h"

#include "overlay_base.hh"

namespace blender::draw::overlay {

/**
 * Weight Paint overlay for Curves Hair.
 * Visualizes vertex group weights on curve points with color coding and fake shading.
 */
class CurvesWeightPaint : Overlay {
 private:
  PassMain curves_weight_ps_ = {"CurvesWeightPaint"};
  PassMain::Sub *weight_ps_ = nullptr;
  PassMain::Sub *weight_fake_shading_ps_ = nullptr;
  
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
  bool is_curves_weight_paint_mode(const Object *object, const State &state);
  void curves_weight_sync(Manager &manager, 
                         const ObjectRef &ob_ref, 
                         Resources &res, 
                         const State &state);
};

}  // namespace blender::draw::overlay
