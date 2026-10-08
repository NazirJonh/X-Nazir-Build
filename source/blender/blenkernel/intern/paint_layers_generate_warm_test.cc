/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Paint Layers generator tests: warm slots, sampler budget, live-until-bake, cold tier (plan 9.3).
 *
 * Fixture and shared helpers live in
 * `paint_layers_generate_test_fixture.hh` /
 * `paint_layers_generate_test_shared.hh`; this TU holds only its own tests.
 */

#include "intern/paint_layers_generate_test_fixture.hh"
#include "intern/paint_layers_generate_test_shared.hh"

namespace blender::bke::tests {

TEST_F(PaintLayersGenerateTest, warm_plan_follows_role_source_and_level)
{
  MaterialPaintLayer *paint = add_paint_layer("Paint", add_image("PaintMap"));
  MaterialPaintLayer *fill = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_CONSTANT, "Fill", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Folder", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *nested = add_paint_layer_into(folder, "Nested", add_image("NestedMap"));
  ASSERT_NE(fill, nullptr);
  ASSERT_NE(nested, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 0.5f));

  PaintLayerWarmPlan plan = paint_layers_warm_plan(*ma, *paint);
  EXPECT_TRUE(plan.mask);
  EXPECT_EQ(plan.effect, PaintLayerWarmEffect::Paint);

  plan = paint_layers_warm_plan(*ma, *fill);
  EXPECT_TRUE(plan.mask);
  EXPECT_EQ(plan.effect, PaintLayerWarmEffect::Fill);

  /* A folder in the root gets a mask only. */
  plan = paint_layers_warm_plan(*ma, *folder);
  EXPECT_TRUE(plan.mask);
  EXPECT_EQ(plan.effect, PaintLayerWarmEffect::None);

  /* Rows inside a folder get nothing. */
  plan = paint_layers_warm_plan(*ma, *nested);
  EXPECT_FALSE(plan.mask);
  EXPECT_EQ(plan.effect, PaintLayerWarmEffect::None);
}

TEST_F(PaintLayersGenerateTest, warm_plan_gives_a_correction_nothing)
{
  MaterialPaintLayer *layer = add_paint_layer("Owner", add_image("OwnerMap"));
  MaterialPaintLayer *fx = BKE_paint_layers_correction_add(
      *ma, layer, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_IMAGE, "Fx");
  ASSERT_NE(fx, nullptr);
  const PaintLayerWarmPlan plan = paint_layers_warm_plan(*ma, *fx);
  EXPECT_FALSE(plan.mask);
  EXPECT_EQ(plan.effect, PaintLayerWarmEffect::None);
}

TEST_F(PaintLayersGenerateTest, warm_plan_skips_a_pass_through_folder)
{
  MaterialPaintLayer *a = add_paint_layer("PtA", add_image("PtAImg"));
  MaterialPaintLayer *b = add_paint_layer("PtB", add_image("PtBImg"));
  MaterialPaintLayer *members[2] = {a, b};
  MaterialPaintLayer *folder = BKE_paint_layers_group(*ma, Span<MaterialPaintLayer *>(members, 2));
  ASSERT_NE(folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));
  EXPECT_FALSE(paint_layers_warm_plan(*ma, *folder).mask)
      << "a Pass Through folder inlines its children, so a mask chain could not apply";
}

TEST_F(PaintLayersGenerateTest, warm_state_is_created_consumed_and_forgotten)
{
  MaterialPaintLayer *layer = add_paint_layer("Bottom", add_image("Bottom"));
  Image *warm = add_image("Warm");
  paint_layers_warm_reconcile(*ma, warm);
  const MaterialPaintLayer *mask = paint_layers_warm_item(*ma, *layer, WarmKind::Mask);
  ASSERT_NE(mask, nullptr);
  EXPECT_EQ(mask->opacity, 0.0f);
  EXPECT_EQ(mask->role, MA_PAINT_LAYER_ROLE_MASK_ITEM);
  /* +warm: the virtual mask base stands first, then the spare mask. */
  EXPECT_EQ(paint_layers_build_mask_items(*ma, *layer).size(), 2);

  /* A compatible real mask takes the slot once it builds its chain: the UI picks the map after the
   * correction exists, and only the built chain replaces the spare's. */
  MaterialPaintLayer *real = BKE_paint_layers_correction_add(
      *ma, layer, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_IMAGE, "Mask");
  ASSERT_NE(real, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, real, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, real, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("RealMaskMap")));
  paint_layers_warm_reconcile(*ma, warm);
  EXPECT_EQ(paint_layers_warm_item(*ma, *layer, WarmKind::Mask), nullptr);
  const Vector<const MaterialPaintLayer *> items = paint_layers_build_mask_items(*ma, *layer);
  /* +warm: the virtual mask base, then the real mask that took the spare's slot. */
  ASSERT_EQ(items.size(), 2);
  EXPECT_EQ(items[1], real);

  /* Losing the runtime models a file load: the entry comes back present. */
  BKE_paint_layers_generate_runtime_free(*ma);
  paint_layers_warm_reconcile(*ma, warm);
  EXPECT_NE(paint_layers_warm_item(*ma, *layer, WarmKind::Mask), nullptr);
}

/** An incompatible real item does not take the slot. */
TEST_F(PaintLayersGenerateTest, warm_slot_is_kept_by_an_incompatible_real_item)
{
  MaterialPaintLayer *layer = add_paint_layer("Bottom", add_image("Bottom"));
  Image *warm = add_image("Warm");
  paint_layers_warm_reconcile(*ma, warm);
  ASSERT_NE(BKE_paint_layers_correction_add(
                *ma, layer, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_CONSTANT, "FillMask"),
            nullptr);
  paint_layers_warm_reconcile(*ma, warm);
  EXPECT_NE(paint_layers_warm_item(*ma, *layer, WarmKind::Mask), nullptr);
  /* +warm: the virtual mask base as well. */
  EXPECT_EQ(paint_layers_build_mask_items(*ma, *layer).size(), 3)
      << "the virtual base, the real Fill mask, then the untouched spare";
}

/** Reload: losing the runtime state costs at most one rebuild, and the next pass keeps the root. */
TEST_F(PaintLayersGenerateTest, warm_slots_survive_a_runtime_reload_in_one_pass)
{
  add_paint_layer("Bottom", add_image("Bottom"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  BKE_paint_layers_generate_runtime_free(*ma);
  PaintLayersRegenerateReport first;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &first));
  PaintLayersRegenerateReport second;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &second));
  EXPECT_FALSE(second.root_rebuilt);
  EXPECT_EQ(second.layer_groups_rebuilt, 0);
}

