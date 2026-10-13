/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Tests for the shared authoring verbs of `BKE_paint_layers_edit.hh`: the policy-driven Add against
 * the low-level description edit, the unique default names and the marker-addressed reorder.
 */

#include "paint_layers_test_util.hh"

#include "DNA_material_types.h"

#include "BLI_index_range.hh"
#include "BLI_string.h"
#include "BLI_uuid.h"

#include "BKE_material.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_edit.hh"

namespace blender::bke::tests {

/* -------------------------------------------------------------------- */
/** \name Add policy against the low-level description edit
 * \{ */

TEST_F(PaintLayersTestBase, add_with_policy_paint_matches_low_level_add)
{
  Material *plain = BKE_material_add(bmain, "Plain");

  PaintLayerAddParams params;
  params.name = "Row";
  MaterialPaintLayer *via_policy = BKE_paint_layers_add_with_policy(*plain, params);
  ASSERT_NE(via_policy, nullptr);

  MaterialPaintLayer *by_hand = BKE_paint_layers_add(*ma, "Row", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(by_hand, nullptr);
  /* The default-channel rule is the policy's own; a bare `BKE_paint_layers_add` row carries no
   * channel record, so the by-hand row gets the same rule applied explicitly. */
  EXPECT_EQ(by_hand->channels_num, 0);
  BKE_paint_layers_default_channels_apply(*ma, *by_hand);

  /* Same shape, whether the policy or the caller built it. */
  EXPECT_STREQ(via_policy->name, by_hand->name);
  EXPECT_EQ(via_policy->flag, by_hand->flag);
  EXPECT_FLOAT_EQ(via_policy->opacity, by_hand->opacity);
  ASSERT_EQ(via_policy->channels_num, by_hand->channels_num);
  EXPECT_GT(via_policy->channels_num, 0);
  for (const int i : IndexRange(via_policy->channels_num)) {
    EXPECT_EQ(via_policy->channels[i].channel, by_hand->channels[i].channel);
    EXPECT_EQ(via_policy->channels[i].state, MA_PAINT_LAYER_CHANNEL_ENABLED);
    EXPECT_EQ(via_policy->channels[i].image, nullptr);
  }
  EXPECT_TRUE(BKE_paint_layers_is_layered(*plain));
}

TEST_F(PaintLayersTestBase, add_with_policy_takes_every_default_channel_once)
{
  MaterialPaintLayer *layer = BKE_paint_layers_add_with_policy(*ma, {});
  ASSERT_NE(layer, nullptr);
  const int channels_num = layer->channels_num;

  /* Base Color is part of the default set, and applying the set again changes nothing. */
  bool has_base_color = false;
  for (const int i : IndexRange(layer->channels_num)) {
    has_base_color |= layer->channels[i].channel == PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  }
  EXPECT_TRUE(has_base_color);

  BKE_paint_layers_default_channels_apply(*ma, *layer);
  EXPECT_EQ(layer->channels_num, channels_num);
}

TEST_F(PaintLayersTestBase, add_with_policy_derives_unique_default_names)
{
  MaterialPaintLayer *first = BKE_paint_layers_add_with_policy(*ma, {});
  MaterialPaintLayer *second = BKE_paint_layers_add_with_policy(*ma, {});
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_STREQ(first->name, "Layer 1");
  EXPECT_STREQ(second->name, "Layer 2");

  /* One past the highest number in use, so removing the first row does not hand its name out
   * again while the second stands. */
  ASSERT_TRUE(BKE_paint_layers_remove(*ma, first));
  MaterialPaintLayer *third = BKE_paint_layers_add_with_policy(*ma, {});
  ASSERT_NE(third, nullptr);
  EXPECT_STREQ(third->name, "Layer 3");

  /* A name that merely starts like the base is not a number. */
  BKE_paint_layers_rename(*ma, third, "Layer 7x");
  EXPECT_EQ(BKE_paint_layers_unique_default_name(*ma, "Layer"), "Layer 3");

  /* A name given by the caller is kept as it is. */
  PaintLayerAddParams params;
  params.name = "Skin";
  MaterialPaintLayer *named = BKE_paint_layers_add_with_policy(*ma, params);
  ASSERT_NE(named, nullptr);
  EXPECT_STREQ(named->name, "Skin");
}

TEST_F(PaintLayersTestBase, add_with_policy_places_beside_the_anchor_and_refuses_a_foreign_one)
{
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_solid_image("BottomImg"));
  MaterialPaintLayer *top = add_paint_layer("Top", add_solid_image("TopImg"));

  PaintLayerAddParams params;
  params.name = "Between";
  params.anchor = bottom;
  params.place = PaintLayerPlace::Above;
  MaterialPaintLayer *between = BKE_paint_layers_add_with_policy(*ma, params);
  ASSERT_NE(between, nullptr);
  EXPECT_EQ(bottom->next, between);
  EXPECT_EQ(between->next, top);

  /* An anchor of another material is foreign, never a slot of this one. */
  Material *other = BKE_material_add(bmain, "Other");
  MaterialPaintLayer *foreign = BKE_paint_layers_add(
      *other, "Foreign", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(foreign, nullptr);
  params.anchor = foreign;
  EXPECT_EQ(BKE_paint_layers_add_with_policy(*ma, params), nullptr);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Edit verbs
 * \{ */

TEST_F(PaintLayersTestBase, edit_reorder_takes_the_slot_of_the_target)
{
  MaterialPaintLayer *a = add_paint_layer("A", add_solid_image("A"));
  MaterialPaintLayer *b = add_paint_layer("B", add_solid_image("B"));
  MaterialPaintLayer *c = add_paint_layer("C", add_solid_image("C"));

  /* Bottom to top: A, B, C. A dropped on C takes C's slot. */
  EXPECT_TRUE(BKE_paint_layers_edit_reorder(*ma, *a, *c));
  EXPECT_EQ(ma->paint_layers.first, b);
  EXPECT_EQ(b->next, c);
  EXPECT_EQ(c->next, a);

  /* C dropped on B takes B's slot, which is the bottom. */
  EXPECT_TRUE(BKE_paint_layers_edit_reorder(*ma, *c, *b));
  EXPECT_EQ(ma->paint_layers.first, c);
  EXPECT_EQ(c->next, b);
  EXPECT_EQ(b->next, a);

  /* A row is not reordered onto itself, nor onto a row of another material. */
  EXPECT_FALSE(BKE_paint_layers_edit_reorder(*ma, *a, *a));
  Material *other = BKE_material_add(bmain, "Other");
  MaterialPaintLayer *foreign = BKE_paint_layers_add(
      *other, "Foreign", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(foreign, nullptr);
  EXPECT_FALSE(BKE_paint_layers_edit_reorder(*ma, *a, *foreign));
  EXPECT_FALSE(BKE_paint_layers_edit_reorder(*ma, *foreign, *a));
}

/** \} */

}  // namespace blender::bke::tests
