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

/** Fold one #MTex's mapping (never its texture, which the caller digests separately) into \a
 * digest. The shared source mapping is what keeps multi-channel patterns aligned, so a change
 * to it has to re-stamp. */
static uint64_t curve_patch_mtex_mapping_digest(uint64_t digest, const MTex &mtex)
{
  const auto fold = [&digest](const uint64_t value) {
    digest = (digest ^ value) * 1099511628211ull;
  };
  const auto fold_float = [&fold](const float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    fold(uint64_t(bits));
  };
  for (const int i : {0, 1, 2}) {
    fold_float(mtex.size[i]);
    fold_float(mtex.ofs[i]);
  }
  fold_float(mtex.rot);
  fold(uint64_t(uint32_t(mtex.brush_map_mode)));
  return digest;
}

/** Every #BrushMaterialPaint field a re-stamp reads. See
 * #CurvePatchLiveInputs::material_paint_digest for why this is a digest. */
static uint64_t curve_patch_material_paint_digest(const Brush &brush)
{
  uint64_t digest = 1469598103934665603ull;
  const BrushMaterialPaint *brush_paint = brush.material_paint;
  if (brush_paint == nullptr) {
    return digest;
  }
  const auto fold = [&digest](const uint64_t value) {
    digest = (digest ^ value) * 1099511628211ull;
  };
  const auto fold_float = [&fold](const float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    fold(uint64_t(bits));
  };
  for (const BrushMaterialPaintChannel &channel : brush_paint->channels) {
    for (const int i : {0, 1, 2}) {
      fold_float(channel.value[i]);
    }
    fold(uint64_t(uint16_t(channel.blend)));
    fold(uint64_t(uint8_t(channel.use)));
    fold(uint64_t(uint8_t(channel.normal_space)));
    /* The source's own mapping overrides can differ per channel even though the shared block
     * below supplies the common part. */
    digest = curve_patch_mtex_mapping_digest(digest, channel.source_mtex);
  }
  for (const int i : {0, 1, 2}) {
    fold_float(brush_paint->base_color[i]);
  }
  fold(uint64_t(uint8_t(brush_paint->use_sync_base_color_with_brush)));
  fold(uint64_t(uint8_t(brush_paint->use_alpha_map)));
  fold(uint64_t(uint8_t(brush_paint->use_alpha_stroke_mask)));
  digest = curve_patch_mtex_mapping_digest(digest, brush_paint->shared_source_mapping);
  return digest;
}

/** The channels' source texture identities alone -- see
 * #CurvePatchLiveInputs::material_source_digest. */
static uint64_t curve_patch_material_source_digest(const Brush &brush)
{
  uint64_t digest = 1469598103934665603ull;
  const BrushMaterialPaint *brush_paint = brush.material_paint;
  if (brush_paint == nullptr) {
    return digest;
  }
  for (const BrushMaterialPaintChannel &channel : brush_paint->channels) {
    const Tex *tex = channel.source_mtex.tex;
    digest = (digest ^ uint64_t(uintptr_t(tex))) * 1099511628211ull;
    /* The #Tex pointer alone does not identify what gets sampled. Assigning an image to a channel
     * that already has an image source REUSES the existing `Tex` and only swaps `Tex::ima`
     * (`rna_BrushMaterialPaintChannel_source_image_set()`), so hashing the wrapper would report
     * "nothing changed" for every assignment after the first -- the live session would keep
     * sampling the previous image until some unrelated edit forced a re-stamp. */
    if (tex != nullptr && tex->type == TEX_IMAGE) {
      digest = (digest ^ uint64_t(uintptr_t(tex->ima))) * 1099511628211ull;
    }
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

  in.material_paint_digest = curve_patch_material_paint_digest(brush);
  in.material_source_digest = curve_patch_material_source_digest(brush);
  in.visible_material_channels = paint.visible_material_channels;

  in.falloff_preset = brush.curve_distance_falloff_preset;
  in.falloff_curve_ts = brush.curve_distance_falloff ?
                            brush.curve_distance_falloff->changed_timestamp :
                            0;
  return in;
}

}  // namespace blender::ed::sculpt_paint