/** The warm chain is neutral: the row's opacity input for the spare is zero. */
TEST_F(PaintLayersGenerateTest, warm_mask_is_built_with_a_zero_opacity_input)
{
  add_paint_layer("Bottom", add_image("Bottom"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *group = layer_tree_find(*bmain, "Bottom");
  ASSERT_NE(group, nullptr);
  bool found = false;
  group->ensure_interface_cache();
  for (const bNodeTreeInterfaceSocket *socket : group->interface_inputs()) {
    /* The real item that takes a slot builds its values in the spare's sockets (by slot). */
    if (STRPREFIX(socket->name, "Bottom Warm Mask")) {
      found = true;
      EXPECT_EQ(static_cast<const bNodeSocketValueFloat *>(socket->socket_data)->value, 0.0f);
    }
  }
  EXPECT_TRUE(found);
}

/** A hidden root row keeps its warm chain (visibility stays a value). */
TEST_F(PaintLayersGenerateTest, warm_slot_does_not_rebuild_on_visibility_toggle)
{
  MaterialPaintLayer *layer = add_paint_layer("Bottom", add_image("Bottom"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, layer, false));
  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_FALSE(report.root_rebuilt);
  EXPECT_EQ(report.layer_groups_rebuilt, 0);
}

TEST_F(PaintLayersGenerateTest, adding_a_paint_correction_to_a_root_paint_layer_keeps_the_code_shape)
{
  MaterialPaintLayer *layer = add_paint_layer("Bottom", add_image("Bottom"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  const uint64_t before = code_shape_signature(*layer_tree_find(*bmain, "Bottom"));

  MaterialPaintLayer *fx = BKE_paint_layers_correction_add(
      *ma, layer, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_IMAGE, "Fx");
  ASSERT_NE(fx, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, fx, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, fx, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("FxMap")));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(code_shape_signature(*layer_tree_find(*bmain, "Bottom")), before);
}

TEST_F(PaintLayersGenerateTest, adding_a_fill_correction_to_a_fill_layer_keeps_the_code_shape)
{
  MaterialPaintLayer *fill = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_CONSTANT, "Fill", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(fill, nullptr);
  BKE_paint_layers_default_channels_apply(*ma, *fill);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  const uint64_t before = code_shape_signature(*layer_tree_find(*bmain, "Fill"));

  ASSERT_NE(BKE_paint_layers_correction_add(
                *ma, fill, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_CONSTANT, "FillFx"),
            nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(code_shape_signature(*layer_tree_find(*bmain, "Fill")), before);
}

TEST_F(PaintLayersGenerateTest, adding_a_mask_to_a_root_folder_keeps_the_code_shape)
{
  MaterialPaintLayer *child = add_paint_layer("FolderChild", add_image("FolderChildMap"));
  MaterialPaintLayer *folder = group_one(*ma, child);
  ASSERT_NE(folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 0.5f));
  ASSERT_FALSE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  const uint64_t before = code_shape_signature(*layer_tree_find_folder(*bmain, "Folder"));

  MaterialPaintLayer *mask = BKE_paint_layers_correction_add(
      *ma, folder, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_IMAGE, "Mask");
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, mask, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
  Image *folder_mask_map = add_image("FolderMask");
  make_generate_image_mask_like(*folder_mask_map);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, mask, PAINT_MATERIAL_CHANNEL_BASE_COLOR, folder_mask_map));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(code_shape_signature(*layer_tree_find_folder(*bmain, "Folder")), before);
}

/** A Fill correction on the Normal channel is not built, so the spare must not be either. */
TEST_F(PaintLayersGenerateTest, warm_fill_effect_is_not_built_for_the_normal_channel)
{
  MaterialPaintLayer *fill = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_CONSTANT, "Fill", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, fill, PAINT_MATERIAL_CHANNEL_NORMAL), nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *group = layer_tree_find(*bmain, "Fill");
  ASSERT_NE(group, nullptr);
  /* The spare's Fill effect is skipped on Normal exactly like a real one: a real Fill correction
   * taking the slot must not move the code either. */
  const uint64_t before = code_shape_signature(*group);

  ASSERT_NE(BKE_paint_layers_correction_add(
                *ma, fill, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_CONSTANT, "FillFx"),
            nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(code_shape_signature(*layer_tree_find(*bmain, "Fill")), before);
}

TEST_F(PaintLayersGenerateTest, replenish_puts_a_consumed_warm_slot_back)
{
  MaterialPaintLayer *layer = add_paint_layer("Bottom", add_image("Bottom"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  MaterialPaintLayer *mask = BKE_paint_layers_correction_add(
      *ma, layer, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_IMAGE, "Mask");
  ASSERT_NE(mask, nullptr);
  /* The slot is spent when the real chain is built: the UI picks the map after the correction. */
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, mask, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, mask, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("MaskMap")));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_EQ(paint_layers_warm_item(*ma, *layer, WarmKind::Mask), nullptr);

  EXPECT_TRUE(BKE_paint_layers_warm_replenish(*ma));
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_NE(paint_layers_warm_item(*ma, *layer, WarmKind::Mask), nullptr);
  /* +warm: the virtual mask base, the real mask and the replenished spare. */
  EXPECT_EQ(paint_layers_build_mask_items(*ma, *layer).size(), 3);

  /* Nothing left to replenish. */
  EXPECT_FALSE(BKE_paint_layers_warm_replenish(*ma));
}

/**
 * The UI path: Add Mask creates the base, the mask item is added without a map, and the map is
 * picked last. None of the three steps may change the shader code of the row.
 */
TEST_F(PaintLayersGenerateTest, ui_path_mask_base_item_and_map_keep_the_code_shape)
{
  MaterialPaintLayer *layer = add_paint_layer("Bottom", add_image("Bottom"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  const uint64_t before = code_shape_signature(*layer_tree_find(*bmain, "Bottom"));

  ASSERT_NE(BKE_paint_layers_mask_add(*ma, layer, 1.0f), nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(code_shape_signature(*layer_tree_find(*bmain, "Bottom")), before) << "mask base";

  MaterialPaintLayer *item = BKE_paint_layers_correction_add(
      *ma, layer, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_IMAGE, "Mask");
  ASSERT_NE(item, nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(code_shape_signature(*layer_tree_find(*bmain, "Bottom")), before) << "imageless item";

  Image *map = add_image("UiMaskMap");
  make_generate_image_mask_like(*map);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, item, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
  ASSERT_TRUE(
      BKE_paint_layers_channel_set_image(*ma, item, PAINT_MATERIAL_CHANNEL_BASE_COLOR, map));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(code_shape_signature(*layer_tree_find(*bmain, "Bottom")), before) << "item map";
}

/** I2: a slot spent by a real mask comes back in the same regeneration that removes the mask. */
TEST_F(PaintLayersGenerateTest, removing_the_real_mask_brings_the_spare_back_at_once)
{
  MaterialPaintLayer *layer = add_paint_layer("Bottom", add_image("Bottom"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  MaterialPaintLayer *mask = BKE_paint_layers_correction_add(
      *ma, layer, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_IMAGE, "Mask");
  ASSERT_NE(mask, nullptr);
  Image *map = add_image("RemovedMaskMap");
  make_generate_image_mask_like(*map);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, mask, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
  ASSERT_TRUE(
      BKE_paint_layers_channel_set_image(*ma, mask, PAINT_MATERIAL_CHANNEL_BASE_COLOR, map));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_EQ(paint_layers_warm_item(*ma, *layer, WarmKind::Mask), nullptr);

  ASSERT_TRUE(BKE_paint_layers_remove(*ma, mask));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_NE(paint_layers_warm_item(*ma, *layer, WarmKind::Mask), nullptr);
}

/**
 * A real mask that took the slot and the spare that replenishes after it do not share a socket: the
 * real one keeps its own value (here 0.7), the new spare stays neutral (0).
 */
TEST_F(PaintLayersGenerateTest, replenished_spare_does_not_share_the_real_masks_socket)
{
  MaterialPaintLayer *layer = add_paint_layer("Bottom", add_image("Bottom"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  MaterialPaintLayer *mask = BKE_paint_layers_correction_add(
      *ma, layer, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_IMAGE, "Mask");
  ASSERT_NE(mask, nullptr);
  Image *map = add_image("SlotMaskMap");
  make_generate_image_mask_like(*map);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, mask, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
  ASSERT_TRUE(
      BKE_paint_layers_channel_set_image(*ma, mask, PAINT_MATERIAL_CHANNEL_BASE_COLOR, map));
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, mask, 0.7f));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_EQ(paint_layers_warm_item(*ma, *layer, WarmKind::Mask), nullptr);

  ASSERT_TRUE(BKE_paint_layers_warm_replenish(*ma));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_NE(paint_layers_warm_item(*ma, *layer, WarmKind::Mask), nullptr);
  BKE_paint_layers_values_sync(*ma);

  bNodeTree *group = layer_tree_find(*bmain, "Bottom");
  ASSERT_NE(group, nullptr);
  group->ensure_interface_cache();
  int real_sockets = 0;
  int spare_sockets = 0;
  int root_real = 0;
  for (const bNodeTreeInterfaceSocket *socket : group->interface_inputs()) {
    if (!STRPREFIX(socket->name, "Bottom Warm Mask Base Color Opacity")) {
      continue;
    }
    const float value = static_cast<const bNodeSocketValueFloat *>(socket->socket_data)->value;
    if (std::abs(value - 0.7f) < 1e-4f) {
      real_sockets++;
      const bNodeSocket *mirror = root_instance_input(socket->name);
      if (mirror != nullptr &&
          std::abs(static_cast<const bNodeSocketValueFloat *>(mirror->default_value)->value -
                   0.7f) < 1e-4f)
      {
        root_real++;
      }
    }
    else if (std::abs(value) < 1e-4f) {
      spare_sockets++;
    }
  }
  EXPECT_EQ(real_sockets, 1) << "the real mask keeps its own opacity socket";
  EXPECT_EQ(spare_sockets, 1) << "the replenished spare has its own neutral socket";
  EXPECT_EQ(root_real, 1) << "values_sync writes the real value to the root mirror";
}

/** I3: the spare's marker survives losing the runtime, and the reload rebuilds nothing. */
TEST_F(PaintLayersGenerateTest, warm_marker_is_stable_across_a_runtime_loss)
{
  MaterialPaintLayer *layer = add_paint_layer("Bottom", add_image("Bottom"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  const MaterialPaintLayer *before = paint_layers_warm_item(*ma, *layer, WarmKind::Mask);
  ASSERT_NE(before, nullptr);
  const bUUID marker_before = before->marker;

  BKE_paint_layers_generate_runtime_free(*ma);
  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  const MaterialPaintLayer *after = paint_layers_warm_item(*ma, *layer, WarmKind::Mask);
  ASSERT_NE(after, nullptr);
  EXPECT_TRUE(BLI_uuid_equal(after->marker, marker_before));
  EXPECT_FALSE(report.root_rebuilt);
  EXPECT_EQ(report.layer_groups_rebuilt, 0);
}



/* -------------------------------------------------------------------- */
/** \name Adding a mask from the UI must not recompile the shader
 *
 * A live report: adding a mask to a just-added Material row showed EEVEE's "Compilation" at once.
 * Every row kind is driven through the exact call sequence of the Outliner (see
 * `paint_layers_edit_mask_set` and `BKE_paint_layers_target_ensure_writable`), and the shader code
 * of the whole graph has to stay the same at every step.
 * \{ */

namespace {

enum class UiRowKind { Material, Paint, Fill, IsolatingFolder };

MaterialPaintLayer *ui_row_make(PaintLayersGenerateTest &t, const UiRowKind kind)
{
  Material &ma = *t.ma;
  switch (kind) {
    case UiRowKind::Material: {
      bNodeTree *shared = nullptr;
      bNodeTree *on_path = nullptr;
      Material *source = make_hash_source(*t.bmain, &shared, &on_path);
      MaterialPaintLayer *row = BKE_paint_layers_add(
          ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Row", nullptr, PaintLayerPlace::Above);
      BKE_paint_layers_set_material(ma, row, source);
      /* The layer that was just added is the active one. */
      BKE_paint_layers_active_set(ma, row->marker);
      return row;
    }
    case UiRowKind::Paint:
      return t.add_paint_layer("Row", t.add_image("RowMap"));
    case UiRowKind::Fill: {
      MaterialPaintLayer *row = BKE_paint_layers_add(
          ma, MA_PAINT_LAYER_SOURCE_CONSTANT, "Row", nullptr, PaintLayerPlace::Above);
      BKE_paint_layers_default_channels_apply(ma, *row);
      return row;
    }
    case UiRowKind::IsolatingFolder: {
      MaterialPaintLayer *child = t.add_paint_layer("Child", t.add_image("ChildMap"));
      MaterialPaintLayer *folder = group_one(ma, child);
      BKE_paint_layers_set_opacity(ma, folder, 0.5f);
      BKE_paint_layers_rename(ma, folder, "Row");
      return folder;
    }
  }
  return nullptr;
}

/**
 * \param via_correction: the Add Mask Correction verb (base, then an imageless item, its map
 * picked afterward) instead of the Add Mask verb (base, its map and the switch to an image).
 */
void ui_mask_shape_check(PaintLayersGenerateTest &t, const UiRowKind kind, const bool via_correction)
{
  Material &ma = *t.ma;
  MaterialPaintLayer *row = ui_row_make(t, kind);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*t.bmain, ma));
  const uint64_t before = code_shape_deep(*ma.paint_layers_tree);
  const int mode_before = int(BKE_paint_layers_material_mode(ma, *row));
  const auto row_group = [&]() -> bNodeTree * {
    return kind == UiRowKind::IsolatingFolder ? t.layer_tree_find_folder(*t.bmain, "Row") :
                                                t.layer_tree_find(*t.bmain, "Row");
  };
  ASSERT_NE(row_group(), nullptr);
  const uint64_t group_before = code_shape_signature(*row_group());

  const auto step = [&](const char *label, const bool shape_must_change = false) {
    PaintLayersRegenerateReport report;
    ASSERT_TRUE(BKE_paint_layers_regenerate(*t.bmain, ma, &report)) << label;
    ASSERT_NE(row_group(), nullptr) << label;
    if (shape_must_change) {
      /* Documented and unavoidable: the constant base becomes an image read (a sampler plus the
       * straighten Divide), which is new shader code. */
      EXPECT_NE(code_shape_signature(*row_group()), group_before) << label;
      return;
    }
    EXPECT_EQ(code_shape_signature(*row_group()), group_before)
        << label << ": the row's own group changed shape";
    EXPECT_EQ(code_shape_deep(*ma.paint_layers_tree), before) << label << ": the code changed";
    EXPECT_FALSE(report.root_rebuilt) << label << ": the root was rebuilt";
    EXPECT_EQ(int(BKE_paint_layers_material_mode(ma, *row)), mode_before) << label << ": mode";
  };

  MaterialPaintLayer *base = BKE_paint_layers_mask_add(ma, row, 1.0f);
  ASSERT_NE(base, nullptr);
  step("mask_add");
  Image *map = t.add_image("UiMask");
  make_generate_image_mask_like(*map);
  if (via_correction) {
    MaterialPaintLayer *item = BKE_paint_layers_correction_add(
        ma, row, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_IMAGE, "Correction");
    ASSERT_NE(item, nullptr);
    step("imageless correction");
    ASSERT_NE(BKE_paint_layers_channel_add(ma, item, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
    ASSERT_TRUE(BKE_paint_layers_channel_set_image(ma, item, PAINT_MATERIAL_CHANNEL_BASE_COLOR, map));
    step("correction map");
  }
  else {
    ASSERT_NE(BKE_paint_layers_channel_add(ma, base, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
    ASSERT_TRUE(BKE_paint_layers_channel_set_image(ma, base, PAINT_MATERIAL_CHANNEL_BASE_COLOR, map));
    BKE_paint_layers_correction_source_set(ma, base, MA_PAINT_LAYER_SOURCE_IMAGE);
    step("base map", true);
  }
}

}  // namespace

TEST_F(PaintLayersGenerateTest, ui_add_mask_material_row_keeps_the_code)
{
  ui_mask_shape_check(*this, UiRowKind::Material, false);
}
TEST_F(PaintLayersGenerateTest, ui_add_mask_paint_row_keeps_the_code)
{
  ui_mask_shape_check(*this, UiRowKind::Paint, false);
}
TEST_F(PaintLayersGenerateTest, ui_add_mask_fill_row_keeps_the_code)
{
  ui_mask_shape_check(*this, UiRowKind::Fill, false);
}
TEST_F(PaintLayersGenerateTest, ui_add_mask_isolating_folder_keeps_the_code)
{
  ui_mask_shape_check(*this, UiRowKind::IsolatingFolder, false);
}
TEST_F(PaintLayersGenerateTest, ui_add_mask_correction_material_row_keeps_the_code)
{
  ui_mask_shape_check(*this, UiRowKind::Material, true);
}
TEST_F(PaintLayersGenerateTest, ui_add_mask_correction_paint_row_keeps_the_code)
{
  ui_mask_shape_check(*this, UiRowKind::Paint, true);
}
TEST_F(PaintLayersGenerateTest, ui_add_mask_correction_fill_row_keeps_the_code)
{
  ui_mask_shape_check(*this, UiRowKind::Fill, true);
}
TEST_F(PaintLayersGenerateTest, ui_add_mask_correction_isolating_folder_keeps_the_code)
{
  ui_mask_shape_check(*this, UiRowKind::IsolatingFolder, true);
}

/** \} */

/** I1: grouping a root Paint row into a Pass Through folder and back does not change its code. */
TEST_F(PaintLayersGenerateTest, grouping_a_root_paint_row_keeps_its_code_shape)
{
  MaterialPaintLayer *row = add_paint_layer("Bottom", add_image("Bottom"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  const uint64_t before = code_shape_signature(*layer_tree_find(*bmain, "Bottom"));

  MaterialPaintLayer *folder = group_one(*ma, row);
  ASSERT_NE(folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(code_shape_signature(*layer_tree_find(*bmain, "Bottom")), before) << "grouped";

  ASSERT_TRUE(BKE_paint_layers_ungroup(*ma, folder));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(code_shape_signature(*layer_tree_find(*bmain, "Bottom")), before) << "ungrouped";
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Sampler budget
 *
 * The counter reproduces EEVEE's `gpu_node_graph.cc` rules; these tests pin its arithmetic, then
 * exercise the fallback: hidden rows are dropped first, then live rows are pinned to their bakes.
 * \{ */

TEST_F(PaintLayersGenerateTest, sampler_count_follows_the_eevee_rules)
{
  Material *source = add_principled_source("CountSource", 0.3f);
  bNodeTree &tree = *source->nodetree;
  bNode *principled = principled_of(*source);
  bNodeSocket *base = in_socket(*principled, "Base Color");
  Image *shared = add_image("Shared");
  const int baseline = BKE_paint_layers_sampler_count(*source);

  /* One image in two nodes with the same sampler state -- the second reachable through the first's
   * Vector input -- is one sampler (`gpu_node_graph.cc:507-514` dedups by image and state). */
  bNode *a = add_tex_image_node(tree, *shared, SHD_INTERP_LINEAR, SHD_PROJ_FLAT);
  bNode *b = add_tex_image_node(tree, *shared, SHD_INTERP_LINEAR, SHD_PROJ_FLAT);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  bke::node_add_link(tree, *a, *out_socket(*a, "Color"), *principled, *base);
  bke::node_add_link(tree, *b, *out_socket(*b, "Color"), *a, *in_socket(*a, "Vector"));
  EXPECT_EQ(BKE_paint_layers_sampler_count(*source), baseline + 1);

  /* Closest changes the filtering, so the same image becomes a second sampler. */
  static_cast<NodeTexImage *>(b->storage)->interpolation = SHD_INTERP_CLOSEST;
  EXPECT_EQ(BKE_paint_layers_sampler_count(*source), baseline + 2);
}

TEST_F(PaintLayersGenerateTest, sampler_count_udim_is_two)
{
  Material *source = add_principled_source("UdimSource", 0.3f);
  bNodeTree &tree = *source->nodetree;
  bNode *principled = principled_of(*source);
  Image *udim = add_image("Udim");
  udim->source = IMA_SRC_TILED;
  const int baseline = BKE_paint_layers_sampler_count(*source);
  bNode *node = add_tex_image_node(tree, *udim, SHD_INTERP_LINEAR, SHD_PROJ_FLAT);
  ASSERT_NE(node, nullptr);
  bke::node_add_link(
      tree, *node, *out_socket(*node, "Color"), *principled, *in_socket(*principled, "Base Color"));
  /* A tiled image needs its tile mapping array: one image sampler plus one mapping sampler. */
  EXPECT_EQ(BKE_paint_layers_sampler_count(*source), baseline + 2);
}

TEST_F(PaintLayersGenerateTest, sampler_count_colorband_nodes_share_one)
{
  Material *source = add_principled_source("BandSource", 0.3f);
  bNodeTree &tree = *source->nodetree;
  bNode *principled = principled_of(*source);
  bNodeSocket *base = in_socket(*principled, "Base Color");
  const int baseline = BKE_paint_layers_sampler_count(*source);

  bNode *value = bke::node_add_static_node(nullptr, tree, SH_NODE_VALUE);
  bNode *ramp = bke::node_add_static_node(nullptr, tree, SH_NODE_VALTORGB);
  bNode *curves = bke::node_add_static_node(nullptr, tree, SH_NODE_CURVE_RGB);
  ASSERT_NE(value, nullptr);
  ASSERT_NE(ramp, nullptr);
  ASSERT_NE(curves, nullptr);
  bke::node_add_link(tree, *value, *out_socket(*value, "Value"), *ramp, *in_socket(*ramp, "Fac"));
  bke::node_add_link(tree, *ramp, *out_socket(*ramp, "Color"), *curves, *in_socket(*curves, "Color"));
  bke::node_add_link(tree, *curves, *out_socket(*curves, "Color"), *principled, *base);
  /* Every colorband node shares the single per-material ramp texture: one sampler, not two. */
  EXPECT_EQ(BKE_paint_layers_sampler_count(*source), baseline + 1);
}

TEST_F(PaintLayersGenerateTest, sampler_count_ignores_muted_and_unconnected)
{
  Material *source = add_principled_source("ReachSource", 0.3f);
  bNodeTree &tree = *source->nodetree;
  bNode *principled = principled_of(*source);
  bNodeSocket *base = in_socket(*principled, "Base Color");
  const int baseline = BKE_paint_layers_sampler_count(*source);
  Image *image = add_image("Reach");
  bNode *node = add_tex_image_node(tree, *image, SHD_INTERP_LINEAR, SHD_PROJ_FLAT);
  bke::node_add_link(
      tree, *node, *out_socket(*node, "Color"), *principled, *base);
  EXPECT_EQ(BKE_paint_layers_sampler_count(*source), baseline + 1);

  /* A muted node is not compiled. */
  node->flag |= NODE_MUTED;
  EXPECT_EQ(BKE_paint_layers_sampler_count(*source), baseline);
  node->flag &= ~NODE_MUTED;

  /* A branch that reaches no output is not compiled either. */
  add_tex_image_node(tree, *add_image("Unlinked"), SHD_INTERP_LINEAR, SHD_PROJ_FLAT);
  EXPECT_EQ(BKE_paint_layers_sampler_count(*source), baseline + 1);
}

TEST_F(PaintLayersGenerateTest, sampler_budget_under_limit_leaves_modes_untouched)
{
  Material *source = add_principled_source("NoOverSource", 0.5f);
  source_set_three_image_base_color(
      *source, *add_image("NoOverA"), *add_image("NoOverB"), *add_image("NoOverC"));
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(
      *ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("NoOverBaked")));
  BKE_paint_layers_active_set(*ma, row->marker);

  BKE_paint_layers_sampler_budget_set(1000, 1000);
  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_FALSE(report.sampler_budget_exceeded);
  EXPECT_FALSE(BKE_paint_layers_material_forced_bake(*ma, *row));
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::SourceGroup);
  ASSERT_EQ(report.material_rows.size(), 1u);
  EXPECT_EQ(report.material_rows[0].refusal, PaintLayersSourceGroupRefusal::None);
}

TEST_F(PaintLayersGenerateTest, sampler_budget_cleanup_drops_hidden_pass_through)
{
  add_paint_layer("A", add_image("CleanupA"));
  MaterialPaintLayer *b = add_paint_layer("B", add_image("CleanupB"));
  MaterialPaintLayer *folder = group_one(*ma, b);
  ASSERT_NE(folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, folder, false));

  /* Without a budget the hidden folder stays: it is a value edit. */
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_NE(layer_tree_find(*bmain, "B"), nullptr);

  /* With a budget that only the visible row fits, the hidden folder and its child are dropped
   * instead of refusing the visible material. */
  /* +warm: the visible row is its map plus the two shared warm images. */
  BKE_paint_layers_sampler_budget_set(3, 3);
  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_FALSE(report.sampler_budget_exceeded);
  EXPECT_EQ(layer_tree_find(*bmain, "B"), nullptr);
  EXPECT_EQ(folder_tree_find(*bmain, "Folder"), nullptr);
  EXPECT_NE(layer_tree_find(*bmain, "A"), nullptr);
}

TEST_F(PaintLayersGenerateTest, sampler_budget_falls_back_a_live_row_to_its_bake)
{
  Material *source = add_principled_source("FallbackSource", 0.5f);
  source_set_three_image_base_color(
      *source, *add_image("FallbackA"), *add_image("FallbackB"), *add_image("FallbackC"));
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  ASSERT_NE(BKE_paint_layers_bake_struct_ensure(*row), nullptr);
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(
      *ma, *row, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("FallbackBaked")));
  BKE_paint_layers_bake_finalize(*ma, *row);
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *row));
  BKE_paint_layers_active_set(*ma, row->marker);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::SourceGroup);
  const int live_count = BKE_paint_layers_sampler_count(*ma);
  ASSERT_GT(live_count, 2);

  BKE_paint_layers_sampler_budget_set(live_count - 1, live_count);
  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));

  EXPECT_FALSE(report.sampler_budget_exceeded);
  EXPECT_TRUE(BKE_paint_layers_material_forced_bake(*ma, *row));
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Baked);
  ASSERT_EQ(report.material_rows.size(), 1u);
  EXPECT_EQ(report.material_rows[0].refusal, PaintLayersSourceGroupRefusal::TooManyTextures);
  EXPECT_LE(BKE_paint_layers_sampler_count(*ma), live_count - 1);

  /* Re-running without edits must not rebuild again: the forced set is re-derived identically. */
  bNodeTree *root = ma->paint_layers_tree;
  ASSERT_NE(root, nullptr);
  const Vector<bNode *> before = root_nodes(*root);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_TRUE(same_nodes(before, root_nodes(*root)));

  /* Raising the budget lifts the pin and the row goes live again. */
  BKE_paint_layers_sampler_budget_set(1000, 1000);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_FALSE(BKE_paint_layers_material_forced_bake(*ma, *row));
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::SourceGroup);
}

TEST_F(PaintLayersGenerateTest, sampler_budget_without_a_bake_keeps_the_graph)
{
  Material *source = add_principled_source("NoBakeSource", 0.5f);
  source_set_three_image_base_color(
      *source, *add_image("NoBakeA"), *add_image("NoBakeB"), *add_image("NoBakeC"));
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  BKE_paint_layers_active_set(*ma, row->marker);

  BKE_paint_layers_sampler_budget_set(1, 1);
  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));

  /* Nothing to fall back to: the graph is built as-is and the report carries the warning. */
  EXPECT_TRUE(report.sampler_budget_exceeded);
  EXPECT_FALSE(BKE_paint_layers_material_forced_bake(*ma, *row));
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::SourceGroup);
  ASSERT_NE(ma->paint_layers_tree, nullptr);
  ASSERT_NE(instance_find(), nullptr);
  EXPECT_NE(layer_tree_find(*bmain, "Source"), nullptr);
}

/* -------------------------------------------------------------------- */
/** \name Sampler estimate completeness, gain fallback, loop safety
 * \{ */

namespace {

/** Give \a row a Material bake with \a maps as per-channel images and \a coverage, then finalize. */
void material_bake_set(PaintLayersGenerateTest &t,
                       Material &ma,
                       MaterialPaintLayer &row,
                       const Span<Image *> maps,
                       Image *coverage)
{
  EXPECT_NE(BKE_paint_layers_bake_struct_ensure(row), nullptr);
  for (const int channel : maps.index_range()) {
    if (maps[channel] != nullptr) {
      EXPECT_TRUE(BKE_paint_layers_bake_set_map(ma, row, channel, maps[channel]));
    }
  }
  if (coverage != nullptr) {
    EXPECT_TRUE(BKE_paint_layers_bake_set_map(ma, row, -1, coverage));
  }
  BKE_paint_layers_bake_finalize(ma, row);
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(ma, row));
}

}  // namespace

/** A Material row carries the warm mask only while it is live; once it is on its maps it does not. */
TEST_F(PaintLayersGenerateTest, material_row_carries_a_warm_mask_only_while_live)
{
  bNodeTree *shared = nullptr;
  bNodeTree *on_path = nullptr;
  Material *source = make_hash_source(*bmain, &shared, &on_path);
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "MatRow", nullptr, PaintLayerPlace::Above);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  BKE_paint_layers_active_set(*ma, row->marker);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_NE(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Baked);
  EXPECT_NE(paint_layers_warm_item(*ma, *row, WarmKind::Mask), nullptr);

  /* Give the row maps and delete its source: it can only stay on those maps (Baked), so the spare
   * goes with it. */
  std::array<Image *, PAINT_MATERIAL_CHANNEL_NUM> maps{};
  maps[PAINT_MATERIAL_CHANNEL_BASE_COLOR] = add_image("RowMap");
  material_bake_set(*this, *ma, *row, maps, nullptr);
  BKE_id_delete(bmain, source);
  ASSERT_EQ(row->material, nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Baked);
  EXPECT_EQ(paint_layers_warm_item(*ma, *row, WarmKind::Mask), nullptr);
}

/**
 * Leaving the row (not deleting its source) bakes it. +warm: the spare now stays with any Material
 * row that still has a source, whatever its mode, so leaving the active spot does not change the
 * root.
 */
TEST_F(PaintLayersGenerateTest, material_row_leaving_the_active_spot_keeps_its_warm_mask)
{
  bNodeTree *shared = nullptr;
  bNodeTree *on_path = nullptr;
  Material *source = make_hash_source(*bmain, &shared, &on_path);
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "MatRowLeave", nullptr, PaintLayerPlace::Above);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  BKE_paint_layers_active_set(*ma, row->marker);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_NE(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Baked);
  EXPECT_NE(paint_layers_warm_item(*ma, *row, WarmKind::Mask), nullptr);

  /* The user moves to another row; every resolved channel gets a map, so nothing stays live and
   * the row turns Baked where it stands, spare included. */
  MaterialPaintLayer *other = add_paint_layer("OtherRow", add_image("OtherRow"));
  ASSERT_NE(other, nullptr);
  BKE_paint_layers_active_set(*ma, other->marker);
  std::array<Image *, PAINT_MATERIAL_CHANNEL_NUM> maps{};
  for (const int channel : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
    if (channel != PAINT_MATERIAL_CHANNEL_ALPHA) {
      maps[channel] = add_image("LeftRowMap");
    }
  }
  /* Alpha has no slot of its own: the coverage map is what stands in for it. */
  material_bake_set(*this, *ma, *row, maps, add_image("LeftRowCoverage"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Baked);
  EXPECT_NE(paint_layers_warm_item(*ma, *row, WarmKind::Mask), nullptr);
}

/** A Material row that was just added carries its spare from the first regeneration on. */
TEST_F(PaintLayersGenerateTest, freshly_added_material_row_needs_no_second_rebuild)
{
  bNodeTree *shared = nullptr;
  bNodeTree *on_path = nullptr;
  Material *source = make_hash_source(*bmain, &shared, &on_path);
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "FreshRow", nullptr, PaintLayerPlace::Above);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  BKE_paint_layers_active_set(*ma, row->marker);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_NE(paint_layers_warm_item(*ma, *row, WarmKind::Mask), nullptr);

  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_FALSE(report.root_rebuilt);
  EXPECT_EQ(report.layer_groups_rebuilt, 0);
}

