/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Session-only "record" mechanism that forces a group of brush settings (Stroke, Falloff or
 * Face Sets) from one brush onto the next one the user activates, so a configured group does
 * not have to be recreated on every brush.
 */

#include "BLI_math_vector.h"
#include "BLI_utildefines.h"

#include "BKE_asset_edit.hh"
#include "BKE_brush.hh"
#include "BKE_colortools.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_paint.hh"
#include "BKE_paint_types.hh"
#include "BKE_report.hh"

#include "DNA_brush_enums.h"
#include "DNA_brush_types.h"
#include "DNA_scene_types.h"

namespace blender::bke {

/**
 * Copies a bit-field flag from \a src to \a dst without touching the other bits.
 *
 * \return True when the flag's state on \a dst actually changed.
 */
static bool brush_flag_copy(const Brush &src, Brush &dst, const eBrushFlags flag)
{
  const bool src_set = (src.flag & flag) != 0;
  const bool dst_set = (dst.flag & flag) != 0;
  if (src_set == dst_set) {
    return false;
  }
  if (src_set) {
    dst.flag |= flag;
  }
  else {
    dst.flag &= ~flag;
  }
  return true;
}

static bool brush_flag2_copy(const Brush &src, Brush &dst, const eBrushFlags2 flag)
{
  const bool src_set = (src.flag2 & flag) != 0;
  const bool dst_set = (dst.flag2 & flag) != 0;
  if (src_set == dst_set) {
    return false;
  }
  if (src_set) {
    dst.flag2 |= flag;
  }
  else {
    dst.flag2 &= ~flag;
  }
  return true;
}

/**
 * Assigns \a value to \a dst if it differs, reporting whether it changed anything.
 */
template<typename T> static bool assign_changed(T &dst, const T &value)
{
  if (dst == value) {
    return false;
  }
  dst = value;
  return true;
}

/**
 * Copies the contents of the Stroke popover. Fields the target brush cannot use are skipped so
 * that recording never writes values the brush has no UI for.
 *
 * \return True when any value changed.
 */
static bool brush_group_copy_stroke(const Brush &src, Brush &dst, const PaintMode mode)
{
  if (&src == &dst) {
    return false;
  }

  bool changed = false;

  /* Copied first: the conditions below depend on the resulting stroke method. */
  changed |= assign_changed(dst.stroke_method, src.stroke_method);
  changed |= assign_changed(dst.input_samples, src.input_samples);
  changed |= assign_changed(dst.dash_ratio, src.dash_ratio);
  changed |= assign_changed(dst.dash_samples, src.dash_samples);
  changed |= assign_changed(dst.spacing, src.spacing);
  changed |= brush_flag_copy(src, dst, BRUSH_SPACING_PRESSURE);

  if (mode == PaintMode::Sculpt) {
    changed |= brush_flag_copy(src, dst, BRUSH_SCENE_SPACING);
  }

  if (brush::supports_space_attenuation(dst)) {
    changed |= brush_flag_copy(src, dst, BRUSH_SPACE_ATTEN);
  }

  if (dst.stroke_method == BRUSH_STROKE_AIRBRUSH) {
    changed |= assign_changed(dst.rate, src.rate);
  }
  if (dst.stroke_method == BRUSH_STROKE_ANCHORED) {
    changed |= brush_flag_copy(src, dst, BRUSH_EDGE_TO_EDGE);
  }

  /* Outside sculpt mode the Stroke popover always shows jitter, see #StrokePanel.draw. */
  if (mode != PaintMode::Sculpt || brush::supports_jitter(dst)) {
    changed |= assign_changed(dst.jitter, src.jitter);
    changed |= assign_changed(dst.jitter_absolute, src.jitter_absolute);
    changed |= brush_flag_copy(src, dst, BRUSH_ABSOLUTE_JITTER);
    changed |= brush_flag_copy(src, dst, BRUSH_JITTER_PRESSURE);
  }

  return changed;
}

/**
 * Copies the contents of the Falloff popover. The custom curve is only copied when the preset
 * actually uses it, since the preset path never evaluates the #CurveMapping.
 *
 * \return True when any value changed.
 */
static bool brush_group_copy_falloff(const Brush &src, Brush &dst, const PaintMode mode)
{
  if (&src == &dst) {
    return false;
  }
  /* The popover is not shown at all without a distance falloff curve, see #FalloffPanel.poll. */
  if (dst.curve_distance_falloff == nullptr) {
    return false;
  }

  bool changed = assign_changed(dst.curve_distance_falloff_preset,
                                src.curve_distance_falloff_preset);

  if (src.curve_distance_falloff_preset == BRUSH_CURVE_CUSTOM &&
      src.curve_distance_falloff != nullptr)
  {
    /* The curve's sample tables are non-trivial to diff cheaply; a custom-preset copy is always
     * treated as a change, matching how other brush-copy code in Blender handles #CurveMapping. */
    BKE_curvemapping_copy_data(dst.curve_distance_falloff, src.curve_distance_falloff);
    BKE_curvemapping_changed_all(dst.curve_distance_falloff);
    changed = true;
  }

  /* The falloff and texture clip shapes are only exposed for these modes, and never for the
   * Pose brush, see #FalloffPanel.draw. */
  const bool show_shape = ELEM(mode, PaintMode::Sculpt, PaintMode::Vertex, PaintMode::Weight) &&
                          dst.sculpt_brush_type != SCULPT_BRUSH_TYPE_POSE;
  if (show_shape) {
    changed |= assign_changed(dst.falloff_shape, src.falloff_shape);
    if (mode == PaintMode::Sculpt) {
      changed |= assign_changed(dst.texture_clip_shape, src.texture_clip_shape);
    }
  }

  return changed;
}

/**
 * Whether the given group can be copied onto \a brush. Matches the early-exit conditions of the
 * corresponding #brush_group_copy_* helpers.
 */
static bool brush_group_applicable_to_brush(const Brush &brush, const BrushOverrideGroup group)
{
  switch (group) {
    case BrushOverrideGroup::FaceSets:
      return brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_DRAW_FACE_SETS ||
             brush::supports_face_set_texture(brush);
    case BrushOverrideGroup::Falloff:
      return brush.curve_distance_falloff != nullptr;
    case BrushOverrideGroup::Stroke:
      return true;
  }
  BLI_assert_unreachable();
  return false;
}

/**
 * Copies the contents of the Face Sets popovers. The texture data-block pointer is deliberately
 * left alone: recording carries settings, not asset references.
 *
 * \return True when any value changed.
 */
static bool brush_group_copy_face_sets(const Brush &src, Brush &dst)
{
  if (&src == &dst) {
    return false;
  }

  const bool draw_face_sets = dst.sculpt_brush_type == SCULPT_BRUSH_TYPE_DRAW_FACE_SETS;
  const bool from_texture = brush::supports_face_set_texture(dst);
  if (!draw_face_sets && !from_texture) {
    return false;
  }

  bool changed = assign_changed(dst.face_set_draw_mode, src.face_set_draw_mode);
  if (src.face_set_draw_mode == SCULPT_FACE_SET_DRAW_MODE_COLOR) {
    if (!equals_v3v3(dst.face_set_color, src.face_set_color)) {
      copy_v3_v3(dst.face_set_color, src.face_set_color);
      changed = true;
    }
    if (!equals_v3v3(dst.face_set_secondary_color, src.face_set_secondary_color)) {
      copy_v3_v3(dst.face_set_secondary_color, src.face_set_secondary_color);
      changed = true;
    }
  }

  if (from_texture) {
    changed |= assign_changed(dst.texture_data_mode, src.texture_data_mode);
    changed |= assign_changed(dst.texture_threshold, src.texture_threshold);
    changed |= brush_flag2_copy(src, dst, BRUSH_TEXTURE_INVERT_ALPHA);
    changed |= brush_flag2_copy(src, dst, BRUSH_DISABLE_FACE_SET_WRITE);
    changed |= assign_changed(dst.write_vcol, src.write_vcol);
    changed |= assign_changed(dst.vcol_channel, src.vcol_channel);
    changed |= assign_changed(dst.vcol_mode, src.vcol_mode);

    /* Face Set color texture uses the mask texture mapping. Keep the derived mapping in sync
     * when recording changes the mode, just as the interactive Face Sets operator does. */
    if (dst.texture_data_mode == BRUSH_TEXTURE_DATA_MODE_FACE_SETS_COLOR_FROM_TEXTURE) {
      const MTex *mask_mtex = BKE_brush_mask_texture_get(&dst, OB_MODE_SCULPT);
      if (mask_mtex != nullptr) {
        changed |= assign_changed(dst.face_set_color_mtex.brush_map_mode,
                                  mask_mtex->brush_map_mode);
        if (!equals_v3v3(dst.face_set_color_mtex.ofs, mask_mtex->ofs)) {
          copy_v3_v3(dst.face_set_color_mtex.ofs, mask_mtex->ofs);
          changed = true;
        }
        if (!equals_v3v3(dst.face_set_color_mtex.size, mask_mtex->size)) {
          copy_v3_v3(dst.face_set_color_mtex.size, mask_mtex->size);
          changed = true;
        }
        changed |= assign_changed(dst.face_set_color_mtex.rot, mask_mtex->rot);
        changed |= assign_changed(dst.face_set_color_mtex.brush_angle_mode,
                                  mask_mtex->brush_angle_mode);
        changed |= assign_changed(dst.face_set_color_mtex.random_angle,
                                  mask_mtex->random_angle);
      }
    }
  }

  return changed;
}

}  // namespace blender::bke

