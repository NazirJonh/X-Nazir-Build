/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "BLI_listbase.h"
#include "BLI_uuid.h"

#include "DNA_material_types.h"

#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_composite.hh"

#include "../paint_layers_intern.hh"
#include "../paint_layers_kind.hh"

#include "paint_layers_test_util.hh"

namespace blender::bke::tests {

class PaintLayersAnchorPointTest : public PaintLayersTestBase {
 protected:
  MaterialPaintLayer *add_point(MaterialPaintLayer &row,
                                const PaintLayerAnchorPointList list = PaintLayerAnchorPointList::Effects)
  {
    return BKE_paint_layers_anchor_point_add(
        *ma, &row, list, nullptr, false, "Anchor Point", {});
  }

  MaterialPaintLayer *add_reader(MaterialPaintLayer &row,
                                 const PaintLayerAnchorPointList list,
                                 const bUUID &source)
  {
    return BKE_paint_layers_anchor_point_add(
        *ma, &row, list, nullptr, true, "Anchor Point Reader", source);
  }
};

TEST_F(PaintLayersAnchorPointTest, anchor_point_kinds_registered)
{
  const PaintLayerKindInfo *marker = paint_layer_kind_find(MA_PAINT_LAYER_SOURCE_ANCHOR_POINT,
                                                           MA_PAINT_LAYER_ANCHOR_POINT_MARKER);
  const PaintLayerKindInfo *reader = paint_layer_kind_find(MA_PAINT_LAYER_SOURCE_ANCHOR_POINT,
                                                           MA_PAINT_LAYER_ANCHOR_POINT_READER);
  ASSERT_NE(marker, nullptr);
  ASSERT_NE(reader, nullptr);
  EXPECT_EQ(marker->flags & PAINT_LAYER_KIND_ANCHOR_POINT_MARK, PAINT_LAYER_KIND_ANCHOR_POINT_MARK);
  EXPECT_EQ(marker->flags & PAINT_LAYER_KIND_READS_ANCHOR_POINT, 0);
  EXPECT_EQ(reader->flags & PAINT_LAYER_KIND_READS_ANCHOR_POINT,
            PAINT_LAYER_KIND_READS_ANCHOR_POINT);
  EXPECT_EQ(reader->flags & PAINT_LAYER_KIND_READS_SOURCE, 0);
  EXPECT_EQ(marker->allowed_roles,
            uint8_t(paint_layer_role_bit(MA_PAINT_LAYER_ROLE_EFFECT) |
                    paint_layer_role_bit(MA_PAINT_LAYER_ROLE_MASK_ITEM)));
  EXPECT_EQ(reader->allowed_roles,
            uint8_t(paint_layer_role_bit(MA_PAINT_LAYER_ROLE_LAYER) |
                    paint_layer_role_bit(MA_PAINT_LAYER_ROLE_EFFECT) |
                    paint_layer_role_bit(MA_PAINT_LAYER_ROLE_MASK_ITEM)));
}

TEST_F(PaintLayersAnchorPointTest, object_mask_rejected_on_effect_and_mask_item)
{
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "Row", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  EXPECT_TRUE(BKE_paint_layers_object_mask_role_allowed(*row));
  MaterialPaintLayer *effect = BKE_paint_layers_correction_add(*ma, row, "Effect");
  ASSERT_NE(effect, nullptr);
  EXPECT_FALSE(BKE_paint_layers_object_mask_role_allowed(*effect));
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, row, 1.0f), nullptr);
  MaterialPaintLayer *item = BKE_paint_layers_mask_item_add(
      *ma, row, MA_PAINT_LAYER_SOURCE_CONSTANT, "Mask");
  ASSERT_NE(item, nullptr);
  EXPECT_FALSE(BKE_paint_layers_object_mask_role_allowed(*item));
}

TEST_F(PaintLayersAnchorPointTest, resolve_not_found)
{
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "Row", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *reader = add_reader(*row, PaintLayerAnchorPointList::Effects, {});
  ASSERT_NE(reader, nullptr);
  const PaintLayerAnchorPointRef ref = paint_layer_anchor_point_resolve(*ma, *reader);
  EXPECT_EQ(ref.issue, PaintLayerAnchorPointIssue::NotFound);
  EXPECT_EQ(ref.target, PaintLayerAnchorPointTarget::None);
}

