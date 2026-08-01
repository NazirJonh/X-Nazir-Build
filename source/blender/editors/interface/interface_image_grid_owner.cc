/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 */

#include "DNA_space_types.h"
#include "DNA_view3d_types.h"

#include "BKE_context.hh"
#include "BLI_utildefines.h"

#include "ED_image_grid.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

namespace blender::ed::image_grid {

ImageGridOwner ImageGridOwner::from(View3D &v3d)
{
  return ImageGridOwner(Kind::View3D, &v3d);
}

ImageGridOwner ImageGridOwner::from(SpaceImage &sima)
{
  return ImageGridOwner(Kind::SpaceImage, &sima);
}

ImageGridSlotDNA &ImageGridOwner::slot_dna(const ImageGridSlot grid_slot) const
{
  const bool is_mask = (grid_slot == ImageGridSlot::Mask);
  switch (kind_) {
    case Kind::View3D: {
      View3D &v3d = *static_cast<View3D *>(space_);
      return is_mask ? v3d.image_grid_mask : v3d.image_grid;
    }
    case Kind::SpaceImage: {
      SpaceImage &sima = *static_cast<SpaceImage *>(space_);
      return is_mask ? sima.image_grid_mask : sima.image_grid;
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
      return static_cast<SpaceImage *>(space_)->runtime.image_grid_state;
  }
  BLI_assert_unreachable();
  return static_cast<View3D *>(space_)->runtime.image_grid_state;
}

const void *ImageGridOwner::identity() const
{
  return space_;
}

PointerRNA ImageGridOwner::owner_rna() const
{
  switch (kind_) {
    case Kind::View3D:
      return RNA_pointer_create_discrete(nullptr, RNA_SpaceView3D, space_);
    case Kind::SpaceImage:
      return RNA_pointer_create_discrete(nullptr, RNA_SpaceImageEditor, space_);
  }
  BLI_assert_unreachable();
  return PointerRNA{};
}

View3D *ImageGridOwner::as_view3d() const
{
  return (kind_ == Kind::View3D) ? static_cast<View3D *>(space_) : nullptr;
}

SpaceImage *ImageGridOwner::as_space_image() const
{
  return (kind_ == Kind::SpaceImage) ? static_cast<SpaceImage *>(space_) : nullptr;
}

std::optional<ImageGridOwner> image_grid_owner_from_space(SpaceLink *space)
{
  if (!space) {
    return std::nullopt;
  }
  if (space->spacetype == SPACE_VIEW3D) {
    return ImageGridOwner::from(*reinterpret_cast<View3D *>(space));
  }
  if (space->spacetype == SPACE_IMAGE) {
    return ImageGridOwner::from(*reinterpret_cast<SpaceImage *>(space));
  }
  return std::nullopt;
}

std::optional<ImageGridOwner> image_grid_owner_from_rna(const PointerRNA &ptr)
{
  if (ptr.data == nullptr || ptr.type == nullptr) {
    return std::nullopt;
  }
  if (RNA_struct_is_a(ptr.type, RNA_SpaceView3D)) {
    return ImageGridOwner::from(*static_cast<View3D *>(ptr.data));
  }
  if (RNA_struct_is_a(ptr.type, RNA_SpaceImageEditor)) {
    return ImageGridOwner::from(*static_cast<SpaceImage *>(ptr.data));
  }
  return std::nullopt;
}

std::optional<ImageGridOwner> image_grid_owner_from_context(const bContext &C)
{
  /* Layout/button store first: popovers and operators inherit the host the template published,
   * even when #CTX_wm_view3d / #CTX_wm_space_image is missing or belongs to another area. */
  if (std::optional<ImageGridOwner> owner = image_grid_owner_from_rna(
          CTX_data_pointer_get(&C, IMAGE_GRID_CONTEXT_OWNER_KEY)))
  {
    return owner;
  }
  if (View3D *v3d = CTX_wm_view3d(&C)) {
    return ImageGridOwner::from(*v3d);
  }
  if (SpaceImage *sima = CTX_wm_space_image(&C)) {
    return ImageGridOwner::from(*sima);
  }
  return std::nullopt;
}

}  // namespace blender::ed::image_grid