/**
 * The estimate must equal the finished count for a Material row, and the row's mask and correction
 * maps must be part of it in every mode. Arithmetic: the live SourceGroup row embeds a wrapper of
 * three image textures (its default alpha is a constant, so the wrapper coverage adds no sampler),
 * plus its Paint-correction map and its Paint-mask map = 5. Pinned to Baked it becomes the baked
 * Base Color map, the baked coverage and the same two correction maps = 4.
 */
TEST_F(PaintLayersGenerateTest, sampler_estimate_matches_a_material_row_with_mask_and_correction)
{
  Material *source = add_principled_source("MCSource", 0.3f);
  source_set_three_image_base_color(
      *source, *add_image("MC_A"), *add_image("MC_B"), *add_image("MC_C"));
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Mat", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  material_bake_set(*this,
                    *ma,
                    *row,
                    Span<Image *>(std::array<Image *, 1>{add_image("MC_Base")}.data(), 1),
                    add_image("MC_Coverage"));

  MaterialPaintLayer *corr = BKE_paint_layers_correction_add(
      *ma, row, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_IMAGE, "C");
  ASSERT_NE(corr, nullptr);
  MaterialPaintLayerChannel *corr_rec = BKE_paint_layers_channel_add(
      *ma, corr, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(corr_rec, nullptr);
  corr_rec->image = add_image("MC_Corr");
  corr_rec->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  ASSERT_TRUE(BKE_paint_layers_correction_source_set(*ma, corr, MA_PAINT_LAYER_SOURCE_IMAGE));

  MaterialPaintLayer *mask = BKE_paint_layers_mask_add(*ma, row, 1.0f);
  ASSERT_NE(mask, nullptr);
  MaterialPaintLayerChannel *mask_rec = BKE_paint_layers_channel_add(
      *ma, mask, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(mask_rec, nullptr);
  mask_rec->image = add_image("MC_Mask");
  mask_rec->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  /* A map mask is Paint, not the constant Fill that #BKE_paint_layers_mask_add creates. */
  ASSERT_TRUE(BKE_paint_layers_correction_source_set(*ma, mask, MA_PAINT_LAYER_SOURCE_IMAGE));

  BKE_paint_layers_active_set(*ma, row->marker);
  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::SourceGroup);
  EXPECT_EQ(report.sampler_estimate, BKE_paint_layers_sampler_count(*ma));
  const int live_count = BKE_paint_layers_sampler_count(*ma);
  EXPECT_EQ(live_count, 6); /* +warm: the shared warm image is one more sampler. */

  /* A budget of live-1 forces the pin; the count becomes the four baked/correction maps and the
   * estimate follows it. */
  BKE_paint_layers_sampler_budget_set(live_count - 1, live_count);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Baked);
  EXPECT_EQ(report.sampler_estimate, BKE_paint_layers_sampler_count(*ma));
  EXPECT_EQ(BKE_paint_layers_sampler_count(*ma), 5); /* +warm: the shared warm image. */
}

