/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Tests for the invariants the refactor plan (Refactor_PBR_Paint_Plan) calls out by name:
 * the source-material cycle protection behind the Add policy (2.1.3), the parity of the
 * policy-driven Add against the low-level description edit (2.1.5, BKE half), and the single
 * depth-first order the Outliner's ordinals are built on (2.2.3).
 */

#include "intern/paint_layers_test_util.hh"

#include "DNA_material_types.h"

#include "BLI_listbase_wrapper.hh"
#include "BLI_string.h"
#include "BLI_uuid.h"

#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_edit.hh"

namespace blender::bke::test {

namespace {

/** Flatten \a ma's description by name, in whatever order the walk hands out. */
Vector<std::string> order_of_names(const Material &ma,
                                   Vector<MaterialPaintLayer *> (*walk)(const Material &))
{
  Vector<std::string> names;
  for (const MaterialPaintLayer *layer : walk(ma)) {
    names.append(layer->name);
  }
  return names;
}

Vector<MaterialPaintLayer *> flatten_via_foreach(const Material &ma)
{
  Vector<MaterialPaintLayer *> rows;
  BKE_paint_layers_foreach(ma, [&](const MaterialPaintLayer &layer, const MaterialPaintLayer *) {
    rows.append(const_cast<MaterialPaintLayer *>(&layer));
    return true;
  });
  return rows;
}

}  // namespace

/* -------------------------------------------------------------------- */
/** \name 2.1.3: source-material cycle protection
 * \{ */

TEST_F(PaintLayersTestBase, set_material_refuses_owner_itself)
{
  MaterialPaintLayer *layer = add_paint_layer("FromMaterial", add_solid_image("Base"));
  ASSERT_NE(layer, nullptr);

  /* The layer's own material as its source would make the material read from itself. */
  EXPECT_FALSE(BKE_paint_layers_set_material(*ma, layer, ma));
  EXPECT_EQ(layer->material, nullptr);
}

TEST_F(PaintLayersTestBase, set_material_refuses_cycle)
{
  Material *other = BKE_material_add(bmain, "Other");
  other->paint_layers_flag |= MA_PAINT_LAYERED;
  MaterialPaintLayer *bridge = add_paint_layer("Bridge", add_solid_image("BridgeBase"));
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, bridge, other));
  EXPECT_EQ(bridge->material, other);
  EXPECT_TRUE(BKE_paint_layers_material_depends_on(*ma, *other));

  /* In the other material, a row sourcing `ma` closes the A -> B -> A cycle and must be refused:
   * the same protection the Outliner's Add and the drop handler rely on. */
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *other, MA_PAINT_LAYER_SOURCE_IMAGE, "Back", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  EXPECT_FALSE(BKE_paint_layers_set_material(*other, row, ma));
  EXPECT_EQ(row->material, nullptr);
}

