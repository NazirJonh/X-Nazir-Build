/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Public interface for the Sculpt "Extract Region" gesture tool: connected
 * face-region selection by face set or mask under the cursor, with extrude or
 * new-mesh output, plus the idle-hover preview API consumed by the paint cursor.
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

namespace blender::ed::sculpt_paint::extract_region {

enum class RegionSource {
  FaceSet = 0,
  Mask = 1,
};

void SCULPT_OT_extract_region(wmOperatorType *ot);
wmKeyMap *modal_keymap(wmKeyConfig *keyconf);

void extract_region_hover_init(bContext *C,
                               Object *ob,
                               ARegion *region,
                               RegionView3D *rv3d,
                               RegionSource source,
                               float mask_threshold);
void extract_region_hover_free();
void extract_region_hover_update(bContext *C,
                                 const float2 &mval,
                                 RegionSource source,
                                 float mask_threshold);
void extract_region_hover_draw();
bool extract_region_hover_is_enabled();
void extract_region_hover_activate();
void extract_region_hover_deactivate();
bool extract_region_hover_is_activated();

}  // namespace blender::ed::sculpt_paint::extract_region
