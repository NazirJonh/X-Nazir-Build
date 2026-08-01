/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_space_types.h"
#include "DNA_view3d_types.h"

#include "ED_image_grid.hh"
#include "testing/testing.h"

namespace blender::ed::image_grid::tests {

TEST(image_grid_owner, from_view3d_slot_dna_selects_texture_or_mask)
{
  View3D v3d;
  ImageGridOwner owner = ImageGridOwner::from(v3d);

  EXPECT_EQ(&owner.slot_dna(false), &v3d.image_grid);
  EXPECT_EQ(&owner.slot_dna(true), &v3d.image_grid_mask);
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

  EXPECT_EQ(&owner.slot_dna(false), &sima.image_grid);
  EXPECT_EQ(&owner.slot_dna(true), &sima.image_grid_mask);
}

TEST(image_grid_owner, from_space_image_preview_size_dna_points_at_space_image_field)
{
  SpaceImage sima;
  ImageGridOwner owner = ImageGridOwner::from(sima);

  EXPECT_EQ(&owner.preview_size_dna(), &sima.image_grid_preview_size);
}

TEST(image_grid_owner, from_space_image_runtime_state_slot_points_at_space_image_field)
{
  SpaceImage sima;
  ImageGridOwner owner = ImageGridOwner::from(sima);

  EXPECT_EQ(&owner.runtime_state_slot(), &sima.image_grid_runtime);
}

TEST(image_grid_owner, from_space_image_identity_is_the_space_image_address)
{
  SpaceImage sima;
  ImageGridOwner owner = ImageGridOwner::from(sima);

  EXPECT_EQ(owner.identity(), static_cast<const void *>(&sima));
}

}  // namespace blender::ed::image_grid::tests
