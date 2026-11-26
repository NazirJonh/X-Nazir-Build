/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "paint_layers_cache_budgets.hh"

#include <cstdio>
#include <cstdlib>

#include "BLI_utildefines.h"

#include "BKE_paint_layers_composite.hh"

#include "../paint_material_cache_common.hh"
#include "paint_layers_intern.hh"

namespace blender::bke::paint_cache {

namespace {

/* Adapters: the cache setters take `size_t`, the table carries the sign-explicit `int64_t` so the
 * sum and the scaling arithmetic cannot wrap. */
void apply_anchor_slices(const int64_t bytes)
{
  paint_layer_anchor_point_slice_cache_set_budget(size_t(bytes));
}

void apply_effects(const int64_t bytes)
{
  paint_layer_effect_cache_set_budget(size_t(bytes));
}

void apply_mask_effects(const int64_t bytes)
{
  paint_layer_mask_effect_cache_set_budget(size_t(bytes));
}

void apply_gpu_effects(const int64_t bytes)
{
  paint_layer_effect_gpu_cache_set_budget(size_t(bytes));
}

void apply_combined(const int64_t bytes)
{
  paint_material_cache::combined_cache_set_budget(bytes);
}

void apply_composite(const int64_t bytes)
{
  paint_material_cache::composite_cache_set_budget(bytes);
}

}  // namespace

Span<const PaintCacheBudgetEntry> paint_cache_budget_entries()
{
  /* Function-local rather than a file-scope static: a full-scope object is built before `main` and
   * destroyed after the leak detector has run, which reports its storage as a leak on every
   * exit. */
  static const PaintCacheBudgetEntry entries[] = {
      {"anchor_slices", anchor_slice_cache_default_bytes, apply_anchor_slices},
      {"effect_results", effect_cache_default_bytes, apply_effects},
      {"mask_effects", mask_effect_cache_default_bytes, apply_mask_effects},
      {"gpu_effect_results", gpu_effect_cache_default_bytes, apply_gpu_effects},
      /* The correction cache reads the preference itself when it evicts, so it has no apply hook;
       * its default is here for the sum alone (see paint_material_correction_cache.cc). */
      {"stroke_corrections", correction_cache_default_bytes, nullptr},
      {"combined_preview", combined_cache_default_bytes, apply_combined},
      {"composite", composite_cache_default_bytes, apply_composite},
  };
  return Span<const PaintCacheBudgetEntry>(entries, int64_t(ARRAY_SIZE(entries)));
}

int64_t paint_cache_budget_total_default_bytes()
{
  int64_t total = 0;
  for (const PaintCacheBudgetEntry &entry : paint_cache_budget_entries()) {
    total += entry.default_bytes;
  }
  return total;
}

bool paint_mem_log_enabled()
{
  /* Function-local static: the probes sit on stroke paths, and the switch cannot change under a
   * running Blender anyway. */
  static const bool enabled = []() {
    const char *value = getenv("BLENDER_PAINT_MEM_LOG");
    return value != nullptr && value[0] != '\0';
  }();
  return enabled;
}

}  // namespace blender::bke::paint_cache

namespace blender {

namespace {

struct PaintCacheDebugRow {
  const char *id;
  size_t used_bytes;
  size_t budget_bytes;
};

}  // namespace

bool BKE_paint_mem_log_enabled()
{
  return bke::paint_cache::paint_mem_log_enabled();
}

void BKE_paint_layers_cache_debug_log(const char *where)
{
  if (!bke::paint_cache::paint_mem_log_enabled()) {
    return;
  }
  const PaintCacheDebugRow rows[] = {
      {"anchor_slices",
       paint_layer_anchor_point_slice_cache_bytes(),
       paint_layer_anchor_point_slice_cache_budget()},
      {"effect_results", paint_layer_effect_cache_bytes(), paint_layer_effect_cache_budget()},
      {"mask_effects",
       paint_layer_mask_effect_cache_bytes(),
       paint_layer_mask_effect_cache_budget()},
      {"gpu_effect_results",
       paint_layer_effect_gpu_cache_bytes(),
       paint_layer_effect_gpu_cache_budget()},
      {"combined_preview",
       size_t(bke::paint_material_cache::combined_cache_bytes()),
       size_t(bke::paint_material_cache::combined_cache_get_budget())},
      {"composite",
       size_t(bke::paint_material_cache::composite_cache_bytes()),
       size_t(bke::paint_material_cache::composite_cache_get_budget())},
  };
  int64_t total_used = 0;
  int64_t total_budget = 0;
  for (const PaintCacheDebugRow &row : rows) {
    fprintf(stderr,
            "PAINT_MEM caches where=%s id=%s used_bytes=%lld budget_bytes=%lld\n",
            where,
            row.id,
            (long long)row.used_bytes,
            (long long)row.budget_bytes);
    total_used += int64_t(row.used_bytes);
    total_budget += int64_t(row.budget_bytes);
  }
  fprintf(stderr,
          "PAINT_MEM caches_total where=%s used_bytes=%lld budget_bytes=%lld\n",
          where,
          (long long)total_used,
          (long long)total_budget);
}

int64_t BKE_paint_layers_cache_budget_default_bytes(const char *id)
{
  for (const bke::paint_cache::PaintCacheBudgetEntry &entry :
       bke::paint_cache::paint_cache_budget_entries())
  {
    if (STREQ(entry.id, id)) {
      return entry.default_bytes;
    }
  }
  return 0;
}

}  // namespace blender
