/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file BKE_paint_layers_tile.hh
 * \ingroup bke
 *
 * The UDIM tile vocabulary the paint-layer composite, its caches and the bakes share: the tile
 * numbers, the context a call reads its maps through, and the tile a face's UVs cover. Kept in a
 * header of its own so a caller that only names a tile does not pull the composite types in.
 */

#include <algorithm>
#include <cmath>

#include "BLI_index_range.hh"
#include "BLI_math_base.h"
#include "BLI_math_vector_types.hh"
#include "BLI_vector.hh"

namespace blender {

struct Object;
struct PaintMaterialCompositeSurface;

/**
 * The tile a composite, slice or bake call reads its maps on: an explicit \a tile_number (any
 * number but #PAINT_TILE_NUMBER_AUTO) stands, the AUTO one falls back to the tile of \a surface's
 * UVs when the caller gave a surface, then to the painted object's own UV tile, then to the base
 * square. One function, so every caller resolves an unset tile the same way. Defined beside the
 * composite in `paint_layers_composite.cc`.
 */
int BKE_paint_layers_tile_resolve(int tile_number,
                                  const PaintMaterialCompositeSurface *surface,
                                  const Object *painted_object);

/**
 * A tile the caller leaves to the call to resolve: from the surface it was given (the UVs the
 * surface rasterized), then from the painted object's own UVs, and finally the base tile when
 * neither names one. Naming #PAINT_TILE_NUMBER_BASE instead is an explicit request for the base
 * square, which nothing overrides. See #BKE_paint_layers_tile_resolve.
 */
inline constexpr int PAINT_TILE_NUMBER_AUTO = 0;

/**
 * The first UDIM tile number. A non-tiled image reads it as its only tile, so it is the default
 * tile of every composite and bake that names no tile.
 */
inline constexpr int PAINT_TILE_NUMBER_BASE = 1001;

/**
 * The object-and-tile context a composite, slice or bake call reads its maps through: the object a
 * projected row samples its surface from, and the UDIM tile the maps are read on. One parameter
 * instead of two optional trailing ones, so a caller cannot pass one and forget the other.
 */
struct PaintLayerTileContext {
  /**
   * The object a projected row, correction or mask item of the call reads its surface from, or
   * null (the projection is then refused where one is needed).
   */
  const Object *painted_object = nullptr;
  /**
   * The UDIM tile the maps are read on; #PAINT_TILE_NUMBER_AUTO resolves from the context.
   */
  int tile_number = PAINT_TILE_NUMBER_AUTO;
};

/**
 * Append the UDIM tiles a face whose corner UVs lie within \a uv_min .. \a uv_max covers to \a
 * r_tiles, once each, in no order. The upper bound is half-open, so a face ending exactly on a
 * tile edge owns no tile past it -- the standard 0..1 layout covers the base tile alone -- and a
 * degenerate face (min == max) owns the tile its point lies in. \a r_outside_udim is set when a
 * bound reaches outside the 10 x 10 tiles UDIM can name; the tiles of the clamped range still go
 * in, so the caller can warn without losing what is bakeable.
 */
inline void BKE_paint_layers_tiles_from_uv_bounds(const float2 &uv_min,
                                                  const float2 &uv_max,
                                                  bool &r_outside_udim,
                                                  Vector<int> &r_tiles)
{
  /* A NaN or infinite bound has no tile of its own and cannot be clamped: warn about it and stop
   * before the float-to-int casts, which are undefined there. */
  if (!std::isfinite(uv_min.x) || !std::isfinite(uv_min.y) || !std::isfinite(uv_max.x) ||
      !std::isfinite(uv_max.y))
  {
    r_outside_udim = true;
    return;
  }
  if (uv_min.x < 0.0f || uv_min.y < 0.0f || uv_max.x > 10.0f || uv_max.y > 10.0f) {
    r_outside_udim = true;
  }
  /* The tiles of the half-open span [floor(min), max), clamped to what UDIM numbers. The max()
   * keeps a degenerate span on its own tile instead of losing it to the exclusive upper bound. */
  const int u_lo = clamp_i(int(std::floor(uv_min.x)), 0, 9);
  const int v_lo = clamp_i(int(std::floor(uv_min.y)), 0, 9);
  const int u_hi = clamp_i(std::max(u_lo, int(std::ceil(uv_max.x)) - 1), 0, 9);
  const int v_hi = clamp_i(std::max(v_lo, int(std::ceil(uv_max.y)) - 1), 0, 9);
  for (const int v : IndexRange(v_lo, v_hi - v_lo + 1)) {
    for (const int u : IndexRange(u_lo, u_hi - u_lo + 1)) {
      const int tile_number = PAINT_TILE_NUMBER_BASE + u + 10 * v;
      if (!r_tiles.contains(tile_number)) {
        r_tiles.append(tile_number);
      }
    }
  }
}

}  // namespace blender
