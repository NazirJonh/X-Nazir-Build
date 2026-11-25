/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * The one table of the paint caches' built-in byte budgets.
 *
 * Every cache used to carry its own default beside it and the preference scaled each by a literal
 * sum mirrored in a third place (#BKE_paint_layers_cache_budget_bytes), so changing one constant
 * silently broke the proportion and the "8 GiB" the UI and DNA name. The table is now the single
 * source: a cache's default, the sum that scales it and the sync from the preference all read it,
 * and adding a cache is one entry rather than edits in five files.
 *
 * The Mask Source coverage cache and the bake object/generated gate are deliberately *not* here:
 * the first is an editor-side derived-image limit with its own lifetime (see
 * paint_mask_coverage_cache.hh), the second decides whether a bake happens at all and is not a
 * cache ceiling. Both stay outside the sum this table proportions.
 */

#include <cstdint>

#include "BLI_span.hh"

namespace blender::bke::paint_cache {

/* The caches' built-in defaults, moved here from the implementations so the table below is their
 * only definition. Each cache's live budget starts at its constant and the preference scales it. */
inline constexpr int64_t anchor_slice_cache_default_bytes = int64_t(4096) << 20;
inline constexpr int64_t effect_cache_default_bytes = int64_t(1024) << 20;
inline constexpr int64_t mask_effect_cache_default_bytes = int64_t(1024) << 20;
inline constexpr int64_t gpu_effect_cache_default_bytes = int64_t(512) << 20;
inline constexpr int64_t correction_cache_default_bytes = int64_t(768) << 20;
inline constexpr int64_t combined_cache_default_bytes = int64_t(512) << 20;
inline constexpr int64_t composite_cache_default_bytes = int64_t(256) << 20;

/** The built-in default of one cache, and how the preference applies a scaled budget to it. */
struct PaintCacheBudgetEntry {
  /** Stable name the default is looked up by (#BKE_paint_layers_cache_budget_default_bytes). */
  const char *id;
  /** The cache's built-in ceiling, in bytes; the sum of these is the proportion's denominator. */
  int64_t default_bytes;
  /**
   * Applies a scaled byte budget to the cache, or null for a cache that reads the preference
   * itself when it evicts (the stroke corrections).
   */
  void (*apply)(int64_t bytes);
};

/** The paint caches' defaults. Function-local static in the definition, so no static init order. */
Span<const PaintCacheBudgetEntry> paint_cache_budget_entries();

/** The sum of every entry's #PaintCacheBudgetEntry::default_bytes, in bytes. */
int64_t paint_cache_budget_total_default_bytes();

/**
 * The one switch of the `PAINT_MEM` diagnostics: true when `BLENDER_PAINT_MEM_LOG` is set to
 * anything non-empty, read once so a run cannot toggle it mid-stroke. The probes print to stderr
 * (Blender owns stdout) and are skipped entirely with no value set.
 */
bool paint_mem_log_enabled();

}  // namespace blender::bke::paint_cache