TEST_F(PaintLayersAnchorPointTest, resolve_point_below_reader_ok)
{
  MaterialPaintLayer *source = BKE_paint_layers_add(*ma, "Source", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *point = add_point(*source);
  ASSERT_NE(point, nullptr);
  MaterialPaintLayer *reader_row = BKE_paint_layers_add(
      *ma, "Reader", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *reader = add_reader(
      *reader_row, PaintLayerAnchorPointList::Row, point->marker);
  ASSERT_NE(reader, nullptr);
  const PaintLayerAnchorPointRef ref = paint_layer_anchor_point_resolve(*ma, *reader);
  EXPECT_EQ(ref.issue, PaintLayerAnchorPointIssue::None);
  EXPECT_EQ(ref.target, PaintLayerAnchorPointTarget::Content);
  EXPECT_EQ(ref.source_row, source);
  EXPECT_LT(ref.rank, paint_layer_anchor_point_rank(*ma, reader->marker));
}

TEST_F(PaintLayersAnchorPointTest, resolve_point_above_reader_forward)
{
  MaterialPaintLayer *reader_row = BKE_paint_layers_add(
      *ma, "Reader", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *reader = add_reader(
      *reader_row, PaintLayerAnchorPointList::Row, {});
  MaterialPaintLayer *source = BKE_paint_layers_add(*ma, "Source", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *point = add_point(*source);
  ASSERT_NE(point, nullptr);
  ASSERT_NE(reader, nullptr);
  reader->anchor_point_source = point->marker;
  const PaintLayerAnchorPointRef ref = paint_layer_anchor_point_resolve(*ma, *reader);
  EXPECT_EQ(ref.issue, PaintLayerAnchorPointIssue::Forward);
}

TEST_F(PaintLayersAnchorPointTest, resolve_parent_folder_point_from_child_forward)
{
  MaterialPaintLayer *folder = BKE_paint_layers_add(*ma, "Folder", nullptr, PaintLayerPlace::Above);
  folder->source = MA_PAINT_LAYER_SOURCE_STACK;
  MaterialPaintLayer *point = add_point(*folder);
  ASSERT_NE(point, nullptr);
  MaterialPaintLayer *child = BKE_paint_layers_add(*ma, "Child", folder, PaintLayerPlace::Into);
  MaterialPaintLayer *reader = add_reader(
      *child, PaintLayerAnchorPointList::Effects, point->marker);
  ASSERT_NE(reader, nullptr);
  const PaintLayerAnchorPointRef ref = paint_layer_anchor_point_resolve(*ma, *reader);
  EXPECT_EQ(ref.issue, PaintLayerAnchorPointIssue::Forward);
}

TEST_F(PaintLayersAnchorPointTest, resolve_effect_reads_same_row_mask_point_order_in_row)
{
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "Row", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, row, 1.0f), nullptr);
  MaterialPaintLayer *point = add_point(*row, PaintLayerAnchorPointList::MaskStack);
  ASSERT_NE(point, nullptr);
  MaterialPaintLayer *reader = add_reader(
      *row, PaintLayerAnchorPointList::Effects, point->marker);
  ASSERT_NE(reader, nullptr);
  const PaintLayerAnchorPointRef ref = paint_layer_anchor_point_resolve(*ma, *reader);
  EXPECT_EQ(ref.issue, PaintLayerAnchorPointIssue::OrderInRow);
}

TEST_F(PaintLayersAnchorPointTest, resolve_mask_item_reads_same_row_content_point_ok)
{
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "Row", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *point = add_point(*row);
  ASSERT_NE(point, nullptr);
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, row, 1.0f), nullptr);
  MaterialPaintLayer *reader = add_reader(
      *row, PaintLayerAnchorPointList::MaskStack, point->marker);
  ASSERT_NE(reader, nullptr);
  const PaintLayerAnchorPointRef ref = paint_layer_anchor_point_resolve(*ma, *reader);
  EXPECT_EQ(ref.issue, PaintLayerAnchorPointIssue::None);
  EXPECT_EQ(ref.target, PaintLayerAnchorPointTarget::Content);
}

TEST_F(PaintLayersAnchorPointTest, resolve_returns_typed_target_by_list)
{
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "Row", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *content_point = add_point(*row);
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, row, 1.0f), nullptr);
  MaterialPaintLayer *mask_point = add_point(*row, PaintLayerAnchorPointList::MaskStack);
  ASSERT_NE(content_point, nullptr);
  ASSERT_NE(mask_point, nullptr);

  MaterialPaintLayer *content_reader = add_reader(*row,
                                                  PaintLayerAnchorPointList::Effects,
                                                  content_point->marker);
  MaterialPaintLayer *mask_reader = add_reader(*row,
                                               PaintLayerAnchorPointList::MaskStack,
                                               mask_point->marker);
  ASSERT_NE(content_reader, nullptr);
  ASSERT_NE(mask_reader, nullptr);
  EXPECT_EQ(paint_layer_anchor_point_resolve(*ma, *content_reader).target,
            PaintLayerAnchorPointTarget::Content);
  EXPECT_EQ(paint_layer_anchor_point_resolve(*ma, *mask_reader).target,
            PaintLayerAnchorPointTarget::Mask);
}

