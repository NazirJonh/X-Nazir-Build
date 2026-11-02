/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Opt-in stdout timing of the PBR paint dab pipeline.
 *
 * Off by default. To measure, uncomment `DEBUG_PAINT_DAB_TIMING` below and rebuild: every brush
 * application is then timed per stage and the totals of a stroke are printed when it ends
 * (`STROKE END`, preceded by a `map:` line for every channel map and a `source:` line for every
 * textured channel), with a progress report every 200 dabs. With it off every call in this header
 * collapses to an empty inline.
 *
 * Stages marked `wall` are elapsed time on the painting thread. Stages marked `cpu` run inside
 * parallel loops and add up the time of every thread, so they can exceed the wall time several
 * times over and must not be added to it. `items` are texels for the pixel stages.
 *
 * Run Blender without `--debug` when measuring: that switches to the guarded allocator, which
 * inflates everything that allocates. The matching stroke benchmark is `PbrChannelsPaintTest` in
 * `tests/performance/tests/texture_paint.py`.
 *
 * \note The counters are process wide and reset at the end of every stroke.
 */

#pragma once

/* Uncomment to print the stage breakdown of a PBR paint stroke to stdout. */
// #define DEBUG_PAINT_DAB_TIMING

#include <cstdint>
#include <optional>

#include "BLI_utildefines.h"

/* The instrumented translation units get the profiler scope macros (`PRF_scope_with_name`) from
 * here, next to the stdout timers. */
#include "PRF_profile.hh"

#ifdef DEBUG_PAINT_DAB_TIMING
#  include <atomic>
#  include <chrono>
#  include <cstdio>
#  include <mutex>
#  include <set>
#  include <utility>
#endif

