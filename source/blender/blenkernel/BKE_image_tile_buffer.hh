/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file BKE_image_tile_buffer.hh
 * \ingroup bke
 *
 * The acquire-by-tile walks the paint layers and the bakes share: an image buffer held for a scope
 * instead of the paired acquire/release calls, and one visit per buffer a tiled image already
 * holds. Kept beside #BKE_image_acquire_ibuf so every caller picks the same shape.
 */

#include "BKE_image.hh"

#include "DNA_image_types.h"

#include <cstddef>

namespace blender {

/**
 * A tile buffer of \a image acquired for the scope: released exactly once on the way out, however
 * the scope is left. A null \a iuser reads the one buffer a non-tiled image carries. The buffer is
 * null when the image has none there, which the caller checks the usual way.
 */
class ImageTileBufferScope {
 public:
  ImageTileBufferScope(Image &image, const ImageUser *iuser)
  {
    ibuf_ = BKE_image_acquire_ibuf(&image, const_cast<ImageUser *>(iuser), &lock_);
    image_ = &image;
  }
  ImageTileBufferScope(const ImageTileBufferScope &) = delete;
  ImageTileBufferScope &operator=(const ImageTileBufferScope &) = delete;
  ~ImageTileBufferScope()
  {
    if (ibuf_ != nullptr) {
      BKE_image_release_ibuf(image_, ibuf_, lock_);
    }
  }

  ImBuf *get() const
  {
    return ibuf_;
  }
  ImBuf *operator->() const
  {
    return ibuf_;
  }
  ImBuf &operator*() const
  {
    return *ibuf_;
  }
  explicit operator bool() const
  {
    return ibuf_ != nullptr;
  }
  friend bool operator==(const ImageTileBufferScope &scope, std::nullptr_t)
  {
    return scope.ibuf_ == nullptr;
  }
  friend bool operator!=(const ImageTileBufferScope &scope, std::nullptr_t)
  {
    return scope.ibuf_ != nullptr;
  }

 private:
  Image *image_ = nullptr;
  ImBuf *ibuf_ = nullptr;
  void *lock_ = nullptr;
};

/**
 * Visit every buffer \a image already holds: one call per tile with a buffer, named by its UDIM
 * tile number. A non-tiled image visits its single buffer once; a tiled one skips the tiles that
 * have none rather than allocating them -- the walk of "fill what exists, leave what is not there
 * to regenerate from its own settings". \a visit returns false to stop early (a caller that found
 * what it asked for, or must refuse the rest); true keeps the walk going.
 */
template<typename Visit> void BKE_image_tile_buffers_foreach(Image &image, Visit &&visit)
{
  for (const ImageTile &tile : image.tiles) {
    ImageUser tile_user;
    BKE_imageuser_default(&tile_user);
    tile_user.tile = tile.tile_number;
    if (image.source == IMA_SRC_TILED && !BKE_image_has_ibuf(&image, &tile_user)) {
      continue;
    }
    ImageTileBufferScope buffer(image, &tile_user);
    if (buffer == nullptr) {
      continue;
    }
    if (!visit(tile.tile_number, *buffer)) {
      return;
    }
  }
}

}  // namespace blender
