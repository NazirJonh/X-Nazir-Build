/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "paint_image_session_state.hh"

#include "BKE_image.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_paint_bvh_pixels.hh"

#include <algorithm>

namespace blender::ed::sculpt_paint::image::session {

namespace {

constexpr int kHighResolutionCanvasPx = 4096;
constexpr int kHeavyProfileBrushSizePx = 96;

bool should_use_simple_image_brush_heavy_profile_impl(const int canvas_longest_side_px,
                                                      const int brush_size_px)
{
  return canvas_longest_side_px >= kHighResolutionCanvasPx && brush_size_px >= kHeavyProfileBrushSizePx;
}

}  // namespace

void PixelsNodeData::clear()
{
  indices.clear();
  mask = IndexMask();
}

void PixelsNodeData::clear_and_shrink()
{
  indices.clear_and_shrink();
  mask = IndexMask();
}

bool build_pixels_node_data(const Depsgraph &depsgraph,
                            Object &ob,
                            Image &image,
                            ImageUser &image_user,
                            PixelsNodeData &r_pixels_node_data)
{
  r_pixels_node_data.clear();

  bke::pbvh::Tree *pbvh = bke::object::pbvh_get(ob);
  if (pbvh == nullptr || pbvh->type() != bke::pbvh::Type::Mesh) {
    return false;
  }

  bke::pbvh::build_pixels(depsgraph, ob, image, image_user);

  IndexMaskMemory all_nodes_memory;
  const IndexMask all_nodes = bke::pbvh::all_leaf_nodes(*pbvh, all_nodes_memory);
  MutableSpan<bke::pbvh::MeshNode> nodes = pbvh->nodes<bke::pbvh::MeshNode>();

  IndexMaskMemory pixels_node_mask_memory;
  const IndexMask pixels_node_mask = IndexMask::from_predicate(
      all_nodes, pixels_node_mask_memory, [&](const int64_t i) { return nodes[i].pixels_ != nullptr; });
  if (pixels_node_mask.is_empty()) {
    return false;
  }

  r_pixels_node_data.indices.reserve(pixels_node_mask.size());
  pixels_node_mask.foreach_index([&](const int i) { r_pixels_node_data.indices.append(i); });

  r_pixels_node_data.mask = IndexMask::from_indices(r_pixels_node_data.indices.as_span(),
                                                     r_pixels_node_data.mask_memory);
  return !r_pixels_node_data.mask.is_empty();
}

void mark_pixels_node_image_dirty(const PixelsNodeData &pixels_node_data,
                                  Object &ob,
                                  Image &image,
                                  ImageUser &image_user)
{
  if (pixels_node_data.indices.is_empty()) {
    return;
  }

  bke::pbvh::Tree *pbvh = bke::object::pbvh_get(ob);
  if (pbvh == nullptr || pbvh->type() != bke::pbvh::Type::Mesh) {
    return;
  }

  MutableSpan<bke::pbvh::MeshNode> nodes = pbvh->nodes<bke::pbvh::MeshNode>();
  for (const int node_index : pixels_node_data.indices) {
    bke::pbvh::pixels::mark_image_dirty(nodes[node_index], image, image_user);
  }
}

bool collect_merged_dirty_tile_regions(const PixelsNodeData &pixels_node_data,
                                       Object &ob,
                                       Vector<DirtyTileRegion> &r_dirty_regions)
{
  r_dirty_regions.clear();
  if (pixels_node_data.indices.is_empty()) {
    return false;
  }

  bke::pbvh::Tree *pbvh = bke::object::pbvh_get(ob);
  if (pbvh == nullptr || pbvh->type() != bke::pbvh::Type::Mesh) {
    return false;
  }

  MutableSpan<bke::pbvh::MeshNode> nodes = pbvh->nodes<bke::pbvh::MeshNode>();
  for (const int node_index : pixels_node_data.indices) {
    bke::pbvh::pixels::NodeData &node_data = bke::pbvh::pixels::node_data_get(nodes[node_index]);
    if (!node_data.flags.dirty) {
      continue;
    }

    for (const bke::pbvh::pixels::UDIMTilePixels &tile_data : node_data.tiles) {
      if (!tile_data.flags.dirty) {
        continue;
      }
      DirtyTileRegion tile_region;
      tile_region.tile_number = tile_data.tile_number;
      tile_region.region = tile_data.dirty_region;
      r_dirty_regions.append(tile_region);
    }
  }

  if (r_dirty_regions.is_empty()) {
    return false;
  }

  std::sort(r_dirty_regions.begin(),
            r_dirty_regions.end(),
            [](const DirtyTileRegion &a, const DirtyTileRegion &b) {
              return a.tile_number < b.tile_number;
            });

  int write_i = 0;
  for (const int read_i : r_dirty_regions.index_range()) {
    if (write_i == 0 || r_dirty_regions[write_i - 1].tile_number != r_dirty_regions[read_i].tile_number) {
      r_dirty_regions[write_i] = r_dirty_regions[read_i];
      write_i++;
      continue;
    }

    DirtyTileRegion &merged = r_dirty_regions[write_i - 1];
    const DirtyTileRegion &item = r_dirty_regions[read_i];
    merged.region.xmin = std::min(merged.region.xmin, item.region.xmin);
    merged.region.xmax = std::max(merged.region.xmax, item.region.xmax);
    merged.region.ymin = std::min(merged.region.ymin, item.region.ymin);
    merged.region.ymax = std::max(merged.region.ymax, item.region.ymax);
  }
  r_dirty_regions.resize(write_i);
  return !r_dirty_regions.is_empty();
}

bool acquire_tile_image_buffer(Image &image,
                               const ImageUser &base_image_user,
                               short tile_number,
                               ImageUser &r_local_image_user,
                               ImBuf *&r_image_buffer)
{
  r_local_image_user = base_image_user;
  r_local_image_user.tile = tile_number;
  r_image_buffer = BKE_image_acquire_ibuf(&image, &r_local_image_user, nullptr);
  return r_image_buffer != nullptr;
}

void release_image_buffer(Image &image, ImBuf *&io_image_buffer)
{
  if (io_image_buffer == nullptr) {
    return;
  }
  BKE_image_release_ibuf(&image, io_image_buffer, nullptr);
  io_image_buffer = nullptr;
}

bool should_flush_preview_step(const double now_seconds,
                               const double flush_interval_seconds,
                               double &io_last_flush_time_seconds)
{
  if (io_last_flush_time_seconds < 0.0 ||
      now_seconds - io_last_flush_time_seconds >= flush_interval_seconds)
  {
    io_last_flush_time_seconds = now_seconds;
    return true;
  }
  return false;
}

bool should_use_simple_image_brush_heavy_profile(const int canvas_longest_side_px,
                                                 const int brush_size_px)
{
  return should_use_simple_image_brush_heavy_profile_impl(canvas_longest_side_px, brush_size_px);
}

bool should_mark_image_dirty_step(const bool had_updates)
{
  return had_updates;
}

bool should_mark_simple_image_brush_dirty_step(const bool had_updates,
                                               const int64_t tick_version,
                                               const int canvas_longest_side_px,
                                               const int brush_size_px)
{
  if (!had_updates) {
    return false;
  }

  if (!should_use_simple_image_brush_heavy_profile_impl(canvas_longest_side_px, brush_size_px)) {
    return should_mark_image_dirty_step(had_updates);
  }

  const int64_t normalized_tick = std::max<int64_t>(tick_version, 1);
  constexpr int64_t kHeavyProfileMarkDirtyPeriod = 2;
  return normalized_tick == 1 || (normalized_tick % kHeavyProfileMarkDirtyPeriod) == 0;
}

bool should_commit_session_step(const bool had_updates)
{
  return had_updates;
}

bool should_schedule_simple_image_brush_post_step(const bool had_updates,
                                                   const int64_t tick_version,
                                                   const int canvas_longest_side_px,
                                                   const int brush_size_px)
{
  if (!should_mark_image_dirty_step(had_updates)) {
    return false;
  }

  if (should_use_simple_image_brush_heavy_profile_impl(canvas_longest_side_px, brush_size_px)) {
    const int64_t normalized_tick = std::max<int64_t>(tick_version, 1);
    constexpr int64_t kHeavyProfileFlushPeriod = 6;
    return normalized_tick == 1 || (normalized_tick % kHeavyProfileFlushPeriod) == 0;
  }

  if (canvas_longest_side_px >= kHighResolutionCanvasPx) {
    const int64_t normalized_tick = std::max<int64_t>(tick_version, 1);
    const int64_t flush_period = (brush_size_px > 0 && brush_size_px <= 16) ? 4 : 3;
    return normalized_tick == 1 || (normalized_tick % flush_period) == 0;
  }

  return true;
}

bool should_apply_simple_image_brush_seam_fix_step(const bool had_updates,
                                                    const int brush_size_px,
                                                    const int64_t tick_version,
                                                    const int canvas_longest_side_px)
{
  if (!had_updates) {
    return false;
  }

  constexpr int kTinyBrushSizePx = 60;
  const int64_t normalized_tick = std::max<int64_t>(tick_version, 1);

  if (should_use_simple_image_brush_heavy_profile_impl(canvas_longest_side_px, brush_size_px)) {
    constexpr int64_t kHeavyProfileSeamFixPeriod = 4;
    return normalized_tick == 1 || (normalized_tick % kHeavyProfileSeamFixPeriod) == 0;
  }

  if (canvas_longest_side_px >= kHighResolutionCanvasPx) {
    const bool small_brush = brush_size_px > 0 && brush_size_px < kTinyBrushSizePx;
    const int64_t seam_fix_period = small_brush ? 4 : 2;
    return normalized_tick == 1 || (normalized_tick % seam_fix_period) == 0;
  }

  constexpr int64_t kTinyBrushSeamFixPeriod = 3;
  if (brush_size_px > 0 && brush_size_px < kTinyBrushSizePx) {
    return normalized_tick == 1 || (normalized_tick % kTinyBrushSeamFixPeriod) == 0;
  }

  return should_mark_image_dirty_step(had_updates);
}

bool should_push_simple_image_brush_undo_step()
{
  return true;
}

bool should_use_unified_simple_image_brush_backend()
{
  return true;
}

}  // namespace blender::ed::sculpt_paint::image::session
