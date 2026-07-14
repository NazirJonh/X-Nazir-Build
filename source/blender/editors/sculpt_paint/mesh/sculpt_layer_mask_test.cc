/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "sculpt_intern.hh"

#include "DNA_mesh_types.h"

#include "testing/testing.h"

namespace blender::ed::sculpt_paint::layers::tests {

/**
 * The block size a mask is cut at is the one decision in this feature whose failure mode is
 * *silent*: #bke::sculpt_layers::grid_masks_for_composite ignores a grid mask whose `block_size` is
 * not the grid area rather than rejecting it, so a wrongly cut mask produces no crash, no warning
 * and no log — the layer simply contributes fully while the user paints weights that do nothing.
 * That is what these tests are for; the operators that call #mask_layout_for need a live object and
 * cannot be exercised here, but the choice they delegate can.
 */
TEST(SculptLayerMaskLayout, VertexDomainUsesTheFixedVertexBlock)
{
  const MaskLayout layout = mask_layout_for(false, 1000, 0, 0);
  EXPECT_EQ(layout.totelem, 1000);
  EXPECT_EQ(layout.block_size, SCULPT_LAYER_MASK_VERT_BLOCK);
}

TEST(SculptLayerMaskLayout, GridDomainUsesOneBlockPerGrid)
{
  /* Level 4: `grid_size = (1 << 3) + 1 = 9`, so `grid_area = 81`. */
  const MaskLayout layout = mask_layout_for(true, 0, 24, 81);
  EXPECT_EQ(layout.totelem, 24 * 81);
  EXPECT_EQ(layout.block_size, 81);
  /* The property the composite actually enforces: a grid index is a block index. */
  EXPECT_EQ(layout.totelem / layout.block_size, 24);
}

TEST(SculptLayerMaskLayout, GridDomainNeverBorrowsTheVertexBlock)
{
  /* A grid area that happens to be large says nothing about which constant applies. Spelled out
   * because reaching for #SCULPT_LAYER_MASK_VERT_BLOCK on the grid path is the exact mistake the
   * fail-open behavior would hide. */
  const MaskLayout layout = mask_layout_for(true, 0, 2, 4225);
  EXPECT_NE(layout.block_size, SCULPT_LAYER_MASK_VERT_BLOCK);
  EXPECT_EQ(layout.block_size, 4225);
}

TEST(SculptLayerMaskLayout, VertexCountIsIgnoredOnTheGridDomain)
{
  /* A multires object still has a base vertex count, and it is never the mask's domain. */
  const MaskLayout layout = mask_layout_for(true, 500, 24, 81);
  EXPECT_EQ(layout.totelem, 24 * 81);
}

TEST(SculptLayerMaskLayout, EmptyDomainsRefuse)
{
  /* A zeroed layout is the refusal every caller must treat as "no mask can be made here". */
  EXPECT_EQ(mask_layout_for(false, 0, 0, 0).totelem, 0);
  EXPECT_EQ(mask_layout_for(false, -1, 0, 0).totelem, 0);
  EXPECT_EQ(mask_layout_for(true, 0, 0, 81).totelem, 0);
  EXPECT_EQ(mask_layout_for(true, 0, 24, 0).totelem, 0);
  EXPECT_EQ(mask_layout_for(true, 0, -1, 81).totelem, 0);
}

TEST(SculptLayerMaskLayout, GridDomainRefusesAnOverflowingElementCount)
{
  /* #SculptLayerMask counts its elements in an `int`, so a product that does not fit must refuse
   * rather than wrap into a mask that describes a smaller domain than the one being painted. */
  const MaskLayout layout = mask_layout_for(true, 0, 1 << 20, 1 << 12);
  EXPECT_EQ(layout.totelem, 0);
  EXPECT_EQ(layout.block_size, 0);
}

}  // namespace blender::ed::sculpt_paint::layers::tests
