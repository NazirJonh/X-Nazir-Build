/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Master switch for all temporary performance / diagnostic printf logging added during PBR
 * material-paint development.  Set to 0 (or comment out the define entirely) to compile out
 * every logging call at once and measure clean performance without any I/O overhead.
 *
 * Individual subsystem macros below derive from this one master flag so that a single edit here
 * silences the whole system.
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
#endif

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
