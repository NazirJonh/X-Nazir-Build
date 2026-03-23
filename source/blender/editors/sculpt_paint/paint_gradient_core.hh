/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#pragma once

#include <memory>

#include "BLI_span.hh"

#include "paint_gradient_types.hh"

namespace blender::ed::sculpt_paint::gradient {

class Calculator {
 public:
  virtual ~Calculator() = default;

  virtual float evaluate(const float3 &position) const = 0;
  virtual void evaluate_batch(Span<float3> positions, MutableSpan<float> factors) const;
  
  /* Getters for early rejection optimization - default implementation returns zeros */
  virtual float2 get_start_ss() const { return float2(0.0f); }
  virtual float2 get_end_ss() const { return float2(1.0f, 0.0f); }
  virtual bool is_radial() const { return false; }
  virtual bool get_clip_before_start() const { return false; }
};

std::unique_ptr<Calculator> create(const Params &params);

}  // namespace blender::ed::sculpt_paint::gradient
