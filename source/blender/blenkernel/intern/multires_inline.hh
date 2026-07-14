/* SPDX-FileCopyrightText: 2018 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#pragma once

#include "BKE_multires.hh"

#include "BLI_math_constants.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_rotation_legacy.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"

namespace blender {

BLI_INLINE void BKE_multires_construct_tangent_matrix(float3x3 &tangent_matrix,
                                                      const float3 &dPdu,
                                                      const float3 &dPdv,
                                                      const int corner)
{
  if (corner == 0) {
    tangent_matrix.x_axis() = dPdv * -1.0f;
    tangent_matrix.y_axis() = dPdu * -1.0f;
  }
  else if (corner == 1) {
    tangent_matrix.x_axis() = dPdu;
    tangent_matrix.y_axis() = dPdv * -1.0f;
  }
  else if (corner == 2) {
    tangent_matrix.x_axis() = dPdv;
    tangent_matrix.y_axis() = dPdu;
  }
  else if (corner == 3) {
    tangent_matrix.x_axis() = dPdu * -1.0f;
    tangent_matrix.y_axis() = dPdv;
  }
  else {
    BLI_assert_msg(0, "Unhandled corner index");
  }

  /* Compute the normal from the already rotated axes so the matrix keeps the correct chirality
   * for every corner. Do the cross product in double precision: the partial derivative tangent
   * vectors can be nearly parallel, where a single precision cross product loses all signal. */
  const float3 N = float3(math::normalize(
      math::cross(double3(tangent_matrix.x_axis()), double3(tangent_matrix.y_axis()))));

  constexpr float eps = 0.000001f;
  /* A degenerate cross product (normalization of a near-zero vector yields the zero vector) means
   * there is no usable tangent frame at this point. Return the null matrix so the displacement
   * contribution becomes zero instead of producing a spike. */
  if (math::length_squared(N) < eps) {
    tangent_matrix = float3x3::zero();
    return;
  }

  tangent_matrix.z_axis() = N;

  /* The axis lengths feed both the conditioning test and the final scaling, so compute them once.
   * `len_prod` is `length(x) * length(y)`; its ratio with the dot product is the cosine of the
   * angle between the axes, letting the well-conditioning test compare cosines directly instead of
   * paying a `safe_acos` at every grid point. Axis rotation below preserves lengths, so `len_prod`
   * stays valid for the geometric mean afterwards. */
  const float len_prod = math::sqrt(math::length_squared(tangent_matrix.x_axis()) *
                                    math::length_squared(tangent_matrix.y_axis()));
  const float cos_angle = math::dot(tangent_matrix.x_axis(), tangent_matrix.y_axis()) / len_prod;

  /* Rotate both axes apart (or together) around the normal when they are nearly parallel or
   * nearly opposite, so the matrix stays well conditioned. */
  constexpr float threshold = 85.0f;
  constexpr float low_threshold = 90.0f - threshold;
  constexpr float high_threshold = 90.0f + threshold;
  /* `cos(low_threshold)`; by symmetry around 90 degrees this also equals `-cos(high_threshold)`,
   * so it bounds both the near-parallel (angle < low) and near-opposite (angle > high) cases. */
  constexpr float cos_low_threshold = 0.996194698f;

  if (cos_angle > cos_low_threshold) {
    const float angle_between = RAD2DEGF(math::safe_acos(cos_angle));
    const float deg_to_rotate = (low_threshold - angle_between) / 2.0f;
    const float rad_to_rotate = DEG2RADF(deg_to_rotate);
    tangent_matrix.x_axis() = math::rotate_around_axis(
        tangent_matrix.x_axis(), float3(0.0f), tangent_matrix.z_axis(), -rad_to_rotate);
    tangent_matrix.y_axis() = math::rotate_around_axis(
        tangent_matrix.y_axis(), float3(0.0f), tangent_matrix.z_axis(), rad_to_rotate);
  }
  else if (cos_angle < -cos_low_threshold) {
    const float angle_between = RAD2DEGF(math::safe_acos(cos_angle));
    const float deg_to_rotate = (angle_between - high_threshold) / 2.0f;
    const float rad_to_rotate = DEG2RADF(deg_to_rotate);
    tangent_matrix.x_axis() = math::rotate_around_axis(
        tangent_matrix.x_axis(), float3(0.0f), tangent_matrix.z_axis(), rad_to_rotate);
    tangent_matrix.y_axis() = math::rotate_around_axis(
        tangent_matrix.y_axis(), float3(0.0f), tangent_matrix.z_axis(), -rad_to_rotate);
  }

  /* Keep the tangent axes un-normalized so the displacement scales with the surface, and give
   * the normal axis the geometric mean of their lengths so normal displacement scales the same
   * way. */
  const float geometric_mean = math::sqrt(len_prod);
  tangent_matrix.z_axis() = N * geometric_mean;
}

/**
 * The pre-5.x tangent matrix construction (normalized axes, normal from raw `dPdu x dPdv`).
 * Kept only so versioning can decode displacement grids written by older files before
 * re-encoding them with #BKE_multires_construct_tangent_matrix.
 */
BLI_INLINE void BKE_multires_construct_tangent_matrix_for_versioning(float3x3 &tangent_matrix,
                                                                     const float3 &dPdu,
                                                                     const float3 &dPdv,
                                                                     const int corner)
{
  if (corner == 0) {
    tangent_matrix.x_axis() = dPdv * -1.0f;
    tangent_matrix.y_axis() = dPdu * -1.0f;
  }
  else if (corner == 1) {
    tangent_matrix.x_axis() = dPdu;
    tangent_matrix.y_axis() = dPdv * -1.0f;
  }
  else if (corner == 2) {
    tangent_matrix.x_axis() = dPdv;
    tangent_matrix.y_axis() = dPdu;
  }
  else if (corner == 3) {
    tangent_matrix.x_axis() = dPdu * -1.0f;
    tangent_matrix.y_axis() = dPdv;
  }
  else {
    BLI_assert_msg(0, "Unhandled corner index");
  }
  tangent_matrix.z_axis() = math::cross(dPdu, dPdv);

  tangent_matrix.x_axis() = math::normalize(tangent_matrix.x_axis());
  tangent_matrix.y_axis() = math::normalize(tangent_matrix.y_axis());
  tangent_matrix.z_axis() = math::normalize(tangent_matrix.z_axis());
}

}  // namespace blender