/** An isolating folder with a valid bake is its maps: one baked color plus one coverage = 2. */
TEST_F(PaintLayersGenerateTest, sampler_estimate_matches_an_isolating_folder_bake)
{
  MaterialPaintLayer *child = add_paint_layer("IsChild", add_image("IsChildMap"));
  MaterialPaintLayer *folder = group_one(*ma, child);
  ASSERT_NE(folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));
  MaterialPaintLayerBake *bake = BKE_paint_layers_bake_struct_ensure(*folder);
  ASSERT_NE(bake, nullptr);
  bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR] = add_image("IsBakedColor");
  bake->coverage = add_image("IsBakedCoverage");
  uint32_t hash[2];
  BKE_paint_layers_bake_hash(*folder, hash);
  bake->hash[0] = hash[0];
  bake->hash[1] = hash[1];
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *folder));

  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_EQ(report.sampler_estimate, BKE_paint_layers_sampler_count(*ma));
  EXPECT_EQ(BKE_paint_layers_sampler_count(*ma), 2);
}

/** A Pass Through folder is inlined: only its child's map counts, one sampler. */
TEST_F(PaintLayersGenerateTest, sampler_estimate_matches_a_pass_through_folder)
{
  MaterialPaintLayer *child = add_paint_layer("PtChild", add_image("PtChildMap"));
  MaterialPaintLayer *folder = group_one(*ma, child);
  ASSERT_NE(folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));

  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_EQ(report.sampler_estimate, BKE_paint_layers_sampler_count(*ma));
  /* +warm: the child's map plus the two shared warm images (rows in Pass Through folders too). */
  EXPECT_EQ(BKE_paint_layers_sampler_count(*ma), 3);
}

/**
 * Hybrid: the live part is counted instead of the baked map it shadows. The source's Roughness is a
 * live constant and its Alpha is a constant too, so the row's baked Roughness map and the coverage
 * fallback are both not built: the row contributes no sampler. (The pre-fix estimate counted the
 * baked maps regardless, so it disagreed with the graph.)
 */
TEST_F(PaintLayersGenerateTest, sampler_estimate_matches_a_hybrid_row)
{
  Material *source = add_principled_source("HybridSource", 0.3f);
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Hybrid", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  /* The source's Alpha is the row's coverage; Alpha is outside the build default set, so without
   * this the factor falls back to the baked coverage map and adds a sampler. */
  extend_channel_set(channel_bit(PAINT_MATERIAL_CHANNEL_ALPHA));
  std::array<Image *, PAINT_MATERIAL_CHANNEL_NUM> maps{};
  maps[PAINT_MATERIAL_CHANNEL_ROUGHNESS] = add_image("HybridBaked");
  material_bake_set(*this, *ma, *row, maps, add_image("HybridCoverage"));
  BKE_paint_layers_active_set(*ma, row->marker);

  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Hybrid);
  EXPECT_EQ(report.sampler_estimate, BKE_paint_layers_sampler_count(*ma));
  /* +warm: only the shared warm image is sampled. */
  EXPECT_EQ(BKE_paint_layers_sampler_count(*ma), 1);
}

/**
 * A lone constant Fill row contributes no map of its own, but it still builds its group and the
 * warm Mask chain that chain reads the shared warm image from: the estimate must count that sampler
 * even though `leaf_participates` (the build's own predicate) is what keeps the row alive.
 */
TEST_F(PaintLayersGenerateTest, sampler_estimate_matches_a_lone_constant_fill)
{
  MaterialPaintLayer *fill = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_CONSTANT, "Fill", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(fill, nullptr);
  const float color[4] = {0.2f, 0.4f, 0.6f, 1.0f};
  ASSERT_TRUE(BKE_paint_layers_set_fill_color(*ma, fill, color));
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, fill, PAINT_MATERIAL_CHANNEL_ROUGHNESS), nullptr);

  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_EQ(report.sampler_estimate, BKE_paint_layers_sampler_count(*ma));
  /* +warm: the shared `.PL Warm` mask image, deduped across the row's wired channels. */
  EXPECT_EQ(BKE_paint_layers_sampler_count(*ma), 1);
}

/**
 * One image read by the stack and by a node in the user tree is one sampler: the keys must agree.
 */
TEST_F(PaintLayersGenerateTest, sampler_estimate_dedups_a_map_shared_with_the_user_tree)
{
  Image *shared = add_image("SharedMap");
  add_paint_layer("SharedRow", shared);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  /* +warm: the row map plus the two shared warm images. */
  ASSERT_EQ(BKE_paint_layers_sampler_count(*ma), 3);

  bNode *principled = nullptr;
  for (bNode &node : ma->nodetree->nodes) {
    if (node.type_legacy == SH_NODE_BSDF_PRINCIPLED) {
      principled = &node;
      break;
    }
  }
  ASSERT_NE(principled, nullptr);
  bNode *user_tex = add_tex_image_node(*ma->nodetree, *shared, SHD_INTERP_LINEAR, SHD_PROJ_FLAT);
  ASSERT_NE(user_tex, nullptr);
  bke::node_add_link(*ma->nodetree,
                     *user_tex,
                     *out_socket(*user_tex, "Color"),
                     *principled,
                     *in_socket(*principled, "Metallic"));

  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_EQ(report.sampler_estimate, BKE_paint_layers_sampler_count(*ma));
  /* +warm: the shared map plus the two warm images. */
  EXPECT_EQ(BKE_paint_layers_sampler_count(*ma), 3);
}

/**
 * A pin that does not lower the count is not taken: here the live wrapper is one sampler while the
 * baked maps are four, so forcing would raise the count. The row stays live and the report warns.
 */
TEST_F(PaintLayersGenerateTest, sampler_budget_keeps_a_row_when_a_pin_would_not_win)
{
  Material *source = add_principled_source("NoWinSource", 0.3f);
  source_set_three_image_base_color(
      *source, *add_image("NoWinA"), *add_image("NoWinB"), *add_image("NoWinC"));
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "NoWin", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  /* Four baked maps plus coverage (5) cost more than the three-sampler live graph. */
  std::array<Image *, PAINT_MATERIAL_CHANNEL_NUM> maps{};
  maps[PAINT_MATERIAL_CHANNEL_BASE_COLOR] = add_image("NoWinB0");
  maps[PAINT_MATERIAL_CHANNEL_ROUGHNESS] = add_image("NoWinB1");
  maps[PAINT_MATERIAL_CHANNEL_METALLIC] = add_image("NoWinB2");
  maps[PAINT_MATERIAL_CHANNEL_ALPHA] = add_image("NoWinB3");
  material_bake_set(*this, *ma, *row, maps, add_image("NoWinCov"));
  BKE_paint_layers_active_set(*ma, row->marker);

  BKE_paint_layers_sampler_budget_set(2, 4);
  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_FALSE(BKE_paint_layers_material_forced_bake(*ma, *row));
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::SourceGroup);
  EXPECT_TRUE(report.sampler_budget_exceeded);
}

/**
 * The fallback picks the row with the largest saving. Both live wrappers are three samplers; row A's
 * bake is one Base Color map (forcing leaves 1) and row B's is a Base Color map plus coverage (2).
 * So A gains 2 and B gains 1; with a budget of four, the single pin that fits is A.
 */
TEST_F(PaintLayersGenerateTest, sampler_budget_pins_the_largest_gain_first)
{
  /* Both sources are a four-sampler wrapper: a three-image Base Color chain plus a Metallic map.
   * The Metallic map keeps each row in SourceGroup even when it also has a baked Base Color map,
   * because Metallic has no baked map and so stays live-eligible. */
  auto make_source = [&](const char *name) -> Material * {
    Material *source = add_principled_source(name, 0.3f);
    char map_name[64];
    BLI_snprintf(map_name, sizeof(map_name), "%sBC0", name);
    Image *i0 = add_image(map_name);
    BLI_snprintf(map_name, sizeof(map_name), "%sBC1", name);
    Image *i1 = add_image(map_name);
    BLI_snprintf(map_name, sizeof(map_name), "%sBC2", name);
    Image *i2 = add_image(map_name);
    source_set_three_image_base_color(*source, *i0, *i1, *i2);
    bNodeTree &tree = *source->nodetree;
    bNode *principled = principled_of(*source);
    BLI_snprintf(map_name, sizeof(map_name), "%sMet", name);
    bNode *metallic = add_tex_image_node(tree, *add_image(map_name), SHD_INTERP_LINEAR, SHD_PROJ_BOX);
    bke::node_add_link(tree,
                       *metallic,
                       *out_socket(*metallic, "Color"),
                       *principled,
                       *in_socket(*principled, "Metallic"));
    return source;
  };
  auto add_row = [&](const char *name, Material *source) -> MaterialPaintLayer * {
    MaterialPaintLayer *row = BKE_paint_layers_add(
        *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, name, nullptr, PaintLayerPlace::Above);
    EXPECT_NE(row, nullptr);
    EXPECT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
    return row;
  };

  MaterialPaintLayer *row_a = add_row("GainA", make_source("GainA"));
  std::array<Image *, PAINT_MATERIAL_CHANNEL_NUM> maps_a{};
  maps_a[PAINT_MATERIAL_CHANNEL_METALLIC] = add_image("GainAMetBaked");
  material_bake_set(*this, *ma, *row_a, maps_a, nullptr);

  MaterialPaintLayer *row_b = add_row("GainB", make_source("GainB"));
  std::array<Image *, PAINT_MATERIAL_CHANNEL_NUM> maps_b{};
  maps_b[PAINT_MATERIAL_CHANNEL_BASE_COLOR] = add_image("GainBBCBaked");
  maps_b[PAINT_MATERIAL_CHANNEL_METALLIC] = add_image("GainBMetBaked");
  material_bake_set(*this, *ma, *row_b, maps_b, nullptr);

  /* Live is 8 (4 + 4). A pinned leaves 1 + 4 = 5, B pinned leaves 4 + 2 = 6. Budget 5 pins A. */
  BKE_paint_layers_sampler_budget_set(5, 8);
  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_TRUE(BKE_paint_layers_material_forced_bake(*ma, *row_a));
  EXPECT_FALSE(BKE_paint_layers_material_forced_bake(*ma, *row_b));
  EXPECT_FALSE(report.sampler_budget_exceeded);
}

/**
 * The whole point of doing the fallback before the wired set: with a budget exceeded, a hidden Pass
 * Through folder and a pinned row, a second regeneration without edits must keep the root.
 */
TEST_F(PaintLayersGenerateTest, sampler_budget_settles_without_a_rebuild_loop)
{
  Material *source = add_principled_source("LoopSource", 0.5f);
  source_set_three_image_base_color(
      *source, *add_image("LoopA"), *add_image("LoopB"), *add_image("LoopC"));
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "LoopRow", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  std::array<Image *, PAINT_MATERIAL_CHANNEL_NUM> maps{};
  maps[PAINT_MATERIAL_CHANNEL_BASE_COLOR] = add_image("LoopBaked");
  material_bake_set(*this, *ma, *row, maps, nullptr);
  BKE_paint_layers_active_set(*ma, row->marker);

  MaterialPaintLayer *hidden_child = add_paint_layer("LoopHidden", add_image("LoopHiddenMap"));
  MaterialPaintLayer *hidden_folder = group_one(*ma, hidden_child);
  ASSERT_NE(hidden_folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_folder_is_pass_through(*ma, *hidden_folder));
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, hidden_folder, false));

  /* The live estimate is the wrapper (3) plus the inlined hidden map (1); after the cleanup and the
   * pin it is the single baked Base Color map. */
  BKE_paint_layers_sampler_budget_set(2, 4);
  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_TRUE(BKE_paint_layers_material_forced_bake(*ma, *row));
  /* +warm: the baked map plus the shared warm image. */
  EXPECT_EQ(BKE_paint_layers_sampler_count(*ma), 2);

  bNodeTree *root = ma->paint_layers_tree;
  ASSERT_NE(root, nullptr);
  const Vector<bNode *> before = root_nodes(*root);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_TRUE(same_nodes(before, root_nodes(*root)));
  EXPECT_TRUE(BKE_paint_layers_material_forced_bake(*ma, *row));
}

