/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup overlay
 */

#include "DNA_curves_types.h"
#include "DNA_object_types.h"
#include "DNA_view3d_types.h"

#include "DRW_render.hh"

#include "overlay_curves_paint_base.hh"
#include "overlay_private.hh"

namespace blender::draw::overlay {

void CurvesPaintOverlayBase::begin_sync(Resources &res, const State &state)
{
  enabled_ = false;
  use_fake_shading_ = false;
  main_sub_ = nullptr;
  fake_shading_sub_ = nullptr;

  if (!state.is_space_v3d()) {
    return;
  }

  if (state.object_mode != this->paint_object_mode()) {
    return;
  }

  const Object *active_object = state.object_active;
  if (!active_object || active_object->type != OB_CURVES) {
    return;
  }

  enabled_ = this->is_paint_mode(active_object, state);
  if (!enabled_) {
    return;
  }

  use_fake_shading_ = state.v3d && (state.v3d->shading.flag & V3D_SHADING_OBJECT_OUTLINE) != 0;

  main_ps_.init();
  main_ps_.bind_ubo(OVERLAY_GLOBALS_SLOT, &res.globals_buf);
  main_ps_.bind_ubo(DRW_CLIPPING_UBO_SLOT, &res.clip_planes_buf);

  this->setup_passes(res, state);
}

void CurvesPaintOverlayBase::object_sync(Manager &manager,
                                         const ObjectRef &ob_ref,
                                         Resources & /*res*/,
                                         const State &state)
{
  if (!enabled_) {
    return;
  }

  if (ob_ref.object->type != OB_CURVES) {
    return;
  }

  if (!this->is_paint_mode(ob_ref.object, state)) {
    return;
  }

  gpu::Batch *geometry = this->geometry_batch_get(ob_ref.object);
  if (geometry == nullptr) {
    return;
  }

  const ResourceHandleRange handle = manager.unique_handle(ob_ref);
  main_sub_->draw(geometry, handle);

  if (use_fake_shading_ && fake_shading_sub_) {
    fake_shading_sub_->draw(geometry, handle);
  }
}

void CurvesPaintOverlayBase::draw(Framebuffer &fb, Manager &manager, View &view)
{
  if (!enabled_) {
    return;
  }

  GPU_framebuffer_bind(fb);
  manager.submit(main_ps_, view);
}

void CurvesPaintOverlayBase::draw_on_render(gpu::FrameBuffer *fb, Manager &manager, View &view)
{
  /* Guard against nullptr framebuffer (e.g. during selection operations). */
  if (!enabled_ || fb == nullptr) {
    return;
  }

  GPU_framebuffer_bind(fb);
  manager.submit(main_ps_, view);
}

}  // namespace blender::draw::overlay
