/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "BLI_listbase.h"
#include "BLI_uuid.h"

#include "DNA_image_types.h"
#include "DNA_material_types.h"

#include "BKE_image.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_composite.hh"
#include "BKE_paint_layers_edit.hh"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "../paint_layers_generate_intern.hh"
#include "../paint_layers_intern.hh"
#include "../paint_layers_kind.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

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

TEST_F(PaintLayersAnchorPointTest, object_mask_rejected_on_effect_via_rna_setter)
{
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "Row", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  MaterialPaintLayer *effect = BKE_paint_layers_correction_add(*ma, row, "Effect");
  ASSERT_NE(effect, nullptr);
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, row, 1.0f), nullptr);
  MaterialPaintLayer *item = BKE_paint_layers_mask_item_add(
      *ma, row, MA_PAINT_LAYER_SOURCE_CONSTANT, "Mask");
  ASSERT_NE(item, nullptr);

  /* The positive control: a stack row takes the value through the real RNA path. */
  PointerRNA row_ptr = RNA_pointer_create_discrete(&ma->id, RNA_MaterialPaintLayer, row);
  PropertyRNA *row_type = RNA_struct_find_property(&row_ptr, "object_mask_type");
  ASSERT_NE(row_type, nullptr);
  RNA_property_enum_set(&row_ptr, row_type, MA_PAINT_LAYER_OBJECT_MASK_UV_TILES);
  EXPECT_EQ(row->object_mask_type, MA_PAINT_LAYER_OBJECT_MASK_UV_TILES);

  const auto reject = [&](MaterialPaintLayer &owned) {
    PointerRNA ptr = RNA_pointer_create_discrete(&ma->id, RNA_MaterialPaintLayer, &owned);
    PropertyRNA *prop = RNA_struct_find_property(&ptr, "object_mask_type");
    ASSERT_NE(prop, nullptr);
    /* The refused write itself: asking for UV_TILES through RNA must leave a clean MESH row
     * untouched, which is distinguishable now that MESH is the zero value. */
    ASSERT_EQ(owned.object_mask_type, int8_t(MA_PAINT_LAYER_OBJECT_MASK_MESH));
    RNA_property_enum_set(&ptr, prop, MA_PAINT_LAYER_OBJECT_MASK_UV_TILES);
    EXPECT_EQ(owned.object_mask_type, int8_t(MA_PAINT_LAYER_OBJECT_MASK_MESH));
    EXPECT_EQ(owned.object_mask_flag & MA_PAINT_LAYER_OBJECT_MASK_ACTIVE, 0);

    /* A value that reached DNA by another path is dropped too, and the reset reports that it
     * changed the row so the graph is rebuilt. */
    owned.object_mask_type = MA_PAINT_LAYER_OBJECT_MASK_UV_TILES;
    owned.object_mask_flag |= MA_PAINT_LAYER_OBJECT_MASK_ACTIVE;
    EXPECT_TRUE(BKE_paint_layers_object_mask_type_set(*ma, owned, MA_PAINT_LAYER_OBJECT_MASK_MESH))
        << "the reset changed the row";
    EXPECT_EQ(owned.object_mask_type, int8_t(MA_PAINT_LAYER_OBJECT_MASK_MESH));
    EXPECT_EQ(owned.object_mask_flag & MA_PAINT_LAYER_OBJECT_MASK_ACTIVE, 0);
    EXPECT_FALSE(BKE_paint_layers_object_mask_type_set(*ma, owned, MA_PAINT_LAYER_OBJECT_MASK_MESH))
        << "a clean row changes nothing";
  };
  reject(*effect);
  reject(*item);
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
  /* Written directly on purpose: the public setter rejects a forward reference, which is exactly
   * the state this test needs to resolve. */
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
  ASSERT_TRUE(BKE_paint_layers_folder_is_pass_through(*folder))
      << "the rank rule is checked on the folder the generator really flattens";
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

TEST_F(PaintLayersAnchorPointTest, add_point_on_layer_appends_to_effects)
{
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "Row", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(BKE_paint_layers_correction_add(*ma, row, "First"), nullptr);
  PaintLayerAddParams params;
  params.kind = PaintLayerAddKind::AnchorPoint;
  params.anchor = row;
  params.anchor_point_list = PaintLayerAnchorPointList::Effects;
  MaterialPaintLayer *point = BKE_paint_layers_add_with_policy(*ma, params);
  ASSERT_NE(point, nullptr);
  EXPECT_EQ(point->role, MA_PAINT_LAYER_ROLE_EFFECT);
  EXPECT_EQ(BLI_findindex(&row->effects, point), 2);
  EXPECT_EQ(row->effects.last, point);
}

TEST_F(PaintLayersAnchorPointTest, add_point_after_correction_inserts_after_it)
{
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "Row", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *first = BKE_paint_layers_correction_add(*ma, row, "First");
  MaterialPaintLayer *second = BKE_paint_layers_correction_add(*ma, row, "Second");
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  PaintLayerAddParams params;
  params.kind = PaintLayerAddKind::AnchorPoint;
  params.anchor = first;
  params.anchor_point_list = PaintLayerAnchorPointList::Effects;
  params.insert_after = first;
  MaterialPaintLayer *point = BKE_paint_layers_add_with_policy(*ma, params);
  ASSERT_NE(point, nullptr);
  EXPECT_EQ(BLI_findindex(&row->effects, point), 1);
  EXPECT_EQ(row->effects.first, first);
  EXPECT_EQ(row->effects.last, second);
}