/**
 * A pinned row is bakeable even while it is the active row; its ancestors stay live. When the source
 * is edited the row does not revive -- it stays on its stale maps until a fresh bake lands.
 */
TEST_F(PaintLayersGenerateTest, sampler_budget_pinned_active_row_is_bakeable_and_does_not_revive)
{
  Material *source = add_principled_source("PinnedSource", 0.5f);
  source_set_three_image_base_color(
      *source, *add_image("PinA"), *add_image("PinB"), *add_image("PinC"));
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "PinFolder", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "PinChild", folder, PaintLayerPlace::Into);
  ASSERT_NE(child, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, child, source));
  std::array<Image *, PAINT_MATERIAL_CHANNEL_NUM> maps{};
  maps[PAINT_MATERIAL_CHANNEL_BASE_COLOR] = add_image("PinBaked");
  material_bake_set(*this, *ma, *child, maps, nullptr);
  BKE_paint_layers_active_set(*ma, child->marker);

  BKE_paint_layers_sampler_budget_set(2, 4);
  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  ASSERT_TRUE(BKE_paint_layers_material_forced_bake(*ma, *child));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *child), PaintLayerMaterialMode::Baked);

  /* The pinned row is not shown live, so the planner may bake it even in the active chain. Its
   * ancestor (also a subtree of the active marker) stays deferred and live. */
  EXPECT_FALSE(BKE_paint_layers_bake_row_is_deferred(*ma, *child));
  EXPECT_TRUE(BKE_paint_layers_bake_row_is_deferred(*ma, *folder));

  /* Edit the source: the bake hash no longer matches, yet the row stays Baked on the stale maps. */
  BKE_paint_layers_bake_struct_ensure(*child)->hash[0] = 0;
  BKE_paint_layers_bake_struct_ensure(*child)->hash[1] = 0;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_TRUE(BKE_paint_layers_material_forced_bake(*ma, *child));
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *child), PaintLayerMaterialMode::Baked);
}

/* -------------------------------------------------------------------- */
/** \name Material row stays live until its bake is ready
 * \{ */

namespace {

/** Map every channel \a source can show onto \a row, plus coverage, and stamp the bake valid. */
void material_bake_all_channels(PaintLayersGenerateTest &t,
                                Material &ma,
                                MaterialPaintLayer &row,
                                Material &source,
                                const std::string &tag)
{
  const MaterialSourceResolve resolve = BKE_paint_material_source_resolve(&source);
  std::array<Image *, PAINT_MATERIAL_CHANNEL_NUM> maps{};
  for (int channel = 0; channel < PAINT_MATERIAL_CHANNEL_NUM; channel++) {
    /* Alpha has no slot of its own in #MaterialPaintLayerBake::images -- no bake pipeline ever
     * writes there. It is the row's #coverage, passed below, exactly as
     * #BKE_paint_layers_material_bake_apply diverts it. */
    if (channel != int(PAINT_MATERIAL_CHANNEL_ALPHA) &&
        resolve.channels[channel] != ChannelResolution::Unavailable)
    {
      maps[channel] = t.add_image((tag + std::to_string(channel)).c_str());
    }
  }
  material_bake_set(t, ma, row, maps, t.add_image((tag + "Cov").c_str()));
}

/**
 * Hand every channel \a source can show over to \a row through #BKE_paint_layers_material_bake_apply
 * -- the real entry point #material_bake_layered_rows_ensure's before_render callback and
 * #paint_material_layer.cc's hand-over call use, unlike #material_bake_all_channels above (which
 * goes through the low-level #BKE_paint_layers_bake_set_map + an explicit finalize and so cannot
 * catch a regression in #BKE_paint_layers_material_bake_apply's own finalize behaviour). Every
 * available channel is covered so the row can actually reach Baked once claims are released and it
 * is finalized -- a channel left without a map stays live forever
 * (#BKE_paint_layers_material_bake_ready), regardless of the bake hash.
 */
void material_bake_hand_over_all_channels(Main &bmain,
                                          Material &ma,
                                          MaterialPaintLayer &row,
                                          Material &source,
                                          PaintLayersGenerateTest &t,
                                          const std::string &tag)
{
  const MaterialSourceResolve resolve = BKE_paint_material_source_resolve(&source);
  Vector<int> channels;
  Vector<Image *> images;
  for (int channel = 0; channel < PAINT_MATERIAL_CHANNEL_NUM; channel++) {
    if (channel != int(PAINT_MATERIAL_CHANNEL_ALPHA) &&
        resolve.channels[channel] != ChannelResolution::Unavailable)
    {
      channels.append(channel);
      images.append(t.add_image((tag + std::to_string(channel)).c_str()));
    }
  }
  channels.append(int(PAINT_MATERIAL_CHANNEL_ALPHA));
  images.append(t.add_image((tag + "Cov").c_str()));
  BKE_paint_layers_material_bake_apply(
      bmain, ma, row, 64, channels.as_span(), images.as_span());
  /* #BKE_paint_layers_material_bake_apply diverts every #PAINT_MATERIAL_CHANNEL_ALPHA entry into
   * the row's coverage (its transparency, #layer.bake->coverage) rather than
   * #MaterialPaintLayerBake::images[PAINT_MATERIAL_CHANNEL_ALPHA], which no bake pipeline ever
   * fills for a Material row. #paint_layer_material_source_map answers Alpha from `coverage` for
   * exactly that reason, so the row can settle on Baked once every other channel lands too. */
}

/** Claim or release every map of \a row the way a running bake job does. */
void material_bake_claim(const MaterialPaintLayer &row, const bool claim)
{
  Vector<uint32_t> uids;
  for (const Image *image : row.bake->images) {
    if (image != nullptr) {
      uids.append(image->id.session_uid);
    }
  }
  if (row.bake->coverage != nullptr) {
    uids.append(row.bake->coverage->id.session_uid);
  }
  for (const uint32_t uid : uids) {
    if (claim) {
      BKE_paint_layers_bake_image_pending_add(uid);
    }
    else {
      BKE_paint_layers_bake_image_pending_remove(uid);
    }
  }
}

}  // namespace

/**
 * A Material row that is not in the active chain stays live while its bake cannot be shown, and goes
 * to its maps once, when they have landed. Covers the whole edit cycle: a valid bake, an edit to the
 * source (invalid: the row comes alive, and it is still a candidate for the planner), the hand-over
 * (hash stamped, maps claimed: still live, and no longer a candidate) and the landing (one rebuild).
 */
TEST_F(PaintLayersGenerateTest, inactive_material_row_is_live_until_its_bake_lands)
{
  Material *source = add_principled_source("CycleSource", 0.5f);
  source_set_noise_base_color(*bmain, *source);
  Material *other = add_principled_source("CycleOther", 0.3f);
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "CycleX", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *next = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "CycleY", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_NE(next, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, next, other));
  material_bake_all_channels(*this, *ma, *row, *source, "CycleXMap");
  material_bake_all_channels(*this, *ma, *next, *other, "CycleYMap");

  /* The planner's own filter for a Material row: neither the mode nor the live state is in it. */
  const auto is_planner_candidate = [&]() {
    return row->bake != nullptr && row->bake->mode != MA_PAINT_LAYER_BAKE_NEVER &&
           !BKE_paint_layers_bake_is_valid(*ma, *row) &&
           !BKE_paint_layers_bake_row_is_deferred(*ma, *row);
  };
  const auto stamp = [&]() -> bNodeTree * {
    bNodeTree *group = layer_tree_find(*bmain, "CycleX");
    EXPECT_NE(group, nullptr);
    EXPECT_TRUE(group != nullptr && group_mix_sentinel_set(*group, 0.125f));
    return group;
  };

  BKE_paint_layers_active_set(*ma, next->marker);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Baked);
  EXPECT_FALSE(is_planner_candidate());
  bNodeTree *group = stamp();
  ASSERT_NE(group, nullptr);

  /* The source is edited: the bake is invalid, the row comes alive in one rebuild, and the planner
   * still sees it. */
  bNodeSocket *roughness = bke::node_find_socket(
      *principled_of(*source), SOCK_IN, "Roughness"_ustr);
  ASSERT_NE(roughness, nullptr);
  static_cast<bNodeSocketValueFloat *>(roughness->default_value)->value = 0.9f;
  ASSERT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *row));
  EXPECT_TRUE(is_planner_candidate());
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::SourceGroup);
  EXPECT_FALSE(BKE_paint_layers_bake_row_is_deferred(*ma, *row));
  EXPECT_TRUE(is_planner_candidate());
  ASSERT_EQ(layer_tree_find(*bmain, "CycleX"), group);
  EXPECT_FALSE(group_mix_sentinel_get(*group, 0.125f)) << "the row did not come alive";
  stamp();

  /* Nothing changed: the planner has not run yet, so there is nothing to rebuild. */
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_TRUE(group_mix_sentinel_get(*group, 0.125f));

  /* The hand-over: the hash is stamped and the maps claimed. Still live, and the planner has
   * nothing more to start, so the bake is started once. */
  BKE_paint_layers_bake_finalize(*ma, *row);
  material_bake_claim(*row, true);
  EXPECT_FALSE(is_planner_candidate());
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::SourceGroup);
  EXPECT_TRUE(group_mix_sentinel_get(*group, 0.125f)) << "the hand-over rebuilt the row";
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_TRUE(group_mix_sentinel_get(*group, 0.125f));

  /* The maps land: one rebuild, into Baked. */
  material_bake_claim(*row, false);
  BKE_paint_layers_tag_edited(*ma);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Baked);
  ASSERT_EQ(layer_tree_find(*bmain, "CycleX"), group);
  EXPECT_FALSE(group_mix_sentinel_get(*group, 0.125f)) << "the landing did not rebuild the row";
  stamp();
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_TRUE(group_mix_sentinel_get(*group, 0.125f)) << "a second regeneration rebuilt the row";
  EXPECT_FALSE(is_planner_candidate());
}

/**
 * Spec-25b, defect 1: a hand-over (#BKE_paint_layers_material_bake_apply's
 * #BKE_paint_layers_bake_set_map calls, as #material_bake_layered_rows_ensure's before_render does
 * on the main thread ahead of the job) attaches fresh target images to an inactive row before any
 * pixel exists behind them. It must not finalise the row on its own any more, so a job cancelled
 * before it renders anything -- claim released, hash never stamped -- leaves the row live instead of
 * showing it Baked on blank/stale maps. Regenerating again afterward, with nothing else changed,
 * must not keep rebuilding the row: the planner re-queues it only by the normal due/stale rules,
 * covered separately.
 */
TEST_F(PaintLayersGenerateTest, cancelled_bake_hand_over_leaves_row_live_not_baked)
{
  Material *source = add_principled_source("CancelSource", 0.5f);
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "CancelRow", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));

  /* Not the active row: the planner is free to bake it. */
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());
  ASSERT_FALSE(BKE_paint_layers_bake_row_is_deferred(*ma, *row));
  ASSERT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *row));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  /* A plain Principled source with no texture resolves every channel to a constant, so the row is
   * live in Hybrid (a live value, no sampler), not SourceGroup (a wrapper instance) -- either mode
   * is "live", which is all this test needs, but the assertion must match reality. */
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Hybrid);

  bNodeTree *group = layer_tree_find(*bmain, "CancelRow");
  ASSERT_NE(group, nullptr);
  ASSERT_TRUE(group_mix_sentinel_set(*group, 0.25f));

  /* Hand-over through the real entry point a material bake hands its result over with
   * (#BKE_paint_layers_material_bake_apply -- exactly what #material_bake_layered_rows_ensure's
   * before_render callback and #paint_material_layer.cc's hand-over call), the way it runs before
   * any pixel exists behind the images: claimed right after, never finalized. Before the fix this
   * function finalized on its own, which is the bug this test must catch. Every available channel
   * is covered so the row can actually reach Baked later. */
  material_bake_hand_over_all_channels(*bmain, *ma, *row, *source, *this, "CancelMap");
  material_bake_claim(*row, true);
  ASSERT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *row))
      << "BKE_paint_layers_material_bake_apply must not stamp the bake valid before the render "
         "lands -- it used to finalize the row itself";
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Hybrid)
      << "still live: the in-flight claim keeps it off its incomplete maps too";
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_TRUE(group_mix_sentinel_get(*group, 0.25f))
      << "the hand-over must not have rebuilt the row's group a second time";

  /* Cancellation: #material_bake_images_free always releases the claim, but (the fix) never
   * stamps the hash since it was never finalized -- the row cannot look valid over pixels that
   * were never written. */
  material_bake_claim(*row, false);
  BKE_paint_layers_tag_edited(*ma);
  ASSERT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *row));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Hybrid)
      << "a cancelled bake must not turn the row Baked over pixels that were never written";
  ASSERT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *row));

  /* No busy loop: an idle regeneration after the cancel, with nothing else changed, rebuilds
   * nothing further. */
  group_mix_sentinel_set(*group, 0.25f);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_TRUE(group_mix_sentinel_get(*group, 0.25f))
      << "an idle regeneration after the cancel must not keep rebuilding the row";
}

