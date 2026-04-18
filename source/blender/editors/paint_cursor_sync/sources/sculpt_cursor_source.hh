/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "../paint_cursor_sync_types.hh"

#include "BLI_utildefines.h"
#include "DNA_image_types.h"

namespace blender::editors {

class SculptCursorSource : public PaintCursorSource {
 public:
  SculptCursorSource();
  ~SculptCursorSource() override;

  std::string_view get_id() const override;
  bool is_active() const override;
  CursorSyncData get_sync_data() const override;
  void set_update_callback(UpdateCallback callback) override;

  inline void update_from_cursor_data(const float3 &location,
                                      const float3 &normal,
                                      Object *object,
                                      float pixel_radius,
                                      bool is_valid,
                                      const std::optional<float2> &uv_position = std::nullopt,
                                      Image *source_image = nullptr)
  {
    is_active_ = is_valid;

    if (!is_active_) {
      data_.is_valid = false;
      return;
    }

    data_.object_position = location;
    data_.normal = normal;
    data_.source_object = object;
    data_.brush_radius_px = pixel_radius;
    data_.is_valid = true;
    data_.last_update = std::chrono::steady_clock::now();

    if (uv_position.has_value()) {
      data_.uv_position = uv_position;
    }
    if (source_image) {
      data_.source_image = source_image;
    }

    if (callback_) {
      callback_(data_);
    }
  }

  inline void update_from_2d_data(const float2 &image_space_pos,
                                  Image *image,
                                  float pixel_radius,
                                  bool is_valid)
  {
    is_active_ = is_valid;

    if (!is_active_) {
      data_.is_valid = false;
      return;
    }

    data_.image_space_position = image_space_pos;
    data_.source_image = image;
    data_.brush_radius_px = pixel_radius;
    data_.is_valid = true;
    data_.last_update = std::chrono::steady_clock::now();

    if (callback_) {
      callback_(data_);
    }
  }

 private:
  CursorSyncData data_;
  UpdateCallback callback_;
  bool is_active_ = false;
};

}  // namespace blender::editors
