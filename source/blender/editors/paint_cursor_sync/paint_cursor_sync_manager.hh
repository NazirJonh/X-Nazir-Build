/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "paint_cursor_sync_types.hh"

#include "BLI_utility_mixins.hh"
#include "BLI_vector.hh"

namespace blender::editors {

class PaintCursorSyncManager : public NonCopyable, public NonMovable {
 public:
  static PaintCursorSyncManager &get();
  static void initialize();
  static void shutdown();

  void register_source(PaintCursorSource *source);
  void unregister_source(PaintCursorSource *source);
  void register_target(PaintCursorTarget *target);
  void unregister_target(PaintCursorTarget *target);

  void sync_from_source(PaintCursorSource *source);
  const CursorSyncData &get_current_data() const;
  bool has_valid_data() const;

  void set_target_filter(std::optional<PaintCursorTarget::SpaceType> filter);
  void set_image_filter(Image *image);

 private:
  PaintCursorSyncManager() = default;
  ~PaintCursorSyncManager() = default;

  Vector<PaintCursorSource *> sources_;
  Vector<PaintCursorTarget *> targets_;
  CursorSyncData current_data_;

  std::optional<PaintCursorTarget::SpaceType> target_filter_;
  Image *image_filter_ = nullptr;

  bool is_initialized_ = false;
};

}  // namespace blender::editors
