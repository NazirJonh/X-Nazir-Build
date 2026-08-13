/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Opt-in compile-time instrumentation for multi-channel material paint.
 *
 * Disabled by default (`PAINT_MATERIAL_CHANNEL_PERF_DEBUG` is 0): the `#else` branch compiles
 * every public macro to a no-op, so no timing call survives in a regular build. Enabling it is a
 * two-step, deliberate act — set the define to 1 here to compile the timers in, then enable the
 * `bke.paint_channel_perf` CLOG category at runtime to actually emit the report (see
 * #logging_enabled for why the reporting is gated separately).
 */

#pragma once

/* Set to 1 to compile in multi-channel paint performance instrumentation. */
#define PAINT_MATERIAL_CHANNEL_PERF_DEBUG 0

#if PAINT_MATERIAL_CHANNEL_PERF_DEBUG

#  include <cstdint>

#  include "atomic_ops.h"
#  include "BLI_string.h"
#  include "BLI_time.h"
#  include "BLI_utildefines.h"

#  include "CLG_log.h"

namespace blender::bke::paint_material_channel_perf {

/**
 * Section timers are cheap (a couple of `now_seconds()` calls) and always collected. The actual
 * report is not: at 5 enabled Material channels a single dab emits ~50 log lines, and a drag
 * stroke fires a dab per mouse-move sample. Left unconditionally on, the logging itself is enough
 * to stutter the viewport. #CLOG_INFO already gates on the category's runtime log level (enable
 * with `--log "bke.paint_channel_perf"`), so the report only formats and writes when the category
 * has been switched on.
 */
inline CLG_LogRef LOG = {"bke.paint_channel_perf"};

inline bool logging_enabled()
{
  return CLOG_CHECK(&LOG, CLG_LEVEL_INFO);
}

enum class Section : int {
  SculptPbvhUpdatePixels = 0,
  PbvGatherTexpaint,
  DoBrushActionTotal,
  BuildPixelsTotal,
  BuildPixelsUvIslands,
  BuildPixelsEncode,
  BuildPixelsCopyUpdate,
  FetchBuffers,
  UndoPush,
  /** Brush falloff/hardness/strength/texture factors; shared across channels, only actually
   * timed (non-zero) on the dab/target where they were (re)computed. */
  PaintFactors,
  /** Per-channel read/blend/write using the (possibly shared) #PaintFactors result. */
  PaintPixels,
  SeamFix,
  MarkDirty,
  TargetTotal,
  Num,
};

constexpr int k_max_targets = 8;

struct TargetStats {
  char name[64] = "";
  int res_x = 0;
  int res_y = 0;
  uint64_t section_us[int(Section::Num)] = {};
  uint64_t leaf_nodes_updated = 0;
  uint64_t pixels_painted = 0;
  uint64_t rows_painted = 0;
  uint64_t rows_skipped = 0;
};

struct DabStats {
  uint64_t dab_index = 0;
  int symmetry_passes = 1;
  bool pbvh_update_rebuilt = false;
  int gather_node_count = 0;
  uint64_t section_us[int(Section::Num)] = {};
  int target_count = 0;
  TargetStats targets[k_max_targets] = {};
};

struct GlobalStats {
  uint64_t dab_index = 0;
  uint64_t stroke_eval_rebuild_count = 0;
  DabStats dab = {};
  int active_target = -1;
};

inline GlobalStats &global_stats()
{
  static GlobalStats stats;
  return stats;
}

inline double now_seconds()
{
  return BLI_time_now_seconds();
}

inline uint64_t seconds_to_us(const double seconds)
{
  return uint64_t(seconds * 1e6);
}

inline void add_section_us(const Section section, const uint64_t us)
{
  GlobalStats &stats = global_stats();
  atomic_fetch_and_add_uint64(&stats.dab.section_us[int(section)], us);
  if (stats.active_target >= 0 && stats.active_target < stats.dab.target_count) {
    atomic_fetch_and_add_uint64(
        &stats.dab.targets[stats.active_target].section_us[int(section)], us);
  }
}

class ScopeTimer {
 public:
  explicit ScopeTimer(const Section section) : section_(section), start_(now_seconds()) {}

  ~ScopeTimer()
  {
    const uint64_t us = seconds_to_us(now_seconds() - start_);
    add_section_us(section_, us);
  }

 private:
  Section section_;
  double start_;
};

inline void stroke_begin_log()
{
  /* Stroke-begin logging is emitted from the editor after targets are resolved. */
}

inline void dab_begin(const int symmetry_passes)
{
  GlobalStats &stats = global_stats();
  stats.dab = {};
  stats.dab.dab_index = atomic_fetch_and_add_uint64(&stats.dab_index, 1) + 1;
  stats.dab.symmetry_passes = symmetry_passes;
  stats.active_target = -1;
}

inline void set_pbvh_update_rebuilt(const bool rebuilt)
{
  global_stats().dab.pbvh_update_rebuilt = rebuilt;
}

inline void set_gather_node_count(const int count)
{
  global_stats().dab.gather_node_count = count;
}

inline void target_begin(const int target_index,
                         const char *channel_name,
                         const int res_x,
                         const int res_y)
{
  GlobalStats &stats = global_stats();
  stats.active_target = target_index;
  if (target_index < 0 || target_index >= k_max_targets) {
    return;
  }
  stats.dab.target_count = max_ii(stats.dab.target_count, target_index + 1);
  TargetStats &target = stats.dab.targets[target_index];
  target = {};
  if (channel_name) {
    BLI_strncpy(target.name, channel_name, sizeof(target.name));
  }
  target.res_x = res_x;
  target.res_y = res_y;
}

inline void set_build_pixels_leaf_nodes_updated(const int count)
{
  GlobalStats &stats = global_stats();
  if (stats.active_target >= 0 && stats.active_target < stats.dab.target_count) {
    stats.dab.targets[stats.active_target].leaf_nodes_updated = uint64_t(count);
  }
}

inline void add_pixels_painted(const uint64_t count)
{
  GlobalStats &stats = global_stats();
  if (stats.active_target >= 0 && stats.active_target < stats.dab.target_count) {
    atomic_fetch_and_add_uint64(&stats.dab.targets[stats.active_target].pixels_painted, count);
  }
}

inline void add_rows_painted(const uint64_t count)
{
  GlobalStats &stats = global_stats();
  if (stats.active_target >= 0 && stats.active_target < stats.dab.target_count) {
    atomic_fetch_and_add_uint64(&stats.dab.targets[stats.active_target].rows_painted, count);
  }
}

inline void add_rows_skipped(const uint64_t count)
{
  GlobalStats &stats = global_stats();
  if (stats.active_target >= 0 && stats.active_target < stats.dab.target_count) {
    atomic_fetch_and_add_uint64(&stats.dab.targets[stats.active_target].rows_skipped, count);
  }
}

inline void stroke_eval_rebuild()
{
  atomic_fetch_and_add_uint64(&global_stats().stroke_eval_rebuild_count, 1);
}

inline void print_section_ms(const char *label, const uint64_t us, const char *extra = nullptr)
{
  if (extra && extra[0]) {
    CLOG_INFO(&LOG, "    %s: %.3f ms (%s)", label, double(us) / 1000.0, extra);
  }
  else {
    CLOG_INFO(&LOG, "    %s: %.3f ms", label, double(us) / 1000.0);
  }
}

inline void dab_end_log()
{
  if (!logging_enabled()) {
    return;
  }
  const GlobalStats &stats = global_stats();
  const DabStats &dab = stats.dab;

  CLOG_INFO(&LOG,
           "dab=%llu targets=%d symmetry=%d eval_rebuilds=%llu",
           unsigned long long(dab.dab_index),
           dab.target_count,
           dab.symmetry_passes,
           unsigned long long(stats.stroke_eval_rebuild_count));

  print_section_ms("do_brush_action total",
                   dab.section_us[int(Section::DoBrushActionTotal)]);
  {
    char extra[64];
    SNPRINTF(extra,
             "rebuilt=%s",
             dab.pbvh_update_rebuilt ? "yes" : "no");
    print_section_ms("sculpt_pbvh_update_pixels",
                     dab.section_us[int(Section::SculptPbvhUpdatePixels)],
                     extra);
  }
  {
    char extra[64];
    SNPRINTF(extra, "nodes=%d", dab.gather_node_count);
    print_section_ms("pbvh_gather_texpaint",
                     dab.section_us[int(Section::PbvGatherTexpaint)],
                     extra);
  }

  for (int i = 0; i < dab.target_count; i++) {
    const TargetStats &target = dab.targets[i];
    CLOG_INFO(&LOG,
             "  target[%d] %s %dx%d: %.3f ms",
             i,
             target.name[0] ? target.name : "?",
             target.res_x,
             target.res_y,
             double(target.section_us[int(Section::TargetTotal)]) / 1000.0);

    {
      char extra[64];
      SNPRINTF(extra, "leaf_nodes_updated=%llu", unsigned long long(target.leaf_nodes_updated));
      print_section_ms("build_pixels",
                       target.section_us[int(Section::BuildPixelsTotal)],
                       extra);
    }
    print_section_ms("build_pixels_uv_islands",
                     target.section_us[int(Section::BuildPixelsUvIslands)]);
    print_section_ms("build_pixels_encode", target.section_us[int(Section::BuildPixelsEncode)]);
    print_section_ms("build_pixels_copy_update",
                     target.section_us[int(Section::BuildPixelsCopyUpdate)]);
    print_section_ms("fetch_buffers", target.section_us[int(Section::FetchBuffers)]);
    print_section_ms("undo_push", target.section_us[int(Section::UndoPush)]);
    print_section_ms("paint_factors (shared, 0 = reused)",
                     target.section_us[int(Section::PaintFactors)]);
    {
      char extra[96];
      SNPRINTF(extra,
               "pixels=%llu rows=%llu skipped=%llu",
               unsigned long long(target.pixels_painted),
               unsigned long long(target.rows_painted),
               unsigned long long(target.rows_skipped));
      print_section_ms("paint_pixels", target.section_us[int(Section::PaintPixels)], extra);
    }
    print_section_ms("seam_fix", target.section_us[int(Section::SeamFix)]);
    print_section_ms("mark_dirty", target.section_us[int(Section::MarkDirty)]);
  }
}

inline void stroke_begin_targets_log(const int target_count,
                                     const char *const *target_names,
                                     const int *res_x,
                                     const int *res_y)
{
  global_stats().stroke_eval_rebuild_count = 0;
  if (!logging_enabled()) {
    return;
  }
  char targets_desc[512] = "";
  size_t pos = 0;
  for (int i = 0; i < target_count && pos < sizeof(targets_desc); i++) {
    const char *name = (target_names && target_names[i]) ? target_names[i] : "?";
    int written = 0;
    if (res_x && res_y) {
      written = BLI_snprintf_rlen(
          targets_desc + pos, sizeof(targets_desc) - pos, " [%s %dx%d]", name, res_x[i], res_y[i]);
    }
    else {
      written = BLI_snprintf_rlen(targets_desc + pos, sizeof(targets_desc) - pos, " [%s]", name);
    }
    pos += size_t(max_ii(written, 0));
  }
  CLOG_INFO(&LOG, "stroke_begin targets=%d%s", target_count, targets_desc);
}

}  // namespace blender::bke::paint_material_channel_perf

#  define PAINT_CHANNEL_PERF_SCOPE(section) \
    const blender::bke::paint_material_channel_perf::ScopeTimer _paint_channel_perf_scope_##section( \
        blender::bke::paint_material_channel_perf::Section::section)

#else

/**
 * No-op stand-ins for the counters that are called without an `#ifdef` guard around the call site.
 * Keeping them here rather than wrapping each call keeps the paint loops readable: the hot row and
 * pixel counters sit in the middle of the blending code, where a preprocessor block per call would
 * obscure the actual work. Everything else in this header is heavier (it formats and prints) and
 * stays behind explicit guards at the call sites.
 */

#  include <cstdint>

namespace blender::bke::paint_material_channel_perf {

inline void target_begin(int /*target_index*/,
                         const char * /*channel_name*/,
                         int /*res_x*/,
                         int /*res_y*/)
{
}
inline void add_pixels_painted(uint64_t /*count*/) {}
inline void add_rows_painted(uint64_t /*count*/) {}
inline void add_rows_skipped(uint64_t /*count*/) {}

}  // namespace blender::bke::paint_material_channel_perf

#  define PAINT_CHANNEL_PERF_SCOPE(section) \
    do { \
    } while (0)

#endif
