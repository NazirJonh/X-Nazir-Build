/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file BKE_paint_material_composite_surface.hh
 * \ingroup bke
 *
 * The UV-to-object-space surface the 2D preview of Object/Generated Gradient rows samples. Kept
 * apart from the composite description so the rasterizer and its cache
 * (#BKE_paint_layers_gradient_surface_get) need nothing of the layer stack.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "BLI_index_range.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_vector.hh"

namespace blender {

/**
 * A UV-space grid of surface positions for one object and one UV map.
 * Used only by the 2D Image Editor preview of Object/Generated gradients.
 *
 * The rasterizer is the caller's (the Image Editor builds and caches one per object); the
 * evaluator only samples it. Sampling is nearest-texel, so a texel outside the UV islands never
 * borrows a position from a valid neighbour.
 *
 * Known limitations: only UVs in the 0..1 square are represented (no UDIM tiles), and overlapping
 * UV islands are resolved by the rasterizer's last-triangle-wins rule, which is deterministic but
 * not seam-aware.
 */
struct PaintMaterialCompositeSurface {
  int width = 0;
  int height = 0;
  /** Identity of the source: object, UV map, size and geometry revision. */
  uint64_t key = 0;
  /**
   * Monotonic content revision, bumped every time this surface is rebuilt. The map cache key stays
   * #key, but the composite caches need this to notice a rebuilt surface under an unchanged
   * identity: without it a notifier-cleared surface would be sampled through an old ImBuf.
   */
  uint64_t revision = 0;

  /**
   * Evaluated object-space position per texel. Invalid texels are zeroed; #valid is the authority
   * on whether a position means anything.
   */
  Vector<float3> positions;
  /**
   * The mesh's #CD_ORCO per texel, in the shader's -1..1 space, or empty when the mesh had no orco
   * layer. Generated coordinates are `generated_positions * 0.5 + 0.5`, which is exactly what
   * EEVEE's `Generated` output reads for a deformed mesh.
   */
  Vector<float3> generated_positions;
  /** 1 when the texel lies inside a UV island, 0 outside. Shared by both position arrays. */
  Vector<uint8_t> valid;

  /**
   * Generated-coordinate bounding box, the fallback when #generated_positions is empty. Also the
   * object's texspace, so it matches the shader's on-the-fly orco for an undeformed mesh. Invalid
   * when the object has no usable bounds.
   */
  bool generated_bbox_valid = false;
  float3 generated_bbox_min = float3(0.0f);
  float3 generated_bbox_max = float3(1.0f);

  /** Nearest-texel sample. Returns false outside the UV square, on NaN, and outside the islands. */
  bool sample(const float2 &uv, float3 &r_position) const
  {
    int64_t index;
    if (!this->texel_index(uv, index) || int64_t(positions.size()) <= index) {
      return false;
    }
    r_position = positions[index];
    return true;
  }

  /**
   * Normalize an object-space \a position into generated coordinates through the bbox.
   *
   * The fallback for a surface without #generated_positions: an undeformed mesh's orco is exactly
   * its texspace-normalized position. A flat object has a zero extent on one axis; the shader's
   * on-the-fly orco lands at 0.5 there, which keeps the whole mesh visible instead of dividing by
   * zero.
   */
  void normalize_generated(const float3 &position, float3 &r_position) const
  {
    const float3 extent = generated_bbox_max - generated_bbox_min;
    for (const int axis : IndexRange(3)) {
      r_position[axis] = (std::abs(extent[axis]) <= 1e-8f) ?
                             0.5f :
                             (position[axis] - generated_bbox_min[axis]) / extent[axis];
    }
  }

  /**
   * Sample the texel's generated coordinate.
   *
   * Prefers the stored #CD_ORCO sample, which is what EEVEE's `Generated` output reads for a
   * deformed mesh: `orco * 0.5 + 0.5`. A manual or test surface without orco falls back to
   * normalizing its object-space position through #generated_bbox_min / #generated_bbox_max.
   */
  bool sample_generated(const float2 &uv, float3 &r_position) const
  {
    int64_t index;
    if (!this->texel_index(uv, index)) {
      return false;
    }
    /* The orco grid is the primary source; only a surface without it needs the object-space
     * positions for the bbox fallback. Checking both unconditionally would reject a perfectly
     * usable orco-only surface. */
    if (index < int64_t(generated_positions.size())) {
      r_position = generated_positions[index] * 0.5f + 0.5f;
      return true;
    }
    if (!generated_bbox_valid || index >= int64_t(positions.size())) {
      return false;
    }
    this->normalize_generated(positions[index], r_position);
    return true;
  }

  /**
   * The row-major index of the island texel \a uv addresses, or false outside the UV square, on NaN
   * and outside the islands. Does not check the position arrays: callers bound the index by the
   * array they read.
   */
  bool texel_index(const float2 &uv, int64_t &r_index) const
  {
    if (width <= 0 || height <= 0 || int64_t(valid.size()) < int64_t(width) * height) {
      return false;
    }
    /* The comparisons also reject NaN. Outside [0, 1) there is no island to address. */
    if (!(uv.x >= 0.0f && uv.x < 1.0f && uv.y >= 0.0f && uv.y < 1.0f)) {
      return false;
    }
    const int x = std::min(int(uv.x * float(width)), width - 1);
    const int y = std::min(int(uv.y * float(height)), height - 1);
    r_index = int64_t(y) * width + x;
    return valid[r_index] != 0;
  }
};

}  // namespace blender
