/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "sculpt_smooth_voxel.hh"

#include "BLI_array.hh"
#include "BLI_bounds.hh"
#include "BLI_math_base.hh"
#include "BLI_math_vector.hh"
#include "BLI_task.hh"
#include "BLI_vector.hh"

#include <algorithm>
#include <cmath>

namespace blender::ed::sculpt_paint::smooth {

/* Upper bound on the voxel count along one axis. The voxel size grows when this bound would be
 * exceeded, which coarsens the Gaussian approximation but keeps the requested sigma intact and
 * caps the temporary memory at a few tens of megabytes. */
static constexpr int max_dim = 64;
static constexpr int min_dim = 8;

/* Normalized convolution state. `position_sum` and `normal_sum` are weighted sums rather than
 * averages, so empty voxels next to the surface do not pull the result toward the origin: every
 * read divides by `weight` from the same cell. */
struct VoxelField {
  int3 dim = int3(0);
  float3 origin = float3(0.0f);
  float voxel_size = 0.0f;
  Array<float3> position_sum;
  Array<float3> normal_sum;
  Array<float> weight;

  int index_of(const int3 &cell) const
  {
    return (cell.z * this->dim.y + cell.y) * this->dim.x + cell.x;
  }

  int voxel_num() const
  {
    return this->dim.x * this->dim.y * this->dim.z;
  }
};

/* Continuous voxel coordinate of a world position. The field origin is the corner of cell (0,0,0),
 * and cell centers sit at integer coordinates plus one half. */
static float3 voxel_coord_of(const VoxelField &field, const float3 &position)
{
  return (position - field.origin) / field.voxel_size;
}

static int3 clamp_cell(const VoxelField &field, const int3 &cell)
{
  return math::clamp(cell, int3(0), field.dim - int3(1));
}

static VoxelField build_field(const Span<float3> positions,
                              const Span<float3> normals,
                              const BitSpan field_mask,
                              const float kernel_radius)
{
  VoxelField field;

  Bounds<float3> bounds{float3(std::numeric_limits<float>::max()),
                        float3(std::numeric_limits<float>::lowest())};
  bool any = false;
  for (const int i : positions.index_range()) {
    if (!field_mask[i]) {
      continue;
    }
    math::min_max(positions[i], bounds.min, bounds.max);
    any = true;
  }
  if (!any) {
    return field;
  }

  const float3 extent = bounds.max - bounds.min;
  const float max_extent = math::reduce_max(extent);

  /* Two voxels per sigma keep the Gaussian resolvable; the second term raises the voxel size when
   * the region would otherwise need more than `max_dim` cells along its longest axis. The budget
   * is `max_dim - 3` rather than `max_dim` so that the three cells of padding added below always
   * fit: without the margin, the longest axis could compute exactly `max_dim` cells, the `+ 3`
   * padding would then be clamped away, and #clamp_cell would fold the elements beyond the last
   * cell into it, corrupting that cell's average and pushing those elements out of range for
   * #sample_field. */
  field.voxel_size = math::max(kernel_radius * 0.5f, max_extent / float(max_dim - 3));
  /* Written as `!(x > threshold)` rather than `x < threshold` so that a NaN voxel size, which can
   * only reach here through a NaN `kernel_radius`, is rejected too: every comparison against NaN
   * is false. */
  if (!(field.voxel_size > 1e-8f)) {
    return VoxelField();
  }

  for (const int axis : IndexRange(3)) {
    const int cells = int(std::ceil(extent[axis] / field.voxel_size));
    /* Padding keeps the cubic sampler's outer taps (`base - 1` and `base + 2`) in range for border
     * elements. */
    field.dim[axis] = std::clamp(cells + 3, min_dim, max_dim);
  }
  field.origin = bounds.min - float3(field.voxel_size);

  const int voxel_num = field.voxel_num();
  field.position_sum.reinitialize(voxel_num);
  field.position_sum.fill(float3(0.0f));
  field.normal_sum.reinitialize(voxel_num);
  field.normal_sum.fill(float3(0.0f));
  field.weight.reinitialize(voxel_num);
  field.weight.fill(0.0f);

  /* Accumulated on a single thread: this is one pass of plain additions over the region, and
   * splitting it would need either atomics on floats or a full counting sort for a fraction of
   * the total cost. */
  for (const int i : positions.index_range()) {
    if (!field_mask[i]) {
      continue;
    }
    const int3 cell = clamp_cell(field, int3(voxel_coord_of(field, positions[i])));
    const int index = field.index_of(cell);
    field.position_sum[index] += positions[i];
    field.normal_sum[index] += normals[i];
    field.weight[index] += 1.0f;
  }

  return field;
}

/* Cubic B-spline basis weights for the four taps at offsets -1, 0, 1, 2 from `floor(coord)`, where
 * `t` is the fractional part. The B-spline is C2-continuous and approximating (it does not pass
 * through the samples), so reconstructing the field with it removes the slope creases that plain
 * trilinear sampling leaves at every cell boundary -- those creases print the voxel grid onto an
 * otherwise mirror-smooth surface. */
static void cubic_bspline_weights(const float t, float r_weights[4])
{
  const float t2 = t * t;
  const float t3 = t2 * t;
  r_weights[0] = (1.0f - 3.0f * t + 3.0f * t2 - t3) * (1.0f / 6.0f);
  r_weights[1] = (4.0f - 6.0f * t2 + 3.0f * t3) * (1.0f / 6.0f);
  r_weights[2] = (1.0f + 3.0f * t + 3.0f * t2 - 3.0f * t3) * (1.0f / 6.0f);
  r_weights[3] = t3 * (1.0f / 6.0f);
}

/* Reconstruct the smoothed position and normal at `position` for a vertex whose surface normal is
 * `vertex_normal`.
 *
 * Two properties matter here and both come from the weighting:
 *  - Spatial weights are a cubic B-spline (4x4x4 cells) so the reconstruction is C2-smooth and
 *    shows no voxel-grid creases.
 *  - Each cell is additionally weighted by how well its mean normal agrees with `vertex_normal`.
 *    Where two sides of thin geometry share cells their normals cancel (the mean collapses toward
 *    zero length), so such cells are excluded; a front-side vertex then reconstructs the front
 *    surface and a back-side vertex the back surface, and neither is pulled across the gap. Because
 *    cell normals vary smoothly, adjacent vertices weight the cells almost identically, so the
 *    result stays coherent everywhere except at the thin seam, which is exactly where the two
 *    sides must separate.
 *
 * \a r_confidence is the fraction of nearby surface that lies on this vertex's side: 1 on a clean
 * single-sided region, falling smoothly toward 0 as a silhouette brings the opposing side into
 * reach. The caller fades the displacement by it so the smoothing does not step at silhouettes.
 *
 * Returns false when no agreeing, occupied cell is in reach, leaving the vertex in place. */
static bool sample_field(const VoxelField &field,
                         const float3 &position,
                         const float3 &vertex_normal,
                         float3 &r_position,
                         float3 &r_normal,
                         float &r_confidence)
{
  const float3 coord = voxel_coord_of(field, position) - float3(0.5f);
  const int3 base = int3(math::floor(coord));
  const float3 frac = coord - float3(base);

  float wx[4], wy[4], wz[4];
  cubic_bspline_weights(frac.x, wx);
  cubic_bspline_weights(frac.y, wy);
  cubic_bspline_weights(frac.z, wz);

  float3 position_sum(0.0f);
  float3 normal_sum(0.0f);
  float weight_sum = 0.0f;
  /* Spatial weight of every occupied cell in reach, and of the subset that opposes this vertex's
   * side. Their ratio is the confidence below. */
  float occupied_spatial = 0.0f;
  float opposing_spatial = 0.0f;

  /* TEMP DEBUG (root-cause isolation for the Shape-mode tile/crack regression): with this on, every
   * occupied cell is included purely by its cubic B-spline spatial weight, exactly as it was before
   * the agreement/confidence weighting was added. If the tile pattern still reproduces with this
   * set, the regression predates that weighting (cubic reconstruction or the field itself); if it
   * reverts to the old hard-edged silhouette stepping instead, the agreement/confidence mechanism
   * itself is the cause. Remove this flag and the branch below once isolated. */
  constexpr bool debug_disable_agreement_weighting = true;

  for (const int dz : IndexRange(4)) {
    const int cz = base.z + dz - 1;
    if (cz < 0 || cz >= field.dim.z) {
      continue;
    }
    for (const int dy : IndexRange(4)) {
      const int cy = base.y + dy - 1;
      if (cy < 0 || cy >= field.dim.y) {
        continue;
      }
      for (const int dx : IndexRange(4)) {
        const int cx = base.x + dx - 1;
        if (cx < 0 || cx >= field.dim.x) {
          continue;
        }
        const int index = field.index_of(int3(cx, cy, cz));
        const float cell_weight = field.weight[index];
        if (cell_weight <= 0.0f) {
          continue;
        }
        const float spatial = wx[dx] * wy[dy] * wz[dz];
        occupied_spatial += spatial;

        if (debug_disable_agreement_weighting) {
          position_sum += field.position_sum[index] / cell_weight * spatial;
          normal_sum += field.normal_sum[index] * spatial;
          weight_sum += spatial;
          continue;
        }

        /* The mean normal's length measures how coherent the cell is; a cell where opposing sides
         * cancel has a near-zero length and no reliable direction. That cell, and any cell facing
         * away from this vertex, counts as opposing: it must not contribute to the reconstruction,
         * and it drives the confidence down so the displacement fades out smoothly here rather than
         * some vertices freezing while their neighbors move -- that hard split is what steps a
         * silhouette. */
        const float3 cell_normal = field.normal_sum[index];
        const float cell_normal_len = math::length(cell_normal);
        if (cell_normal_len < 1e-6f) {
          opposing_spatial += spatial;
          continue;
        }
        const float3 cell_normal_dir = cell_normal / cell_normal_len;
        const float agreement = math::dot(vertex_normal, cell_normal_dir);
        /* `opposing_spatial` must vary continuously through the point where a cell stops
         * contributing to the reconstruction (`agreement` crossing zero), matching how `w` below
         * already fades to zero there. A hard `agreement <= 0` cutoff here would leave `w`
         * continuous but jump `opposing_spatial`, and therefore `confidence`, by this cell's full
         * spatial weight the instant it crosses -- every cell boundary near a curved region (e.g. a
         * fold crest, where nearby cells' normals easily span more than 90 degrees) becomes a
         * discontinuity in the vertex's final displacement. Re-applied every full-strength stroke
         * step in a drag, that jump bakes a voxel-cell-sized crack into the surface. */
        opposing_spatial += spatial * math::clamp(-agreement, 0.0f, 1.0f);
        if (agreement <= 0.0f) {
          continue;
        }
        /* Weight each cell's average position and direction, not its raw sums, so the result
         * follows shape and normal agreement rather than vertex density. */
        const float w = spatial * agreement;
        position_sum += field.position_sum[index] / cell_weight * w;
        normal_sum += cell_normal_dir * w;
        weight_sum += w;
      }
    }
  }

  if (weight_sum < 1e-6f || occupied_spatial < 1e-6f) {
    return false;
  }
  r_position = position_sum / weight_sum;
  r_normal = normal_sum / weight_sum;
  r_confidence = math::max(0.0f, 1.0f - opposing_spatial / occupied_spatial);
  return true;
}

/* Gaussian weights derived from the actual sigma-to-voxel ratio rather than a fixed tap count, so
 * that clamping the voxel count coarsens the approximation without changing the requested scale. */
static Vector<float> gaussian_weights(const float kernel_radius, const float voxel_size)
{
  const int radius = std::clamp(int(std::ceil(2.0f * kernel_radius / voxel_size)), 1, max_dim / 2);
  const float sigma_sq_inv = 1.0f / (2.0f * kernel_radius * kernel_radius);
  Vector<float> weights;
  weights.reserve(radius + 1);
  for (const int i : IndexRange(radius + 1)) {
    const float distance = float(i) * voxel_size;
    weights.append(std::exp(-distance * distance * sigma_sq_inv));
  }
  return weights;
}

/* One separable pass along `axis`. Sums and weights travel through every pass together, which is
 * what keeps the convolution normalized across the three axes. */
static void blur_axis(const VoxelField &field,
                      const Span<float> weights,
                      const int axis,
                      const Span<float3> src_position,
                      const Span<float3> src_normal,
                      const Span<float> src_weight,
                      MutableSpan<float3> dst_position,
                      MutableSpan<float3> dst_normal,
                      MutableSpan<float> dst_weight)
{
  const int3 dim = field.dim;
  const int3 step(1, dim.x, dim.x * dim.y);

  threading::parallel_for(IndexRange(dim.z), 1, [&](const IndexRange z_range) {
    for (const int z : z_range) {
      for (const int y : IndexRange(dim.y)) {
        for (const int x : IndexRange(dim.x)) {
          const int3 cell(x, y, z);
          const int index = field.index_of(cell);

          float3 position_sum = src_position[index] * weights[0];
          float3 normal_sum = src_normal[index] * weights[0];
          float weight_sum = src_weight[index] * weights[0];

          for (const int offset : IndexRange(1, weights.size() - 1)) {
            for (const int sign : {-1, 1}) {
              const int coord = cell[axis] + sign * offset;
              if (coord < 0 || coord >= dim[axis]) {
                continue;
              }
              const int neighbor = index + sign * offset * step[axis];
              position_sum += src_position[neighbor] * weights[offset];
              normal_sum += src_normal[neighbor] * weights[offset];
              weight_sum += src_weight[neighbor] * weights[offset];
            }
          }

          dst_position[index] = position_sum;
          dst_normal[index] = normal_sum;
          dst_weight[index] = weight_sum;
        }
      }
    }
  });
}

/* Same separable pass as #blur_axis, restricted to `position_sum` and `weight`. Used by
 * #taubin_smooth_field, which reads the field back only as `position_sum / weight` and never
 * touches `normal_sum`, so this avoids computing and discarding a `normal_sum` blur on every
 * Taubin iteration. */
static void blur_axis_position_weight(const VoxelField &field,
                                      const Span<float> weights,
                                      const int axis,
                                      const Span<float3> src_position,
                                      const Span<float> src_weight,
                                      MutableSpan<float3> dst_position,
                                      MutableSpan<float> dst_weight)
{
  const int3 dim = field.dim;
  const int3 step(1, dim.x, dim.x * dim.y);

  threading::parallel_for(IndexRange(dim.z), 1, [&](const IndexRange z_range) {
    for (const int z : z_range) {
      for (const int y : IndexRange(dim.y)) {
        for (const int x : IndexRange(dim.x)) {
          const int3 cell(x, y, z);
          const int index = field.index_of(cell);

          float3 position_sum = src_position[index] * weights[0];
          float weight_sum = src_weight[index] * weights[0];

          for (const int offset : IndexRange(1, weights.size() - 1)) {
            for (const int sign : {-1, 1}) {
              const int coord = cell[axis] + sign * offset;
              if (coord < 0 || coord >= dim[axis]) {
                continue;
              }
              const int neighbor = index + sign * offset * step[axis];
              position_sum += src_position[neighbor] * weights[offset];
              weight_sum += src_weight[neighbor] * weights[offset];
            }
          }

          dst_position[index] = position_sum;
          dst_weight[index] = weight_sum;
        }
      }
    }
  });
}

static void blur_field(VoxelField &field, const float kernel_radius)
{
  const Vector<float> weights = gaussian_weights(kernel_radius, field.voxel_size);
  /* #gaussian_weights clamps its radius to at least 1, so `weights` always holds at least two
   * taps. Assert rather than early-return so that loosening the clamp in the future fails loudly
   * here instead of silently turning every blur pass below into an identity copy. */
  BLI_assert(weights.size() > 1);

  const int voxel_num = field.voxel_num();
  Array<float3> tmp_position(voxel_num);
  Array<float3> tmp_normal(voxel_num);
  Array<float> tmp_weight(voxel_num);

  for (const int axis : IndexRange(3)) {
    blur_axis(field,
              weights,
              axis,
              field.position_sum,
              field.normal_sum,
              field.weight,
              tmp_position,
              tmp_normal,
              tmp_weight);
    field.position_sum.as_mutable_span().copy_from(tmp_position);
    field.normal_sum.as_mutable_span().copy_from(tmp_normal);
    field.weight.as_mutable_span().copy_from(tmp_weight);
  }
}

/* Blurs only `position_sum` and `weight`. Used in place of #blur_field inside the Taubin
 * iterations, which never read `normal_sum`; blurring it there would compute a three-channel
 * result and discard the third channel every single time. */
static void blur_field_position_weight(VoxelField &field, const float kernel_radius)
{
  const Vector<float> weights = gaussian_weights(kernel_radius, field.voxel_size);
  BLI_assert(weights.size() > 1);

  const int voxel_num = field.voxel_num();
  Array<float3> tmp_position(voxel_num);
  Array<float> tmp_weight(voxel_num);

  for (const int axis : IndexRange(3)) {
    blur_axis_position_weight(
        field, weights, axis, field.position_sum, field.weight, tmp_position, tmp_weight);
    field.position_sum.as_mutable_span().copy_from(tmp_position);
    field.weight.as_mutable_span().copy_from(tmp_weight);
  }
}

/* Volume-preserving Taubin lambda/mu on the voxel field. `mu` is derived from `lambda` so that the
 * inflate step cancels the low-frequency loss of the shrink step: mu = lambda / (k * lambda - 1).
 * The pass count is fixed because the field is coarse enough for the iterations to be nearly free,
 * and exposing it would only duplicate what `kernel_radius` already controls. */
static void taubin_smooth_field(VoxelField &field, const float kernel_radius)
{
  constexpr float taubin_lambda = 0.5f;
  constexpr float taubin_k = 0.1f;
  constexpr float taubin_mu = taubin_lambda / (taubin_k * taubin_lambda - 1.0f);
  constexpr int pair_num = 2;

  const int voxel_num = field.voxel_num();

  /* The validity of a voxel is decided once, before any iteration: a cell that held no geometry
   * must not gain any as the positions move. */
  Array<float3> current(voxel_num);
  Array<bool> valid(voxel_num);
  threading::parallel_for(IndexRange(voxel_num), 4096, [&](const IndexRange range) {
    for (const int i : range) {
      valid[i] = field.weight[i] > 0.0f;
      current[i] = valid[i] ? field.position_sum[i] / field.weight[i] : float3(0.0f);
    }
  });

  /* The working field is built once and reused across the steps. Copying the whole field per step
   * would allocate and copy a `normal_sum` array that the Taubin iterations never read. */
  VoxelField pass;
  pass.dim = field.dim;
  pass.origin = field.origin;
  pass.voxel_size = field.voxel_size;
  pass.position_sum.reinitialize(voxel_num);
  pass.weight.reinitialize(voxel_num);

  const auto apply_step = [&](const float factor) {
    /* Every step starts from the original weights, since the blur below overwrites them in place. */
    pass.weight.as_mutable_span().copy_from(field.weight);
    threading::parallel_for(IndexRange(voxel_num), 4096, [&](const IndexRange range) {
      for (const int i : range) {
        pass.position_sum[i] = valid[i] ? current[i] * field.weight[i] : float3(0.0f);
      }
    });
    blur_field_position_weight(pass, kernel_radius);
    threading::parallel_for(IndexRange(voxel_num), 4096, [&](const IndexRange range) {
      for (const int i : range) {
        if (!valid[i] || pass.weight[i] <= 0.0f) {
          continue;
        }
        const float3 average = pass.position_sum[i] / pass.weight[i];
        current[i] += factor * (average - current[i]);
      }
    });
  };

  for ([[maybe_unused]] const int pair : IndexRange(pair_num)) {
    apply_step(taubin_lambda);
    apply_step(taubin_mu);
  }

  /* Write the result back in the weighted form the sampler expects. */
  threading::parallel_for(IndexRange(voxel_num), 4096, [&](const IndexRange range) {
    for (const int i : range) {
      field.position_sum[i] = valid[i] ? current[i] * field.weight[i] : float3(0.0f);
    }
  });
}

/* `blender::math` has no `is_finite` overload for vector types. This is defense in depth against a
 * non-finite value arriving from caller-supplied positions or normals. */
static bool is_finite(const float3 &value)
{
  return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

void voxel_smooth_positions(const Span<float3> positions,
                            const Span<float3> normals,
                            const BitSpan field_mask,
                            const BitSpan anchor_mask,
                            const VoxelSmoothParams &params,
                            const MutableSpan<float3> r_targets)
{
  BLI_assert(positions.size() == normals.size());
  BLI_assert(positions.size() == r_targets.size());

  r_targets.copy_from(positions);
  if (positions.is_empty() || params.kernel_radius < 1e-6f) {
    return;
  }

  VoxelField field = build_field(positions, normals, field_mask, params.kernel_radius);
  if (field.voxel_num() == 0) {
    return;
  }
  if (params.preserve_volume) {
    /* The Gaussian pass also spreads weight into voxels that held no geometry, which is what gives
     * the plain path its coverage. The Taubin pass leaves weights untouched, so the blurred field
     * is kept and the Taubin positions are re-expressed on top of it: the sampler then divides
     * positions and normals by the same denominator, and both modes reach the same voxels. */
    VoxelField blurred = field;
    blur_field(blurred, params.kernel_radius);
    taubin_smooth_field(field, params.kernel_radius);

    const int voxel_num = field.voxel_num();
    threading::parallel_for(IndexRange(voxel_num), 4096, [&](const IndexRange range) {
      for (const int i : range) {
        const float weight = field.weight[i];
        float3 position(0.0f);
        if (weight > 0.0f) {
          position = field.position_sum[i] / weight;
        }
        else if (blurred.weight[i] > 0.0f) {
          /* A voxel the Taubin pass never saw still carries a blurred position, which is the
           * best estimate available there. */
          position = blurred.position_sum[i] / blurred.weight[i];
        }
        blurred.position_sum[i] = position * blurred.weight[i];
      }
    });
    field = std::move(blurred);
  }
  else {
    blur_field(field, params.kernel_radius);
  }

  threading::parallel_for(positions.index_range(), 4096, [&](const IndexRange range) {
    for (const int i : range) {
      if (!field_mask[i] || anchor_mask[i]) {
        continue;
      }
      float3 smoothed_position;
      float3 smoothed_normal;
      float confidence;
      /* The vertex normal steers the reconstruction to its own side of thin geometry, so the
       * returned normal is a well-defined same-side direction rather than a collapsed mean. */
      if (!sample_field(
              field, positions[i], normals[i], smoothed_position, smoothed_normal, confidence)) {
        continue;
      }

      /* Displace only along the smoothed field normal, and use that field normal rather than the
       * per-vertex normal as the axis. Two reasons, both required:
       *
       * - Dropping the tangential component removes the sideways drift toward densely sampled
       *   regions: the field is a centroid of vertex positions weighted by vertex count, so its
       *   tangential offset follows sampling density rather than shape, and applying it would slide
       *   vertices by a distance set by the kernel radius until the surface tears.
       * - The field normal is spatially smooth, so neighboring vertices project the smooth target
       *   onto nearly the same axis and move together. Projecting onto the per-vertex normal
       *   instead scatters a coherent target along each vertex's own normal, which diverges wherever
       *   curvature is high and tears exactly the raised, curved regions while leaving flats intact.
       *
       * The confidence fades the move out where the opposing side of thin geometry comes into
       * reach, so the smoothing tapers off across a silhouette instead of stepping at it. */
      const float3 axis = math::normalize(smoothed_normal);
      const float3 delta = smoothed_position - positions[i];
      const float3 target = positions[i] + axis * (math::dot(delta, axis) * confidence);
      if (is_finite(target)) {
        r_targets[i] = target;
      }
    }
  });
}

}  // namespace blender::ed::sculpt_paint::smooth
