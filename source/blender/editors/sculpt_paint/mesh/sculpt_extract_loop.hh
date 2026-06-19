/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Public interface for the Sculpt "Extract Loop" gesture tool: operator and
 * keymap registration plus the idle-hover preview API consumed by the paint
 * cursor.
 */

#pragma once

#include "BLI_math_vector_types.hh"

struct ARegion;
struct Object;
struct RegionView3D;
struct bContext;
struct wmKeyConfig;
struct wmKeyMap;
struct wmOperatorType;

namespace blender::ed::sculpt_paint::extract_loop {

enum class ExtractionMode {
  Loop = 0,
  Ring = 1,
  FaceStrip = 2,
};

enum class LoopOrientation {
  Horizontal = 0,
  Vertical = 1,
};

enum class ExtractionOutputType {
  Mesh = 0,
  Curves = 1,
  Extrude = 2,
};

void SCULPT_OT_extract_loop_gesture(wmOperatorType *ot);
wmKeyMap *modal_keymap(wmKeyConfig *keyconf);

void extract_loop_hover_init(bContext *C,
                             Object *ob,
                             ARegion *region,
                             RegionView3D *rv3d,
                             ExtractionMode mode,
                             LoopOrientation loop_orientation);
void extract_loop_hover_free();
void extract_loop_hover_update(bContext *C,
                               const float2 &mval,
                               ExtractionMode mode,
                               LoopOrientation loop_orientation);
void extract_loop_hover_draw();
bool extract_loop_hover_is_enabled();
void extract_loop_hover_activate();
void extract_loop_hover_deactivate();
bool extract_loop_hover_is_activated();

}  // namespace blender::ed::sculpt_paint::extract_loop
