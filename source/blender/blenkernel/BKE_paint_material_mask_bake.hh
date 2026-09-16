/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * The session-global cache of baked layer masks.
 *
 * A baked mask B (see `paint_material_layer_mask_bake_intern.hh`) is the per-pixel coverage a layer
 * row's mask-correction chain produces. #BKE_paint_material_mask_bake_ensure recomputes it, and the
 * cache this header exposes is what lets it recompute only what moved: an entry is keyed on the
 * material's #ID.session_uid, the row's marker and the channel, so it survives the pointer churn an
 * undo step causes, and it learns about pixel edits by polling each source image's partial-update
 * log rather than by being told.
 *
 * The invalidation entry points here are for the *other* kind of change: the graph itself, which no
 * per-image tag can report, and teardown.
 */

#include <cstdint>

#include "BLI_uuid.h"

namespace blender {

struct Main;
struct Material;

/**
 * Recompute every baked mask of \a ma whose row has an anchor, so the shader samples current
 * pixels.
 *
 * The public entry point the editors call before a material is drawn or rendered. It is cheap when
 * nothing moved: the session cache this header exposes skips a row whose graph, size and source
 * pixels are unchanged, and a pixel edit refreshes only the rectangle it was reported in. \a
 * force_full recomputes every row regardless.
 *
 * Must run on the main thread: the cache and the image writes it drives are main-thread only.
 */
bool BKE_paint_material_mask_bake_ensure(Main &bmain, Material &ma, bool force_full);

/**
 * Mark the baked masks of \a ma out of date, or of every material when \a ma is null.
 *
 * Marks rather than drops, so a reader asking in between still gets the previous pixels. Called
 * wherever a layer's graph changed -- the stack's own description, not its pixels, which the poll
 * finds on its own -- and, with a null material, after an undo step that may have moved pixels
 * under the cache without any of the per-image tags being able to report it.
 */
void BKE_paint_material_mask_bake_cache_invalidate(const Material *ma);

/**
 * Drop every cached bake of \a ma, releasing its subscriptions.
 *
 * Called when the material is freed: the cache is keyed on #ID.session_uid, so an entry left behind
 * would never be looked up again and would hold its allocations until the budget happened to evict
 * it.
 */
void BKE_paint_material_mask_bake_cache_free_material(const Material &ma);

/** Drop every cached bake. For teardown and for file load. */
void BKE_paint_material_mask_bake_cache_free_all();

/** Whether a bake of (\a row_marker, \a channel) of \a ma is currently cached. Exists for tests. */
bool BKE_paint_material_mask_bake_cache_contains(const Material &ma,
                                                 const bUUID &row_marker,
                                                 int channel);

/**
 * The revision of the cached bake of (\a row_marker, \a channel), or zero when none is cached.
 *
 * Exists for tests and for a consumer that uploads B somewhere and needs to tell whether its copy
 * is still current without comparing pixels.
 */
uint64_t BKE_paint_material_mask_bake_cache_revision(const Material &ma,
                                                     const bUUID &row_marker,
                                                     int channel);

}  // namespace blender
