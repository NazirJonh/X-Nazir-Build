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

/** \} */