namespace blender::ed::sculpt_paint {

/**
 * The stages of one PBR paint dab, in the order the pipeline reaches them.
 *
 * `Row factors` and `Source sampling` are measured around the whole pass, and the write side is
 * split into `Read`/`Blend`/`Write`; `Fused write` covers the one-pass path of
 * `apply_noop_fused`, which already does all three at once and cannot be attributed per texel
 * without undoing the optimization it measures.
 */
enum ePaintDabTimerStage : int {
  /** Wall time of one whole brush application, from entry to return. The reference for the rest. */
  PAINT_DAB_TIMER_DAB_TOTAL = 0,
  PAINT_DAB_TIMER_ROW_FACTORS,
  PAINT_DAB_TIMER_ROW_GEOMETRY,
  PAINT_DAB_TIMER_SOURCE_SAMPLING,
  PAINT_DAB_TIMER_READ,
  PAINT_DAB_TIMER_BLEND,
  PAINT_DAB_TIMER_WRITE,
  PAINT_DAB_TIMER_FUSED_WRITE,
  PAINT_DAB_TIMER_UNDO_PUSH,
  /**
   * The texture loop of source sampling, split by the mapping that drives it. These are a subset of
   * #PAINT_DAB_TIMER_SOURCE_SAMPLING (which also counts constant and unusable sources) and are
   * what tells the Area Plane cost from the Stencil one.
   */
  PAINT_DAB_TIMER_SOURCE_AREA,
  PAINT_DAB_TIMER_SOURCE_STENCIL,
  PAINT_DAB_TIMER_SOURCE_OTHER,
  /**
   * The parts of #PAINT_DAB_TIMER_ROW_FACTORS. SETUP and LOOP are wall time of the serial setup and
   * of the row loop; the rest run inside the loop's parallel chunks (CPU time summed over threads).
   * Whatever is left of ROW_FACTORS after SETUP and LOOP is the merge of the dirty bounds.
   */
  PAINT_DAB_TIMER_ROW_SETUP,
  /** The serial parts of ROW_SETUP: the primitive brush test, the valid-row mask, the arrays. */
  PAINT_DAB_TIMER_ROW_SETUP_TEST,
  PAINT_DAB_TIMER_ROW_SETUP_ROWS,
  PAINT_DAB_TIMER_ROW_SETUP_ARRAYS,
  PAINT_DAB_TIMER_ROW_LOOP,
  PAINT_DAB_TIMER_ROW_SPAN,
  PAINT_DAB_TIMER_ROW_ALLOC,
  PAINT_DAB_TIMER_ROW_FALLOFF,
  PAINT_DAB_TIMER_ROW_BRUSH_TEXTURE,
  PAINT_DAB_TIMER_ROW_MASKS,
  /**
   * Image Editor (2D) paint, all wall time on the painting thread. The Area Plane path rasterizes
   * the dab through the mesh (PREPARE, COVERAGE, FILL, AP_BLEND); the other mappings build a brush
   * image first (BRUSH_BUILD) and blend it (BRUSH_BLEND). DIRTY is the undo capture of the region
   * and sits inside both blends; IMAGE_UPDATE and REDRAW_NOTIFY belong to the redraw of a mouse
   * event, not to a dab.
   */
  PAINT_DAB_TIMER_2D_AP_PREPARE,
  PAINT_DAB_TIMER_2D_AP_COVERAGE,
  PAINT_DAB_TIMER_2D_AP_FILL,
  PAINT_DAB_TIMER_2D_AP_BLEND,
  PAINT_DAB_TIMER_2D_DIRTY,
  PAINT_DAB_TIMER_2D_BRUSH_BUILD,
  PAINT_DAB_TIMER_2D_BRUSH_BLEND,
  PAINT_DAB_TIMER_2D_IMAGE_UPDATE,
  PAINT_DAB_TIMER_2D_REDRAW_NOTIFY,
  PAINT_DAB_TIMER_STAGE_NUM,
};

/**
 * Whether a stage is timed around code that runs on the calling thread (wall time), as opposed to
 * inside a parallel loop where every thread adds its own time (CPU time summed over threads, which
 * can exceed the wall time several times over). The two must not be added together.
 */
inline bool paint_dab_timer_stage_is_wall(const ePaintDabTimerStage stage)
{
  /* No `default`, so the compiler flags a stage added to the enum and forgotten here. (`ELEM` takes
   * at most 17 values, which this list outgrew.) */
  switch (stage) {
    case PAINT_DAB_TIMER_DAB_TOTAL:
    case PAINT_DAB_TIMER_ROW_FACTORS:
    case PAINT_DAB_TIMER_ROW_SETUP:
    case PAINT_DAB_TIMER_ROW_SETUP_TEST:
    case PAINT_DAB_TIMER_ROW_SETUP_ROWS:
    case PAINT_DAB_TIMER_ROW_SETUP_ARRAYS:
    case PAINT_DAB_TIMER_ROW_LOOP:
    case PAINT_DAB_TIMER_UNDO_PUSH:
    case PAINT_DAB_TIMER_2D_AP_PREPARE:
    case PAINT_DAB_TIMER_2D_AP_COVERAGE:
    case PAINT_DAB_TIMER_2D_AP_FILL:
    case PAINT_DAB_TIMER_2D_AP_BLEND:
    case PAINT_DAB_TIMER_2D_DIRTY:
    case PAINT_DAB_TIMER_2D_BRUSH_BUILD:
    case PAINT_DAB_TIMER_2D_BRUSH_BLEND:
    case PAINT_DAB_TIMER_2D_IMAGE_UPDATE:
    case PAINT_DAB_TIMER_2D_REDRAW_NOTIFY:
      return true;
    case PAINT_DAB_TIMER_ROW_GEOMETRY:
    case PAINT_DAB_TIMER_SOURCE_SAMPLING:
    case PAINT_DAB_TIMER_READ:
    case PAINT_DAB_TIMER_BLEND:
    case PAINT_DAB_TIMER_WRITE:
    case PAINT_DAB_TIMER_FUSED_WRITE:
    case PAINT_DAB_TIMER_SOURCE_AREA:
    case PAINT_DAB_TIMER_SOURCE_STENCIL:
    case PAINT_DAB_TIMER_SOURCE_OTHER:
    case PAINT_DAB_TIMER_ROW_SPAN:
    case PAINT_DAB_TIMER_ROW_ALLOC:
    case PAINT_DAB_TIMER_ROW_FALLOFF:
    case PAINT_DAB_TIMER_ROW_BRUSH_TEXTURE:
    case PAINT_DAB_TIMER_ROW_MASKS:
    case PAINT_DAB_TIMER_STAGE_NUM:
      return false;
  }
  return false;
}

inline const char *paint_dab_timer_stage_name(const ePaintDabTimerStage stage)
{
  switch (stage) {
    case PAINT_DAB_TIMER_DAB_TOTAL:
      return "DAB TOTAL";
    case PAINT_DAB_TIMER_SOURCE_AREA:
      return "  src area";
    case PAINT_DAB_TIMER_SOURCE_STENCIL:
      return "  src stencil";
    case PAINT_DAB_TIMER_SOURCE_OTHER:
      return "  src other";
    case PAINT_DAB_TIMER_ROW_SETUP:
      return "  rf setup";
    case PAINT_DAB_TIMER_ROW_SETUP_TEST:
      return "    setup: brush test";
    case PAINT_DAB_TIMER_ROW_SETUP_ROWS:
      return "    setup: valid rows";
    case PAINT_DAB_TIMER_ROW_SETUP_ARRAYS:
      return "    setup: arrays";
    case PAINT_DAB_TIMER_ROW_LOOP:
      return "  rf row loop";
    case PAINT_DAB_TIMER_ROW_SPAN:
      return "    rf brush span";
    case PAINT_DAB_TIMER_ROW_ALLOC:
      return "    rf row alloc";
    case PAINT_DAB_TIMER_ROW_FALLOFF:
      return "    rf falloff";
    case PAINT_DAB_TIMER_ROW_BRUSH_TEXTURE:
      return "    rf brush tex";
    case PAINT_DAB_TIMER_ROW_MASKS:
      return "    rf masks";
    case PAINT_DAB_TIMER_2D_AP_PREPARE:
      return "2D ap prepare";
    case PAINT_DAB_TIMER_2D_AP_COVERAGE:
      return "2D ap coverage";
    case PAINT_DAB_TIMER_2D_AP_FILL:
      return "2D ap fill";
    case PAINT_DAB_TIMER_2D_AP_BLEND:
      return "2D ap blend";
    case PAINT_DAB_TIMER_2D_DIRTY:
      return "2D undo/dirty";
    case PAINT_DAB_TIMER_2D_BRUSH_BUILD:
      return "2D brush build";
    case PAINT_DAB_TIMER_2D_BRUSH_BLEND:
      return "2D brush blend";
    case PAINT_DAB_TIMER_2D_IMAGE_UPDATE:
      return "2D image update";
    case PAINT_DAB_TIMER_2D_REDRAW_NOTIFY:
      return "2D redraw notify";
    case PAINT_DAB_TIMER_ROW_FACTORS:
      return "row factors";
    case PAINT_DAB_TIMER_ROW_GEOMETRY:
      return "row geometry";
    case PAINT_DAB_TIMER_SOURCE_SAMPLING:
      return "source sampling";
    case PAINT_DAB_TIMER_READ:
      return "read";
    case PAINT_DAB_TIMER_BLEND:
      return "blend";
    case PAINT_DAB_TIMER_WRITE:
      return "write";
    case PAINT_DAB_TIMER_FUSED_WRITE:
      return "fused write";
    case PAINT_DAB_TIMER_UNDO_PUSH:
      return "undo push";
    case PAINT_DAB_TIMER_STAGE_NUM:
      break;
  }
  return "unknown";
}

#ifdef DEBUG_PAINT_DAB_TIMING

struct PaintDabTimerCounters {
  /** Nanoseconds spent in the stage, summed over every thread and every channel pass. */
  std::atomic<long long> total_ns[PAINT_DAB_TIMER_STAGE_NUM];
  /** Items the stage processed (texels for the pixel stages, calls for the pass stages). */
  std::atomic<long long> items[PAINT_DAB_TIMER_STAGE_NUM];
  /** Times the stage ran. */
  std::atomic<long long> calls[PAINT_DAB_TIMER_STAGE_NUM];
  std::atomic<long long> dabs{0};
  std::atomic<long long> events{0};
  /** Value of #dabs when the last event ended, to tell a painting event from an empty one. */
  std::atomic<long long> dabs_at_last_event{0};

