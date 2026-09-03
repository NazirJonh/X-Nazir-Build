/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 */

#include "DNA_space_types.h"
#include "DNA_view3d_types.h"
#include "DNA_windowmanager_types.h"

#include "BKE_context.hh"
#include "BKE_global.hh"
#include "BKE_main.hh"
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

ImageGridOwner ImageGridOwner::from(SpaceProperties &sbuts)
{
  return ImageGridOwner(Kind::SpaceProperties, &sbuts);
}

/**
 * The single window manager of the open file, which owns the shared grid state.
 *
 * Read through #G_MAIN rather than a #bContext: the accessors below are called from layout,
 * operator and blend-write code that does not all carry a context, and there is exactly one
 * #wmWindowManager per `.blend`.
 */
static wmWindowManager &image_grid_wm()
{
  wmWindowManager *wm = static_cast<wmWindowManager *>(G_MAIN->wm.first);
  BLI_assert(wm != nullptr);
  return *wm;
}

ImageGridSlotDNA &ImageGridOwner::slot_dna(const ImageGridSlot grid_slot) const
{
  /* Shared across every host: the grid browses for the active brush's texture slot, which is
   * global, so the library, catalog filter and name-match filter are too. See
   * #wmWindowManager::image_grid. */
  wmWindowManager &wm = image_grid_wm();
  return (grid_slot == ImageGridSlot::Mask) ? wm.image_grid_mask : wm.image_grid;
}

short &ImageGridOwner::preview_size_dna() const
{
  return image_grid_wm().image_grid_preview_size;
}

void *&ImageGridOwner::runtime_state_slot() const
{
  /* One runtime cache for the shared DNA above. File-static rather than on
   * #bke::WindowManagerRuntime so the grid keeps its storage in one module; cleared by
   * #image_grid_state_remove() when the last host goes away. */
  static void *shared_state = nullptr;
  return shared_state;
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
    case Kind::SpaceProperties:
      return RNA_pointer_create_discrete(nullptr, RNA_SpaceProperties, space_);
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

SpaceProperties *ImageGridOwner::as_space_properties() const
{
  return (kind_ == Kind::SpaceProperties) ? static_cast<SpaceProperties *>(space_) : nullptr;
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
  if (space->spacetype == SPACE_PROPERTIES) {
    return ImageGridOwner::from(*reinterpret_cast<SpaceProperties *>(space));
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
  if (RNA_struct_is_a(ptr.type, RNA_SpaceProperties)) {
    return ImageGridOwner::from(*static_cast<SpaceProperties *>(ptr.data));
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
  if (SpaceProperties *sbuts = CTX_wm_space_properties(&C)) {
    return ImageGridOwner::from(*sbuts);
  }
  return std::nullopt;
}

}  // namespace blender::ed::image_grid
