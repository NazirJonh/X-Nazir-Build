/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Shared fixtures for the Paint Layers tests: create a layered material, add a row with one
 * image-backed Base Color channel, a solid image to back a row with.
 *
 * These are test-only, so they sit next to the tests in `intern/paint_layers/tests/` rather than in the public
 * headers: nothing outside the test suite may build fixtures through these.
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "BKE_gtest_base.hh"

#include "BKE_paint_layers.hh"

#include "../../paint_material_cache_common.hh"
#include "../paint_layers_intern.hh"

namespace blender {

struct Image;
struct Main;
struct Material;
struct MaterialPaintLayer;

}  // namespace blender

namespace blender::bke::tests {

/** The common Paint Layers fixture: one layered material with a fresh description. */
class PaintLayersTestBase : public BlenderGTestBase {
 public:
  Main *bmain = nullptr;
  Material *ma = nullptr;
  /** The slice-cache budget the test found, restored on teardown. */
  size_t slice_cache_budget_before_ = 0;

  void SetUp() override;
  void TearDown() override;

  /** A generated 8x8 blank image, the smallest thing a channel record can point at. */
  Image *add_solid_image(const char *name);

  /** A Paint row backed by an image on its enabled Base Color channel, added on top. */
  MaterialPaintLayer *add_paint_layer(const char *name, Image *image);
};

/**
 * Sets the slice cache budget for one test and restores the previous one on scope exit, so a test
 * that makes it small cannot change what a later test measures.
 */
class AnchorSliceCacheBudgetGuard {
 public:
  explicit AnchorSliceCacheBudgetGuard(size_t bytes);
  ~AnchorSliceCacheBudgetGuard();
  AnchorSliceCacheBudgetGuard(const AnchorSliceCacheBudgetGuard &) = delete;
  AnchorSliceCacheBudgetGuard &operator=(const AnchorSliceCacheBudgetGuard &) = delete;

 private:
  size_t previous_;
};

/**
 * Snapshot every paint cache budget and restore them on scope exit, so a test that changes one
 * cannot leak into the next test that measures eviction. The getters return the configured floor,
 * not an entry's transient overshoot, which is what a restore needs.
 */
class ScopedPaintCacheBudgets {
 public:
  ScopedPaintCacheBudgets()
      : anchor_(paint_layer_anchor_point_slice_cache_budget()),
        effect_(paint_layer_effect_cache_budget()),
        mask_effect_(paint_layer_mask_effect_cache_budget()),
        gpu_effect_(paint_layer_effect_gpu_cache_budget()),
        combined_(paint_material_cache::combined_cache_get_budget()),
        composite_(paint_material_cache::composite_cache_get_budget())
  {
  }
  ~ScopedPaintCacheBudgets()
  {
    paint_layer_anchor_point_slice_cache_set_budget(anchor_);
    paint_layer_effect_cache_set_budget(effect_);
    paint_layer_mask_effect_cache_set_budget(mask_effect_);
    paint_layer_effect_gpu_cache_set_budget(gpu_effect_);
    paint_material_cache::combined_cache_set_budget(combined_);
    paint_material_cache::composite_cache_set_budget(composite_);
  }
  ScopedPaintCacheBudgets(const ScopedPaintCacheBudgets &) = delete;
  ScopedPaintCacheBudgets &operator=(const ScopedPaintCacheBudgets &) = delete;

 private:
  size_t anchor_;
  size_t effect_;
  size_t mask_effect_;
  size_t gpu_effect_;
  int64_t combined_;
  int64_t composite_;
};

}  // namespace blender::bke::tests