/**
 * Spec-25b, defect 2: the row's topology hash used to fold in the identity of its bake-map Image
 * data-blocks (#paint_layer_channel_image, which for a Material row is
 * #paint_layer_material_source_map -- #MaterialPaintLayer::bake::images) unconditionally, even for
 * a channel the generator shows live from the source and never reads that map for (see the build's
 * Hybrid branch, which `continue`s past #paint_layer_channel_image whenever
 * #BKE_paint_layers_material_live_constant or #BKE_paint_layers_material_live_image answers). A
 * first bake's hand-over mints brand-new Image data-blocks for a row that had none, which used to
 * change the hash and force a rebuild the graph did not need. This is the same hand-over as the
 * cancellation test above, carried through to a successful landing: at most one rebuild for the
 * hand-over (none, with the fix) plus exactly one for the landing into Baked.
 */
TEST_F(PaintLayersGenerateTest, first_bake_hand_over_does_not_force_an_extra_rebuild)
{
  Material *source = add_principled_source("FirstBakeSource", 0.4f);
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "FirstBakeRow", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));

  /* Active elsewhere from the start: the row has never been baked (no map yet), so it is live. */
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  /* A plain Principled source with no texture resolves every channel to a constant: Hybrid, not
   * SourceGroup -- both are "live", which is all this step needs. */
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Hybrid);

  bNodeTree *group = layer_tree_find(*bmain, "FirstBakeRow");
  ASSERT_NE(group, nullptr);
  ASSERT_TRUE(group_mix_sentinel_set(*group, 0.375f));

  /* First bake's hand-over through the real entry point (#BKE_paint_layers_material_bake_apply):
   * brand-new Image data-blocks land in the row's bake slots for every available channel. Each
   * channel stays live (unfinalized, then claimed), so the generator still does not reference these
   * maps -- the fix must not rebuild the row's group over their mere identity. */
  material_bake_hand_over_all_channels(*bmain, *ma, *row, *source, *this, "FirstBakeMap");
  material_bake_claim(*row, true);
  ASSERT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *row));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Hybrid);
  EXPECT_TRUE(group_mix_sentinel_get(*group, 0.375f))
      << "the first bake's hand-over rebuilt the row's group without changing what it reads";

  /* Landing: the claim is released and the render's success finalizes the bake, turning the row
   * Baked -- the one rebuild the row is owed, now that its group must read the new map. */
  material_bake_claim(*row, false);
  BKE_paint_layers_bake_finalize(*ma, *row);
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *row));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Baked);
  ASSERT_EQ(layer_tree_find(*bmain, "FirstBakeRow"), group);
  EXPECT_FALSE(group_mix_sentinel_get(*group, 0.375f)) << "landing did not rebuild the row";
  group_mix_sentinel_set(*group, 0.375f);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_TRUE(group_mix_sentinel_get(*group, 0.375f)) << "a second regeneration rebuilt the row";
}

/**
 * A row pinned onto its maps by the sampler budget outranks the new rule: with its bake invalid and
 * the active row elsewhere it is still Baked, on the maps it has. Lifting the budget lets it live.
 */
TEST_F(PaintLayersGenerateTest, sampler_pin_outranks_the_live_rule_for_an_invalid_bake)
{
  Material *source = add_principled_source("PinLiveSource", 0.5f);
  source_set_three_image_base_color(
      *source, *add_image("PinLiveA"), *add_image("PinLiveB"), *add_image("PinLiveC"));
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "PinLiveRow", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  std::array<Image *, PAINT_MATERIAL_CHANNEL_NUM> maps{};
  maps[PAINT_MATERIAL_CHANNEL_BASE_COLOR] = add_image("PinLiveBaked");
  material_bake_set(*this, *ma, *row, maps, nullptr);
  BKE_paint_layers_active_set(*ma, row->marker);

  BKE_paint_layers_sampler_budget_set(2, 4);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_TRUE(BKE_paint_layers_material_forced_bake(*ma, *row));

  /* The user leaves the row and the source is edited: invalid, not deferred, still pinned. */
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());
  bNodeSocket *roughness = bke::node_find_socket(
      *principled_of(*source), SOCK_IN, "Roughness"_ustr);
  ASSERT_NE(roughness, nullptr);
  static_cast<bNodeSocketValueFloat *>(roughness->default_value)->value = 0.8f;
  ASSERT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *row));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_TRUE(BKE_paint_layers_material_forced_bake(*ma, *row));
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Baked);

  BKE_paint_layers_sampler_budget_set(0, 0);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_FALSE(BKE_paint_layers_material_forced_bake(*ma, *row));
  EXPECT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::SourceGroup);
}

/**
 * A bake still being rendered is not a candidate for the sampler fallback: pinning the row would put
 * blank maps on screen. Once the maps land the same row is pinned.
 */
TEST_F(PaintLayersGenerateTest, sampler_fallback_waits_for_the_bake_to_land)
{
  Material *source = add_principled_source("WaitSource", 0.5f);
  source_set_three_image_base_color(
      *source, *add_image("WaitA"), *add_image("WaitB"), *add_image("WaitC"));
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "WaitRow", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  std::array<Image *, PAINT_MATERIAL_CHANNEL_NUM> maps{};
  maps[PAINT_MATERIAL_CHANNEL_BASE_COLOR] = add_image("WaitBaked");
  material_bake_set(*this, *ma, *row, maps, nullptr);
  BKE_paint_layers_active_set(*ma, row->marker);

  material_bake_claim(*row, true);
  BKE_paint_layers_sampler_budget_set(2, 4);
  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_FALSE(BKE_paint_layers_material_forced_bake(*ma, *row));
  EXPECT_TRUE(report.sampler_budget_exceeded);

  material_bake_claim(*row, false);
  BKE_paint_layers_tag_edited(*ma);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_TRUE(BKE_paint_layers_material_forced_bake(*ma, *row));
  EXPECT_FALSE(report.sampler_budget_exceeded);
}

/**
 * Part B (TZ-26): this test used to be named `hybrid_base_color_constant_edit_rebuilds_the_row_group`
 * and asserted a rebuild -- the old hash folded the live constant's own value into the row's
 * topology, on the documented grounds that #BKE_paint_layers_values_sync could not see a value
 * living on another material's node tree. TZ-26 is exactly the fix for that: the constant is now a
 * #ROLE_LIVE_CONSTANT group input, filled by #create_value_inputs and kept current by
 * #values_sync_socket via #BKE_paint_layers_material_live_constant, so moving the source's Base
 * Color must sync in place instead of rebuilding. The name and the body are rewritten together.
 */
TEST_F(PaintLayersGenerateTest, hybrid_base_color_constant_edit_syncs_the_row_group)
{
  Material *source = add_principled_source("PartBSource", 0.4f);
  add_paint_layer("PartBBottom", add_image("PartBBottom"));
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "PartB", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  BKE_paint_layers_active_set(*ma, row->marker);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::Hybrid);

  bNodeTree *root = ma->paint_layers_tree;
  const Vector<bNode *> root_before = root_nodes(*root);
  bNodeTree *group = layer_tree_find(*bmain, "PartB");
  ASSERT_NE(group, nullptr);
  ASSERT_TRUE(group_mix_sentinel_set(*group, 0.125f));

  bNodeSocket *base = bke::node_find_socket(*principled_of(*source), SOCK_IN, "Base Color"_ustr);
  ASSERT_NE(base, nullptr);
  static_cast<bNodeSocketValueRGBA *>(base->default_value)->value[0] = 0.9f;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_TRUE(group_mix_sentinel_get(*group, 0.125f)) << "Base Color is a value now, not topology";
  EXPECT_EQ(ma->paint_layers_tree, root);
  EXPECT_TRUE(same_nodes(root_before, root_nodes(*root)));

  /* The live constant is mirrored onto the root instance (A1). */
  bNodeSocket *socket = root_instance_input("PartB Base Color Source");
  ASSERT_NE(socket, nullptr);
  const float *value = static_cast<bNodeSocketValueRGBA *>(socket->default_value)->value;
  EXPECT_NEAR(value[0], 0.9f, 1e-4f);
}

/** \} */

/** Deleting a material drops its sampler runtime state, so a reused uid cannot inherit a pin. */
TEST_F(PaintLayersGenerateTest, sampler_runtime_state_is_dropped_with_its_material)
{
  Material *source = add_principled_source("CleanupSource", 0.5f);
  source_set_three_image_base_color(
      *source, *add_image("ClnA"), *add_image("ClnB"), *add_image("ClnC"));
  Material *other = BKE_material_add(bmain, "OtherLayered");
  other->paint_layers_flag |= MA_PAINT_LAYERED;
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *other, MA_PAINT_LAYER_SOURCE_MATERIAL, "ClnRow", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*other, row, source));
  std::array<Image *, PAINT_MATERIAL_CHANNEL_NUM> maps{};
  maps[PAINT_MATERIAL_CHANNEL_BASE_COLOR] = add_image("ClnBaked");
  material_bake_set(*this, *other, *row, maps, nullptr);
  BKE_paint_layers_active_set(*other, row->marker);

  BKE_paint_layers_sampler_budget_set(2, 4);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *other));
  ASSERT_TRUE(BKE_paint_layers_material_forced_bake(*other, *row));

  const uint32_t reused_uid = other->id.session_uid;
  const bUUID reused_marker = row->marker;
  BKE_id_delete(bmain, other);

  /* A fresh material with the same session uid and a row with the same marker must not report the
   * old pin: the runtime state was keyed by uid and has to have been dropped on free. */
  Material *revived = BKE_material_add(bmain, "RevivedLayered");
  revived->paint_layers_flag |= MA_PAINT_LAYERED;
  revived->id.session_uid = reused_uid;
  MaterialPaintLayer *revived_row = BKE_paint_layers_add(
      *revived, MA_PAINT_LAYER_SOURCE_MATERIAL, "ClnRow", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(revived_row, nullptr);
  revived_row->marker = reused_marker;
  EXPECT_FALSE(BKE_paint_layers_material_forced_bake(*revived, *revived_row));
}

/**
 * Spec-23: #BKE_paint_layers_material_live_status answers what the UI shows for a Material row --
 * the same function the RNA getter calls. Live while the row shows its source (SourceGroup and
 * Hybrid), Baking while it stays live only because its bake cannot be shown yet, Baked on valid
 * maps, and Refused with the wrapper refusal when the source cannot be wrapped. Every status is
 * reached through the real mode and bake predicates on real rows, not fabricated inputs.
 */
TEST_F(PaintLayersGenerateTest, material_row_live_status)
{
  /* The refusal names the UI prints after "Refused: ". */
  EXPECT_STREQ(BKE_paint_layers_source_group_refusal_name(
                   PaintLayersSourceGroupRefusal::NoPrincipled),
               "no-principled");
  EXPECT_STREQ(BKE_paint_layers_source_group_refusal_name(
                   PaintLayersSourceGroupRefusal::TooManyTextures),
               "too-many-textures");

  /* Live in SourceGroup: a Noise graph the CPU cannot reproduce, shown from the wrapper. */
  Material *noise_source = add_principled_source("StatusNoiseSource", 0.5f);
  source_set_noise_base_color(*bmain, *noise_source);
  MaterialPaintLayer *live_row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "StatusLive", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(live_row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, live_row, noise_source));
  BKE_paint_layers_active_set(*ma, live_row->marker);
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *live_row),
            PaintLayerMaterialMode::SourceGroup);
  PaintLayersSourceGroupRefusal refusal = PaintLayersSourceGroupRefusal::BuildFailed;
  EXPECT_EQ(BKE_paint_layers_material_live_status(*ma, *live_row, &refusal),
            PaintLayerMaterialLiveStatus::Live);
  EXPECT_EQ(refusal, PaintLayersSourceGroupRefusal::None);

  /* Live in Hybrid: a plain Principled resolves every channel to a constant. */
  Material *const_source = add_principled_source("StatusConstSource", 0.3f);
  MaterialPaintLayer *hybrid_row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "StatusHybrid", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(hybrid_row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, hybrid_row, const_source));
  BKE_paint_layers_active_set(*ma, hybrid_row->marker);
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *hybrid_row), PaintLayerMaterialMode::Hybrid);
  EXPECT_EQ(BKE_paint_layers_material_live_status(*ma, *hybrid_row, &refusal),
            PaintLayerMaterialLiveStatus::Live);
  EXPECT_EQ(refusal, PaintLayersSourceGroupRefusal::None);

  /* Baking: out of the active chain, handed over but still claimed by the job. */
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());
  material_bake_hand_over_all_channels(
      *bmain, *ma, *live_row, *noise_source, *this, "StatusMap");
  material_bake_claim(*live_row, true);
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *live_row),
            PaintLayerMaterialMode::SourceGroup);
  ASSERT_FALSE(BKE_paint_layers_material_bake_ready(*ma, *live_row));
  EXPECT_EQ(BKE_paint_layers_material_live_status(*ma, *live_row, &refusal),
            PaintLayerMaterialLiveStatus::Baking);
  EXPECT_EQ(refusal, PaintLayersSourceGroupRefusal::None);

  /* Baked: the claim is released and the render finalizes the bake. */
  material_bake_claim(*live_row, false);
  BKE_paint_layers_bake_finalize(*ma, *live_row);
  ASSERT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *live_row));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *live_row), PaintLayerMaterialMode::Baked);
  EXPECT_EQ(BKE_paint_layers_material_live_status(*ma, *live_row, &refusal),
            PaintLayerMaterialLiveStatus::Baked);
  EXPECT_EQ(refusal, PaintLayersSourceGroupRefusal::None);

  /* Refused: a source with no Principled cannot be wrapped, so the row keeps its maps. */
  Material *empty_source = BKE_material_add(bmain, "StatusEmptySource");
  MaterialPaintLayer *refused_row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "StatusRefused", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(refused_row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, refused_row, empty_source));
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *refused_row), PaintLayerMaterialMode::Baked);
  EXPECT_EQ(BKE_paint_layers_material_live_status(*ma, *refused_row, &refusal),
            PaintLayerMaterialLiveStatus::Refused);
  EXPECT_EQ(refusal, PaintLayersSourceGroupRefusal::NoPrincipled);
}