TEST_F(PaintLayersAnchorPointTest, add_point_after_mask_item_goes_to_mask_stack)
{
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "Row", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, row, 1.0f), nullptr);
  MaterialPaintLayer *item = BKE_paint_layers_mask_item_add(
      *ma, row, MA_PAINT_LAYER_SOURCE_CONSTANT, "Mask");
  ASSERT_NE(item, nullptr);
  PaintLayerAddParams params;
  params.kind = PaintLayerAddKind::AnchorPoint;
  params.anchor = item;
  params.anchor_point_list = PaintLayerAnchorPointList::MaskStack;
  params.insert_after = item;
  MaterialPaintLayer *point = BKE_paint_layers_add_with_policy(*ma, params);
  ASSERT_NE(point, nullptr);
  EXPECT_EQ(point->role, MA_PAINT_LAYER_ROLE_MASK_ITEM);
  EXPECT_EQ(BLI_findindex(&row->mask_stack, point), BLI_findindex(&row->mask_stack, item) + 1);
}

TEST_F(PaintLayersAnchorPointTest, add_point_with_foreign_insert_after_is_normalized)
{
  MaterialPaintLayer *row_a = BKE_paint_layers_add(*ma, "A", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *row_b = BKE_paint_layers_add(*ma, "B", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *foreign = BKE_paint_layers_correction_add(*ma, row_b, "Foreign");
  ASSERT_NE(foreign, nullptr);
  PaintLayerAddParams params;
  params.kind = PaintLayerAddKind::AnchorPoint;
  params.anchor = row_a;
  params.anchor_point_list = PaintLayerAnchorPointList::Effects;
  params.insert_after = foreign;
  MaterialPaintLayer *point = BKE_paint_layers_add_with_policy(*ma, params);
  ASSERT_NE(point, nullptr) << "a stale position must not refuse the Add";
  EXPECT_EQ(row_a->effects.last, point);
  EXPECT_TRUE(row_b->effects.first == foreign && row_b->effects.last == foreign);
}

TEST_F(PaintLayersAnchorPointTest, add_reader_correction_on_layer_creates_effect_role)
{
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "Row", nullptr, PaintLayerPlace::Above);
  PaintLayerAddParams params;
  params.kind = PaintLayerAddKind::AnchorPointCorrection;
  params.anchor = row;
  MaterialPaintLayer *reader = BKE_paint_layers_add_with_policy(*ma, params);
  ASSERT_NE(reader, nullptr);
  EXPECT_EQ(reader->role, MA_PAINT_LAYER_ROLE_EFFECT);
  EXPECT_EQ(reader->source, MA_PAINT_LAYER_SOURCE_ANCHOR_POINT);
  EXPECT_EQ(reader->kind_subtype, MA_PAINT_LAYER_ANCHOR_POINT_READER);
  EXPECT_EQ(row->effects.last, reader);
}

TEST_F(PaintLayersAnchorPointTest, add_reader_mask_correction_on_layer_creates_mask_item_role)
{
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "Row", nullptr, PaintLayerPlace::Above);
  PaintLayerAddParams params;
  params.kind = PaintLayerAddKind::AnchorPointMaskCorrection;
  params.anchor = row;
  MaterialPaintLayer *reader = BKE_paint_layers_add_with_policy(*ma, params);
  ASSERT_NE(reader, nullptr);
  EXPECT_EQ(reader->role, MA_PAINT_LAYER_ROLE_MASK_ITEM);
  EXPECT_EQ(reader->source, MA_PAINT_LAYER_SOURCE_ANCHOR_POINT);
  EXPECT_EQ(reader->kind_subtype, MA_PAINT_LAYER_ANCHOR_POINT_READER);
  ASSERT_NE(BKE_paint_layers_mask_base(*row), nullptr);
  EXPECT_EQ(row->mask_stack.last, reader);
}

TEST_F(PaintLayersAnchorPointTest, add_reader_layer_creates_stack_row)
{
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "Row", nullptr, PaintLayerPlace::Above);
  PaintLayerAddParams params;
  params.kind = PaintLayerAddKind::AnchorPointLayer;
  params.anchor = row;
  MaterialPaintLayer *reader = BKE_paint_layers_add_with_policy(*ma, params);
  ASSERT_NE(reader, nullptr);
  EXPECT_EQ(reader->role, MA_PAINT_LAYER_ROLE_LAYER);
  EXPECT_EQ(reader->source, MA_PAINT_LAYER_SOURCE_ANCHOR_POINT);
  EXPECT_EQ(reader->kind_subtype, MA_PAINT_LAYER_ANCHOR_POINT_READER);
  EXPECT_NE(BLI_findindex(&ma->paint_layers, reader), -1);
}

