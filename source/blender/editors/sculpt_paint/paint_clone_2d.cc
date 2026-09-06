/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \brief Clone Stamp, 2D Image Editor path: see #paint_clone_2d.hh.
 *
 * The 3D clone core (#clone_stroke_apply_dab and below) works in UV space and is reused as is:
 * with an identity #CloneDabTransform the stamp is UV-locked, which is what an Image Editor wants
 * -- its view has no rotation to carry. This file adds what the 2D path needs around that core:
 * a canvas-UV source with its own Relative anchor, the per-stroke gate that splits the Clone
 * Stamp tool from the legacy image clone, target building for a single-image canvas, and the
 * region-space source marker.
 */

#include <algorithm>
#include <cmath>

#include "paint_clone_2d.hh"

#include "DNA_brush_types.h"
#include "DNA_image_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_workspace_types.h"

#include "MEM_guardedalloc.h"

#include "BLI_math_constants.h"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_utildefines.h"

#include "BKE_brush.hh"
#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_material.hh"
#include "BKE_paint.hh"
#include "BKE_report.hh"

#include "ED_image.hh"
#include "ED_paint.hh"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "WM_api.hh"
#include "WM_toolsystem.hh"
#include "WM_types.hh"

#include "paint_clone.hh"
#include "paint_clone_source.hh"
#include "paint_clone_stroke.hh"

namespace blender::ed::sculpt_paint::clone {

/* -------------------------------------------------------------------- */
/** \name Tool / stroke gating.
 * \{ */

bool clone_2d_tool_active(const bContext *C)
{
  if (C == nullptr) {
    return false;
  }
  /* The idname is the Image Editor tool's; asking it about any other space would answer about a
   * tool that has nothing to do with the 2D stamp. */
  if (CTX_wm_space_image(const_cast<bContext *>(C)) == nullptr) {
    return false;
  }
  const bToolRef *tref = WM_toolsystem_ref_from_context(const_cast<bContext *>(C));
  return (tref != nullptr) && STREQ(tref->idname, "builtin_brush.texture_clone");
}

bool clone_2d_stroke_gate(bContext *C, wmOperator *op, Clone2DStrokeContext &r_context)
{
  r_context = Clone2DStrokeContext();

  const Scene *scene = CTX_data_scene(C);
  if (scene == nullptr) {
    return false;
  }
  if (!clone_2d_tool_active(C)) {
    return false;
  }
  const Brush *brush = BKE_paint_brush(&scene->toolsettings->imapaint.paint);
  if (brush == nullptr || brush->image_brush_type != IMAGE_PAINT_BRUSH_TYPE_CLONE) {
    return false;
  }

  SpaceImage *sima = CTX_wm_space_image(C);

  /* Mirror #paint_2d_new_stroke's canvas resolution so the dab path builds targets for the same
   * image the stroke state was created for. */
  const PaintModeSettings &paint_mode = scene->toolsettings->paint_mode;
  Image *canvas_image = (sima != nullptr) ? sima->image : nullptr;
  if (ePaintCanvasSource(paint_mode.canvas_source) == PAINT_CANVAS_SOURCE_IMAGE &&
      paint_mode.canvas_image != nullptr)
  {
    canvas_image = paint_mode.canvas_image;
  }

  if (clone_2d_source_get(scene->toolsettings->imapaint).has_value() == false) {
    BKE_report(op->reports, RPT_WARNING, "Clone Stamp: set the source first (Shift+Click)");
    return false;
  }
  if (ePaintCanvasSource(paint_mode.canvas_source) != PAINT_CANVAS_SOURCE_MATERIAL &&
      canvas_image != nullptr && canvas_image->source == IMA_SRC_TILED)
  {
    BKE_report(op->reports, RPT_WARNING, "Clone Stamp does not support UDIM tiles yet");
    return false;
  }

  r_context.bmain = CTX_data_main(C);
  r_context.object = CTX_data_active_object(C);
  r_context.image_paint = &scene->toolsettings->imapaint;
  r_context.canvas_source = ePaintCanvasSource(paint_mode.canvas_source);
  r_context.canvas_image = canvas_image;
  /* Borrowed, like every other pointer here: the space outlives the stroke. */
  r_context.canvas_iuser = (sima != nullptr) ? &sima->iuser : nullptr;
  r_context.active = true;
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Dab path.
 * \{ */

/**
 * Targets for an Image-canvas stroke: the single canvas image, read as a color channel. One
 * channel slot means the dab loop stamps exactly once; the pristine source snapshot follows the
 * same #CloneChannelTarget contract as the material builder.
 */
static CloneStrokeTargets clone_2d_targets_build_single_image(Image &image, const ImageUser &iuser)
{
  CloneStrokeTargets targets;

  ImageUser target_iuser = iuser;
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(&image, &target_iuser, &lock);
  if (ibuf == nullptr || (ibuf->byte_data() == nullptr && ibuf->float_data() == nullptr)) {
    if (ibuf != nullptr) {
      BKE_image_release_ibuf(&image, ibuf, lock);
    }
    return targets;
  }
  if (!BKE_image_buffer_format_writable(ibuf)) {
    BKE_image_release_ibuf(&image, ibuf, lock);
    return targets;
  }
  ImBuf *source_ibuf = IMB_dupImBuf(ibuf);
  if (source_ibuf == nullptr) {
    BKE_image_release_ibuf(&image, ibuf, lock);
    return targets;
  }

  CloneChannelTarget target;
  target.channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  target.image = &image;
  target.iuser = target_iuser;
  target.ibuf = ibuf;
  target.lock = lock;
  target.source_ibuf = source_ibuf;
  target.is_data = ibuf->colorspace_is_data();

  CloneLayerTarget layer;
  layer.channels[PAINT_MATERIAL_CHANNEL_BASE_COLOR] = std::move(target);
  targets.layers.append(std::move(layer));
  return targets;
}

/**
 * The active stroke's targets, built lazily on the first dab (the undo step is already open by
 * then -- opened by the outer image-paint stroke, which also closes it; never nest). A Material
 * canvas reuses the 3D ensure, keyed on the material; an Image canvas has no material to key on
 * and builds once, since the canvas cannot change mid-stroke.
 */
static CloneStrokeRuntime *clone_2d_stroke_runtime_ensure(CloneStrokeRuntime *&owner,
                                                          const Clone2DStrokeContext &context,
                                                          const Paint &paint,
                                                          const Brush &brush)
{
  if (owner != nullptr) {
    return owner;
  }
  if (context.image_paint == nullptr) {
    return nullptr;
  }
  const std::optional<Clone2DSource> source_2d = clone_2d_source_get(*context.image_paint);
  if (!source_2d.has_value()) {
    return nullptr;
  }
  /* Only the UV reaches the core: the mesh-flavored fields of #CloneSourcePoint stay default and
   * the dab path never reads them. */
  CloneSourcePoint source_3d;
  source_3d.uv = source_2d->uv;
  source_3d.is_valid = true;

  if (context.canvas_source == PAINT_CANVAS_SOURCE_MATERIAL) {
    if (context.bmain == nullptr || context.object == nullptr) {
      return nullptr;
    }
    Material *ma = BKE_object_material_get(context.object, context.object->actcol);
    if (ma == nullptr) {
      return nullptr;
    }
    return clone_stroke_runtime_ensure(
        owner, context.bmain, source_3d, *ma, &brush, paint.visible_material_channels);
  }

  if (context.canvas_image == nullptr) {
    return nullptr;
  }
  /* No space to borrow from (a paint-mode canvas picked without an Image Editor open): fall back
   * to a default user rather than whatever a previous stroke left behind. */
  const ImageUser default_iuser{};
  const ImageUser &canvas_iuser = context.canvas_iuser != nullptr ? *context.canvas_iuser :
                                                                    default_iuser;
  CloneStrokeRuntime *runtime = MEM_new<CloneStrokeRuntime>(__func__);
  runtime->source = source_3d;
  runtime->brush = &brush;
  runtime->targets = clone_2d_targets_build_single_image(*context.canvas_image, canvas_iuser);
  if (!runtime->targets.is_valid()) {
    MEM_delete(runtime);
    return nullptr;
  }
  owner = runtime;
  return runtime;
}

void clone_2d_stroke_dab(CloneStrokeRuntime *&owner,
                         const Clone2DStrokeContext &context,
                         const Paint &paint,
                         const Brush &brush,
                         const float2 &dest_uv,
                         const float strength,
                         const bool is_main_pass,
                         const float2x2 *symmetry_jacobian)
{
  if (context.image_paint == nullptr) {
    return;
  }
  const std::optional<Clone2DSource> source_2d = clone_2d_source_get(*context.image_paint);
  if (!source_2d.has_value()) {
    return;
  }
  CloneStrokeRuntime *runtime = clone_2d_stroke_runtime_ensure(owner, context, paint, brush);
  if (runtime == nullptr || !runtime->targets.is_valid()) {
    return;
  }

  /* Brush footprint in UV units: radius in canvas pixels over the canvas width. The first target
   * image defines the canvas -- every target shares its UV space. Non-const iteration: the
   * targets' ImageUser is handed to BKE_image_get_size, which takes it mutable. */
  int canvas_w = 0;
  for (CloneLayerTarget &layer : runtime->targets.layers) {
    for (std::optional<CloneChannelTarget> &opt : layer.channels) {
      if (opt.has_value() && opt->image != nullptr) {
        int canvas_h = 0;
        BKE_image_get_size(opt->image, &opt->iuser, &canvas_w, &canvas_h);
        break;
      }
    }
    if (canvas_w > 0) {
      break;
    }
  }
  if (canvas_w <= 0) {
    canvas_w = 1024;
  }
  const float uv_radius = BKE_brush_radius_get(&paint, &brush) / float(canvas_w);
  if (uv_radius <= 0.0f) {
    return;
  }

  /* Symmetry bookkeeping, sculpt parity: the main pass records its center for the duplicate test
   * of the mirrored passes that follow it in the same stroke event, and it is the only pass that
   * may fix the Relative anchor -- a mirrored location must not define the offset. */
  if (is_main_pass) {
    runtime->main_pass_dest_uv = dest_uv;
    runtime->main_pass_dest_uv_valid = true;
  }
  else if (clone_dab_is_symmetry_duplicate(*runtime, dest_uv, uv_radius)) {
    /* A mirrored dab landing on the texels the main pass just painted would blend the source in
     * a second time: a UV layout that mirrors its islands reuses the same patch of the map. */
    return;
  }
  if (paint.clone_mode == CLONE_MODE_RELATIVE && !source_2d->anchor_valid && !is_main_pass) {
    /* A mirrored location must not define the Relative offset the whole stroke then works from --
     * the same rule the sculpt clone follows. */
    return;
  }

  /* Identity transform: an Image Editor view has no rotation, so the stamp stays UV-locked and
   * the core takes its degenerate fallback for the Normal channel (raw copy). A mirrored pass
   * composes the mirror Jacobian into it, which is what turns the stamp into the mirror image of
   * the main pass's -- a mirrored unwrap maps both halves of the stroke onto differently-oriented
   * patches, and stamping the mirrored location with an identity map would copy the source's
   * direction into both unchanged. */
  CloneDabTransform transform;
  if (symmetry_jacobian != nullptr) {
    clone_symmetry_transform_apply(transform, *symmetry_jacobian);
  }

  const float2 dab_center_uv = clone_2d_dab_center_uv_get(
      *context.image_paint, dest_uv, paint.clone_mode);

  const float clamped_strength = std::clamp(strength, 0.0f, 1.0f);

  /* An Image canvas has exactly one target, filed under Base Color by the single-image builder. */
  const int channel_mask = (context.canvas_source == PAINT_CANVAS_SOURCE_MATERIAL) ?
                               paint.visible_material_channels :
                               (1 << PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  clone_stroke_apply_dab(
      runtime, dest_uv, dab_center_uv, transform, uv_radius, channel_mask, clamped_strength);

  /* Publish every target's written rectangle to the 2D redraw pipeline. The undo tiles were
   * captured inside #clone_stroke_apply_dab; this merge is what feeds the Image Editor's partial
   * updates through #imapaint_image_update. The footprint is a UV disk (or square) of
   * #uv_radius at the dab center -- the falloff never writes outside it, so the rectangle is
   * computed from the footprint bounds rather than tracked pixel-precisely. The mirror Jacobian
   * shears that footprint into an ellipse of `uv_radius * ||J||` extent, bounded here by the
   * Frobenius norm (a mirrored unwrap keeps it near 1; this only guards a sheared one). */
  float dirty_radius = uv_radius;
  if (symmetry_jacobian != nullptr) {
    dirty_radius *= math::sqrt(math::length_squared((*symmetry_jacobian)[0]) +
                               math::length_squared((*symmetry_jacobian)[1]));
  }
  for (CloneLayerTarget &layer : runtime->targets.layers) {
    for (std::optional<CloneChannelTarget> &opt : layer.channels) {
      if (!opt.has_value() || opt->ibuf == nullptr || opt->image == nullptr) {
        continue;
      }
      ImBuf &ibuf = *opt->ibuf;
      const int x0 = int(std::floor((dest_uv[0] - dirty_radius) * float(ibuf.x))) - 1;
      const int x1 = int(std::ceil((dest_uv[0] + dirty_radius) * float(ibuf.x))) + 1;
      const int y0 = int(std::floor((dest_uv[1] - dirty_radius) * float(ibuf.y))) - 1;
      const int y1 = int(std::ceil((dest_uv[1] + dirty_radius) * float(ibuf.y))) + 1;
      ED_imapaint_dirty_region(
          opt->image, &ibuf, &opt->iuser, x0, y0, x1 - x0 + 1, y1 - y0 + 1, true);
    }
  }
}

bool clone_2d_targets_contain_image(const CloneStrokeRuntime *runtime, const Image *image)
{
  if (runtime == nullptr || image == nullptr) {
    return false;
  }
  for (const CloneLayerTarget &layer : runtime->targets.layers) {
    for (const std::optional<CloneChannelTarget> &opt : layer.channels) {
      if (opt.has_value() && opt->image == image) {
        return true;
      }
    }
  }
  return false;
}

/** \} */

}  // namespace blender::ed::sculpt_paint::clone
