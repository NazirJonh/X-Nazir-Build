/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#pragma once

#include "BLI_math_vector_types.hh"

namespace blender {

struct CurveMapping;

namespace ed::sculpt_paint::gradient {

enum class Type {
  Linear = 0,
  Radial = 1,
};

enum class Space {
  World = 0,
  Screen = 1,
  UV = 2,
};

struct Params {
  Type type = Type::Linear;
  Space space = Space::World;

  float3 start_ws = float3(0.0f);
  float3 end_ws = float3(1.0f, 0.0f, 0.0f);

  float2 start_ss = float2(0.0f);
  float2 end_ss = float2(1.0f, 0.0f);

  float hardness = 1.0f;
  bool clamp_to_range = false;
  const CurveMapping *curve = nullptr;

  /* For linear gradient: if true, reject pixels before the start point (t < 0).
   * If false (default), allow pixels before start to be painted with original color.
   * This provides backward compatibility option for users who want the old behavior. */
  bool clip_before_start = false;
};

}  // namespace ed::sculpt_paint::gradient

}  // namespace blender