TEST_F(PaintLayersAnchorPointTest, add_point_without_layer_is_refused)
{
  PaintLayerAddParams params;
  params.kind = PaintLayerAddKind::AnchorPoint;
  params.anchor = nullptr;
  params.anchor_point_list = PaintLayerAnchorPointList::Effects;
  EXPECT_EQ(BKE_paint_layers_add_with_policy(*ma, params), nullptr);
}

/* One level of a nested reader chain: a reader in a new row's effects, then a marker above it, so
 * the marker's content slice contains that reader. */
static MaterialPaintLayer *add_chain_level(Material &ma,
                                           MaterialPaintLayer &lower_point,
                                           MaterialPaintLayer **r_reader)
{
  MaterialPaintLayer *row = BKE_paint_layers_add(ma, "Chain", nullptr, PaintLayerPlace::Above);
  *r_reader = BKE_paint_layers_anchor_point_add(
      ma, row, PaintLayerAnchorPointList::Effects, nullptr, true, "Reader", lower_point.marker);
  if (*r_reader == nullptr) {
    return nullptr;
  }
  return BKE_paint_layers_anchor_point_add(
      ma, row, PaintLayerAnchorPointList::Effects, nullptr, false, "Point", {});
}

TEST_F(PaintLayersAnchorPointTest, depth_three_is_ok_and_live_in_auto)
{
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "Row", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *point = add_point(*row);
  ASSERT_NE(point, nullptr);
  MaterialPaintLayer *reader = nullptr;
  for (int level = 0; level < 3; level++) {
    point = add_chain_level(*ma, *point, &reader);
    ASSERT_NE(point, nullptr);
    ASSERT_NE(reader, nullptr);
  }
  EXPECT_EQ(paint_layer_anchor_point_chain_depth(*ma, *reader), 3);
  EXPECT_EQ(paint_layer_anchor_point_resolve(*ma, *reader).issue,
            PaintLayerAnchorPointIssue::None);
  EXPECT_FALSE(paint_layer_anchor_point_reader_baked(*ma, *reader));
}

TEST_F(PaintLayersAnchorPointTest, depth_four_auto_baked)
{
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "Row", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *point = add_point(*row);
  ASSERT_NE(point, nullptr);
  MaterialPaintLayer *reader = nullptr;
  for (int level = 0; level < 4; level++) {
    point = add_chain_level(*ma, *point, &reader);
    ASSERT_NE(point, nullptr);
    ASSERT_NE(reader, nullptr);
  }
  EXPECT_EQ(paint_layer_anchor_point_chain_depth(*ma, *reader), 4);
  EXPECT_EQ(paint_layer_anchor_point_resolve(*ma, *reader).issue,
            PaintLayerAnchorPointIssue::None);
  EXPECT_TRUE(paint_layer_anchor_point_reader_baked(*ma, *reader));
}

TEST_F(PaintLayersAnchorPointTest, depth_four_never_issue_limit)
{
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "Row", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *point = add_point(*row);
  MaterialPaintLayer *reader = nullptr;
  for (int level = 0; level < 4; level++) {
    point = add_chain_level(*ma, *point, &reader);
  }
  ASSERT_NE(reader, nullptr);
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *reader, MA_PAINT_LAYER_BAKE_NEVER));
  EXPECT_FALSE(paint_layer_anchor_point_reader_baked(*ma, *reader));
  Vector<PaintLayersIssue> issues;
  BKE_paint_layers_issues_get(*ma, issues);
  bool limit = false;
  for (const PaintLayersIssue &issue : issues) {
    limit |= issue.text != nullptr &&
             STREQ(issue.text, paint_layer_anchor_point_issue_text(
                                   PaintLayerAnchorPointIssue::LimitExceeded));
  }
  EXPECT_TRUE(limit);
}

TEST_F(PaintLayersAnchorPointTest, limit_exceeded_never_warns)
{
  MaterialPaintLayer *reader = nullptr;
  MaterialPaintLayer *first_reader = nullptr;
  for (int i = 0; i < PAINT_LAYER_ANCHOR_POINT_LIMIT + 1; i++) {
    /* A separate row per pair keeps every chain at depth 1, so only the element limit is tested. */
    MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "Row", nullptr, PaintLayerPlace::Above);
    MaterialPaintLayer *point = add_point(*row);
    ASSERT_NE(point, nullptr);
    reader = add_reader(*row, PaintLayerAnchorPointList::Effects, point->marker);
    ASSERT_NE(reader, nullptr);
    if (first_reader == nullptr) {
      first_reader = reader;
    }
  }
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *reader, MA_PAINT_LAYER_BAKE_NEVER));
  int points = 0;
  int readers = 0;
  paint_layer_anchor_point_counts(*ma, points, readers);
  EXPECT_GT(points, PAINT_LAYER_ANCHOR_POINT_LIMIT);
  ASSERT_NE(first_reader, nullptr);
  EXPECT_TRUE(paint_layer_anchor_point_reader_baked(*ma, *first_reader))
      << "the other Auto readers move to the baked slice when the live limit is exceeded";
  Vector<PaintLayersIssue> issues;
  BKE_paint_layers_issues_get(*ma, issues);
  bool reported = false;
  for (const PaintLayersIssue &issue : issues) {
    reported |= issue.text != nullptr &&
                STREQ(issue.text, paint_layer_anchor_point_issue_text(
                                      PaintLayerAnchorPointIssue::LimitExceeded));
  }
  EXPECT_TRUE(reported);
}

