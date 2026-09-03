/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Master switch for all performance / diagnostic printf logging added during PBR material-paint
 * development. Off by default: every logging call compiles out, so a regular build pays nothing
 * for it. Set to 1 to bring the whole system back for a debugging session.
 *
 * The instrumentation is deliberately kept rather than deleted - it is the only per-dab breakdown
 * of the paint pipeline (factors / automasking / sampling+blending, per channel, per target) and
 * is the fastest way to localize a regression.
 *
 * Individual subsystem macros below derive from this one master flag so that a single edit here
 * silences the whole system. Every flag is defined to 0 or 1 (never left undefined), and every
 * use site must be `#if`, not `#ifdef`: an `#ifdef` against a flag that is defined-to-0 is always
 * true, which would silently re-enable the logging this switch is supposed to remove.
 */

#pragma once

/* -------------------------------------------------------------------- */
/** \name Master enable/disable
 *
 * Set to 1 to enable all PBR debug logging, 0 to compile it all out.
 * \{ */

#define PBR_PAINT_DEBUG_LOG 0

/** \} */

/* -------------------------------------------------------------------- */
/** \name Per-subsystem flags (all inherit from the master switch)
 * \{ */

/** sculpt_paint_image.cc – per-channel pixel pipeline timing and pair-paint stats. */
#if PBR_PAINT_DEBUG_LOG
#  define PBR_PAINT_IMAGE_PROFILE 1
#else
#  define PBR_PAINT_IMAGE_PROFILE 0
#endif

/** paint_image_2d.cc – Area Plane hit/unfold/raster timing. */
#if PBR_PAINT_DEBUG_LOG
#  define PBR_PAINT_2D_PROFILE 1
#else
#  define PBR_PAINT_2D_PROFILE 0
#endif

/**
 * paint_image_2d.cc – symmetry mirror tracing.
 *
 * Deliberately independent of the master switch: turn this on alone to print a few lines per dab
 * describing why a mirrored dab was or was not drawn. Enabling the master switch as well would
 * bury it under the per-dab timing dumps.
 */
#define PBR_PAINT_2D_SYMMETRY_DEBUG 0

/**
 * paint_image_2d.cc – dab-buffer fill timing (#brush_painter_imbuf_new and
 * #brush_painter_imbuf_update), reported as a per-stroke total.
 *
 * Deliberately independent of the master switch: this is the measurement point for the View Plane
 * sampling path, and the per-dab dumps the master switch enables would both perturb the timings
 * and bury the one line this prints per stroke.
 */
#define PBR_PAINT_2D_DAB_PROFILE 0

/**
 * paint_image_2d.cc – per-stroke cross-section of View Plane vs Area Plane.
 *
 * Reports wall time of #paint_2d_stroke, plus dab-fill, #paint_2d_op blending, Area Plane raster,
 * GPU redraw, painter count, imbuf rebuild vs partial update, and a one-shot #DirectSampleKind
 * dump per channel. Independent of the master switch: the per-dab dumps that master enables would
 * both perturb timings and bury the few summary lines this prints at stroke end.
 *
 * Set to 1 for a profiling session, then back to 0.
 */
#define PBR_PAINT_2D_STROKE_PROFILE 0

/** paint_material_source.cc – ChannelSourceSampler construction and image-pool probe timing. */
#if PBR_PAINT_DEBUG_LOG
#  define PBR_PAINT_SOURCE_PROFILE 1
#else
#  define PBR_PAINT_SOURCE_PROFILE 0
#endif

/** paint_image.cc – imapaint_image_update GPU-upload timing. */
#if PBR_PAINT_DEBUG_LOG
#  define PBR_PAINT_IMAGE_UPDATE_PROFILE 1
#else
#  define PBR_PAINT_IMAGE_UPDATE_PROFILE 0
#endif

/** sculpt_paint_material.cc – per-dab vertex-paint printf profiling. */
#if PBR_PAINT_DEBUG_LOG
#  define PBR_PAINT_MATERIAL_PROFILE 1
#else
#  define PBR_PAINT_MATERIAL_PROFILE 0
#endif

/**
 * paint_cursor.cc – Source Mode: Material cursor overlay gating.
 *
 * Prints one line per gate, and only when the reported state changes, so a cursor redrawn at every
 * mouse move does not flood the console. This is the only account of why no overlay is drawn for
 * a brush that looks correctly configured.
 */
#if PBR_PAINT_DEBUG_LOG
#  define PBR_PAINT_CURSOR_DEBUG 1
#else
#  define PBR_PAINT_CURSOR_DEBUG 0
#endif

/**
 * paint_material_source.cc, paint_image_2d.cc – consumption of a source-material bake.
 *
 * Reports which source kind every channel resolved to at stroke start, whether a bake was
 * available for it, and the first sample taken from one.
 *
 * The producing side has a switch of its own, #PBR_MATERIAL_BAKE_DEBUG in
 * `editors/render/render_material_bake.cc`: that is a different module and cannot reach this
 * header, so the two halves are enabled independently.
 */
#if PBR_PAINT_DEBUG_LOG
#  define PBR_PAINT_BAKE_DEBUG 1
#else
#  define PBR_PAINT_BAKE_DEBUG 0
#endif

/** \} */
