/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_space_enums.h"
#include "DNA_space_types.h"
#include "DNA_view3d_types.h"

#include "BKE_gtest_base.hh"

#include "ED_image_grid.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "testing/testing.h"

namespace blender::ed::image_grid::tests {

TEST(image_grid_owner, from_view3d_slot_dna_selects_texture_or_mask)
{
  View3D v3d;
  ImageGridOwner owner = ImageGridOwner::from(v3d);

  EXPECT_EQ(&owner.slot_dna(ImageGridSlot::Texture), &v3d.image_grid);
  EXPECT_EQ(&owner.slot_dna(ImageGridSlot::Mask), &v3d.image_grid_mask);
}

TEST(image_grid_owner, from_view3d_preview_size_dna_points_at_view3d_field)
{
  View3D v3d;
  ImageGridOwner owner = ImageGridOwner::from(v3d);

  EXPECT_EQ(&owner.preview_size_dna(), &v3d.image_grid_preview_size);
}

TEST(image_grid_owner, from_view3d_runtime_state_slot_points_at_runtime_field)
{
  View3D v3d;
  ImageGridOwner owner = ImageGridOwner::from(v3d);

  EXPECT_EQ(&owner.runtime_state_slot(), &v3d.runtime.image_grid_state);
}

TEST(image_grid_owner, from_view3d_identity_is_the_view3d_address)
{
  View3D v3d;
  ImageGridOwner owner = ImageGridOwner::from(v3d);

  EXPECT_EQ(owner.identity(), static_cast<const void *>(&v3d));
}

TEST(image_grid_owner, from_space_image_slot_dna_selects_texture_or_mask)
{
  SpaceImage sima;
  ImageGridOwner owner = ImageGridOwner::from(sima);

  EXPECT_EQ(&owner.slot_dna(ImageGridSlot::Texture), &sima.image_grid);
  EXPECT_EQ(&owner.slot_dna(ImageGridSlot::Mask), &sima.image_grid_mask);
}

TEST(image_grid_owner, from_space_image_preview_size_dna_points_at_space_image_field)
{
  SpaceImage sima;
  ImageGridOwner owner = ImageGridOwner::from(sima);

  EXPECT_EQ(&owner.preview_size_dna(), &sima.image_grid_preview_size);
}

TEST(image_grid_owner, from_space_image_runtime_state_slot_points_at_runtime_field)
{
  SpaceImage sima;
  ImageGridOwner owner = ImageGridOwner::from(sima);

  EXPECT_EQ(&owner.runtime_state_slot(), &sima.runtime.image_grid_state);
}

TEST(image_grid_owner, from_space_image_identity_is_the_space_image_address)
{
  SpaceImage sima;
  ImageGridOwner owner = ImageGridOwner::from(sima);

  EXPECT_EQ(owner.identity(), static_cast<const void *>(&sima));
}

TEST(image_grid_owner, session_id_differs_by_slot_popover_and_space)
{
  View3D v3d;
  SpaceImage sima;
  ImageGridOwner view3d_owner = ImageGridOwner::from(v3d);
  ImageGridOwner image_owner = ImageGridOwner::from(sima);

  const std::string tex_sidebar = image_grid_session_id(
      view3d_owner, ImageGridSlot::Texture, false);
  const std::string mask_sidebar = image_grid_session_id(
      view3d_owner, ImageGridSlot::Mask, false);
  const std::string tex_popover = image_grid_session_id(
      view3d_owner, ImageGridSlot::Texture, true);
  const std::string image_tex_sidebar = image_grid_session_id(
      image_owner, ImageGridSlot::Texture, false);

  EXPECT_NE(tex_sidebar, mask_sidebar);
  EXPECT_NE(tex_sidebar, tex_popover);
  EXPECT_NE(tex_sidebar, image_tex_sidebar);
}

TEST(image_grid_owner, slot_from_int_clamps_unknown_values_to_texture)
{
  EXPECT_EQ(image_grid_slot_from_int(int(ImageGridSlot::Texture)), ImageGridSlot::Texture);
  EXPECT_EQ(image_grid_slot_from_int(int(ImageGridSlot::Mask)), ImageGridSlot::Mask);
  EXPECT_EQ(image_grid_slot_from_int(0), ImageGridSlot::Texture);
  EXPECT_EQ(image_grid_slot_from_int(99), ImageGridSlot::Texture);
  EXPECT_EQ(image_grid_slot_from_mask_flag(false), ImageGridSlot::Texture);
  EXPECT_EQ(image_grid_slot_from_mask_flag(true), ImageGridSlot::Mask);
}

TEST(image_grid_owner, from_space_selects_view3d_and_space_image)
{
  View3D v3d;
  SpaceImage sima;
  SpaceLink other;

  const std::optional<ImageGridOwner> view3d_owner = image_grid_owner_from_space(
      reinterpret_cast<SpaceLink *>(&v3d));
  const std::optional<ImageGridOwner> image_owner = image_grid_owner_from_space(
      reinterpret_cast<SpaceLink *>(&sima));

  ASSERT_TRUE(view3d_owner.has_value());
  ASSERT_TRUE(image_owner.has_value());
  EXPECT_EQ(view3d_owner->identity(), static_cast<const void *>(&v3d));
  EXPECT_EQ(image_owner->identity(), static_cast<const void *>(&sima));
  EXPECT_EQ(view3d_owner->as_view3d(), &v3d);
  EXPECT_EQ(image_owner->as_space_image(), &sima);

  EXPECT_FALSE(image_grid_owner_from_space(nullptr).has_value());
  other.spacetype = SPACE_PROPERTIES;
  EXPECT_FALSE(image_grid_owner_from_space(&other).has_value());
}

class ImageGridOwnerRNATest : public bke::BlenderGTestBase {};

TEST_F(ImageGridOwnerRNATest, from_rna_accepts_space_view3d_and_image_editor)
{
  View3D v3d;
  SpaceImage sima;
  const PointerRNA view3d_ptr = RNA_pointer_create_discrete(nullptr, RNA_SpaceView3D, &v3d);
  const PointerRNA image_ptr = RNA_pointer_create_discrete(nullptr, RNA_SpaceImageEditor, &sima);
  const PointerRNA empty{};

  const std::optional<ImageGridOwner> view3d_owner = image_grid_owner_from_rna(view3d_ptr);
  const std::optional<ImageGridOwner> image_owner = image_grid_owner_from_rna(image_ptr);

  ASSERT_TRUE(view3d_owner.has_value());
  ASSERT_TRUE(image_owner.has_value());
  EXPECT_EQ(view3d_owner->identity(), static_cast<const void *>(&v3d));
  EXPECT_EQ(image_owner->identity(), static_cast<const void *>(&sima));
  EXPECT_FALSE(image_grid_owner_from_rna(empty).has_value());
}

TEST_F(ImageGridOwnerRNATest, owner_rna_roundtrips_through_from_rna)
{
  View3D v3d;
  SpaceImage sima;

  const std::optional<ImageGridOwner> view3d_again = image_grid_owner_from_rna(
      ImageGridOwner::from(v3d).owner_rna());
  const std::optional<ImageGridOwner> image_again = image_grid_owner_from_rna(
      ImageGridOwner::from(sima).owner_rna());

  ASSERT_TRUE(view3d_again.has_value());
  ASSERT_TRUE(image_again.has_value());
  EXPECT_EQ(view3d_again->identity(), static_cast<const void *>(&v3d));
  EXPECT_EQ(image_again->identity(), static_cast<const void *>(&sima));
}

}  // namespace blender::ed::image_grid::tests
