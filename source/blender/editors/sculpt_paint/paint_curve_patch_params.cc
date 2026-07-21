/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Conversion from the brush's DNA into the core's own Curve Patch inputs.
 *
 * This lives in the editor layer, not in blenkernel, for two reasons. It has to include
 * `DNA_brush_types.h` and `DNA_texture_types.h`, which would hand the core back the DNA dependency
 * the Stage 1/2 refactor removed; and the brush is not the only producer -- the modal editor's live
 * poll feeds the same conversion from a partly session-owned state, which is editor business by
 * definition.
 */

#include <algorithm>

#include "DNA_brush_types.h"
#include "DNA_texture_types.h"

#include "BLI_array.hh"
#include "BLI_assert.h"
#include "BLI_index_range.hh"
#include "BLI_listbase_iterator.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"

#include "paint_curve_patch_session.hh"

namespace blender::ed::sculpt_paint {

/* Explicit switch rather than a numeric cast: the core enums are deliberately independent of the
 * DNA values, and a cast would silently bind them together again. Deliberately no `default:` label
 * either, so that adding a DNA enumerator without a core counterpart is a compiler warning here
 * rather than a silent fall-back at runtime. */

static bke::CurvePatchLengthMode length_mode_from_dna(const int dna_value)
{
  switch (eBrushCurvePatchLengthMode(dna_value)) {
    case BRUSH_CURVE_PATCH_LENGTH_DEFAULT:
      return bke::CurvePatchLengthMode::Default;
    case BRUSH_CURVE_PATCH_LENGTH_REPEAT:
      return bke::CurvePatchLengthMode::Repeat;
    case BRUSH_CURVE_PATCH_LENGTH_STRETCH:
      return bke::CurvePatchLengthMode::Stretch;
  }
  BLI_assert_unreachable();
  return bke::CurvePatchLengthMode::Default;
}

static bke::CurvePatchEndFalloff end_falloff_from_dna(const int dna_value)
{
  switch (eBrushCurvePatchEndFalloff(dna_value)) {
    case BRUSH_CURVE_PATCH_END_HARD:
      return bke::CurvePatchEndFalloff::None;
    case BRUSH_CURVE_PATCH_END_SMOOTH:
      return bke::CurvePatchEndFalloff::Smooth;
  }
  BLI_assert_unreachable();
  return bke::CurvePatchEndFalloff::None;
}

static bke::CurvePatchStampMode stamp_mode_from_dna(const int dna_value)
{
  switch (eBrushCurvePatchStampMode(dna_value)) {
    case BRUSH_CURVE_PATCH_STAMP_RIBBON:
      return bke::CurvePatchStampMode::Ribbon;
    case BRUSH_CURVE_PATCH_STAMP_STAMPS:
      return bke::CurvePatchStampMode::Stamps;
  }
  BLI_assert_unreachable();
  return bke::CurvePatchStampMode::Ribbon;
}

static bke::CurvePatchStampProjection stamp_projection_from_dna(const int dna_value)
{
  switch (eBrushCurvePatchStampProjection(dna_value)) {
    case BRUSH_CURVE_PATCH_STAMP_PROJ_CURVE:
      return bke::CurvePatchStampProjection::Curve;
    case BRUSH_CURVE_PATCH_STAMP_PROJ_PLANAR:
      return bke::CurvePatchStampProjection::Planar;
  }
  BLI_assert_unreachable();
  return bke::CurvePatchStampProjection::Curve;
}

float3 curve_patch_plane_normal_from_curve(const bke::CurvesGeometry &curve)
{
  /* Newell's method over the control points, treated as a closed loop: exact for a planar curve,
   * degrades gracefully for a nearly planar one, and view-independent. A straight curve spans no
   * plane at all and falls back to the object's own +Z. */
  const Span<float3> positions = curve.positions();
  float3 normal(0.0f);
  for (const int64_t i : positions.index_range()) {
    const float3 &a = positions[i];
    const float3 &b = positions[(i + 1) % positions.size()];
    normal.x += (a.y - b.y) * (a.z + b.z);
    normal.y += (a.z - b.z) * (a.x + b.x);
    normal.z += (a.x - b.x) * (a.y + b.y);
  }
  if (math::length_squared(normal) < 1e-12f) {
    return float3(0.0f, 0.0f, 1.0f);
  }
  return math::normalize(normal);
}

bke::CurvePatchParams curve_patch_params_from_brush(const Brush &brush,
                                                    const float radius,
                                                    const float radius_per_size,
                                                    const float3 &plane_normal,
                                                    const uint32_t stamp_seed,
                                                    const bool swap_axis)
{
  const BrushCurvePatchSettings &settings = brush.curve_patch;

  bke::CurvePatchParams params;

  params.radius = radius;
  params.radius_per_size = radius_per_size;
  params.plane_normal = plane_normal;
  params.stamp_seed = stamp_seed;
  /* Not read from `settings`: the modal editor's S hotkey and its undo stack write the session's
   * value too, so the caller arbitrates between the two. */
  params.swap_axis = swap_axis;

  params.length_mode = length_mode_from_dna(settings.length_mode);
  params.length_repeat = settings.length_repeat;
  params.end_falloff_mode = end_falloff_from_dna(settings.end_falloff);
  params.end_falloff_percent = settings.end_falloff_percent;

  params.stamp_mode = stamp_mode_from_dna(settings.stamp_mode);
  params.stamp_projection = stamp_projection_from_dna(settings.stamp_projection);
  /* The DNA fields are percentages and the core takes fractions in `[0, 1]`. The division belongs
   * here and only here, so that no consumer has to know which of the two conventions it is holding. */
  params.stamp_size_random = float(settings.stamp_size_random) / 100.0f;
  params.stamp_strength_random = float(settings.stamp_strength_random) / 100.0f;

  /* Blender stores Spacing as a percentage of the brush DIAMETER, the same convention a normal dab
   * stroke uses, so the stamps land at the same density a freehand stroke of this brush would
   * produce. */
  params.spacing_frac = float(brush.spacing) / 100.0f;
  /* Relative jitter is already a fraction of the radius, so it converts to world units for free.
   * Absolute jitter is in screen pixels and needs the ratio captured at patch start --
   * `paint_stroke_jitter_pos()` cannot be reused here because it works in screen space while the
   * ribbon's UV is world-space. `brush.curve_jitter`, which modulates jitter over stroke time, has
   * no meaning for a patch: a patch has no stroke timeline. */
  params.jitter_amount = (brush.flag & BRUSH_ABSOLUTE_JITTER) ?
                             brush.jitter_absolute * radius_per_size :
                             brush.jitter * radius;
  /* Texture rotation stays on the texture slot -- it is mapping, not Curve Patch business. */
  params.base_angle = brush.mtex.rot;
  params.random_angle = brush.mtex.random_angle;

  return params;
}

void curve_patch_texture_binding_clear(CurvePatchTextureBinding &r_binding)
{
  r_binding.stamp_texture_variants.reinitialize(0);
  r_binding.stamp_texture_weights_cdf.clear();
  /* Element-wise rather than `std::array::fill()`: `MTex` deletes its copy assignment (see
   * #DNA_DEFINE_CXX_METHODS), so DNA's explicit shallow-copy path is the only way to write one. */
  for (MTex &zone_variant : r_binding.ribbon_zone_variants) {
    zone_variant = dna::shallow_zero_initialize();
  }
  r_binding.caps_enabled = false;
  r_binding.world_cap_start = 0.0f;
  r_binding.world_cap_end = 0.0f;
}

void curve_patch_texture_binding_from_brush(const Brush &brush,
                                            const float radius,
                                            CurvePatchTextureBinding &r_binding)
{
  const BrushCurvePatchSettings &settings = brush.curve_patch;
  /* Every variant below is this slot with only its `tex` swapped, so all the mapping settings stay
   * shared with the brush. */
  const MTex &mtex = brush.mtex;

  curve_patch_texture_binding_clear(r_binding);

  if (settings.stamp_texture_source == BRUSH_CURVE_PATCH_TEX_MULTI) {
    /* Sized up front because `Array` cannot grow -- the container is fixed-size precisely because
     * `MTex` has no move constructor for a growing one to relocate through. */
    const int slot_num = settings.texture_slots.count();
    r_binding.stamp_texture_variants.reinitialize(slot_num);
    r_binding.stamp_texture_weights_cdf.reserve(slot_num);
    float running = 0.0f;
    int slot_index = 0;
    /* Range-for, NOT `LISTBASE_FOREACH`: that macro is private to `listbase.cc` in this branch.
     * `ListBaseT<T>` iterates directly -- see `for (PaletteColor &color : palette->colors)` in
     * `blenkernel/intern/paint.cc`. */
    for (const BrushCurvePatchTextureSlot &slot : settings.texture_slots) {
      /* `dna::shallow_copy()` rather than plain assignment: `MTex` deletes its copy assignment so
       * that DNA structs cannot be duplicated without opting into a shallow (non-owning) copy,
       * which is exactly what is wanted here -- only `tex` differs between variants. */
      MTex &variant = r_binding.stamp_texture_variants[slot_index];
      variant = dna::shallow_copy(mtex);
      variant.tex = slot.tex;
      /* Negative weights would make the table decrease and break the search; clamp rather than
       * reject, so a Python-set value cannot corrupt the layout. */
      running += std::max(slot.weight, 0.0f);
      r_binding.stamp_texture_weights_cdf.append(running);
      slot_index++;
    }
  }

  if (settings.ribbon_texture_source == BRUSH_CURVE_PATCH_TEX_MULTI) {
    r_binding.caps_enabled = true;
    const Tex *zone_textures[3] = {
        settings.tex_start, settings.tex_middle, settings.tex_end};
    for (const int i : IndexRange(3)) {
      r_binding.ribbon_zone_variants[i] = dna::shallow_copy(mtex);
      r_binding.ribbon_zone_variants[i].tex = const_cast<Tex *>(zone_textures[i]);
    }
    /* The UI stores cap lengths in brush DIAMETERS while `radius` is the ribbon's half-width, hence
     * the factor of two. The BASE radius, not the per-point one: a zone boundary that moved with
     * `radius_at_s` would not be a boundary at all. */
    r_binding.world_cap_start = settings.cap_start_length * 2.0f * radius;
    r_binding.world_cap_end = settings.cap_end_length * 2.0f * radius;
  }
}

}  // namespace blender::ed::sculpt_paint

namespace blender {

/* Declared in `ED_paint.hh`, which puts the `ED_*` API in `blender` rather than in this file's own
 * `blender::ed::sculpt_paint` -- hence the separate namespace block. */
Array<float> ED_curve_patch_stamp_texture_weights_from_brush(const Brush &brush, const float radius)
{
  ed::sculpt_paint::CurvePatchTextureBinding binding;
  ed::sculpt_paint::curve_patch_texture_binding_from_brush(brush, radius, binding);
  return Array<float>(binding.stamp_texture_weights_cdf.as_span());
}

}  // namespace blender
