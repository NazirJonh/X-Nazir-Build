/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Public (cross-module) declarations for the mesh-warp part of the Image Paint selection
 * system. #paint_image_select_warp_intern.hh holds the rest -- state layout, math, operators --
 * and is only meant to be included from within this module.
 */

#pragma once

namespace blender {

/** Bump when #ImagePaintSettings::warp_grid_size changes (triggers a live grid resize on the
 * next non-blocking paint-cursor tick of any floating warp session; see
 * paint_image_select_warp.cc). */
void image_paint_warp_bump_settings_revision();

} /* namespace blender */
