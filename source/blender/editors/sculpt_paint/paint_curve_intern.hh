/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Umbrella over the paint-curve internal API, kept so that the many translation units that grew up
 * against one header keep compiling unchanged. It declares nothing of its own.
 *
 * New code should include the one it actually needs:
 *
 * - `paint_curve_geometry.hh` -- the curve as DATA: queries and mutation in the curve's own space.
 * - `paint_curve_screen.hh` -- projection, picking, the radius handle, surface placement: anything
 *   that takes a #ViewContext.
 * - `paint_curve_ops.hh` -- the `PAINTCURVE_OT_*` registrations, the slide keymap and its state,
 *   source-object sync and object conversion.
 *
 * Cross-module entry points live in `ED_paint.hh` instead.
 */

#pragma once

#include "paint_curve_geometry.hh"
#include "paint_curve_ops.hh"
#include "paint_curve_screen.hh"