TEST_F(PaintLayersAnchorPointTest, depth_seven_hard_error)
{
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "Row", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *point = add_point(*row);
  MaterialPaintLayer *reader = nullptr;
  for (int level = 0; level < 7; level++) {
    point = add_chain_level(*ma, *point, &reader);
  }
  ASSERT_NE(reader, nullptr);
  EXPECT_EQ(paint_layer_anchor_point_resolve(*ma, *reader).issue,
            PaintLayerAnchorPointIssue::ChainTooDeep);
}

TEST_F(PaintLayersAnchorPointTest, mask_reader_depth_ignores_content_readers)
{
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "Row", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *content_point = add_point(*row);
  ASSERT_NE(content_point, nullptr);
  /* A content reader in the effects is not part of the mask slice. */
  ASSERT_NE(add_reader(*row, PaintLayerAnchorPointList::Effects, content_point->marker), nullptr);
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, row, 1.0f), nullptr);
  MaterialPaintLayer *mask_point = add_point(*row, PaintLayerAnchorPointList::MaskStack);
  ASSERT_NE(mask_point, nullptr);
  MaterialPaintLayer *mask_reader = add_reader(
      *row, PaintLayerAnchorPointList::MaskStack, mask_point->marker);
  ASSERT_NE(mask_reader, nullptr);
  EXPECT_EQ(paint_layer_anchor_point_chain_depth(*ma, *mask_reader), 1);
}

