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

#include "BKE_gtest_base.hh"

#include "BKE_paint_layers.hh"

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

}  // namespace blender::bke::tests