/**
 * A Material row whose source wrapper failed to build must report Refused/BuildFailed through the
 * Main-free status the UI reads. The failure is a defensive branch of the wrapper factory that valid
 * data cannot reach, so the test seeds the runtime record at the row level -- exactly what
 * #populate_material_rows calls when the factory refuses -- and checks the status read. Clearing the
 * record (what a successful rebuild does) returns the row to Live.
 */
TEST_F(PaintLayersGenerateTest, build_failed_row_reports_refused_without_main)
{
  Material *source = add_principled_source("BuildFailSource", 0.5f);
  source_set_noise_base_color(*bmain, *source);
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "BuildFailRow", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  BKE_paint_layers_active_set(*ma, row->marker);
  ASSERT_EQ(BKE_paint_layers_material_mode(*ma, *row), PaintLayerMaterialMode::SourceGroup);

  PaintLayersSourceGroupRefusal refusal = PaintLayersSourceGroupRefusal::None;
  EXPECT_EQ(BKE_paint_layers_material_live_status(*ma, *row, &refusal),
            PaintLayerMaterialLiveStatus::Live);

  BKE_paint_layers_source_group_build_failed_set(*ma, *row, true);
  EXPECT_TRUE(BKE_paint_layers_source_group_build_failed_get(*ma, *row));
  refusal = PaintLayersSourceGroupRefusal::None;
  EXPECT_EQ(BKE_paint_layers_material_live_status(*ma, *row, &refusal),
            PaintLayerMaterialLiveStatus::Refused);
  EXPECT_EQ(refusal, PaintLayersSourceGroupRefusal::BuildFailed);
  EXPECT_STREQ(BKE_paint_layers_source_group_refusal_name(refusal), "build-failed");

  /* Fixing the source drops the record; the status returns to Live. */
  BKE_paint_layers_source_group_build_failed_set(*ma, *row, false);
  refusal = PaintLayersSourceGroupRefusal::None;
  EXPECT_EQ(BKE_paint_layers_material_live_status(*ma, *row, &refusal),
            PaintLayerMaterialLiveStatus::Live);
  EXPECT_EQ(refusal, PaintLayersSourceGroupRefusal::None);
}

/**
 * Spec-27: freeing a material drops the generator's runtime state keyed by its `session_uid`, so a
 * reused uid cannot inherit the removed-rows set. The per-marker clear is the observable probe: it
 * reports whether the marker is still recorded.
 */
TEST_F(PaintLayersGenerateTest, removed_rows_state_is_dropped_with_its_material)
{
  /* Two disabled rows: the witness proves the reconcile recorded, the probe survives the delete.
   * Clearing consumes the marker it finds, so one marker cannot serve both roles -- and a second
   * regeneration is not guaranteed to reconcile again, so the probe must stay untouched. */
  MaterialPaintLayer *probe = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "GoneProbe", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *witness = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "GoneWitness", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(probe, nullptr);
  ASSERT_NE(witness, nullptr);
  const bUUID marker = probe->marker;
  const uint32_t uid = ma->id.session_uid;
  disable_and_age(probe);
  disable_and_age(witness);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  /* The disables were recorded. */
  EXPECT_TRUE(BKE_paint_layers_row_removed_clear(*ma, witness->marker));

  BKE_id_delete(bmain, ma);
  ma = nullptr;

  /* A fresh material reusing the uid must not inherit the probe's entry. */
  Material *revived = BKE_material_add(bmain, "RevivedLayered");
  revived->paint_layers_flag |= MA_PAINT_LAYERED;
  revived->id.session_uid = uid;
  MaterialPaintLayer *revived_row = BKE_paint_layers_add(
      *revived, MA_PAINT_LAYER_SOURCE_IMAGE, "GoneProbe", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(revived_row, nullptr);
  revived_row->marker = marker;
  EXPECT_FALSE(BKE_paint_layers_row_removed_clear(*revived, marker))
      << "a reused session_uid inherited the freed material's removed-rows entry";
}

/**
 * The runtime lives on the material that owns the description. A copy is a different owner: it
 * starts empty and never shares the original's removed-rows set, so disabling a row on one cannot
 * silently drop it from the other.
 */
TEST_F(PaintLayersGenerateTest, runtime_removed_rows_is_not_shared_with_a_copy)
{
  add_paint_layer("OnLayer", add_image("On"));
  MaterialPaintLayer *off = add_paint_layer("OffLayer", add_image("Off"));
  ASSERT_NE(off, nullptr);
  disable_and_age(off);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  ASSERT_NE(bke::paint_layers_runtime_get(*ma), nullptr);
  ASSERT_FALSE(bke::paint_layers_runtime_get(*ma)->removed_rows.is_empty());

  Material *copy = id_cast<Material *>(BKE_id_copy(bmain, &ma->id));
  ASSERT_NE(copy, nullptr);
  EXPECT_EQ(bke::paint_layers_runtime_get(*copy), nullptr);
  EXPECT_NE(bke::paint_layers_runtime_get(*ma), nullptr);
}

/** A file load drops the runtime, which is what #BKE_paint_layers_generate_runtime_free models. */
TEST_F(PaintLayersGenerateTest, generate_runtime_free_clears_the_runtime)
{
  add_paint_layer("OnLayer", add_image("On"));
  MaterialPaintLayer *off = add_paint_layer("OffLayer", add_image("Off"));
  ASSERT_NE(off, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, off, false));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_NE(bke::paint_layers_runtime_get(*ma), nullptr);

  BKE_paint_layers_generate_runtime_free(*ma);
  EXPECT_EQ(bke::paint_layers_runtime_get(*ma), nullptr);
}

/**
 * The memfile-undo preserve is a move, not a copy: the runtime ends up on the re-read ID and the
 * old one is left empty, so its free path cannot double free it. This keeps the removed-rows set
 * available after Ctrl+Z, exactly as the session-uid-keyed global map used to.
 */
TEST_F(PaintLayersGenerateTest, runtime_transfer_moves_the_state_to_the_new_id)
{
  add_paint_layer("OnLayer", add_image("On"));
  MaterialPaintLayer *off = add_paint_layer("OffLayer", add_image("Off"));
  ASSERT_NE(off, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, off, false));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  const MaterialPaintLayersRuntime *original = bke::paint_layers_runtime_get(*ma);
  ASSERT_NE(original, nullptr);

  Material *restored = id_cast<Material *>(BKE_id_copy(bmain, &ma->id));
  ASSERT_NE(restored, nullptr);
  ASSERT_EQ(bke::paint_layers_runtime_get(*restored), nullptr);

  bke::paint_layers_runtime_transfer(*restored, *ma);
  EXPECT_EQ(bke::paint_layers_runtime_get(*restored), original);
  EXPECT_EQ(bke::paint_layers_runtime_get(*ma), nullptr);
}

/**
 * Like the bake subscription, the sampler's per-material state is keyed by `session_uid`; the
 * memfile-undo preserve must re-own it, or the old ID's free path drops the re-read ID's forced
 * rows and cleanup mark.
 */
TEST_F(PaintLayersGenerateTest, sampler_runtime_survives_release_of_a_same_uid_old_id)
{
  Material *old = BKE_material_add(bmain, "SamplerUndoOld");
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      *old, MA_PAINT_LAYER_SOURCE_IMAGE, "L", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(layer, nullptr);
  const bUUID marker = layer->marker;
  bke::paint_layers::forced_bake_add(*old, marker);
  bke::paint_layers::budget_cleanup_owner_set(*old, true);
  ASSERT_TRUE(bke::paint_layers::forced_bake_contains(*old, marker));
  ASSERT_TRUE(bke::paint_layers::budget_cleanup_active(*old));

  Material *neu = BKE_material_add(bmain, "SamplerUndoNew");
  neu->id.session_uid = old->id.session_uid;
  BKE_paint_layers_sampler_state_owner_transfer(*neu, *old);

  BKE_id_delete(bmain, old);

  EXPECT_TRUE(bke::paint_layers::forced_bake_contains(*neu, marker));
  EXPECT_TRUE(bke::paint_layers::budget_cleanup_active(*neu));
}

/* -------------------------------------------------------------------- */
/** \name Paint Layers cold tier (Spec-I2)
 * \{ */

/** (a) A row hidden under the cold tier survives a rebuild: the off/on stays a value edit. */
TEST_F(PaintLayersGenerateTest, cold_tier_keeps_a_young_hidden_row)
{
  MaterialPaintLayer *row = add_paint_layer("Young", add_image("Young"));
  ASSERT_NE(row, nullptr);
  /* The mark #BKE_paint_layers_set_enabled writes is only a moment old. */
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, row, false));
  EXPECT_FALSE(BKE_paint_layers_cold_tier_poll(*ma, BLI_time_now_seconds()));
  EXPECT_GT(BKE_paint_layers_cold_tier_seconds(*ma, BLI_time_now_seconds()), 0.0);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_NE(layer_tree_find(*bmain, "Young"), nullptr);
  EXPECT_FALSE(bke::paint_layers::removed_rows_contains(*ma, row->marker));
}

/** (b) A row hidden past the cold tier is dropped by the tick and the rebuild that follows. */
TEST_F(PaintLayersGenerateTest, cold_tier_drops_a_row_hidden_past_the_tier)
{
  MaterialPaintLayer *row = add_paint_layer("Old", add_image("Old"));
  ASSERT_NE(row, nullptr);
  disable_and_age(row);

  EXPECT_LE(BKE_paint_layers_cold_tier_seconds(*ma, BLI_time_now_seconds()), 0.0);
  EXPECT_TRUE(BKE_paint_layers_cold_tier_poll(*ma, BLI_time_now_seconds()));
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(layer_tree_find(*bmain, "Old"), nullptr);
  EXPECT_TRUE(bke::paint_layers::removed_rows_contains(*ma, row->marker));
}

/** (c) Enabling a cold row brings it back with one rebuild. */
TEST_F(PaintLayersGenerateTest, cold_tier_row_returns_when_enabled)
{
  MaterialPaintLayer *row = add_paint_layer("Back", add_image("Back"));
  ASSERT_NE(row, nullptr);
  disable_and_age(row);
  ASSERT_TRUE(BKE_paint_layers_cold_tier_poll(*ma, BLI_time_now_seconds()));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_EQ(layer_tree_find(*bmain, "Back"), nullptr);

  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, row, true));
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
  /* set_enabled cleared the recorded marker, so the rebuild brings the row back. */
  EXPECT_FALSE(bke::paint_layers::removed_rows_contains(*ma, row->marker));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_NE(layer_tree_find(*bmain, "Back"), nullptr);
}

/**
 * (d) The active subtree and a Pass Through folder are never cold-tier candidates: the user is
 * working in the first, and the second keeps its children in the graph with a zero factor.
 */
TEST_F(PaintLayersGenerateTest, cold_tier_spares_the_active_subtree_and_a_pass_through_folder)
{
  MaterialPaintLayer *active = add_paint_layer("Active", add_image("Active"));
  ASSERT_NE(active, nullptr);
  BKE_paint_layers_active_set(*ma, active->marker);
  disable_and_age(active);
  EXPECT_FALSE(BKE_paint_layers_cold_tier_poll(*ma, BLI_time_now_seconds()));
  EXPECT_LT(BKE_paint_layers_cold_tier_seconds(*ma, BLI_time_now_seconds()), 0.0);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_NE(layer_tree_find(*bmain, "Active"), nullptr);

  /* Leave the active state and re-enable that row, so only the folder below is a candidate. */
  BKE_paint_layers_active_set(*ma, BLI_uuid_nil());
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, active, true));

  MaterialPaintLayer *child = add_paint_layer("PassChild", add_image("PassChild"));
  ASSERT_NE(child, nullptr);
  MaterialPaintLayer *folder = group_one(*ma, child);
  ASSERT_NE(folder, nullptr);
  ASSERT_TRUE(BKE_paint_layers_folder_is_pass_through(*ma, *folder));
  disable_and_age(folder);
  EXPECT_FALSE(BKE_paint_layers_cold_tier_poll(*ma, BLI_time_now_seconds()));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_NE(layer_tree_find(*bmain, "PassChild"), nullptr);
}