TEST_F(PaintLayersAnchorPointTest, slice_with_object_mask_child_auto_stays_live)
{
  MaterialPaintLayer *folder = BKE_paint_layers_add(*ma, "Folder", nullptr, PaintLayerPlace::Above);
  folder->source = MA_PAINT_LAYER_SOURCE_STACK;
  MaterialPaintLayer *child = BKE_paint_layers_add(*ma, "Child", folder, PaintLayerPlace::Into);
  child->object_mask_flag |= MA_PAINT_LAYER_OBJECT_MASK_ACTIVE;
  MaterialPaintLayer *point = add_point(*folder);
  MaterialPaintLayer *reader_row = BKE_paint_layers_add(
      *ma, "Reader", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *reader = add_reader(
      *reader_row, PaintLayerAnchorPointList::Effects, point->marker);
  ASSERT_NE(reader, nullptr);
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *reader, MA_PAINT_LAYER_BAKE_AUTO));
  EXPECT_TRUE(paint_layer_anchor_point_slice_has_object_mask(*ma, *reader));
  EXPECT_TRUE(paint_layer_row_reads_anchor_point(
      *ma, *reader, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  EXPECT_FALSE(paint_layer_anchor_point_reader_baked(*ma, *reader));
}

TEST_F(PaintLayersAnchorPointTest, slice_with_object_mask_child_always_not_participating_and_issue)
{
  MaterialPaintLayer *folder = BKE_paint_layers_add(*ma, "Folder", nullptr, PaintLayerPlace::Above);
  folder->source = MA_PAINT_LAYER_SOURCE_STACK;
  MaterialPaintLayer *child = BKE_paint_layers_add(*ma, "Child", folder, PaintLayerPlace::Into);
  child->object_mask_flag |= MA_PAINT_LAYER_OBJECT_MASK_ACTIVE;
  MaterialPaintLayer *point = add_point(*folder);
  MaterialPaintLayer *reader_row = BKE_paint_layers_add(
      *ma, "Reader", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *reader = add_reader(
      *reader_row, PaintLayerAnchorPointList::Effects, point->marker);
  ASSERT_NE(reader, nullptr);
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *reader, MA_PAINT_LAYER_BAKE_ALWAYS));
  EXPECT_FALSE(paint_layer_anchor_point_reader_baked(*ma, *reader));
  EXPECT_FALSE(paint_layer_row_reads_anchor_point(
      *ma, *reader, PAINT_MATERIAL_CHANNEL_BASE_COLOR));
  Vector<PaintLayersIssue> issues;
  BKE_paint_layers_issues_get(*ma, issues);
  bool reported = false;
  for (const PaintLayersIssue &issue : issues) {
    reported |= issue.text != nullptr &&
                STREQ(issue.text, paint_layer_anchor_point_issue_text(
                                      PaintLayerAnchorPointIssue::ObjectMaskInSlice));
  }
  EXPECT_TRUE(reported);
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

/* -------------------------------------------------------------------- */
/** \name Slice cache ownership and partial renders
 * \{ */

namespace {

/** A row with a Base Color map and a marker in its effects, plus a reader of it in a later row. */
struct SliceFixture {
  MaterialPaintLayer *source = nullptr;
  MaterialPaintLayer *point = nullptr;
  MaterialPaintLayer *reader_row = nullptr;
  MaterialPaintLayer *reader = nullptr;
};

SliceFixture make_slice_fixture(PaintLayersAnchorPointTest &test,
                                const char *name,
                                Image *image)
{
  SliceFixture fixture;
  fixture.source = BKE_paint_layers_add(*test.ma, name, nullptr, PaintLayerPlace::Above);
  BKE_paint_layers_channel_add(*test.ma, fixture.source, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  BKE_paint_layers_channel_set_image(
      *test.ma, fixture.source, PAINT_MATERIAL_CHANNEL_BASE_COLOR, image);
  fixture.point = BKE_paint_layers_anchor_point_add(
      *test.ma, fixture.source, PaintLayerAnchorPointList::Effects, nullptr, false, "Point", {});
  if (fixture.point == nullptr) {
    return fixture;
  }
  fixture.reader_row = BKE_paint_layers_add(
      *test.ma, "Reader", nullptr, PaintLayerPlace::Above);
  fixture.reader = BKE_paint_layers_anchor_point_add(*test.ma,
                                                     fixture.reader_row,
                                                     PaintLayerAnchorPointList::Effects,
                                                     nullptr,
                                                     true,
                                                     "Reader",
                                                     fixture.point->marker);
  return fixture;
}

}  // namespace

TEST_F(PaintLayersAnchorPointTest, slice_buffer_survives_cache_clear_while_referenced)
{
  SliceFixture fixture = make_slice_fixture(*this, "Source", add_solid_image("Source"));
  ASSERT_NE(fixture.reader, nullptr);
  std::shared_ptr<ImBuf> color;
  std::shared_ptr<ImBuf> coverage;
  ASSERT_TRUE(BKE_paint_layers_slice_render(*ma,
                                            *fixture.reader,
                                            PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                            &color,
                                            &coverage));
  ASSERT_NE(color, nullptr);
  ASSERT_NE(coverage, nullptr);
  ASSERT_GT(color.use_count(), 1) << "the cache holds a reference as well";
  const float *pixels = color->float_data();

  paint_layer_anchor_point_slice_cache_clear();

  EXPECT_EQ(color.use_count(), 1) << "the cache only released its reference";
  EXPECT_EQ(color->float_data(), pixels);
  EXPECT_EQ(coverage->x, 8);
  EXPECT_EQ(coverage->y, 8);
  EXPECT_EQ(paint_layer_anchor_point_slice_cache_bytes(), 0);
}

TEST_F(PaintLayersAnchorPointTest, slice_cache_key_follows_source_pixel_version)
{
  Image *image = add_solid_image("Source");
  SliceFixture fixture = make_slice_fixture(*this, "Source", image);
  ASSERT_NE(fixture.reader, nullptr);
  paint_layer_anchor_point_slice_cache_clear();

  std::shared_ptr<ImBuf> first_color;
  std::shared_ptr<ImBuf> first_coverage;
  ASSERT_TRUE(BKE_paint_layers_slice_render(*ma,
                                            *fixture.reader,
                                            PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                            &first_color,
                                            &first_coverage));
  std::shared_ptr<ImBuf> same_color;
  std::shared_ptr<ImBuf> same_coverage;
  ASSERT_TRUE(BKE_paint_layers_slice_render(*ma,
                                            *fixture.reader,
                                            PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                            &same_color,
                                            &same_coverage));
  EXPECT_EQ(same_color.get(), first_color.get()) << "an untouched source reuses the entry";

  /* No cache clear in between: the stroke alone has to change what the lookup finds. */
  BKE_image_partial_update_mark_full_update(image);
  std::shared_ptr<ImBuf> after_color;
  std::shared_ptr<ImBuf> after_coverage;
  ASSERT_TRUE(BKE_paint_layers_slice_render(*ma,
                                            *fixture.reader,
                                            PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                            &after_color,
                                            &after_coverage));
  EXPECT_NE(after_color.get(), first_color.get()) << "a marked source renders the slice again";
  paint_layer_anchor_point_slice_cache_clear();
}

TEST_F(PaintLayersAnchorPointTest, slice_cache_drops_entry_of_older_pixels)
{
  Image *image = add_solid_image("Source");
  SliceFixture fixture = make_slice_fixture(*this, "Source", image);
  ASSERT_NE(fixture.reader, nullptr);
  paint_layer_anchor_point_slice_cache_clear();

  std::shared_ptr<ImBuf> color;
  std::shared_ptr<ImBuf> coverage;
  ASSERT_TRUE(BKE_paint_layers_slice_render(
      *ma, *fixture.reader, PAINT_MATERIAL_CHANNEL_BASE_COLOR, &color, &coverage));
  const size_t pair_bytes = paint_layer_anchor_point_slice_cache_bytes();
  ASSERT_GT(pair_bytes, 0u);

  for (int round = 0; round < 3; round++) {
    BKE_image_partial_update_mark_full_update(image);
    ASSERT_TRUE(BKE_paint_layers_slice_render(
        *ma, *fixture.reader, PAINT_MATERIAL_CHANNEL_BASE_COLOR, &color, &coverage));
    EXPECT_EQ(paint_layer_anchor_point_slice_cache_entry_count(), 1)
        << "round " << round << ": the pair rendered from older pixels is gone";
    EXPECT_EQ(paint_layer_anchor_point_slice_cache_bytes(), pair_bytes);
  }
  paint_layer_anchor_point_slice_cache_clear();
}

TEST_F(PaintLayersAnchorPointTest, source_set_to_the_same_value_succeeds_even_when_unresolved)
{
  MaterialPaintLayer *row = BKE_paint_layers_add(*ma, "Row", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *point = add_point(*row);
  MaterialPaintLayer *reader = add_reader(
      *row, PaintLayerAnchorPointList::Effects, point->marker);
  ASSERT_NE(reader, nullptr);
  /* A reference that names nothing, as a file whose point was deleted by another path holds. */
  const bUUID stale = BLI_uuid_generate_random();
  reader->anchor_point_source = stale;
  EXPECT_TRUE(BKE_paint_layers_anchor_point_source_set(*ma, reader, stale))
      << "writing the value the reader already has is not an edit";
}

TEST_F(PaintLayersAnchorPointTest, slice_cache_evicts_by_bytes)
{
  SliceFixture first = make_slice_fixture(*this, "First", add_solid_image("First"));
  SliceFixture second = make_slice_fixture(*this, "Second", add_solid_image("Second"));
  ASSERT_NE(first.reader, nullptr);
  ASSERT_NE(second.reader, nullptr);
  paint_layer_anchor_point_slice_cache_clear();

  std::shared_ptr<ImBuf> color;
  std::shared_ptr<ImBuf> coverage;
  ASSERT_TRUE(BKE_paint_layers_slice_render(*ma,
                                            *first.reader,
                                            PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                            &color,
                                            &coverage));
  const size_t pair_bytes = size_t(color->x) * color->y * sizeof(float[4]) +
                            size_t(coverage->x) * coverage->y * sizeof(float);
  AnchorSliceCacheBudgetGuard guard(pair_bytes);
  paint_layer_anchor_point_slice_cache_clear();

  ASSERT_TRUE(BKE_paint_layers_slice_render(*ma,
                                            *first.reader,
                                            PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                            &color,
                                            &coverage));
  EXPECT_EQ(paint_layer_anchor_point_slice_cache_entry_count(), 1);
  std::shared_ptr<ImBuf> second_color;
  std::shared_ptr<ImBuf> second_coverage;
  ASSERT_TRUE(BKE_paint_layers_slice_render(*ma,
                                            *second.reader,
                                            PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                            &second_color,
                                            &second_coverage));
  EXPECT_EQ(paint_layer_anchor_point_slice_cache_entry_count(), 1)
      << "the older slice no longer fits the budget";
  EXPECT_EQ(paint_layer_anchor_point_slice_cache_bytes(), pair_bytes);
  paint_layer_anchor_point_slice_cache_clear();
}

TEST_F(PaintLayersAnchorPointTest, slice_larger_than_limit_is_still_usable)
{
  SliceFixture fixture = make_slice_fixture(*this, "Source", add_solid_image("Source"));
  AnchorSliceCacheBudgetGuard guard(1);
  paint_layer_anchor_point_slice_cache_clear();
  std::shared_ptr<ImBuf> color;
  std::shared_ptr<ImBuf> coverage;
  ASSERT_TRUE(BKE_paint_layers_slice_render(*ma,
                                            *fixture.reader,
                                            PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                            &color,
                                            &coverage));
  ASSERT_NE(color, nullptr);
  ASSERT_NE(coverage, nullptr);
  EXPECT_EQ(paint_layer_anchor_point_slice_cache_entry_count(), 0)
      << "a slice larger than the budget is not kept";
  EXPECT_EQ(paint_layer_anchor_point_slice_cache_bytes(), 0);
  EXPECT_NE(color->float_data(), nullptr);
  EXPECT_FLOAT_EQ(color->float_data()[0], 0.0f);
  EXPECT_FLOAT_EQ(color->float_data()[3], 1.0f);
  paint_layer_anchor_point_slice_cache_clear();
}

TEST_F(PaintLayersAnchorPointTest, partial_dst_rect_renders_only_region_and_does_not_leak)
{
  /* A coloured source, so a black buffer cannot pass for a rendered one. */
  const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
  Image *image = BKE_image_add_generated(
      bmain, 8, 8, "Coloured", 32, false, IMA_GENTYPE_BLANK, red, false, false, false);
  SliceFixture fixture = make_slice_fixture(*this, "Source", image);
  paint_layer_anchor_point_slice_cache_clear();
  const int rect[4] = {2, 2, 4, 4};

  std::shared_ptr<ImBuf> color;
  std::shared_ptr<ImBuf> coverage;
  ASSERT_TRUE(BKE_paint_layers_slice_render(*ma,
                                            *fixture.reader,
                                            PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                            &color,
                                            &coverage,
                                            rect));
  ASSERT_NE(color, nullptr);
  EXPECT_EQ(paint_layer_anchor_point_slice_cache_entry_count(), 0)
      << "a partial render is not a complete slice and is not cached";
  EXPECT_EQ(paint_layer_anchor_point_slice_cache_bytes(), 0);
  /* Outside the rectangle the buffer is defined as zero. */
  EXPECT_EQ(color->float_data()[0], 0.0f);
  EXPECT_EQ(color->float_data()[3], 0.0f);
  EXPECT_EQ(coverage->float_data()[0], 0.0f);
  /* Inside it the pixel is the same one a full render produces, and it is red. */
  const int64_t inside = (int64_t(2) * color->x + 2) * 4;
  EXPECT_NEAR(color->float_data()[inside + 0], 1.0f, 1e-5f);
  std::shared_ptr<ImBuf> full_color;
  std::shared_ptr<ImBuf> full_coverage;
  ASSERT_TRUE(BKE_paint_layers_slice_render(*ma,
                                            *fixture.reader,
                                            PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                            &full_color,
                                            &full_coverage));
  for (int i = 0; i < 4; i++) {
    EXPECT_EQ(color->float_data()[inside + i], full_color->float_data()[inside + i]);
  }
  paint_layer_anchor_point_slice_cache_clear();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name UDIM definitions
 * \{ */

TEST_F(PaintLayersAnchorPointTest, udim_content_with_plain_mask_slice_is_ok)
{
  Image *tiled = add_solid_image("Tiled");
  tiled->source = IMA_SRC_TILED;
  MaterialPaintLayer *row = add_paint_layer("Row", tiled);
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, row, 1.0f), nullptr);
  MaterialPaintLayer *point = add_point(*row, PaintLayerAnchorPointList::MaskStack);
  MaterialPaintLayer *reader_row = BKE_paint_layers_add(
      *ma, "Reader", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *reader = add_reader(
      *reader_row, PaintLayerAnchorPointList::Effects, point->marker);
  ASSERT_NE(reader, nullptr);
  EXPECT_TRUE(paint_layer_anchor_point_resolve(*ma, *reader).target ==
              PaintLayerAnchorPointTarget::Mask);
  EXPECT_FALSE(paint_layer_anchor_point_slice_is_udim(*ma, *reader));
}

TEST_F(PaintLayersAnchorPointTest, udim_mask_slice_issue)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_solid_image("Source"));
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, row, 1.0f), nullptr);
  Image *tiled = add_solid_image("Tiled Mask");
  tiled->source = IMA_SRC_TILED;
  MaterialPaintLayer *item = BKE_paint_layers_mask_item_add(
      *ma, row, MA_PAINT_LAYER_SOURCE_IMAGE, "Mask", 1.0f);
  ASSERT_NE(item, nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, item, PAINT_MATERIAL_CHANNEL_BASE_COLOR, tiled));
  MaterialPaintLayer *point = add_point(*row, PaintLayerAnchorPointList::MaskStack);
  MaterialPaintLayer *reader_row = BKE_paint_layers_add(
      *ma, "Reader", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *reader = add_reader(
      *reader_row, PaintLayerAnchorPointList::Effects, point->marker);
  ASSERT_NE(reader, nullptr);
  EXPECT_TRUE(paint_layer_anchor_point_slice_is_udim(*ma, *reader));
  EXPECT_FALSE(paint_layer_anchor_point_reader_baked(*ma, *reader));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Reader groups
 * \{ */

TEST_F(PaintLayersAnchorPointTest, graph_reader_target_change_rebuilds_group)
{
  MaterialPaintLayer *first = add_paint_layer("First", add_solid_image("First"));
  MaterialPaintLayer *first_point = add_point(*first);
  Image *second_image = add_solid_image("Second");
  MaterialPaintLayer *second = add_paint_layer("Second", second_image);
  MaterialPaintLayer *second_point = add_point(*second);
  MaterialPaintLayer *reader_row = BKE_paint_layers_add(
      *ma, "Reader", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *reader = add_reader(
      *reader_row, PaintLayerAnchorPointList::Effects, first_point->marker);
  ASSERT_NE(reader, nullptr);

  const auto hash = [&](const MaterialPaintLayer &layer) {
    const paint_layers::PaintLayerFormMap forms = paint_layers::paint_layers_form_map(*ma);
    return paint_layers::paint_layers_layer_topology_hash(
        *ma, layer, paint_layers::paint_layers_wired_channels(*ma), forms);
  };
  /* The generator hashes the row that owns the reader, not the reader element itself: a correction
   * or a mask item is part of its owner's structure. */
  const uint64_t before = hash(*reader_row);
  ASSERT_TRUE(BKE_paint_layers_anchor_point_source_set(*ma, reader, second_point->marker));
  const uint64_t after_target = hash(*reader_row);
  EXPECT_NE(after_target, before) << "the reader group must be rebuilt, not relinked with defaults";

  /* The source row's channel composition decides which taps the reader's group exposes. */
  ASSERT_NE(BKE_paint_layers_channel_add(
                *ma, second, PAINT_MATERIAL_CHANNEL_ROUGHNESS),
            nullptr);
  EXPECT_NE(hash(*reader_row), after_target);

  /* Values and pixels are inputs, not structure: neither moves the owner row's topology. */
  const uint64_t after_channels = hash(*reader_row);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, reader, 0.25f));
  EXPECT_EQ(hash(*reader_row), after_channels);
  ASSERT_NE(second_image, nullptr);
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(second_image, nullptr, &lock);
  ASSERT_NE(ibuf, nullptr);
  uchar *pixels = ibuf->byte_data_for_write();
  pixels[0] = uchar(255 - pixels[0]);
  BKE_image_release_ibuf(second_image, ibuf, lock);
  EXPECT_EQ(hash(*reader_row), after_channels)
      << "a pixel edit is not a topology change";

  /* A reader in the mask stack is part of the owner row's structure too (the 0x606 hash block). */
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, reader_row, 1.0f), nullptr);
  MaterialPaintLayer *mask_reader = add_reader(
      *reader_row, PaintLayerAnchorPointList::MaskStack, first_point->marker);
  ASSERT_NE(mask_reader, nullptr);
  const uint64_t before_mask = hash(*reader_row);
  ASSERT_TRUE(BKE_paint_layers_anchor_point_source_set(*ma, mask_reader, second_point->marker));
  EXPECT_NE(hash(*reader_row), before_mask)
      << "a mask reader's target is part of its owner's topology";
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Planner: Auto at depth
 * \{ */

TEST_F(PaintLayersAnchorPointTest, auto_reader_depth_four_light_row_is_baked)
{
  MaterialPaintLayer *row = add_paint_layer("Level 0", add_solid_image("Level 0"));
  MaterialPaintLayer *point = add_point(*row);
  ASSERT_NE(point, nullptr);
  MaterialPaintLayer *reader = nullptr;
  for (int level = 1; level <= 4; level++) {
    MaterialPaintLayer *next_row = add_paint_layer("Level", add_solid_image("Level"));
    reader = add_reader(*next_row, PaintLayerAnchorPointList::Effects, point->marker);
    ASSERT_NE(reader, nullptr);
    point = add_point(*next_row);
    ASSERT_NE(point, nullptr);
  }
  ASSERT_TRUE(paint_layer_anchor_point_reader_baked(*ma, *reader));
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());
  bool changed = false;
  EXPECT_TRUE(BKE_paint_layers_bake_plan_run(*bmain, *ma, &changed));
  ASSERT_NE(reader->bake, nullptr)
      << "a light reader deep in an Auto chain still gets the slice cache the depth rule asked for";
  EXPECT_NE(reader->bake->coverage, nullptr);
}

