/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "paint_curve_patch_live.hh"

#include <cstring>

/* `BrushCurvePatchSettings::texture_slots` is a `ListBaseT<T>`; the range-for over it below needs
 * the iterator definition, which `DNA_brush_types.h` only forward-declares. */
#include "BLI_listbase_iterator.hh"

#include "BKE_brush.hh"
#include "BKE_paint.hh"

#include "DNA_brush_types.h"
#include "DNA_color_types.h"
#include "DNA_texture_types.h"

namespace blender::ed::sculpt_paint {

/* Cheap order-sensitive digest of a brush's Curve Patch texture list, used only to detect that the
 * list changed and the patch must be re-stamped. Not a hash with any security or collision
 * guarantee -- a missed change would merely delay a re-stamp until the next edit. */
static uint64_t curve_patch_texture_list_digest(const Brush &brush)
{
  uint64_t digest = 1469598103934665603ull;
  /* Range-for, NOT `LISTBASE_FOREACH`: that macro is private to `listbase.cc` in this branch.
   * `ListBaseT<T>` iterates directly -- see `for (PaletteColor &color : palette->colors)` in
   * `blenkernel/intern/paint.cc`. */
  for (const BrushCurvePatchTextureSlot &slot : brush.curve_patch.texture_slots) {
    digest = (digest ^ uint64_t(uintptr_t(slot.tex))) * 1099511628211ull;
    uint32_t weight_bits;
    memcpy(&weight_bits, &slot.weight, sizeof(weight_bits));
    digest = (digest ^ uint64_t(weight_bits)) * 1099511628211ull;
  }
  return digest;
}

/* Pointer-only counterpart to the digest above, folding in each slot's `tex` pointer but not its
 * weight. A weight-only edit re-stamps (it changes #curve_patch_texture_list_digest) but does not
 * change WHICH images are sampled, so it must not by itself invalidate the session's texture pool
 * -- this narrower digest is what #CurvePatchLiveInputs::needs_texture_pool_rebuild watches
 * instead. Same combinator as above, so a pure slot reorder still changes it. */
static uint64_t curve_patch_texture_pointer_digest(const Brush &brush)
{
  uint64_t digest = 1469598103934665603ull;
  for (const BrushCurvePatchTextureSlot &slot : brush.curve_patch.texture_slots) {
    digest = (digest ^ uint64_t(uintptr_t(slot.tex))) * 1099511628211ull;
  }
  return digest;
}

CurvePatchLiveInputs curve_patch_live_inputs_capture(const Paint &paint, const Brush &brush)
{
  CurvePatchLiveInputs in;
  in.alpha = BKE_brush_alpha_get(&paint, &brush);
  in.dir_in = (brush.flag & BRUSH_DIR_IN) != 0;
  in.brush_color = BKE_brush_color_get(&paint, &brush);

  in.stamp_tex_source = brush.curve_patch.stamp_texture_source;
  in.ribbon_tex_source = brush.curve_patch.ribbon_texture_source;
  in.cap_start_length = brush.curve_patch.cap_start_length;
  in.cap_end_length = brush.curve_patch.cap_end_length;
  in.texture_list_digest = curve_patch_texture_list_digest(brush);
  in.texture_pointer_digest = curve_patch_texture_pointer_digest(brush);
  in.cap_tex_start = brush.curve_patch.tex_start;
  in.cap_tex_middle = brush.curve_patch.tex_middle;
  in.cap_tex_end = brush.curve_patch.tex_end;

  in.tex_size = float2(brush.mtex.size[0], brush.mtex.size[1]);
  in.tex_ofs = float2(brush.mtex.ofs[0], brush.mtex.ofs[1]);
  in.tex = brush.mtex.tex;
  in.tex_edit_count = BKE_paint_get_overlay_texture_edit_count();

  in.falloff_preset = brush.curve_distance_falloff_preset;
  in.falloff_curve_ts = brush.curve_distance_falloff ?
                            brush.curve_distance_falloff->changed_timestamp :
                            0;
  return in;
}

}  // namespace blender::ed::sculpt_paint