namespace blender {

void BKE_paint_brush_group_overrides_apply(Paint *paint, const Brush *src, Brush *dst)
{
  using namespace blender::bke;

  if (paint == nullptr || paint->runtime == nullptr || src == nullptr || dst == nullptr ||
      src == dst)
  {
    return;
  }

  const PaintMode mode = paint->runtime->paint_mode;
  bool changed = false;

  if (paint->runtime->override_stroke) {
    changed |= brush_group_copy_stroke(*src, *dst, mode);
  }
  if (paint->runtime->override_falloff) {
    changed |= brush_group_copy_falloff(*src, *dst, mode);
  }
  if (paint->runtime->override_face_sets) {
    changed |= brush_group_copy_face_sets(*src, *dst);
  }

  if (changed) {
    BKE_brush_tag_unsaved_changes(dst);
  }
}

bool BKE_paint_brush_group_reset_from_asset(Main *bmain,
                                            Paint *paint,
                                            const BrushOverrideGroup group,
                                            ReportList *reports)
{
  using namespace blender::bke;

  if (bmain == nullptr || paint == nullptr || paint->runtime == nullptr) {
    return false;
  }

  Brush *brush = BKE_paint_brush(paint);
  if (brush == nullptr || !asset_edit_id_is_editable(brush->id)) {
    BKE_report(reports, RPT_WARNING, "Active brush is not an editable asset");
    return false;
  }

  if (!brush_group_applicable_to_brush(*brush, group)) {
    BKE_report(reports, RPT_WARNING, "Setting group is not applicable to the active brush type");
    return false;
  }

  const PaintMode mode = paint->runtime->paint_mode;

  /* Snapshot every setting the user has changed. #asset_edit_id_revert reverts the whole brush,
   * so the snapshot is what makes a per-group revert possible. */
  Brush *backup = id_cast<Brush *>(
      BKE_id_copy_ex(nullptr, &brush->id, nullptr, LIB_ID_COPY_LOCALIZE));
  if (backup == nullptr) {
    return false;
  }

  ID *reverted_id = asset_edit_id_revert(*bmain, brush->id, *reports);
  if (reverted_id == nullptr) {
    /* #asset_edit_id_revert deletes the brush even when it fails, mirroring the fallback of
     * #BRUSH_OT_asset_revert. */
    BKE_id_free(nullptr, backup);
    BKE_paint_brush_set_default(bmain, paint);
    return false;
  }

  Brush *reverted = id_cast<Brush *>(reverted_id);

  /* Take just this group from the pristine asset, keep everything else from the snapshot. */
  switch (group) {
    case BrushOverrideGroup::FaceSets:
      brush_group_copy_face_sets(*reverted, *backup);
      break;
    case BrushOverrideGroup::Stroke:
      brush_group_copy_stroke(*reverted, *backup, mode);
      break;
    case BrushOverrideGroup::Falloff:
      brush_group_copy_falloff(*reverted, *backup, mode);
      break;
  }

  /* Swap only the brush payload: each #ID keeps its own name and library, so the re-linked
   * data-block stays a valid asset while carrying the merged settings. */
  BKE_lib_id_swap(bmain, &reverted->id, &backup->id, false, 0);
  BKE_id_free(nullptr, backup);

  /* Recording must be off before re-activating: the source brush of the override mechanism has
   * just been deleted by the revert. */
  switch (group) {
    case BrushOverrideGroup::FaceSets:
      paint->runtime->override_face_sets = false;
      break;
    case BrushOverrideGroup::Stroke:
      paint->runtime->override_stroke = false;
      break;
    case BrushOverrideGroup::Falloff:
      paint->runtime->override_falloff = false;
      break;
  }

  BKE_paint_brush_set(paint, reverted);
  BKE_brush_tag_unsaved_changes(reverted);

  return true;
}

}  // namespace blender
