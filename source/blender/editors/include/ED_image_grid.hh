/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editors
 *
 * Space-agnostic accessor for the compact brush-texture asset grid
 * (#template_asset_image_grid). #ImageGridOwner stands in for a specific
 * space's persisted DNA + runtime cache (currently only #View3D; #SpaceImage
 * is added in a later stage) so the rest of the grid subsystem never touches
 * #View3D directly.
 */

#pragma once

#include <optional>
#include <string>

#include "BLI_function_ref.hh"
#include "BLI_set.hh"

#include "ED_view3d.hh"

struct View3D;
struct SpaceImage;
struct ImageGridSlotDNA;
struct bContext;
struct AssetLibraryReference;

namespace blender {
struct BlendDataReader;
struct BlendWriter;
}  // namespace blender

namespace blender::ed::image_grid {

/**
 * Non-owning accessor over one space's persisted image-grid DNA and runtime
 * cache slot. Constructed via #ImageGridOwner::from(); carries no context or
 * lifetime ownership, so it is passed by value like a reference.
 */
class ImageGridOwner {
 public:
  ImageGridSlotDNA &slot_dna(bool is_mask_slot) const;
  short &preview_size_dna() const;
  /** Opaque lazy-cache anchor, equivalent to #View3D_Runtime::image_grid_state. */
  void *&runtime_state_slot() const;
  /** Stable pointer used to derive session/registry keys (currently the
   * owning space's address). */
  const void *identity() const;

  /** Concrete-space downcasts for code that must construct a space-specific #PointerRNA (RNA
   * property panels); returns null when the owner wraps a different kind. */
  View3D *as_view3d() const;
  SpaceImage *as_space_image() const;

  static ImageGridOwner from(View3D &v3d);
  static ImageGridOwner from(SpaceImage &sima);

 private:
  enum class Kind { View3D, SpaceImage };

  ImageGridOwner(Kind kind, void *space) : kind_(kind), space_(space) {}

  Kind kind_;
  void *space_;
};

std::optional<ImageGridOwner> image_grid_owner_from_context(const bContext &C);

using ed::view3d::ImageGridUIState;

ImageGridUIState &image_grid_state_get(ImageGridOwner owner, bool is_mask_slot = false);
ImageGridUIState &image_grid_state_get_from_context(const bContext &C);
bool image_grid_library_is_missing(ImageGridOwner owner, bool is_mask_slot);
void image_grid_state_remove(ImageGridOwner owner);
void image_grid_foreach_live_library_ref(ImageGridOwner owner,
                                         blender::FunctionRef<void(AssetLibraryReference &)> fn);
/**
 * Run \a fn on the active name-match map-type ID set of every runtime state this owner already has
 * (never creates one), so a map type removed or renamed in the Preferences does not stay selected
 * in a grid that is currently open.
 */
void image_grid_foreach_live_name_match_ids(
    ImageGridOwner owner, blender::FunctionRef<void(blender::Set<std::string> &)> fn);
std::string image_grid_session_id(ImageGridOwner owner, bool is_mask_slot, bool is_popover);
void image_grid_reset_scroll(ImageGridOwner owner, bool is_mask_slot);

int image_grid_effective_rows(ImageGridOwner owner, bool is_mask_slot);
int image_grid_preview_size_get(ImageGridOwner owner);
void image_grid_state_persist(ImageGridOwner owner, ImageGridUIState &state, bool is_mask_slot = false);

void image_grid_slot_dna_free(ImageGridSlotDNA &slot);
void image_grid_slot_dna_duplicate(ImageGridSlotDNA &dst, const ImageGridSlotDNA &src);
void image_grid_slot_dna_blend_read(blender::BlendDataReader *reader, ImageGridSlotDNA &slot);
void image_grid_slot_dna_blend_write(blender::BlendWriter *writer, const ImageGridSlotDNA &slot);

}  // namespace blender::ed::image_grid
