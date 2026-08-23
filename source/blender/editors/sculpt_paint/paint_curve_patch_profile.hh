/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup edsculpt
 *
 * Opt-in performance instrumentation for the whole Curve Patch re-stamp. One toggle drives:
 * - session-level timing in `paint_curve_patch_session.cc` (`DEBUG-cpatch`: restore/ribbon/apply)
 * - the sampler's per-sample counters in `paint_curve_patch_sampler.hh`
 * - the per-sub-phase Image-effect breakdown in `paint_curve_patch_effect_image.cc`
 *   (`DEBUG-cpatch-image`)
 *
 * Lives in this header so the sampler does not include `paint_curve_patch_effect.hh` (M9).
 * Must stay 0 outside a measurement pass: the reports write to stdout and flush on every
 * interactive re-stamp. Grep `DEBUG-cpatch` for every touch point.
 */
#define CURVE_PATCH_PROFILING 0