  PaintDabTimerCounters()
  {
    for (int stage = 0; stage < PAINT_DAB_TIMER_STAGE_NUM; stage++) {
      total_ns[stage].store(0, std::memory_order_relaxed);
      items[stage].store(0, std::memory_order_relaxed);
      calls[stage].store(0, std::memory_order_relaxed);
    }
  }
};

inline PaintDabTimerCounters &paint_dab_timer_counters()
{
  /* A function-local static so the counters stay alive across translation units without a
   * link-order dependent object. */
  static PaintDabTimerCounters counters;
  return counters;
}

/**
 * Print the header and one line per stage. Safe to call from any thread.
 *
 * `wall` stages are real elapsed time on the painting thread; `cpu` stages are summed over every
 * worker thread and are only comparable with each other (see #paint_dab_timer_stage_is_wall).
 * `events` counts the mouse events that produced at least one dab.
 */
inline void paint_dab_timer_report(const char *context)
{
  PaintDabTimerCounters &counters = paint_dab_timer_counters();
  const long long dabs = counters.dabs.load(std::memory_order_relaxed);
  const long long events = counters.events.load(std::memory_order_relaxed);
  const double dab_total_ms =
      double(counters.total_ns[PAINT_DAB_TIMER_DAB_TOTAL].load(std::memory_order_relaxed)) / 1e6;
  printf("[pbr-dab-timing] %s: dabs=%lld events=%lld dabs/event=%.1f | dab wall %.3f ms/dab, "
         "%.3f ms/event\n",
         context,
         dabs,
         events,
         events ? double(dabs) / double(events) : 0.0,
         dabs ? dab_total_ms / double(dabs) : 0.0,
         events ? dab_total_ms / double(events) : 0.0);
  for (int stage = 0; stage < PAINT_DAB_TIMER_STAGE_NUM; stage++) {
    const long long ns = counters.total_ns[stage].load(std::memory_order_relaxed);
    const long long call_num = counters.calls[stage].load(std::memory_order_relaxed);
    const long long item_num = counters.items[stage].load(std::memory_order_relaxed);
    if (call_num == 0) {
      continue;
    }
    printf("  %-15s %-4s %9.3f ms total | %7.4f ms/dab | %7lld calls",
           paint_dab_timer_stage_name(ePaintDabTimerStage(stage)),
           paint_dab_timer_stage_is_wall(ePaintDabTimerStage(stage)) ? "wall" : "cpu",
           double(ns) / 1e6,
           dabs ? double(ns) / 1e6 / double(dabs) : 0.0,
           call_num);
    /* A stage timed once per call with the default item count has nothing per item to report. */
    if (item_num > call_num) {
      printf(" | %8.1f ns/item | %9.0f items/dab",
             double(ns) / double(item_num),
             dabs ? double(item_num) / double(dabs) : 0.0);
    }
    printf("\n");
  }
  fflush(stdout);
}

/** Keys already described in the current stroke, see #paint_dab_timer_first_report. */
struct PaintDabTimerLogged {
  std::mutex mutex;
  std::set<std::pair<const void *, int>> keys;
};

inline PaintDabTimerLogged &paint_dab_timer_logged()
{
  static PaintDabTimerLogged logged;
  return logged;
}

/**
 * True the first time (\a key, \a sub) is seen in a stroke, so a setup description is printed once
 * per stroke and not once per dab. Safe to call from any thread.
 */
inline bool paint_dab_timer_first_report(const void *key, const int sub = 0)
{
  PaintDabTimerLogged &logged = paint_dab_timer_logged();
  std::lock_guard lock(logged.mutex);
  return logged.keys.insert({key, sub}).second;
}

/** Zero every counter, so the next stroke reports on its own. */
inline void paint_dab_timer_reset()
{
  {
    PaintDabTimerLogged &logged = paint_dab_timer_logged();
    std::lock_guard lock(logged.mutex);
    logged.keys.clear();
  }
  PaintDabTimerCounters &counters = paint_dab_timer_counters();
  for (int stage = 0; stage < PAINT_DAB_TIMER_STAGE_NUM; stage++) {
    counters.total_ns[stage].store(0, std::memory_order_relaxed);
    counters.items[stage].store(0, std::memory_order_relaxed);
    counters.calls[stage].store(0, std::memory_order_relaxed);
  }
  counters.dabs.store(0, std::memory_order_relaxed);
  counters.events.store(0, std::memory_order_relaxed);
  counters.dabs_at_last_event.store(0, std::memory_order_relaxed);
}

/** The stroke is over: print its totals if it painted anything, then start from zero. */
inline void paint_dab_timer_stroke_end()
{
  if (paint_dab_timer_counters().dabs.load(std::memory_order_relaxed) > 0) {
    paint_dab_timer_report("STROKE END");
  }
  paint_dab_timer_reset();
}

/** Time one stage over \a items work units. */
class PaintDabTimer {
  const ePaintDabTimerStage stage_;
  const long long items_;
  const std::chrono::steady_clock::time_point start_;

