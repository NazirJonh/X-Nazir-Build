/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Cross-mode brush synchronization for Poly Paint.
 *
 * Makes Sculpt and Image Paint share one brush while the canvas is
 * #PAINT_CANVAS_SOURCE_MATERIAL and #PAINT_MATERIAL_BRUSH_SYNC is set. Deliberately kept in its
 * own module: this is a convenience layer on top of Blender's paint modes rather than a concept
 * the paint modes themselves know about, so every call site in an unrelated file must stay a
 * single call carrying no policy of its own, and the layer can be dropped or re-based cheaply.
 *
 * Automatic entry points no-op when sync is off or the canvas is not Material, so callers never
 * need to check first. The explicit directional entry point is the exception: it is intended for
 * the manual Sync Brush menu and therefore works while automatic sync is disabled.
 *
 * Turning sync off stops further mirroring but does not undo what already happened — notably
 * #Brush.ob_mode stays widened for a brush that was opted into the paired mode.
 */

#pragma once

#include <cstdint>

namespace blender {

struct Main;
struct Paint;
struct Scene;
enum class PaintMode : int8_t;

/**
 * The #Paint that should mirror \a source while brush sync is active, or null when nothing should
 * be synced.
 *
 * Returns null unless \a source is one of the Sculpt / Image Paint pair, brush sync is enabled and
 * the canvas is #PAINT_CANVAS_SOURCE_MATERIAL. #PAINT_CANVAS_SOURCE_MATERIAL_PAINT is deliberately
 * excluded: it writes per-vertex attributes the Image Editor never paints.
 */
Paint *BKE_paint_material_sync_target_get(Scene *scene, Paint *source);

/**
 * True when \a paint_mode is 3D Texture Paint and it must be left alone.
 *
 * 3D Texture Paint shares #ImagePaintSettings with the Image Editor's Paint, so restoring a
 * Texture Paint brush or syncing from that mode would overwrite the Sculpt / Image Editor pair
 * this module maintains.
 */
bool BKE_paint_material_sync_texture3d_conflicts(const Scene *scene, PaintMode paint_mode);

/**
 * Make the paint mode paired with \a source use the same brush, palette and cavity curve.
 *
 * The brush is shared as one ID rather than copied, so its settings cannot drift apart. Does not
 * touch the tool system: callers with a #bContext are responsible for the receiving side's tool
 * bindings.
 */
void BKE_paint_material_brush_sync(Scene *scene, Paint *source);

/**
 * Manually copy the brush, palette, cavity curve, visible material channels and unified settings
 * from \a source to \a destination. Unlike #BKE_paint_material_brush_sync this deliberately
 * bypasses the automatic sync flag, and it never changes that flag: it is a one-shot copy.
 *
 * \return false when nothing was copied, i.e. the canvas is not Material or \a source and
 * \a destination are not the Sculpt/Image Paint pair.
 */
bool BKE_paint_material_brush_sync_directional(Scene *scene, Paint *source, Paint *destination);

/**
 * Stop sharing the brush and give Image Paint its own copy when the automatic sync is disabled.
 * A copy is only made on the transition out of a shared brush, so repeated calls are no-ops.
 *
 * \return whether a copy was made and assigned.
 */
bool BKE_paint_material_brush_sync_disable(Main *bmain, Scene *scene);

/**
 * Align Image Paint with the Sculpt brush after a blend file or startup is loaded.
 *
 * Not sufficient on its own: the Image Editor tool still restores its last brush on first draw.
 * #BKE_paint_material_sync_on_tool_restore re-applies this afterwards. No-op when sync is off or
 * the canvas is not #PAINT_CANVAS_SOURCE_MATERIAL.
 */
void BKE_paint_material_brush_sync_after_load(Main *bmain);

/**
 * Mirror \a source's #UnifiedPaintSettings (size, strength, color, jitter) onto the paired paint
 * mode. Kept separate from #BKE_paint_material_brush_sync because these change far more often than
 * the active brush and are driven from a different callback.
 */
void BKE_paint_material_unified_settings_sync(Scene *scene, Paint *source);

/**
 * Call after the active brush of \a source changed through the tool system.
 *
 * Hands the paired paint mode the same brush.
 *
 * \return the #Paint whose tool brush binding the caller must refresh, or null when nothing was
 * synced. The receiving editor's tool is not switched — that would require its space to be active
 * — but its binding must follow the new brush, otherwise the next tool change there restores the
 * previously remembered brush and silently undoes the sync.
 */
Paint *BKE_paint_material_sync_on_brush_activated(Scene *scene,
                                                  Paint *source,
                                                  PaintMode paint_mode);

/**
 * Call after the tool system restored a remembered brush, with Sculpt as the source.
 *
 * \return the #Paint whose tool brush binding the caller must refresh, or null.
 */
Paint *BKE_paint_material_sync_on_tool_restore(Scene *scene, PaintMode paint_mode);

/**
 * True when the tool system must skip restoring \a paint's own remembered brush.
 *
 * Image Paint restoring its last used brush is exactly what undoes load-time sync, so while the
 * pair is synced Sculpt stays the source and Image Paint does not restore. Matches
 * #rna_PaintModeSettings_brush_sync_update.
 */
bool BKE_paint_material_sync_suppresses_brush_restore(Scene *scene, const Paint *paint);

}  // namespace blender