/** A mark of a deleted row, or of a row shown again behind set_enabled's back, does not linger. */
TEST_F(PaintLayersGenerateTest, cold_tier_reconcile_prunes_stale_marks)
{
  MaterialPaintLayer *gone = add_paint_layer("Gone", add_image("Gone"));
  MaterialPaintLayer *shown = add_paint_layer("Shown", add_image("Shown"));
  ASSERT_NE(gone, nullptr);
  ASSERT_NE(shown, nullptr);
  const bUUID shown_marker = shown->marker;
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, gone, false));
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, shown, false));
  ASSERT_EQ(bke::paint_layers_runtime_get(*ma)->hidden_since.size(), 2);

  ASSERT_TRUE(BKE_paint_layers_remove(*ma, gone));
  shown->flag |= MA_PAINT_LAYER_ENABLED;

  ma->paint_layers_flag |= MA_PAINT_LAYERS_REGEN;
  BKE_paint_layers_root_hash_invalidate(*ma);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  const bke::MaterialPaintLayersRuntime *runtime = bke::paint_layers_runtime_get(*ma);
  ASSERT_NE(runtime, nullptr);
  EXPECT_TRUE(runtime->hidden_since.is_empty());
  EXPECT_EQ(runtime->hidden_since.lookup_ptr(shown_marker), nullptr);
}

/** A row hidden without set_enabled is young at first, then ages like any other. */
TEST_F(PaintLayersGenerateTest, cold_tier_unmarked_hidden_row_is_young_until_stamped_and_aged)
{
  MaterialPaintLayer *row = add_paint_layer("Direct", add_image("Direct"));
  ASSERT_NE(row, nullptr);
  row->flag &= ~MA_PAINT_LAYER_ENABLED;
  const double now = BLI_time_now_seconds();
  EXPECT_FALSE(BKE_paint_layers_cold_tier_poll(*ma, now));
  EXPECT_FALSE(bke::paint_layers::cold_tier_hidden_long_enough(*ma, row->marker, now));
  EXPECT_DOUBLE_EQ(BKE_paint_layers_cold_tier_seconds(*ma, now), PAINT_LAYERS_COLD_TIER_SECONDS);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_NE(layer_tree_find(*bmain, "Direct"), nullptr);
  EXPECT_FALSE(bke::paint_layers::removed_rows_contains(*ma, row->marker));
  /* The reconcile started the clock. */
  ASSERT_NE(bke::paint_layers_runtime_get(*ma), nullptr);
  ASSERT_NE(bke::paint_layers_runtime_get(*ma)->hidden_since.lookup_ptr(row->marker), nullptr);

  age_cold_mark(row);
  ASSERT_TRUE(BKE_paint_layers_cold_tier_poll(*ma, BLI_time_now_seconds()));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(layer_tree_find(*bmain, "Direct"), nullptr);
  EXPECT_TRUE(bke::paint_layers::removed_rows_contains(*ma, row->marker));
  EXPECT_EQ(bke::paint_layers_runtime_get(*ma)->hidden_since.lookup_ptr(row->marker), nullptr);
}

/** An overdue row keeps reporting zero when a younger hidden row sits next to it, in any order. */
TEST_F(PaintLayersGenerateTest, cold_tier_seconds_reports_an_overdue_row_beside_a_younger_one)
{
  MaterialPaintLayer *young_a = add_paint_layer("YoungA", add_image("YoungA"));
  MaterialPaintLayer *old = add_paint_layer("OldOne", add_image("OldOne"));
  MaterialPaintLayer *young_b = add_paint_layer("YoungB", add_image("YoungB"));
  ASSERT_NE(young_a, nullptr);
  ASSERT_NE(old, nullptr);
  ASSERT_NE(young_b, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, young_a, false));
  disable_and_age(old);
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, young_b, false));

  const double now = BLI_time_now_seconds();
  EXPECT_DOUBLE_EQ(BKE_paint_layers_cold_tier_seconds(*ma, now), 0.0);

  /* Only the young rows left: a positive wait, below the full tier. */
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, old, true));
  const double wait = BKE_paint_layers_cold_tier_seconds(*ma, BLI_time_now_seconds());
  EXPECT_GT(wait, 0.0);
  EXPECT_LE(wait, PAINT_LAYERS_COLD_TIER_SECONDS);

  /* No candidate at all reads -1, not zero. */
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, young_a, true));
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, young_b, true));
  EXPECT_DOUBLE_EQ(BKE_paint_layers_cold_tier_seconds(*ma, BLI_time_now_seconds()), -1.0);
}

/** A row already out of the graph and still disabled stays out on every later rebuild. */
TEST_F(PaintLayersGenerateTest, cold_tier_removed_row_stays_out_across_rebuilds)
{
  add_paint_layer("Keep", add_image("Keep"));
  MaterialPaintLayer *row = add_paint_layer("Stay", add_image("Stay"));
  ASSERT_NE(row, nullptr);
  disable_and_age(row);
  ASSERT_TRUE(BKE_paint_layers_cold_tier_poll(*ma, BLI_time_now_seconds()));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_TRUE(bke::paint_layers::removed_rows_contains(*ma, row->marker));
  /* The drop forgets the mark: the row now reads as young unless the reconcile remembers it. */
  ASSERT_EQ(bke::paint_layers_runtime_get(*ma)->hidden_since.lookup_ptr(row->marker), nullptr);

  for (int pass = 0; pass < 3; pass++) {
    ma->paint_layers_flag |= MA_PAINT_LAYERS_REGEN;
    BKE_paint_layers_root_hash_invalidate(*ma);
    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
    EXPECT_TRUE(bke::paint_layers::removed_rows_contains(*ma, row->marker)) << "pass " << pass;
    EXPECT_EQ(layer_tree_find(*bmain, "Stay"), nullptr) << "pass " << pass;
    EXPECT_NE(layer_tree_find(*bmain, "Keep"), nullptr) << "pass " << pass;
  }
}

/** Two hidden rows of different age: the old one drops first, the young one when it has aged too. */
TEST_F(PaintLayersGenerateTest, cold_tier_rows_of_different_age_drop_independently)
{
  MaterialPaintLayer *old = add_paint_layer("AgedRow", add_image("AgedRow"));
  MaterialPaintLayer *young = add_paint_layer("FreshRow", add_image("FreshRow"));
  ASSERT_NE(old, nullptr);
  ASSERT_NE(young, nullptr);
  disable_and_age(old);
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, young, false));

  ASSERT_TRUE(BKE_paint_layers_cold_tier_poll(*ma, BLI_time_now_seconds()));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_TRUE(bke::paint_layers::removed_rows_contains(*ma, old->marker));
  EXPECT_FALSE(bke::paint_layers::removed_rows_contains(*ma, young->marker));
  EXPECT_EQ(layer_tree_find(*bmain, "AgedRow"), nullptr);
  EXPECT_NE(layer_tree_find(*bmain, "FreshRow"), nullptr);

  age_cold_mark(young);
  ASSERT_TRUE(BKE_paint_layers_cold_tier_poll(*ma, BLI_time_now_seconds()));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_TRUE(bke::paint_layers::removed_rows_contains(*ma, old->marker))
      << "the first dropped row came back when the second one was dropped";
  EXPECT_TRUE(bke::paint_layers::removed_rows_contains(*ma, young->marker));
  EXPECT_EQ(layer_tree_find(*bmain, "AgedRow"), nullptr);
  EXPECT_EQ(layer_tree_find(*bmain, "FreshRow"), nullptr);
}

/**
 * The user's return scenario: a row moved to the cold tier comes back on enabling, in its old
 * place, and hiding it again starts a fresh window rather than inheriting the old mark.
 */
TEST_F(PaintLayersGenerateTest, cold_tier_returned_row_keeps_its_place_and_gets_a_fresh_window)
{
  add_paint_layer("Bottom", add_image("Bottom"));
  MaterialPaintLayer *row = add_paint_layer("Middle", add_image("Middle"));
  add_paint_layer("Top", add_image("Top"));
  ASSERT_NE(row, nullptr);
  const bUUID marker = row->marker;

  auto stack_order = [&]() {
    Vector<const MaterialPaintLayer *> layers;
    BKE_paint_layers_flatten(*ma, layers);
    Vector<bUUID> order;
    for (const MaterialPaintLayer *layer : layers) {
      order.append(layer->marker);
    }
    return order;
  };

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  const Vector<bUUID> order_before = stack_order();
  uint64_t hash_before = 0;
  ASSERT_TRUE(bke::paint_layers::tree_root_hash_get(*ma->paint_layers_tree, hash_before));

  disable_and_age(row);
  ASSERT_TRUE(BKE_paint_layers_cold_tier_poll(*ma, BLI_time_now_seconds()));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ASSERT_TRUE(bke::paint_layers::removed_rows_contains(*ma, marker));
  ASSERT_EQ(layer_tree_find(*bmain, "Middle"), nullptr);

  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, row, true));
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
  EXPECT_FALSE(bke::paint_layers::removed_rows_contains(*ma, marker))
      << "row_removed_clear did not run";
  EXPECT_EQ(bke::paint_layers_runtime_get(*ma)->hidden_since.lookup_ptr(marker), nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_NE(layer_tree_find(*bmain, "Middle"), nullptr);

  const Vector<bUUID> order_after = stack_order();
  ASSERT_EQ(order_after.size(), order_before.size());
  for (const int64_t i : order_before.index_range()) {
    EXPECT_TRUE(BLI_uuid_equal(order_before[i], order_after[i])) << "row " << i << " moved";
  }
  /* Same rows in the same order give the same graph topology as before the drop. */
  uint64_t hash_after = 0;
  ASSERT_TRUE(bke::paint_layers::tree_root_hash_get(*ma->paint_layers_tree, hash_after));
  EXPECT_EQ(hash_after, hash_before);

  /* Hiding it again begins a fresh window: not due, and not the old aged mark. */
  const double hide_time = BLI_time_now_seconds();
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, row, false));
  const double *since = bke::paint_layers_runtime_get(*ma)->hidden_since.lookup_ptr(marker);
  ASSERT_NE(since, nullptr);
  EXPECT_GE(*since, hide_time - 1.0);
  EXPECT_FALSE(BKE_paint_layers_cold_tier_poll(*ma, BLI_time_now_seconds()));
  EXPECT_GT(BKE_paint_layers_cold_tier_seconds(*ma, BLI_time_now_seconds()),
            PAINT_LAYERS_COLD_TIER_SECONDS - 5.0);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_NE(layer_tree_find(*bmain, "Middle"), nullptr);
}

/** A quick off/on inside the tier neither requests a rebuild nor changes the root topology. */
TEST_F(PaintLayersGenerateTest, cold_tier_quick_toggle_does_not_rebuild_the_graph)
{
  add_paint_layer("Base", add_image("Base"));
  MaterialPaintLayer *row = add_paint_layer("Toggled", add_image("Toggled"));
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *tree = ma->paint_layers_tree;
  ASSERT_NE(tree, nullptr);
  uint64_t hash_before = 0;
  ASSERT_TRUE(bke::paint_layers::tree_root_hash_get(*tree, hash_before));
  ASSERT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);

  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, row, false));
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, row, true));
  uint64_t hash_after = 0;
  ASSERT_TRUE(bke::paint_layers::tree_root_hash_get(*tree, hash_after));
  EXPECT_EQ(hash_after, hash_before);
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0)
      << "a quick toggle of a row still in the graph must stay a value edit";
  EXPECT_FALSE(BKE_paint_layers_cold_tier_poll(*ma, BLI_time_now_seconds()));
}

/** The poll touches the REGEN flag and the stored root hash only when a row is due. */
TEST_F(PaintLayersGenerateTest, cold_tier_poll_invalidates_only_when_due)
{
  add_paint_layer("Base", add_image("Base"));
  MaterialPaintLayer *row = add_paint_layer("Polled", add_image("Polled"));
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  uint64_t stored = 0;
  ASSERT_TRUE(bke::paint_layers::tree_root_hash_get(*ma->paint_layers_tree, stored));
  ASSERT_NE(stored, 0u);
  ASSERT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);

  /* Nothing hidden, then a young hidden row: no due row, nothing touched. */
  EXPECT_FALSE(BKE_paint_layers_cold_tier_poll(*ma, BLI_time_now_seconds()));
  ASSERT_TRUE(BKE_paint_layers_set_enabled(*ma, row, false));
  ma->paint_layers_flag &= ~MA_PAINT_LAYERS_REGEN;
  EXPECT_FALSE(BKE_paint_layers_cold_tier_poll(*ma, BLI_time_now_seconds()));
  EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
  uint64_t after_idle = 0;
  ASSERT_TRUE(bke::paint_layers::tree_root_hash_get(*ma->paint_layers_tree, after_idle));
  EXPECT_EQ(after_idle, stored);

  /* A far-future "now" makes the same row due without touching the mark. */
  EXPECT_TRUE(BKE_paint_layers_cold_tier_poll(
      *ma, BLI_time_now_seconds() + PAINT_LAYERS_COLD_TIER_SECONDS + 5.0));
  EXPECT_NE(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
  uint64_t after_due = stored;
  ASSERT_TRUE(bke::paint_layers::tree_root_hash_get(*ma->paint_layers_tree, after_due));
  EXPECT_EQ(after_due, 0u);
}

/* The over-budget pass ignoring age is covered by
 * sampler_budget_cleanup_drops_hidden_pass_through. */

/** \} */

/** \} */

}  // namespace blender::bke::tests
