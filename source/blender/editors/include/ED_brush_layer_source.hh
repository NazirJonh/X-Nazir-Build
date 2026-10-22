/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup editors
 *
 * The bake behind a PBR Paint brush's Layer source: the isolated result of one stack row, or a
 * layered material's whole stack, delivered as the same #MaterialSourceBake snapshot the Default
 * Material source uses. The paint path only ever reads the cache; #brush_layer_source_bake_ensure
 * starts the work from interaction-frequency code and never from a panel redraw.
 */

#include <cstdint>
#include <memory>
#include <optional>

#include "ED_material_bake.hh"

#include "DNA_brush_types.h"

struct bContext;

namespace blender {
struct Brush;
struct BrushMaterialPaint;
struct Image;
struct Main;
struct Material;
struct ReportList;
}  // namespace blender

namespace blender::ed::brush_layer_source {

using material_bake::MaterialSourceBake;
using material_bake::MaterialSourcePreview;

/**
 * The bake of \a brush_paint's Layer source as it is right now, or null while it is missing,
 * stale, or still rendering.
 *
 * Never renders and never blocks. A stale bake is deliberately not returned: unlike the Default
 * Material source, a Layer source strokes are gated on, so slightly stale pixels are not an
 * acceptable substitute for "not ready yet".
 */
std::shared_ptr<const MaterialSourceBake> brush_layer_source_bake_get(
    const BrushMaterialPaint &brush_paint);

/** Whether #brush_layer_source_bake_get would return a current bake. */
bool brush_layer_source_bake_ready(const BrushMaterialPaint &brush_paint);

/**
 * Whether the Layer source differs from what its bake holds, including the cases where no source
 * can be resolved at all (no owner, no such row).
 */
bool brush_layer_source_is_stale(const BrushMaterialPaint &brush_paint);

/** Whether a bake, a dependent row render, or the refresh debounce is in flight for \a brush. */
bool brush_layer_source_is_baking(const Brush &brush);

/**
 * Idempotent ensure for redraw-frequency callers. In Auto mode the bake starts once
 * #BrushMaterialPaint.source_refresh_delay has passed since the source last changed; in Manual mode
 * this does nothing. A source that needs a Material row's maps rendered first waits for them and
 * starts on a later call.
 */
void brush_layer_source_bake_ensure(const bContext &C, const Brush &brush);

/**
 * As above for callers with no #bContext (an RNA update, the refresh timer). Starts nothing when
 * \a bmain has no window manager. \a force is the Update Now path: it bypasses Manual mode and the
 * debounce.
 */
void brush_layer_source_bake_ensure(Main &bmain, const Brush &brush, bool force = false);

/**
 * Whether a stroke with \a brush may start. A Layer source the brush samples must have a current
 * bake before the first dab: painting the brush's own value instead would silently write something
 * the source does not describe. When it is not ready this starts the bake, reports \a reports and
 * returns false. Brushes without a Layer source always return true.
 */
bool brush_layer_source_stroke_ready(Main &bmain, const Brush &brush, ReportList *reports);

/** Update Now: start the current source's bake immediately, cancelling any refresh debounce. */
void brush_layer_source_bake_update_now(Main &bmain, const Brush &brush);

/**
 * Drop the cached bakes of \a owner, or all of them when \a owner is null, and the refresh timers.
 * Called on file load and quit; a stroke keeps its own snapshot alive through the `shared_ptr` it
 * was handed.
 */
void brush_layer_source_bake_invalidate(const Material *owner);

/** Mark every cached row bake that sampled \a image stale; the next ensure rebakes it. */
void brush_layer_source_tag_image_changed(const Image &image);

/**
 * Something a Layer source may read changed (a material edit, an image's pixels): ask every brush
 * with a Layer source to refresh. A brush whose bake is still current does nothing; one that went
 * stale restarts its debounce, so a burst of edits (a held slider, a paint stroke) is one bake
 * after the last of them.
 *  pixels_changed is true for an image edit, which leaves the source state unchanged but stale; a
 * material update is answered only when the state differs from the one last requested.
 */
void brush_layer_source_notify_changed(Main &bmain, bool pixels_changed);

/**
 * Fill \a r_preview from the Layer source, following the same channel order and placement as
 * #material_bake::material_source_preview_get. Keeps the bake alive through \a r_bake.
 */
bool brush_layer_source_preview_get(
    const BrushMaterialPaint &brush_paint,
    const PaintModeSettings &mode_settings,
    int visible_material_channels,
    MaterialSourcePreview &r_preview,
    std::shared_ptr<const MaterialSourceBake> &r_bake,
    std::optional<eMaterialPaintChannel> only_channel = std::nullopt);

}  // namespace blender::ed::brush_layer_source
