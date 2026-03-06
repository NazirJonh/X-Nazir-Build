/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <chrono>
#include <optional>
#include <string_view>
#include <functional>

#include "BLI_math_vector_types.hh"
#include "DNA_image_types.h"

struct Object;

namespace blender::editors {

/**
 * Data structure containing cursor position and related information
 * for synchronization between different editors.
 */
struct CursorSyncData {
  float3 object_position = float3(0.0f);
  float3 normal = float3(0.0f, 0.0f, 1.0f);
  std::optional<float2> uv_position = std::nullopt;
  int uv_layer_index = 0;
  Object *source_object = nullptr;
  Image *source_image = nullptr;
  float brush_radius_px = 0.0f;
  std::optional<float> brush_radius_uv = std::nullopt;
  bool is_valid = false;
  std::chrono::steady_clock::time_point last_update;

  /* Cursor position in image space (0-1) for 2D paint. */
  std::optional<float2> image_space_position = std::nullopt;

  CursorSyncData() = default;
};

/**
 * Callback type for cursor update notifications.
 */
using UpdateCallback = std::function<void(const CursorSyncData &data)>;

/**
 * Abstract interface for cursor data sources.
 * Implementations provide cursor position data from various editors.
 */
class PaintCursorSource {
 public:
  virtual ~PaintCursorSource() = default;

  virtual std::string_view get_id() const = 0;
  virtual bool is_active() const = 0;
  virtual CursorSyncData get_sync_data() const = 0;
  virtual void set_update_callback(UpdateCallback callback) = 0;

 protected:
  PaintCursorSource() = default;
};

/**
 * Abstract interface for cursor display targets.
 * Implementations display cursor position in various editors.
 */
class PaintCursorTarget {
 public:
  enum class SpaceType {
    View3D,
    Image,
    UV,
  };

  virtual ~PaintCursorTarget() = default;

  virtual std::string_view get_id() const = 0;
  virtual bool can_display() const = 0;
  virtual SpaceType get_space_type() const = 0;
  virtual void update_cursor(const CursorSyncData &data) = 0;
  virtual bool is_compatible_image(Image *image) const = 0;

 protected:
  PaintCursorTarget() = default;
};

}  // namespace blender::editors