TEST_F(PaintLayersAnchorPointTest, rebake_ignores_own_stale_maps)
{
  MaterialPaintLayer *source = add_paint_layer("Source", add_solid_image("Source"));
  MaterialPaintLayer *point = add_point(*source);
  MaterialPaintLayer *reader_row = BKE_paint_layers_add(
      *ma, "Reader", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *reader = add_reader(
      *reader_row, PaintLayerAnchorPointList::Effects, point->marker);
  ASSERT_NE(reader, nullptr);
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *reader, MA_PAINT_LAYER_BAKE_ALWAYS));
  BKE_paint_layers_bake_struct_ensure(*reader);
  reader->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR] = add_solid_image("Stale Bake");
  reader->bake->coverage = add_solid_image("Stale Coverage");

  paint_layer_anchor_point_slice_cache_clear();
  float color[8 * 8 * 4];
  float coverage[8 * 8];
  ASSERT_TRUE(BKE_paint_layers_bake_render_node(*ma,
                                                *reader,
                                                PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                                8,
                                                color,
                                                coverage));
  EXPECT_EQ(paint_layer_anchor_point_slice_cache_entry_count(), 1)
      << "the reader being re-baked renders its live slice, not its own stale maps";
  paint_layer_anchor_point_slice_cache_clear();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Nested readers
 * \{ */

TEST_F(PaintLayersAnchorPointTest, nested_reader_uses_own_valid_bake_while_outer_rebakes)
{
  MaterialPaintLayer *inner = add_paint_layer("Inner", add_solid_image("Inner"));
  MaterialPaintLayer *inner_point = add_point(*inner);
  ASSERT_NE(inner_point, nullptr);
  MaterialPaintLayer *outer = add_paint_layer("Outer", add_solid_image("Outer"));
  MaterialPaintLayer *nested = add_reader(
      *outer, PaintLayerAnchorPointList::Effects, inner_point->marker);
  ASSERT_NE(nested, nullptr);
  MaterialPaintLayer *outer_point = add_point(*outer);
  ASSERT_NE(outer_point, nullptr);
  ASSERT_TRUE(BKE_paint_layers_bake_mode_set(*ma, *nested, MA_PAINT_LAYER_BAKE_ALWAYS));
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());
  BKE_paint_layers_bake_plan_run(*bmain, *ma);
  ASSERT_NE(nested->bake, nullptr);
  ASSERT_NE(nested->bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR], nullptr);
  ASSERT_NE(nested->bake->coverage, nullptr);
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *nested));
  ASSERT_TRUE(paint_layer_anchor_point_reader_baked(*ma, *nested));

  paint_layer_anchor_point_slice_cache_clear();
  float color[8 * 8 * 4];
  float coverage[8 * 8];
  ASSERT_TRUE(BKE_paint_layers_bake_render_node(*ma,
                                                *outer,
                                                PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                                8,
                                                color,
                                                coverage));
  EXPECT_EQ(paint_layer_anchor_point_slice_cache_entry_count(), 0)
      << "the nested reader read its own valid bake instead of rendering its slice live";
}

/** \} */

}  // namespace blender::bke::tests