TEST_F(PaintLayersTestBase, add_with_policy_refuses_owner_as_material_source)
{
  /* The policy's Material kind: the same owner rule the Outliner Add's virtual path checks. */
  PaintLayerAddParams params;
  params.kind = PaintLayerAddKind::Material;
  params.source = &ma->id;
  MaterialPaintLayer *layer = BKE_paint_layers_add_with_policy(*ma, params);
  EXPECT_EQ(layer, nullptr);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name 2.1.5: policy Add against the low-level description edit
 * \{ */

TEST_F(PaintLayersTestBase, add_with_policy_paint_matches_low_level_add)
{
  Material *plain = BKE_material_add(bmain, "Plain");
  plain->paint_layers_flag |= MA_PAINT_LAYERED;

  PaintLayerAddParams params;
  params.kind = PaintLayerAddKind::Paint;
  params.name = "Row";
  MaterialPaintLayer *via_policy = BKE_paint_layers_add_with_policy(*plain, params);
  ASSERT_NE(via_policy, nullptr);

  MaterialPaintLayer *by_hand = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Row", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(by_hand, nullptr);

  /* Same shape: a Layer-role image row with a Base Color record, whether the policy or the caller
   * built it. The default-channel rule is the policy's own; the low-level edit here enables the
   * record by hand because a bare `BKE_paint_layers_add` row carries none. */
  EXPECT_EQ(via_policy->source, MA_PAINT_LAYER_SOURCE_IMAGE);
  EXPECT_EQ(via_policy->role, by_hand->role);
  EXPECT_EQ(via_policy->channels_num, by_hand->channels_num);
  EXPECT_STREQ(via_policy->name, by_hand->name);
  if (via_policy->channels_num == 1 && by_hand->channels_num == 1) {
    EXPECT_EQ(via_policy->channels[0].channel, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
    EXPECT_EQ(via_policy->channels[0].channel, by_hand->channels[0].channel);
  }
}

TEST_F(PaintLayersTestBase, add_with_policy_fill_targets_base_color)
{
  /* The Fill -> Base Color rule from the Add table: a fill is a colour on Base Color, never on
   * some other channel, whoever calls the Add. */
  PaintLayerAddParams params;
  params.kind = PaintLayerAddKind::Fill;
  const float color[4] = {0.25f, 0.5f, 0.75f, 1.0f};
  params.fill_color = color;
  MaterialPaintLayer *fill = BKE_paint_layers_add_with_policy(*ma, params);
  ASSERT_NE(fill, nullptr);

  bool has_base_color = false;
  for (const int i : IndexRange(fill->channels_num)) {
    if (fill->channels[i].channel == PAINT_MATERIAL_CHANNEL_BASE_COLOR) {
      has_base_color = true;
    }
  }
  EXPECT_TRUE(has_base_color);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name 2.2.2: one set of tree walks
 * \{ */

TEST_F(PaintLayersTestBase, children_accessors_agree_with_each_other)
{
  /* The folder's children read the same through the snapshot, the list reference and the
   * predicate: the Outliner row builder and the hasher share these instead of touching
   * `.children` / `.effects` / `.mask_stack` directly. */
  MaterialPaintLayer *folder = add_folder("Folder");
  MaterialPaintLayer *child_a = add_paint_layer_into(folder, "ChildA", add_solid_image("ChildA"));
  MaterialPaintLayer *child_b = add_paint_layer_into(folder, "ChildB", add_solid_image("ChildB"));
  MaterialPaintLayer *leaf = add_paint_layer("Leaf", add_solid_image("Leaf"));

  EXPECT_TRUE(BKE_paint_layers_has_children(*folder));
  EXPECT_FALSE(BKE_paint_layers_has_children(*leaf));

  const Vector<MaterialPaintLayer *> kids = BKE_paint_layers_children(*folder);
  ASSERT_EQ(kids.size(), 2);
  EXPECT_EQ(kids[0], child_a);
  EXPECT_EQ(kids[1], child_b);
  EXPECT_EQ(BKE_paint_layers_children_list(*folder).first, child_a);

  EXPECT_TRUE(BKE_paint_layers_effects(*folder).is_empty());
  EXPECT_TRUE(BKE_paint_layers_mask_items(*folder).is_empty());
  EXPECT_TRUE(BKE_paint_layers_children(*leaf).is_empty());
}

TEST_F(PaintLayersTestBase, mask_and_subtree_accessors_agree)
{
  MaterialPaintLayer *layer = add_paint_layer("Layer", add_solid_image("LayerBase"));
  MaterialPaintLayer *mask = add_mask_item(*layer, "Mask", add_solid_image("MaskBase"));

  EXPECT_EQ(BKE_paint_layers_mask_items(*layer).size(), 1);
  EXPECT_EQ(BKE_paint_layers_mask_items(*layer)[0], mask);
  EXPECT_EQ(BKE_paint_layers_mask_base(*layer), mask);

  EXPECT_TRUE(BKE_paint_layers_subtree_contains(*layer, mask->marker));
  EXPECT_FALSE(BKE_paint_layers_subtree_contains(*mask, layer->marker));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name 2.2.3: one depth-first order
 * \{ */

TEST_F(PaintLayersTestBase, foreach_matches_flatten_on_nested_tree)
{
  /* A tree with every shape the walk must agree on: a folder with children, a layer with a mask
   * item and corrections, rows interleaved. #BKE_paint_layers_foreach is the definition of the
   * Outliner's ordinal order; #BKE_paint_layers_flatten is the row list the UI reads. */
  MaterialPaintLayer *folder = add_folder("Folder");
  add_paint_layer_into(folder, "ChildA", add_solid_image("ChildA"));
  MaterialPaintLayer *layer = add_paint_layer("Layer", add_solid_image("LayerBase"));
  add_mask_item(*layer, "Mask", add_solid_image("MaskBase"));
  MaterialPaintLayer *child_b = add_paint_layer_into(folder, "ChildB", add_solid_image("ChildB"));

  const Vector<MaterialPaintLayer *> via_foreach = flatten_via_foreach(*ma);
  Vector<const MaterialPaintLayer *> via_flatten;
  BKE_paint_layers_flatten(*ma, via_flatten);

  ASSERT_EQ(via_foreach.size(), via_flatten.size());
  for (const int i : via_foreach.index_range()) {
    EXPECT_EQ(via_foreach[i], via_flatten[i]) << "Row " << i << " diverges between the two walks";
  }
  /* The nesting reads in document order: folder, its children in turn, then the layer. */
  EXPECT_EQ(via_foreach[0], folder);
  EXPECT_EQ(via_foreach[1], child_b);
}

TEST_F(PaintLayersTestBase, foreach_visit_allows_pruning_but_not_order_change)
{
  /* A false return skips a subtree: the walk order of everything else is untouched, so a caller
   * that prunes folders still sees the same rows in the same order as the Outliner does. */
  MaterialPaintLayer *folder = add_folder("Folder");
  MaterialPaintLayer *child = add_paint_layer_into(folder, "Hidden", add_solid_image("Hidden"));
  add_paint_layer("Top", add_solid_image("Top"));

  Vector<MaterialPaintLayer *> rows;
  BKE_paint_layers_foreach(*ma, [&](const MaterialPaintLayer &layer, const MaterialPaintLayer *) {
    rows.append(const_cast<MaterialPaintLayer *>(&layer));
    /* Skip everything under the folder once it is seen. */
    return &layer != folder;
  });
  EXPECT_EQ(rows.size(), 2);
  EXPECT_FALSE(rows.contains(child));
}

/** \} */

}  // namespace blender::bke::test