TEST_F(PaintLayersAnchorPointTest, marker_does_not_change_row_participation)
{
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "Row", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  const bool before = paint_layer_row_participates(*ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(add_point(*row), nullptr);
  const bool after = paint_layer_row_participates(*ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  EXPECT_EQ(before, after);
}

TEST_F(PaintLayersAnchorPointTest, rank_matches_generator_order_with_pass_through)
{
  MaterialPaintLayer *bottom = BKE_paint_layers_add(*ma, "Bottom", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *folder = BKE_paint_layers_add(*ma, "Folder", nullptr, PaintLayerPlace::Above);
  folder->source = MA_PAINT_LAYER_SOURCE_STACK;
  MaterialPaintLayer *child = BKE_paint_layers_add(*ma, "Child", folder, PaintLayerPlace::Into);
  MaterialPaintLayer *child_point = add_point(*child);
  MaterialPaintLayer *top = BKE_paint_layers_add(*ma, "Top", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *top_point = add_point(*top);
  ASSERT_NE(child_point, nullptr);
  ASSERT_NE(top_point, nullptr);
  EXPECT_LT(paint_layer_anchor_point_rank(*ma, bottom->marker),
            paint_layer_anchor_point_rank(*ma, child_point->marker));
  EXPECT_LT(paint_layer_anchor_point_rank(*ma, child_point->marker),
            paint_layer_anchor_point_rank(*ma, folder->marker));
  EXPECT_LT(paint_layer_anchor_point_rank(*ma, folder->marker),
            paint_layer_anchor_point_rank(*ma, top_point->marker));
}

TEST_F(PaintLayersAnchorPointTest, add_point_in_effects_at_position)
{
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "Row", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *first = BKE_paint_layers_correction_add(*ma, row, "First");
  MaterialPaintLayer *point = BKE_paint_layers_anchor_point_add(
      *ma, row, PaintLayerAnchorPointList::Effects, first, false, "Anchor Point", {});
  ASSERT_NE(point, nullptr);
  EXPECT_EQ(point->role, MA_PAINT_LAYER_ROLE_EFFECT);
  EXPECT_EQ(BLI_findindex(&row->effects, point), 1);
  EXPECT_EQ(row->effects.first, first);
}

TEST_F(PaintLayersAnchorPointTest, add_point_in_mask_stack_at_position)
{
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "Row", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, row, 1.0f), nullptr);
  MaterialPaintLayer *point = BKE_paint_layers_anchor_point_add(
      *ma, row, PaintLayerAnchorPointList::MaskStack, nullptr, false, "Anchor Point", {});
  ASSERT_NE(point, nullptr);
  EXPECT_EQ(point->role, MA_PAINT_LAYER_ROLE_MASK_ITEM);
  EXPECT_EQ(BLI_findindex(&row->mask_stack, point), 1);
}

TEST_F(PaintLayersAnchorPointTest, duplicate_branch_remaps_internal_reference)
{
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "Row", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *point = add_point(*row);
  MaterialPaintLayer *reader = add_reader(
      *row, PaintLayerAnchorPointList::Effects, point->marker);
  ASSERT_NE(reader, nullptr);
  MaterialPaintLayer *copy = BKE_paint_layers_duplicate(*bmain, *ma, row);
  ASSERT_NE(copy, nullptr);
  ASSERT_NE(copy->effects.first, nullptr);
  const MaterialPaintLayer *copy_point = copy->effects.first;
  const MaterialPaintLayer *copy_reader = copy->effects.last;
  ASSERT_TRUE(BLI_uuid_equal(copy_reader->anchor_point_source, copy_point->marker));
  EXPECT_FALSE(BLI_uuid_equal(copy_reader->anchor_point_source, point->marker));
}

TEST_F(PaintLayersAnchorPointTest, duplicate_branch_keeps_external_reference)
{
  MaterialPaintLayer *source = BKE_paint_layers_add(*ma, "Source", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *point = add_point(*source);
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "Row", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *reader = add_reader(
      *row, PaintLayerAnchorPointList::Effects, point->marker);
  ASSERT_NE(reader, nullptr);
  MaterialPaintLayer *copy = BKE_paint_layers_duplicate(*bmain, *ma, row);
  ASSERT_NE(copy, nullptr);
  ASSERT_NE(copy->effects.first, nullptr);
  EXPECT_TRUE(BLI_uuid_equal(copy->effects.first->anchor_point_source, point->marker));
}

}  // namespace blender::bke::tests
