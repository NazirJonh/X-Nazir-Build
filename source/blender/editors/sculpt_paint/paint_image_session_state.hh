/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#pragma once

#include "BLI_rect.h"

#include "BLI_index_mask.hh"
#include "BLI_vector.hh"

#include <cstdint>

struct Depsgraph;
struct Image;
struct ImBuf;
struct ImageUser;
struct Object;

namespace blender::ed::sculpt_paint::image::session {

struct PixelsNodeData {
  Vector<int> indices;
  IndexMaskMemory mask_memory;
  IndexMask mask;

  void clear();
  void clear_and_shrink();
};

struct DirtyTileRegion {
  short tile_number = 0;
  rcti region = {};
};

bool build_pixels_node_data(const Depsgraph &depsgraph,
                            Object &ob,
                            Image &image,
                            ImageUser &image_user,
                            PixelsNodeData &r_pixels_node_data);

void mark_pixels_node_image_dirty(const PixelsNodeData &pixels_node_data,
                                  Object &ob,
                                  Image &image,
                                  ImageUser &image_user);

bool collect_merged_dirty_tile_regions(const PixelsNodeData &pixels_node_data,
                                       Object &ob,
                                       Vector<DirtyTileRegion> &r_dirty_regions);

bool acquire_tile_image_buffer(Image &image,
                               const ImageUser &base_image_user,
                               short tile_number,
                               ImageUser &r_local_image_user,
                               ImBuf *&r_image_buffer);

void release_image_buffer(Image &image, ImBuf *&io_image_buffer);

bool should_flush_preview_step(const double now_seconds,
                               const double flush_interval_seconds,
                               double &io_last_flush_time_seconds);

bool should_use_simple_image_brush_heavy_profile(int canvas_longest_side_px, int brush_size_px);

bool should_mark_image_dirty_step(const bool had_updates);

bool should_mark_simple_image_brush_dirty_step(const bool had_updates,
                                               int64_t tick_version,
                                               int canvas_longest_side_px,
                                               int brush_size_px);

bool should_commit_session_step(const bool had_updates);

bool should_schedule_simple_image_brush_post_step(const bool had_updates,
                                                  int64_t tick_version,
                                                  int canvas_longest_side_px,
                                                  int brush_size_px);

bool should_apply_simple_image_brush_seam_fix_step(const bool had_updates,
                                                   const int brush_size_px,
                                                   const int64_t tick_version,
                                                   int canvas_longest_side_px);

bool should_push_simple_image_brush_undo_step();

bool should_use_unified_simple_image_brush_backend();

}  // namespace blender::ed::sculpt_paint::image::session
