/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup DNA
 */

#pragma once

#include "BLI_enum_flags.hh"

namespace blender {

/** Settings for off-screen rendering. */
enum eV3DOffscreenDrawFlag : int {
  V3D_OFSDRAW_NONE = (0),
  V3D_OFSDRAW_SHOW_ANNOTATION = (1 << 0),
  V3D_OFSDRAW_OVERRIDE_SCENE_SETTINGS = (1 << 1),
  V3D_OFSDRAW_SHOW_GRIDFLOOR = (1 << 2),
  V3D_OFSDRAW_SHOW_SELECTION = (1 << 3),
  V3D_OFSDRAW_XR_SHOW_CONTROLLERS = (1 << 4),
  V3D_OFSDRAW_XR_SHOW_CUSTOM_OVERLAYS = (1 << 5),
  V3D_OFSDRAW_SHOW_OBJECT_EXTRAS = (1 << 6),
  V3D_OFSDRAW_XR_SHOW_PASSTHROUGH = (1 << 7),
  /* By default, the viewport background is set to use the world.
   * In some specific case, we want to use the actual setting from the viewport or scene data. */
  V3D_OFSDRAW_NO_WORLD_BACKGROUND_OVERRIDE = (1 << 8),
};
ENUM_OPERATORS(eV3DOffscreenDrawFlag)

/** #View3DShading.light */
enum eV3DShadingLightingMode : char {
  V3D_LIGHTING_FLAT = 0,
  V3D_LIGHTING_STUDIO = 1,
  V3D_LIGHTING_MATCAP = 2,
};

/** #View3DShading.color_type, #View3DShading.wire_color_type */
enum eV3DShadingColorType : char {
  V3D_SHADING_MATERIAL_COLOR = 0,
  V3D_SHADING_RANDOM_COLOR = 1,
  V3D_SHADING_SINGLE_COLOR = 2,
  V3D_SHADING_TEXTURE_COLOR = 3,
  V3D_SHADING_OBJECT_COLOR = 4,
  V3D_SHADING_VERTEX_COLOR = 5,
};

/** #View3DShading.background_type */
enum eV3DShadingBackgroundType : char {
  V3D_SHADING_BACKGROUND_THEME = 0,
  V3D_SHADING_BACKGROUND_WORLD = 1,
  V3D_SHADING_BACKGROUND_VIEWPORT = 2,
};

/** #View3DOverlay.vertex_paint_channel_flag */
enum eV3DOverlay_VertexPaintChannel {
  V3D_OVERLAY_VPAINT_SHOW_R = (1 << 0),
  V3D_OVERLAY_VPAINT_SHOW_G = (1 << 1),
  V3D_OVERLAY_VPAINT_SHOW_B = (1 << 2),
  V3D_OVERLAY_VPAINT_SHOW_A = (1 << 3),
  /** Display single channel in grayscale mode instead of channel color */
  V3D_OVERLAY_VPAINT_GRAYSCALE = (1 << 4),
  /** Mask for all RGB channels (R, G, B) */
  V3D_OVERLAY_VPAINT_SHOW_RGB_MASK = (V3D_OVERLAY_VPAINT_SHOW_R | V3D_OVERLAY_VPAINT_SHOW_G |
                                      V3D_OVERLAY_VPAINT_SHOW_B),
  /** Mask for all channels (R, G, B, A) */
  V3D_OVERLAY_VPAINT_SHOW_ALL_MASK = (V3D_OVERLAY_VPAINT_SHOW_R | V3D_OVERLAY_VPAINT_SHOW_G |
                                      V3D_OVERLAY_VPAINT_SHOW_B | V3D_OVERLAY_VPAINT_SHOW_A),
};

}  // namespace blender
