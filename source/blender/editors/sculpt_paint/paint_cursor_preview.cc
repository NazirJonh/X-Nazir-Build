/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Selection of what the brush cursor's texture overlay shows (see #PaintCursorContext's
 * `material_preview_*` members). Each kind of preview is one #OverlayPreviewProvider; adding a new
 * one means writing its `resolve` function and registering it in #overlay_preview_providers,
 * without touching the cursor drawing code.
 */

#include "paint_cursor.hh"

#include "BLI_utildefines.h"

#include "DNA_brush_enums.h"
#include "DNA_brush_types.h"
#include "DNA_defs.h"
#include "DNA_scene_types.h"
#include "DNA_texture_types.h"

#include "BKE_paint.hh"

#include "ED_material_bake.hh"

#include "mesh/paint_material_source.hh"

namespace blender::ed::sculpt_paint {

/** Everything a provider needs to decide, resolved once per cursor redraw. */
struct OverlayPreviewInput {
  const Brush &brush;
  const BrushMaterialPaint &brush_paint;
  const PaintModeSettings &mode_settings;
  PaintMode mode;
  int visible_channels;
};

struct OverlayPreviewProvider {
  /** Whether this provider owns the preview for the input. The first match wins. */
  bool (*applies)(const OverlayPreviewInput &in);
  /** Whether the stroke paints the previewed source only on the Material canvas, so that the
   * preview must follow the same canvas gate (#paint_2d_new_stroke for the Image Editor). */
  bool canvas_gated;
  /** Fills the context's `material_preview_*` members. Returns false when there is nothing to
   * show. */
  bool (*resolve)(const OverlayPreviewInput &in, PaintCursorContext &pcontext);
};

/* -------------------------------------------------------------------- */
/** \name Providers
 * \{ */

/* Mask mode reads the one channel #BrushMaterialPaint::mask_source selects instead of the PBR
 * channels. It is canvas gated like them: the source sampler exists only for the brushes and the
 * Material canvas the channel sources are painted on. The Erase brush keeps its own texture. */
static bool mask_source_applies(const OverlayPreviewInput &in)
{
  return in.mode_settings.layer_target_mode == PAINT_LAYER_TARGET_MASK &&
         ELEM(in.mode, PaintMode::Sculpt, PaintMode::Texture2D) &&
         in.brush.sculpt_brush_type != SCULPT_BRUSH_TYPE_ERASE;
}

static bool mask_source_resolve(const OverlayPreviewInput &in, PaintCursorContext &pcontext)
{
  const std::optional<eMaterialPaintChannel> mask_channel = material::mask_source_channel_get(
      in.brush_paint);
  if (!mask_channel) {
    /* Coverage is a flat brush color: there is no texture to place. */
    return false;
  }
  const bool is_alpha = *mask_channel == PAINT_MATERIAL_CHANNEL_ALPHA;
  pcontext.material_preview_texel.kind = OverlayTexelMode::Kind::MaskCoverage;
  pcontext.material_preview_texel.use_alpha = is_alpha;
  pcontext.material_preview_texel.invert =
      is_alpha && in.brush_paint.channels[PAINT_MATERIAL_CHANNEL_ALPHA].source_invert != 0;

  if (in.brush_paint.source_mode == BRUSH_MATERIAL_PAINT_SOURCE_MATERIAL) {
    /* A Mask stroke reads exactly this channel from the bake, regardless of the PBR channel's
     * enable and visibility gates. */
    const bool ok = material_bake::material_source_preview_get(in.brush_paint,
                                                               in.mode_settings,
                                                               in.visible_channels,
                                                               pcontext.material_preview_source,
                                                               pcontext.material_preview_bake,
                                                               mask_channel);
    pcontext.material_preview_mtex_storage = dna::shallow_copy(
        pcontext.material_preview_source.mtex);
    /* A constant channel has no texture to draw; only a baked buffer is previewed. */
    return ok && pcontext.material_preview_source.ibuf != nullptr;
  }
  return BKE_paint_material_mask_preview_mtex_get(
      in.brush_paint, *mask_channel, pcontext.material_preview_mtex_storage);
}

/* The Erase brush has no channel sources; its own texture is the strength mask, so it is what the
 * stroke samples and what the overlay must show. */
static bool erase_applies(const OverlayPreviewInput &in)
{
  return in.brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_ERASE;
}

static bool erase_resolve(const OverlayPreviewInput &in, PaintCursorContext &pcontext)
{
  pcontext.material_preview_texel.kind = OverlayTexelMode::Kind::EraseStrength;
  pcontext.material_preview_mtex_storage = dna::shallow_copy(in.brush.mtex);
  return in.brush.mtex.tex != nullptr;
}

/* Source Mode: Material has no #Tex to sample, so its pixels come from the bake instead.
 * Everything downstream (placement, map mode, hide rules) stays shared. */
static bool material_bake_applies(const OverlayPreviewInput &in)
{
  return in.brush_paint.source_mode == BRUSH_MATERIAL_PAINT_SOURCE_MATERIAL;
}

static bool material_bake_resolve(const OverlayPreviewInput &in, PaintCursorContext &pcontext)
{
  const bool ok = material_bake::material_source_preview_get(in.brush_paint,
                                                             in.mode_settings,
                                                             in.visible_channels,
                                                             pcontext.material_preview_source,
                                                             pcontext.material_preview_bake);
  pcontext.material_preview_mtex_storage = dna::shallow_copy(
      pcontext.material_preview_source.mtex);
  return ok;
}

/* Source Mode: Maps previews the first enabled PBR channel texture. */
static bool pbr_channel_applies(const OverlayPreviewInput & /*in*/)
{
  return true;
}

static bool pbr_channel_resolve(const OverlayPreviewInput &in, PaintCursorContext &pcontext)
{
  return BKE_paint_material_preview_mtex_get(in.brush_paint,
                                             in.mode_settings,
                                             in.visible_channels,
                                             pcontext.material_preview_mtex_storage);
}

/** \} */

/** Ordered: the first provider that applies decides. */
static const OverlayPreviewProvider overlay_preview_providers[] = {
    {mask_source_applies, /*canvas_gated*/ true, mask_source_resolve},
    {erase_applies, /*canvas_gated*/ true, erase_resolve},
    {material_bake_applies, /*canvas_gated*/ true, material_bake_resolve},
    {pbr_channel_applies, /*canvas_gated*/ true, pbr_channel_resolve},
};

void paint_cursor_material_preview_init(const PaintModeSettings &mode_settings,
                                        PaintCursorContext &pcontext)
{
  const Brush &brush = *pcontext.brush;
  if (brush.material_paint == nullptr) {
    return;
  }
  /* Sculpt previews only for the brushes that paint channel sources; the Image Editor for all. */
  const bool brush_eligible = (pcontext.mode == PaintMode::Sculpt &&
                               ELEM(brush.sculpt_brush_type,
                                    SCULPT_BRUSH_TYPE_PAINT,
                                    SCULPT_BRUSH_TYPE_ERASE)) ||
                              pcontext.mode == PaintMode::Texture2D;

  const OverlayPreviewInput in{
      brush,
      *brush.material_paint,
      mode_settings,
      pcontext.mode,
      pcontext.paint != nullptr ? pcontext.paint->visible_material_channels :
                                  PAINT_MATERIAL_CHANNELS_VISIBLE_ALL,
  };

  const OverlayPreviewProvider *provider = nullptr;
  for (const OverlayPreviewProvider &candidate : overlay_preview_providers) {
    if (!candidate.applies(in)) {
      continue;
    }
    /* A canvas-independent provider decides on its own; the others need an eligible brush. */
    if (candidate.canvas_gated && !brush_eligible) {
      continue;
    }
    provider = &candidate;
    break;
  }
  if (provider == nullptr) {
    return;
  }

  /* Preview the channel sources only on the canvases whose strokes actually paint them: the Image
   * Editor paints them on the Material canvas alone (#paint_2d_new_stroke), otherwise the brush's
   * own texture is what lands and must be the one previewed. */
  if (provider->canvas_gated) {
    const bool canvas_ok = (pcontext.mode == PaintMode::Texture2D) ?
                               mode_settings.canvas_source == PAINT_CANVAS_SOURCE_MATERIAL :
                               ELEM(mode_settings.canvas_source,
                                    PAINT_CANVAS_SOURCE_MATERIAL_PAINT,
                                    PAINT_CANVAS_SOURCE_MATERIAL);
    if (!canvas_ok) {
      return;
    }
  }

  const bool has_preview = provider->resolve(in, pcontext);
  MTex &mtex = pcontext.material_preview_mtex_storage;

  /* Image Editor overlays are flat screen-space quads, so it cannot draw Area or 3D mapping.
   * Sculpt's 3D cursor path handles Area mapping itself; preserving it here lets the PBR channel
   * overlay follow the surface just like a regular Sculpt brush texture. */
  if (pcontext.mode == PaintMode::Texture2D &&
      ELEM(mtex.brush_map_mode, MTEX_MAP_MODE_AREA, MTEX_MAP_MODE_3D))
  {
    mtex.brush_map_mode = MTEX_MAP_MODE_VIEW;
  }
  /* Tiled covers the whole viewport, obscuring the stroke result while painting; hide it for the
   * duration of the stroke and let it reappear once the stroke ends. */
  const bool hide_for_active_tiled_stroke = mtex.brush_map_mode == MTEX_MAP_MODE_TILED &&
                                            pcontext.paint->runtime->stroke_active;
  const bool hide_idle_view_in_sculpt = pcontext.mode != PaintMode::Texture2D &&
                                        brush.sculpt_brush_type != SCULPT_BRUSH_TYPE_ERASE &&
                                        mtex.brush_map_mode == MTEX_MAP_MODE_VIEW &&
                                        !pcontext.paint->runtime->draw_anchored;
  if (has_preview && !hide_idle_view_in_sculpt && !hide_for_active_tiled_stroke) {
    pcontext.material_preview_mtex = &mtex;
  }
  else {
    pcontext.material_preview_texel = OverlayTexelMode();
  }
}

}  // namespace blender::ed::sculpt_paint