 public:
  explicit PaintDabTimer(const ePaintDabTimerStage stage, const long long items = 1)
      : stage_(stage), items_(items), start_(std::chrono::steady_clock::now())
  {
  }

  PaintDabTimer(const PaintDabTimer &) = delete;
  PaintDabTimer &operator=(const PaintDabTimer &) = delete;

  ~PaintDabTimer()
  {
    const long long ns = static_cast<long long>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start_)
            .count());
    PaintDabTimerCounters &counters = paint_dab_timer_counters();
    counters.total_ns[stage_].fetch_add(ns, std::memory_order_relaxed);
    counters.items[stage_].fetch_add(items_, std::memory_order_relaxed);
    counters.calls[stage_].fetch_add(1, std::memory_order_relaxed);
  }
};

/** Called once per applied dab. */
inline void paint_dab_timer_count_dab()
{
  /* Print every 200 applications: the stroke-end report carries the final numbers, so these only
   * show progress, and stdout must not become the cost being measured. */
  constexpr long long report_period = 200;
  const long long dabs = paint_dab_timer_counters().dabs.fetch_add(
                             1, std::memory_order_relaxed) +
                         1;
  if (dabs % report_period == 0) {
    paint_dab_timer_report("periodic");
  }
}

/**
 * Called at the end of every mouse event. Counts it only when it applied a dab: moves shorter than
 * the brush spacing end an event without painting and would dilute dabs/event.
 */
inline void paint_dab_timer_count_event()
{
  PaintDabTimerCounters &counters = paint_dab_timer_counters();
  const long long dabs = counters.dabs.load(std::memory_order_relaxed);
  if (dabs != counters.dabs_at_last_event.load(std::memory_order_relaxed)) {
    counters.dabs_at_last_event.store(dabs, std::memory_order_relaxed);
    counters.events.fetch_add(1, std::memory_order_relaxed);
  }
}

#else /* !DEBUG_PAINT_DAB_TIMING */

class PaintDabTimer {
 public:
  explicit PaintDabTimer(const ePaintDabTimerStage /*stage*/, const long long /*items*/ = 1)
  {
  }
  PaintDabTimer(const PaintDabTimer &) = delete;
  PaintDabTimer &operator=(const PaintDabTimer &) = delete;
};

inline void paint_dab_timer_report(const char * /*context*/) {}
inline void paint_dab_timer_reset() {}
inline bool paint_dab_timer_first_report(const void * /*key*/, const int /*sub*/ = 0)
{
  return false;
}
inline void paint_dab_timer_stroke_end() {}
inline void paint_dab_timer_count_dab() {}
inline void paint_dab_timer_count_event() {}

#endif

}  // namespace blender::ed::sculpt_paint
