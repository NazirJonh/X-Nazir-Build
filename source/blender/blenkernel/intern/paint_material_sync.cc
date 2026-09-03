/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

/* Mirroring #UnifiedPaintSettings reaches the deprecated sRGB color fields through
 * #BKE_brush_color_sync_legacy. */
#define DNA_DEPRECATED_ALLOW

#include "DNA_brush_types.h"
#include "DNA_scene_types.h"

#include "BLI_listbase_iterator.hh"
#include "BLI_math_vector.h"
#include "BLI_utildefines.h"

#include "BKE_brush.hh"
#include "BKE_colortools.hh"
#include "BKE_idtype.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_paint.hh"
#include "BKE_paint_material_sync.hh"
#include "BKE_paint_types.hh"

#include "WM_api.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Cross-mode Brush Sync
 * \{ */

Paint *BKE_paint_material_sync_target_get(Scene *scene, Paint *source)
{
  if (scene == nullptr || scene->toolsettings == nullptr || source == nullptr) {
    return nullptr;
  }

  ToolSettings *ts = scene->toolsettings;
  const PaintModeSettings &mode_settings = ts->paint_mode;
  if ((mode_settings.material_paint_flag & PAINT_MATERIAL_BRUSH_SYNC) == 0) {
    return nullptr;
  }
  if (mode_settings.canvas_source != PAINT_CANVAS_SOURCE_MATERIAL) {
    return nullptr;
  }

  Paint *sculpt_paint = ts->sculpt != nullptr ? &ts->sculpt->paint : nullptr;
  Paint *image_paint = &ts->imapaint.paint;
  if (source == sculpt_paint) {
    return image_paint;
  }
  if (source == image_paint) {
    return sculpt_paint;
  }
  return nullptr;
}

bool BKE_paint_material_sync_texture3d_conflicts(const Scene *scene, const PaintMode paint_mode)
{
  /* 3D Texture Paint shares #ImagePaintSettings with Image Editor: Paint. Restoring a Texture
   * Paint brush or syncing from that mode overwrites the Image Editor / Sculpt PBR pair. */
  if (paint_mode != PaintMode::Texture3D || scene == nullptr || scene->toolsettings == nullptr) {
    return false;
  }
  return scene->toolsettings->paint_mode.canvas_source == PAINT_CANVAS_SOURCE_MATERIAL;
}

static void paint_material_curve_replace(CurveMapping **dst, const CurveMapping *src)
{
  /* Both #Paint free their own curves, so the pointers must never be shared. */
  BKE_curvemapping_free(*dst);
  *dst = BKE_curvemapping_copy(src);
}

static void paint_material_unified_settings_sync_to(Paint *source, Paint *dst_paint)
{
  const UnifiedPaintSettings &src_ups = source->unified_paint_settings;
  UnifiedPaintSettings &dst_ups = dst_paint->unified_paint_settings;

  /* Assigning the struct would alias the three #CurveMapping pointers, so scalar fields are copied
   * explicitly and the curves deep-copied below. */
  dst_ups.size = src_ups.size;
  dst_ups.unprojected_size = src_ups.unprojected_size;
  dst_ups.alpha = src_ups.alpha;
  dst_ups.weight = src_ups.weight;
  copy_v3_v3(dst_ups.color, src_ups.color);
  copy_v3_v3(dst_ups.secondary_color, src_ups.secondary_color);
  dst_ups.color_jitter_flag = src_ups.color_jitter_flag;
  copy_v3_v3(dst_ups.hsv_jitter, src_ups.hsv_jitter);
  dst_ups.input_samples = src_ups.input_samples;
  dst_ups.flag = src_ups.flag;

  paint_material_curve_replace(&dst_ups.curve_rand_hue, src_ups.curve_rand_hue);
  paint_material_curve_replace(&dst_ups.curve_rand_saturation, src_ups.curve_rand_saturation);
  paint_material_curve_replace(&dst_ups.curve_rand_value, src_ups.curve_rand_value);

  /* The deprecated sRGB fields are derived rather than copied: the color update callback refreshes
   * them on the source only after this runs, so copying would leave the destination one edit
   * behind in saved files. */
  BKE_brush_color_sync_legacy(&dst_ups);
}

void BKE_paint_material_unified_settings_sync(Scene *scene, Paint *source)
{
  Paint *dst_paint = BKE_paint_material_sync_target_get(scene, source);
  if (dst_paint != nullptr) {
    paint_material_unified_settings_sync_to(source, dst_paint);
  }
}

static void paint_material_brush_sync_apply(Scene *scene, Paint *source, Paint *dst_paint)
{
  Brush *src_brush = BKE_paint_brush(source);
  if (src_brush == nullptr || src_brush->material_paint == nullptr) {
    return;
  }

  /* Most brushes are restricted (#Brush.ob_mode) to the paint mode they were authored for, so
   * #BKE_paint_brush_set would refuse a Sculpt brush in Image Paint. Sync is exactly a request to
   * use this brush in both modes, so opt it in rather than making the user find the matching
   * checkbox in Brush Settings first. */
  BLI_assert(dst_paint->runtime != nullptr);
  if (!BKE_paint_can_use_brush(dst_paint, src_brush)) {
    src_brush->ob_mode |= dst_paint->runtime->ob_mode;
    BKE_brush_tag_unsaved_changes(src_brush);
  }

  /* #BKE_paint_brush_set_synced applies the incoming brush's stored preset over its live state.
   * That is right when the user switches brushes inside one mode, but here the incoming brush is
   * the source editor's own brush: its live channels are what the user just set up and are the
   * truth. Refresh its preset from that live state first, so the apply below is an identity copy
   * instead of resurrecting the channel sources as of the last brush switch. */
  BKE_paint_material_brush_preset_snapshot(*scene, *src_brush);

  if (!BKE_paint_brush_set_synced(*scene, *dst_paint, src_brush)) {
    /* The brush stays unusable in the receiving mode, so the two editors would keep different
     * brushes. Mirroring the remaining settings on top of that would only obscure the mismatch. */
    return;
  }

  if (dst_paint->palette != source->palette) {
    id_us_min(id_cast<ID *>(dst_paint->palette));
    dst_paint->palette = source->palette;
    id_us_plus(id_cast<ID *>(dst_paint->palette));
  }

  paint_material_curve_replace(&dst_paint->cavity_curve, source->cavity_curve);
  dst_paint->visible_material_channels = source->visible_material_channels;

  paint_material_unified_settings_sync_to(source, dst_paint);

  WM_main_add_notifier(NC_BRUSH | NA_SELECTED, src_brush);
  WM_main_add_notifier(NC_SCENE | ND_TOOLSETTINGS, nullptr);
}

void BKE_paint_material_brush_sync(Scene *scene, Paint *source)
{
  Paint *dst_paint = BKE_paint_material_sync_target_get(scene, source);
  if (dst_paint == nullptr) {
    return;
  }
  paint_material_brush_sync_apply(scene, source, dst_paint);
}

bool BKE_paint_material_brush_sync_directional(Scene *scene, Paint *source, Paint *destination)
{
  if (scene == nullptr || scene->toolsettings == nullptr || source == nullptr ||
      destination == nullptr ||
      scene->toolsettings->paint_mode.canvas_source != PAINT_CANVAS_SOURCE_MATERIAL)
  {
    return false;
  }

  ToolSettings *ts = scene->toolsettings;
  Paint *sculpt_paint = ts->sculpt != nullptr ? &ts->sculpt->paint : nullptr;
  Paint *image_paint = &ts->imapaint.paint;
  const bool is_pair = (source == sculpt_paint && destination == image_paint) ||
                       (source == image_paint && destination == sculpt_paint);
  if (!is_pair) {
    return false;
  }

  paint_material_brush_sync_apply(scene, source, destination);
  return true;
}

bool BKE_paint_material_brush_sync_disable(Main *bmain, Scene *scene)
{
  if (bmain == nullptr || scene == nullptr || scene->toolsettings == nullptr ||
      scene->toolsettings->sculpt == nullptr)
  {
    return false;
  }

  ToolSettings *ts = scene->toolsettings;
  Paint *sculpt_paint = &ts->sculpt->paint;
  Paint *image_paint = &ts->imapaint.paint;
  Brush *sculpt_brush = BKE_paint_brush(sculpt_paint);
  Brush *image_brush = BKE_paint_brush(image_paint);
  if (sculpt_brush == nullptr || sculpt_brush != image_brush) {
    /* Already independent, so there is nothing to split apart. This also makes repeated disables
     * idempotent: only the transition out of a shared brush ever creates a copy. */
    return false;
  }

  Brush *image_brush_copy = id_cast<Brush *>(BKE_id_copy(bmain, &image_brush->id));
  if (image_brush_copy == nullptr) {
    return false;
  }
  /* #Paint.brush is not user-counted, so the copy keeps the user #BKE_id_copy gave it; dropping it
   * here would make the new brush a zero-user ID that is purged on the next save. */
  if (!BKE_paint_brush_set_synced(*scene, *image_paint, image_brush_copy)) {
    return false;
  }

  WM_main_add_notifier(NC_BRUSH | NA_SELECTED, image_brush_copy);
  return true;
}

void BKE_paint_material_brush_sync_after_load(Main *bmain)
{
  if (bmain == nullptr) {
    return;
  }

  for (Scene &scene : bmain->scenes) {
    if (scene.toolsettings == nullptr) {
      continue;
    }
    /* Runtime is not stored in the file; without it #BKE_paint_material_brush_sync cannot opt the
     * brush into Image Paint's object mode. */
    BKE_paint_ensure_from_paintmode(&scene, PaintMode::Sculpt);
    BKE_paint_ensure_from_paintmode(&scene, PaintMode::Texture2D);
    if (scene.toolsettings->sculpt == nullptr) {
      continue;
    }
    BKE_paint_material_brush_sync(&scene, &scene.toolsettings->sculpt->paint);
  }
}

Paint *BKE_paint_material_sync_on_brush_activated(Scene *scene,
                                                  Paint *source,
                                                  const PaintMode paint_mode)
{
  if (scene == nullptr || BKE_paint_material_sync_texture3d_conflicts(scene, paint_mode)) {
    return nullptr;
  }
  Paint *synced_paint = BKE_paint_material_sync_target_get(scene, source);
  if (synced_paint == nullptr) {
    return nullptr;
  }
  BKE_paint_material_brush_sync(scene, source);
  return synced_paint;
}

Paint *BKE_paint_material_sync_on_tool_restore(Scene *scene, const PaintMode paint_mode)
{
  if (scene == nullptr || scene->toolsettings == nullptr ||
      scene->toolsettings->sculpt == nullptr ||
      BKE_paint_material_sync_texture3d_conflicts(scene, paint_mode))
  {
    return nullptr;
  }

  Paint *sculpt_paint = &scene->toolsettings->sculpt->paint;
  if (BKE_paint_material_sync_target_get(scene, sculpt_paint) == nullptr) {
    return nullptr;
  }

  BKE_paint_material_brush_sync(scene, sculpt_paint);
  return &scene->toolsettings->imapaint.paint;
}

bool BKE_paint_material_sync_suppresses_brush_restore(Scene *scene, const Paint *paint)
{
  if (scene == nullptr || scene->toolsettings == nullptr) {
    return false;
  }
  if (paint != &scene->toolsettings->imapaint.paint) {
    return false;
  }
  /* #BKE_paint_material_sync_target_get takes a mutable #Paint only because it returns one; the
   * lookup itself does not modify the argument. */
  return BKE_paint_material_sync_target_get(scene, const_cast<Paint *>(paint)) != nullptr;
}

/** \} */

}  // namespace blender
