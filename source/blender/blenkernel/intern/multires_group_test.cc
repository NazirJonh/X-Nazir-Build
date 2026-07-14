/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "testing/testing.h"

#include "BKE_gtest_base.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_modifier.hh"
#include "BKE_multires.hh"
#include "BKE_scene.hh"

#include "BLI_listbase.h"

#include "DNA_modifier_types.h"
#include "DNA_object_types.h"

namespace blender::bke::tests {

class MultiresGroupTest : public bke::BlenderGTestBase {};

static MultiresModifierData *add_multires(Object *ob, const int lvl, const int totlvl)
{
  ModifierData *md = BKE_modifier_new(eModifierType_Multires);
  BLI_addtail(&ob->modifiers, md);
  MultiresModifierData *mmd = reinterpret_cast<MultiresModifierData *>(md);
  mmd->totlvl = char(totlvl);
  mmd->lvl = char(lvl);
  mmd->sculptlvl = char(lvl);
  mmd->renderlvl = char(lvl);
  return mmd;
}

TEST_F(MultiresGroupTest, level_get_set_clamps_to_totlvl)
{
  MultiresModifierData mmd{};
  mmd.totlvl = 4;

  multires_level_set(&mmd, MultiresLevelType::Sculpt, 2);
  EXPECT_EQ(multires_level_get(&mmd, MultiresLevelType::Sculpt), 2);

  multires_level_set(&mmd, MultiresLevelType::Sculpt, 99);
  EXPECT_EQ(multires_level_get(&mmd, MultiresLevelType::Sculpt), 4);

  multires_level_set(&mmd, MultiresLevelType::Sculpt, -5);
  EXPECT_EQ(multires_level_get(&mmd, MultiresLevelType::Sculpt), 0);

  multires_level_set(&mmd, MultiresLevelType::Viewport, 3);
  multires_level_set(&mmd, MultiresLevelType::Render, 1);
  EXPECT_EQ(multires_level_get(&mmd, MultiresLevelType::Viewport), 3);
  EXPECT_EQ(multires_level_get(&mmd, MultiresLevelType::Render), 1);
}

TEST_F(MultiresGroupTest, group_sync_only_matching_protects_diverged_objects)
{
  Main *bmain = BKE_main_new();
  Scene *scene = BKE_scene_add(bmain, "Scene");
  ViewLayer *view_layer = static_cast<ViewLayer *>(scene->view_layers.first);

  Object *active_ob = BKE_object_add(bmain, scene, view_layer, OB_MESH, "Active");
  Object *synced_ob = BKE_object_add(bmain, scene, view_layer, OB_MESH, "Synced");
  Object *diverged_ob = BKE_object_add(bmain, scene, view_layer, OB_MESH, "Diverged");

  add_multires(active_ob, 4, 6);
  add_multires(synced_ob, 4, 6);
  add_multires(diverged_ob, 2, 6);

  const Vector<Object *> candidates = {active_ob, synced_ob, diverged_ob};
  const Vector<Object *> changed = multires_level_group_sync(
      candidates, active_ob, MultiresLevelType::Sculpt, 4, 6, true);

  ASSERT_EQ(changed.size(), 1);
  EXPECT_EQ(changed[0], synced_ob);
  EXPECT_EQ(BKE_modifiers_findby_type(synced_ob, eModifierType_Multires) != nullptr, true);
  EXPECT_EQ(reinterpret_cast<MultiresModifierData *>(
                BKE_modifiers_findby_type(synced_ob, eModifierType_Multires))
                ->sculptlvl,
           6);
  EXPECT_EQ(reinterpret_cast<MultiresModifierData *>(
                BKE_modifiers_findby_type(diverged_ob, eModifierType_Multires))
                ->sculptlvl,
           2);

  BKE_main_free(bmain);
}

TEST_F(MultiresGroupTest, group_sync_force_updates_everyone_and_clamps_per_object)
{
  Main *bmain = BKE_main_new();
  Scene *scene = BKE_scene_add(bmain, "Scene");
  ViewLayer *view_layer = static_cast<ViewLayer *>(scene->view_layers.first);

  Object *active_ob = BKE_object_add(bmain, scene, view_layer, OB_MESH, "Active");
  Object *diverged_ob = BKE_object_add(bmain, scene, view_layer, OB_MESH, "Diverged");
  Object *low_totlvl_ob = BKE_object_add(bmain, scene, view_layer, OB_MESH, "LowTotlvl");

  add_multires(active_ob, 4, 6);
  add_multires(diverged_ob, 2, 6);
  add_multires(low_totlvl_ob, 1, 3);

  const Vector<Object *> candidates = {active_ob, diverged_ob, low_totlvl_ob};
  const Vector<Object *> changed = multires_level_group_sync(
      candidates, active_ob, MultiresLevelType::Sculpt, 4, 4, false);

  ASSERT_EQ(changed.size(), 2);
  EXPECT_EQ(reinterpret_cast<MultiresModifierData *>(
                BKE_modifiers_findby_type(diverged_ob, eModifierType_Multires))
                ->sculptlvl,
           4);
  /* Clamped to this object's own totlvl (3), not the requested 4. */
  EXPECT_EQ(reinterpret_cast<MultiresModifierData *>(
                BKE_modifiers_findby_type(low_totlvl_ob, eModifierType_Multires))
                ->sculptlvl,
           3);

  BKE_main_free(bmain);
}

TEST_F(MultiresGroupTest, group_sync_skips_objects_without_multires)
{
  Main *bmain = BKE_main_new();
  Scene *scene = BKE_scene_add(bmain, "Scene");
  ViewLayer *view_layer = static_cast<ViewLayer *>(scene->view_layers.first);

  Object *active_ob = BKE_object_add(bmain, scene, view_layer, OB_MESH, "Active");
  Object *plain_ob = BKE_object_add(bmain, scene, view_layer, OB_MESH, "Plain");
  add_multires(active_ob, 4, 6);

  const Vector<Object *> candidates = {active_ob, plain_ob};
  const Vector<Object *> changed = multires_level_group_sync(
      candidates, active_ob, MultiresLevelType::Sculpt, 4, 5, false);

  EXPECT_TRUE(changed.is_empty());

  BKE_main_free(bmain);
}

}  // namespace blender::bke::tests
