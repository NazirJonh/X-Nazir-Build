/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#pragma once

#include "DNA_brush_enums.h"

namespace blender::ed::sculpt_paint {

/**
 * Which RGBA channels a vertex-color paint operation is allowed to write to, derived from
 * #Brush.vertex_paint_channel_flag. Shared between Vertex Paint Mode (#paint_vertex.cc) and
 * Sculpt Mode's Paint brush (#sculpt_paint_color.cc) so both respect the same channel toggles.
 */
struct VPaintChannelMask {
  bool r;
  bool g;
  bool b;
  bool a;

  static inline VPaintChannelMask from_flag(const int channel_flag)
  {
    return {
        (channel_flag & BRUSH_VPAINT_CHANNEL_R) != 0,
        (channel_flag & BRUSH_VPAINT_CHANNEL_G) != 0,
        (channel_flag & BRUSH_VPAINT_CHANNEL_B) != 0,
        (channel_flag & BRUSH_VPAINT_CHANNEL_A) != 0,
    };
  }
};

}  // namespace blender::ed::sculpt_paint
