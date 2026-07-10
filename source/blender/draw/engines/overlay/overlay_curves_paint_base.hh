/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup overlay
 */

#pragma once

#include "DNA_object_enums.h"

#include "overlay_base.hh"

struct Object;

namespace blender::gpu {
class Batch;
}

namespace blender::draw::overlay {

/**
 * Shared base for the Curves weight/vertex paint overlays.
 *
 * Both overlays follow the same structure: a single #PassMain with a main sub-pass and an optional
 * fake-shading sub-pass, drawing the same point/line geometry. Only the paint-mode check, the
 * sub-pass setup (shader and push constants) and the geometry batch differ, so those are exposed as
 * hooks implemented by the concrete overlays.
 */
class CurvesPaintOverlayBase : public Overlay {
 protected:
  PassMain main_ps_;
  PassMain::Sub *main_sub_ = nullptr;
  PassMain::Sub *fake_shading_sub_ = nullptr;

  bool use_fake_shading_ = false;

  CurvesPaintOverlayBase(const char *pass_name) : main_ps_(pass_name) {}

  /** Object mode this overlay is active in (weight or vertex curves). */
  virtual eObjectMode paint_object_mode() const = 0;

  /** Whether \a object is currently in this paint mode and has data to visualize. */
  virtual bool is_paint_mode(const Object *object, const State &state) = 0;

  /**
   * Create the main (and optional fake-shading) sub-passes on #main_ps_ and assign #main_sub_ /
   * #fake_shading_sub_. May clear #enabled_ or #use_fake_shading_ if a required shader is missing.
   */
  virtual void setup_passes(Resources &res, const State &state) = 0;

  /** Geometry batch for \a object (lines, falling back to points). */
  virtual gpu::Batch *geometry_batch_get(Object *object) = 0;

 public:
  void begin_sync(Resources &res, const State &state) final;
  void object_sync(Manager &manager,
                   const ObjectRef &ob_ref,
                   Resources &res,
                   const State &state) final;
  void draw(Framebuffer &fb, Manager &manager, View &view) final;
  void draw_on_render(gpu::FrameBuffer *fb, Manager &manager, View &view) final;
};

}  // namespace blender::draw::overlay
