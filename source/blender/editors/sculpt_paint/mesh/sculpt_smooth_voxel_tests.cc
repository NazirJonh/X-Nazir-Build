/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "BLI_array.hh"
#include "BLI_bit_vector.hh"
#include "BLI_math_vector.hh"

#include "sculpt_smooth_voxel.hh"

#include <cmath>

namespace blender::ed::sculpt_paint::smooth::tests {

/* Build a flat grid in the XY plane with a deterministic high-frequency displacement along Z. */
static void build_noisy_plane(const int side,
                              const float spacing,
                              const float amplitude,
                              Array<float3> &r_positions,
                              Array<float3> &r_normals)
{
  r_positions.reinitialize(side * side);
  r_normals.reinitialize(side * side);
  for (const int y : IndexRange(side)) {
    for (const int x : IndexRange(side)) {
      const int i = y * side + x;
      /* Alternating sign gives a displacement whose wavelength is a single grid step, well below
       * the smoothing scale used by the test. */
      const float noise = ((x + y) % 2 == 0) ? amplitude : -amplitude;
      r_positions[i] = float3(float(x) * spacing, float(y) * spacing, noise);
      r_normals[i] = float3(0.0f, 0.0f, 1.0f);
    }
  }
}

TEST(sculpt_smooth_voxel, FlatteningRemovesHighFrequencyNoise)
{
  constexpr int side = 32;
  constexpr float spacing = 0.1f;
  constexpr float amplitude = 0.05f;

  Array<float3> positions;
  Array<float3> normals;
  build_noisy_plane(side, spacing, amplitude, positions, normals);

  BitVector<> field_mask(positions.size(), true);
  BitVector<> anchor_mask(positions.size(), false);

  VoxelSmoothParams params;
  params.kernel_radius = 0.4f;
  params.preserve_volume = false;

  Array<float3> targets(positions.size());
  voxel_smooth_positions(positions, normals, field_mask, anchor_mask, params, targets);

  /* Only the interior is checked: the field has no data beyond the border of the region, so the
   * outermost row is expected to stay closer to its input. */
  float max_abs_z = 0.0f;
  for (const int y : IndexRange(4, side - 8)) {
    for (const int x : IndexRange(4, side - 8)) {
      max_abs_z = math::max(max_abs_z, math::abs(targets[y * side + x].z));
    }
  }
  EXPECT_LT(max_abs_z, amplitude * 0.25f);
}

/* Amplitude of a sine component along X, recovered by projecting the height field onto it. */
static float sine_amplitude(const Span<float3> positions, const float wavelength)
{
  float acc = 0.0f;
  for (const float3 &position : positions) {
    acc += position.z * std::sin(2.0f * float(M_PI) * position.x / wavelength);
  }
  return 2.0f * acc / float(positions.size());
}

TEST(sculpt_smooth_voxel, KernelRadiusSelectsTheScaleThatIsRemoved)
{
  constexpr int side = 64;
  constexpr float spacing = 0.05f;
  constexpr float long_wavelength = 1.6f;
  constexpr float short_wavelength = 0.2f;

  Array<float3> positions(side * side);
  Array<float3> normals(side * side);
  for (const int y : IndexRange(side)) {
    for (const int x : IndexRange(side)) {
      const int i = y * side + x;
      const float px = float(x) * spacing;
      const float height = 0.2f * std::sin(2.0f * float(M_PI) * px / long_wavelength) +
                           0.05f * std::sin(2.0f * float(M_PI) * px / short_wavelength);
      positions[i] = float3(px, float(y) * spacing, height);
      normals[i] = float3(0.0f, 0.0f, 1.0f);
    }
  }

  BitVector<> field_mask(positions.size(), true);
  BitVector<> anchor_mask(positions.size(), false);

  const float long_before = sine_amplitude(positions, long_wavelength);
  const float short_before = sine_amplitude(positions, short_wavelength);

  Array<float3> small_kernel(positions.size());
  VoxelSmoothParams params;
  params.preserve_volume = false;
  params.kernel_radius = short_wavelength * 0.5f;
  voxel_smooth_positions(positions, normals, field_mask, anchor_mask, params, small_kernel);

  Array<float3> large_kernel(positions.size());
  params.kernel_radius = long_wavelength * 0.5f;
  voxel_smooth_positions(positions, normals, field_mask, anchor_mask, params, large_kernel);

  /* A kernel matched to the short wavelength removes the detail and leaves the broad shape. */
  EXPECT_LT(std::abs(sine_amplitude(small_kernel, short_wavelength)), std::abs(short_before) * 0.4f);
  EXPECT_GT(std::abs(sine_amplitude(small_kernel, long_wavelength)), std::abs(long_before) * 0.8f);

  /* A kernel matched to the long wavelength removes the broad shape as well. */
  EXPECT_LT(std::abs(sine_amplitude(large_kernel, long_wavelength)), std::abs(long_before) * 0.5f);
}

TEST(sculpt_smooth_voxel, PreserveVolumeShrinksLessThanPlainBlur)
{
  constexpr int rings = 48;
  constexpr int segments = 96;
  constexpr float radius = 1.0f;

  Array<float3> positions(rings * segments);
  Array<float3> normals(rings * segments);
  for (const int ring : IndexRange(rings)) {
    /* Rings are placed on the polar angle so the samples stay reasonably even over the sphere. */
    const float theta = float(M_PI) * (float(ring) + 0.5f) / float(rings);
    for (const int segment : IndexRange(segments)) {
      const float phi = 2.0f * float(M_PI) * float(segment) / float(segments);
      const float3 direction(std::sin(theta) * std::cos(phi),
                             std::sin(theta) * std::sin(phi),
                             std::cos(theta));
      const int i = ring * segments + segment;
      positions[i] = direction * radius;
      normals[i] = direction;
    }
  }

  BitVector<> field_mask(positions.size(), true);
  BitVector<> anchor_mask(positions.size(), false);

  const auto mean_radius = [](const Span<float3> values) {
    float acc = 0.0f;
    for (const float3 &value : values) {
      acc += math::length(value);
    }
    return acc / float(values.size());
  };

  VoxelSmoothParams params;
  params.kernel_radius = 0.3f;

  Array<float3> plain(positions.size());
  params.preserve_volume = false;
  voxel_smooth_positions(positions, normals, field_mask, anchor_mask, params, plain);

  Array<float3> preserved(positions.size());
  params.preserve_volume = true;
  voxel_smooth_positions(positions, normals, field_mask, anchor_mask, params, preserved);

  /* A plain Laplacian pulls a closed surface inward; the mu step of Taubin cancels most of it. */
  EXPECT_LT(mean_radius(plain), radius);
  EXPECT_GT(mean_radius(preserved), mean_radius(plain));
}

/* `blender::math` has no `is_finite` overload for vector types. */
static bool all_components_finite(const float3 &value)
{
  return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

TEST(sculpt_smooth_voxel, AnchoredAndExcludedElementsKeepTheirPosition)
{
  constexpr int side = 16;
  constexpr float spacing = 0.1f;

  Array<float3> positions;
  Array<float3> normals;
  build_noisy_plane(side, spacing, 0.05f, positions, normals);

  BitVector<> field_mask(positions.size(), true);
  BitVector<> anchor_mask(positions.size(), false);
  const int anchored = side * (side / 2) + side / 2;
  const int excluded = anchored + 1;
  anchor_mask[anchored].set();
  field_mask[excluded].reset();

  VoxelSmoothParams params;
  params.kernel_radius = 0.4f;
  params.preserve_volume = false;

  Array<float3> targets(positions.size());
  voxel_smooth_positions(positions, normals, field_mask, anchor_mask, params, targets);

  EXPECT_EQ(targets[anchored], positions[anchored]);
  EXPECT_EQ(targets[excluded], positions[excluded]);
}

TEST(sculpt_smooth_voxel, DegenerateInputsProduceFiniteResults)
{
  VoxelSmoothParams params;
  params.kernel_radius = 0.5f;
  params.preserve_volume = true;

  {
    Array<float3> positions(0);
    Array<float3> normals(0);
    BitVector<> field_mask(0);
    BitVector<> anchor_mask(0);
    Array<float3> targets(0);
    voxel_smooth_positions(positions, normals, field_mask, anchor_mask, params, targets);
  }

  {
    Array<float3> positions({float3(1.0f, 2.0f, 3.0f)});
    Array<float3> normals({float3(0.0f, 0.0f, 1.0f)});
    BitVector<> field_mask(1, true);
    BitVector<> anchor_mask(1, false);
    Array<float3> targets(1);
    voxel_smooth_positions(positions, normals, field_mask, anchor_mask, params, targets);
    EXPECT_TRUE(all_components_finite(targets[0]));
  }

  {
    /* Coincident positions collapse the bounding box to a point, which must not divide by zero.
     * Every input lands in the same single voxel, so the field's only occupied voxel averages to
     * exactly that point: the smoothed target must equal the shared input position. */
    Array<float3> positions(64, float3(0.5f));
    Array<float3> normals(64, float3(0.0f, 1.0f, 0.0f));
    BitVector<> field_mask(64, true);
    BitVector<> anchor_mask(64, false);
    Array<float3> targets(64);
    voxel_smooth_positions(positions, normals, field_mask, anchor_mask, params, targets);
    for (const int i : targets.index_range()) {
      EXPECT_TRUE(all_components_finite(targets[i]));
      EXPECT_EQ(targets[i], positions[i]);
    }
  }
}

TEST(sculpt_smooth_voxel, UnevenVertexDensityDoesNotSlideVerticesAlongTheSurface)
{
  /* A perfectly flat plane sampled unevenly along X. There is no shape to smooth, so every vertex
   * must stay where it is. A smoother that followed the field's centroid would instead drag them
   * toward the densely sampled side by a distance set by the kernel radius rather than by the edge
   * length, and since sliding raises the local density the drift compounds until the surface
   * tears. This is the failure that destroyed real meshes under the brush. */
  constexpr int side = 48;
  constexpr float extent = 2.0f;

  Array<float3> positions(side * side);
  Array<float3> normals(side * side);
  for (const int y : IndexRange(side)) {
    for (const int x : IndexRange(side)) {
      const int i = y * side + x;
      const float tx = float(x) / float(side - 1);
      const float ty = float(y) / float(side - 1);
      /* Squaring the parameter packs the samples toward x = 0, so vertex density varies by a
       * large factor across the plane while the surface itself stays exactly flat. */
      positions[i] = float3(tx * tx * extent, ty * extent, 0.0f);
      normals[i] = float3(0.0f, 0.0f, 1.0f);
    }
  }

  BitVector<> field_mask(positions.size(), true);
  BitVector<> anchor_mask(positions.size(), false);

  VoxelSmoothParams params;
  params.kernel_radius = 0.3f;
  params.preserve_volume = false;

  Array<float3> targets(positions.size());
  voxel_smooth_positions(positions, normals, field_mask, anchor_mask, params, targets);

  float max_tangential_drift = 0.0f;
  for (const int i : positions.index_range()) {
    const float3 delta = targets[i] - positions[i];
    max_tangential_drift = math::max(max_tangential_drift,
                                     math::length(float2(delta.x, delta.y)));
  }
  EXPECT_LT(max_tangential_drift, 1.0e-5f);
}

TEST(sculpt_smooth_voxel, SmoothSurfaceStaysSmoothWithoutVoxelGrid)
{
  /* A broad, already-smooth dome sampled far finer than the voxel size. Smoothing it must not add
   * high-frequency roughness: trilinear reconstruction leaves a slope crease at every voxel-cell
   * boundary, which prints the voxel grid onto the surface, while the cubic B-spline used here is
   * C2 and leaves the dome smooth. The test measures roughness as the discrete Laplacian of the
   * output height over the fine grid and requires it to stay well below the voxel scale. */
  constexpr int side = 96;
  constexpr float spacing = 0.02f;      /* Fine: ~15 samples per voxel at the kernel below. */
  constexpr float dome_height = 0.2f;
  const float span = float(side - 1) * spacing;
  const float3 center(span * 0.5f, span * 0.5f, 0.0f);

  Array<float3> positions(side * side);
  Array<float3> normals(side * side);
  for (const int y : IndexRange(side)) {
    for (const int x : IndexRange(side)) {
      const int i = y * side + x;
      const float px = float(x) * spacing;
      const float py = float(y) * spacing;
      const float r2 = math::length_squared(float2(px, py) - float2(center.x, center.y));
      /* A Gaussian dome: smooth everywhere, with a genuinely curved (non-planar) surface so that
       * trilinear reconstruction, which is exact only for linear data, would crease. */
      const float z = dome_height * std::exp(-r2 / (2.0f * 0.09f));
      positions[i] = float3(px, py, z);
      /* Analytic outward normal of the dome. */
      const float dz_dx = z * (-(px - center.x) / 0.09f);
      const float dz_dy = z * (-(py - center.y) / 0.09f);
      normals[i] = math::normalize(float3(-dz_dx, -dz_dy, 1.0f));
    }
  }

  BitVector<> field_mask(positions.size(), true);
  BitVector<> anchor_mask(positions.size(), false);

  VoxelSmoothParams params;
  params.kernel_radius = 0.3f;
  params.preserve_volume = false;

  Array<float3> targets(positions.size());
  voxel_smooth_positions(positions, normals, field_mask, anchor_mask, params, targets);

  /* Roughness = |z(x,y) - average of the four axis neighbors|. On a smooth surface this is tiny;
   * a voxel-grid crease would spike it periodically. Only the interior is checked, since the field
   * is ragged at the region border. */
  float max_roughness = 0.0f;
  for (const int y : IndexRange(2, side - 4)) {
    for (const int x : IndexRange(2, side - 4)) {
      const float z = targets[y * side + x].z;
      const float avg = 0.25f * (targets[y * side + (x - 1)].z + targets[y * side + (x + 1)].z +
                                 targets[(y - 1) * side + x].z + targets[(y + 1) * side + x].z);
      max_roughness = math::max(max_roughness, std::abs(z - avg));
    }
  }
  /* The dome's own curvature contributes a small, smooth second difference at this spacing (cubic
   * reconstruction measures ~1.1e-4). The threshold sits above that but below the ~6e-4 that plain
   * trilinear reconstruction produces from its per-cell slope creases, so the test discriminates
   * the smooth reconstruction from a regression back to trilinear. */
  EXPECT_LT(max_roughness, 3.0e-4f);
}

}  // namespace blender::ed::sculpt_paint::smooth::tests
