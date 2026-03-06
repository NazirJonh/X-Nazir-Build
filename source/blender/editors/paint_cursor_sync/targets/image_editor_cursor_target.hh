/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "../paint_cursor_sync_types.hh"

#include "DNA_space_types.h"
#include "DNA_image_types.h"

#include "BLI_map.hh"

struct ARegion;

namespace blender::editors {

class ImageEditorCursorTarget : public PaintCursorTarget {
 public:
  ImageEditorCursorTarget(SpaceImage *sima, ARegion *region);
  ~ImageEditorCursorTarget() override;

  std::string_view get_id() const override;
  bool can_display() const override;
  SpaceType get_space_type() const override;
  void update_cursor(const CursorSyncData &data) override;
  bool is_compatible_image(Image *image) const override;

  void draw();

  /* Static registry for region -> target mapping. */
  static void register_target_for_region(ARegion *region, ImageEditorCursorTarget *target);
  static void unregister_target_for_region(ARegion *region);
  static ImageEditorCursorTarget *get_target_for_region(ARegion *region);

 private:
  SpaceImage *sima_;
  ARegion *region_;
  CursorSyncData current_data_;
  bool needs_redraw_ = false;

  /* Global map for region -> target. */
  static Map<ARegion *, ImageEditorCursorTarget *> region_target_map_;
};

}  // namespace blender::editors
