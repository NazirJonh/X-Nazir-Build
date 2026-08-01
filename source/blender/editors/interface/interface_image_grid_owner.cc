/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 */

#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_view3d_types.h"

#include "BKE_context.hh"
#include "BKE_screen.hh"
#include "BLI_utildefines.h"

#include "ED_image_grid.hh"

namespace blender::ed::image_grid {

ImageGridOwner ImageGridOwner::from(View3D &v3d)
{
  return ImageGridOwner(Kind::View3D, &v3d);
}

ImageGridOwner ImageGridOwner::from(SpaceImage &sima)
{
  return ImageGridOwner(Kind::SpaceImage, &sima);
}

ImageGridSlotDNA &ImageGridOwner::slot_dna(const bool is_mask_slot) const
{
  switch (kind_) {
    case Kind::View3D: {
      View3D &v3d = *static_cast<View3D *>(space_);
      return is_mask_slot ? v3d.image_grid_mask : v3d.image_grid;
    }
    case Kind::SpaceImage: {
      SpaceImage &sima = *static_cast<SpaceImage *>(space_);
      return is_mask_slot ? sima.image_grid_mask : sima.image_grid;
    }
  }
  BLI_assert_unreachable();
  return static_cast<View3D *>(space_)->image_grid;
}

short &ImageGridOwner::preview_size_dna() const
{
  switch (kind_) {
    case Kind::View3D:
      return static_cast<View3D *>(space_)->image_grid_preview_size;
    case Kind::SpaceImage:
      return static_cast<SpaceImage *>(space_)->image_grid_preview_size;
  }
  BLI_assert_unreachable();
  return static_cast<View3D *>(space_)->image_grid_preview_size;
}

void *&ImageGridOwner::runtime_state_slot() const
{
  switch (kind_) {
    case Kind::View3D:
      return static_cast<View3D *>(space_)->runtime.image_grid_state;
    case Kind::SpaceImage:
      return static_cast<SpaceImage *>(space_)->image_grid_runtime;
  }
  BLI_assert_unreachable();
  return static_cast<View3D *>(space_)->runtime.image_grid_state;
}

const void *ImageGridOwner::identity() const
{
  return space_;
}

View3D *ImageGridOwner::as_view3d() const
{
  return (kind_ == Kind::View3D) ? static_cast<View3D *>(space_) : nullptr;
}

SpaceImage *ImageGridOwner::as_space_image() const
{
  return (kind_ == Kind::SpaceImage) ? static_cast<SpaceImage *>(space_) : nullptr;
}

std::optional<ImageGridOwner> image_grid_owner_from_context(const bContext &C)
{
  if (View3D *v3d = CTX_wm_view3d(&C)) {
    return ImageGridOwner::from(*v3d);
  }
  if (SpaceImage *sima = CTX_wm_space_image(&C)) {
    return ImageGridOwner::from(*sima);
  }
  bScreen *screen = CTX_wm_screen(&C);
  if (!screen) {
    return std::nullopt;
  }
  if (ScrArea *area = BKE_screen_find_big_area(screen, SPACE_VIEW3D, 0)) {
    return ImageGridOwner::from(*static_cast<View3D *>(area->spacedata.first));
  }
  return std::nullopt;
}

}  // namespace blender::ed::image_grid
