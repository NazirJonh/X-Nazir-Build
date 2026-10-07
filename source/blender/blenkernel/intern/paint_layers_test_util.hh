/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Shared fixtures for the Paint Layers description tests.
 *
 * Every description test used to carry its own copies of the same helpers: create a layered
 * material, add a row with one image-backed Base Color channel, a solid image to back a row with,
 * a mask item. The copies drifted -- one enabled a channel state another forgot -- so the helpers
 * live here once, in the shape the generator, the compositor and the Outliner seam all agree on.
 *
 * These are test-only, so they sit next to the tests in `intern/` rather than in the public
 * headers: nothing outside the test suite may build fixtures through these.
 */

#pragma once

#include "BKE_gtest_base.hh"

#include "BKE_paint_layers.hh"

struct Image;
struct Main;
struct Material;
struct MaterialPaintLayer;

namespace blender::bke::test {

/**
 * The common Paint Layers fixture: one layered material with a fresh description and the helpers
 * to grow rows, folders and mask items on it.
 */
class PaintLayersTestBase : public BlenderGTestBase {
 public:
  Main *bmain = nullptr;
  Material *ma = nullptr;

  void SetUp() override;
  void TearDown() override;

  /** A generated 8x8 blank image, the smallest thing a channel record can point at. */
  Image *add_solid_image(const char *name);

  /** A Paint row backed by an image on its enabled Base Color channel, anchored at the top. */
  MaterialPaintLayer *add_paint_layer(const char *name, Image *image);

  /** A row placed into \a anchor (a folder's #children, a layer's corrections). */
  MaterialPaintLayer *add_paint_layer_into(MaterialPaintLayer *anchor,
                                           const char *name,
                                           Image *image);

  /** A folder row; its children are what carry channels. */
  MaterialPaintLayer *add_folder(const char *name);

  /** A Mask Item hanging off \a parent, enabled, its Base-Color record backed by \a image. */
  MaterialPaintLayer *add_mask_item(MaterialPaintLayer &parent, const char *name, Image *image);

  /** Age \a layer's hidden-at mark past the cold tier, so the next rebuild may drop it. */
  void age_cold_mark(MaterialPaintLayer *layer);
};

/** A `rna_set_channel_opacity` style helper is deliberately not here: tests that drive RNA belong
 * to the RNA-facing suites, not to the BKE fixtures -- see the refactor plan's 9.2. */

}  // namespace blender::bke::test
