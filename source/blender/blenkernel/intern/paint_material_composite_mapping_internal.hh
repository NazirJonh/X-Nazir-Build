/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** ile
 * \ingroup bke
 *
 * The CPU composite's UV mapping steps, shared by the map resample and the projected resample.
 */

#include <cmath>

#include "BKE_paint_layers.hh"
#include "BKE_paint_material_composite.hh"

#include "DNA_material_types.h"

namespace blender {

/**
 * The canvas-UV transform of a row's mapping, the one both CPU passes (the resample of a map and
 * the clip of a Material row's coverage) read through, so they cannot disagree on a texel: the
 * Mirror fold of the mapped axes around the center, then rotate around Z, then scale along the
 * canvas axes, then offset. An axis that repeats wraps with floor; one that clips is range-tested.
 */
struct CompositeMappingTransform {
  float scale_x = 1.0f;
  float scale_y = 1.0f;
  float cos_rot = 1.0f;
  float sin_rot = 0.0f;
  float offset_u = 0.0f;
  float offset_v = 0.0f;
  bool mirror_u = false;
  bool mirror_v = false;
  /* The fold keeps the smaller side of the center instead of the larger one. */
  bool mirror_negative_u = false;
  bool mirror_negative_v = false;
  float mirror_center_u = 0.5f;
  float mirror_center_v = 0.5f;
  bool repeat_u = true;
  bool repeat_v = true;

  /* The identity; only the unused slots of #CompositeMappingChain::steps hold it. */
  CompositeMappingTransform() = default;

  explicit CompositeMappingTransform(const PaintMaterialCompositeMapping &mapping)
      : scale_x(BKE_paint_layers_mapping_scale_normalize(mapping.scale[0])),
        scale_y(BKE_paint_layers_mapping_scale_normalize(mapping.scale[1])),
        cos_rot(cosf(mapping.rotation)),
        sin_rot(sinf(mapping.rotation)),
        offset_u(mapping.offset[0]),
        offset_v(mapping.offset[1]),
        mirror_u(mapping.mirror_axis(MA_PAINT_LAYER_MAPPING_MIRROR_U)),
        mirror_v(mapping.mirror_axis(MA_PAINT_LAYER_MAPPING_MIRROR_V)),
        mirror_negative_u(mapping.mirror_axis(MA_PAINT_LAYER_MAPPING_MIRROR_U) &&
                          mapping.mirror_axis(MA_PAINT_LAYER_MAPPING_MIRROR_U_NEGATIVE)),
        mirror_negative_v(mapping.mirror_axis(MA_PAINT_LAYER_MAPPING_MIRROR_V) &&
                          mapping.mirror_axis(MA_PAINT_LAYER_MAPPING_MIRROR_V_NEGATIVE)),
        mirror_center_u(mapping.mirror_center[0]),
        mirror_center_v(mapping.mirror_center[1])
  {
    BKE_paint_layers_mapping_wrap_axes(mapping.wrap, repeat_u, repeat_v);
  }

  /** Whether any axis clips, so a texel can fall outside the unit square. */
  bool clips() const
  {
    return !(repeat_u && repeat_v);
  }

  /** Which side of the fold center \a u lies on along U, when U folds; always +1 when it does not.
   * The covariant counterpart of the fold, the flip a tangent normal's U component takes. */
  float side_u(const float u) const
  {
    return mirror_u ? BKE_paint_layers_mapping_mirror_side(u, mirror_center_u, mirror_negative_u) :
                      1.0f;
  }

  /** #side_u along V. */
  float side_v(const float v) const
  {
    return mirror_v ? BKE_paint_layers_mapping_mirror_side(v, mirror_center_v, mirror_negative_v) :
                      1.0f;
  }

  /** The mapped coordinate of the canvas point (\a u, \a v), with no repeat: the chain floors the
   * composed lookup at its end, and a step whose axis clips only reports the texel transparent.
   * \return false outside the square on an axis that clips. */
  bool apply_fold_affine(const float u, const float v, float &r_u, float &r_v) const
  {
    float fu = u;
    float fv = v;
    if (mirror_u) {
      fu = BKE_paint_layers_mapping_mirror_fold(u, mirror_center_u, mirror_negative_u);
    }
    if (mirror_v) {
      fv = BKE_paint_layers_mapping_mirror_fold(v, mirror_center_v, mirror_negative_v);
    }
    const float ru = fu * cos_rot - fv * sin_rot;
    const float rv = fu * sin_rot + fv * cos_rot;
    r_u = ru * scale_x + offset_u;
    r_v = rv * scale_y + offset_v;
    return !((!repeat_u && (r_u < 0.0f || r_u >= 1.0f)) ||
             (!repeat_v && (r_v < 0.0f || r_v >= 1.0f)));
  }

  /** The mapped coordinate of the canvas point (\a u, \a v). \return false outside the square on
   * an axis that clips. Only a clipping axis is range-tested: a wrapped one can round to 1. The
   * fold reads the canvas UV the painting addresses, before the transform moves it. */
  bool apply(const float u, const float v, float &r_u, float &r_v) const
  {
    if (!apply_fold_affine(u, v, r_u, r_v)) {
      return false;
    }
    if (repeat_u) {
      r_u -= floorf(r_u);
    }
    if (repeat_v) {
      r_v -= floorf(r_v);
    }
    return true;
  }
};

/**
 * The composed read of a row's own map: the transform effects' steps first, in the effects list's
 * order, then the row's own mapping, the order the generated graph composes the same effects in.
 * Every step folds about its center and then transforms; an axis a step clips makes the texel
 * transparent, and the composed lookup repeats at the end, the way the graph's Image Texture
 * extension repeats the composed lookup it is fed (a clipping step has already declared the texel
 * transparent, so flooring an in-range coordinate is the identity it reads as).
 */
struct CompositeMappingChain {
  /** The most steps one read composes (#PAINT_MATERIAL_COMPOSITE_INPUT_TRANSFORM_MAX transform
   * effects plus the row's own mapping). The description leaves the extras out. */
  static constexpr int MAX_STEPS = PAINT_MATERIAL_COMPOSITE_INPUT_TRANSFORM_MAX + 1;

  CompositeMappingTransform steps[MAX_STEPS];
  int steps_num = 0;
  /** The composed map holds tangent-space normals: the read also remaps the vectors. */
  bool tangent_normal = false;

  bool is_identity() const
  {
    return steps_num == 0;
  }

  /** Whether any step clips an axis, so a texel can fall outside the square. */
  bool clips() const
  {
    for (int i = 0; i < steps_num; i++) {
      if (steps[i].clips()) {
        return true;
      }
    }
    return false;
  }

  /**
   * The composed lookup of the canvas point (\a u, \a v). \return false when a step's clipping
   * axis puts the texel outside the square. \a r_step_sides, when given, receives the fold's side
   * sign per step at the coordinate that enters it, for the tangent-Normal remap below.
   */
  bool apply(const float u,
             const float v,
             float &r_u,
             float &r_v,
             float (*r_step_sides)[2] = nullptr) const
  {
    float cu;
    float cv;
    if (!this->apply_unwrapped(u, v, cu, cv, r_step_sides)) {
      return false;
    }
    r_u = cu - floorf(cu);
    r_v = cv - floorf(cv);
    return true;
  }

  /**
   * #apply without the closing repeat: the steps compose and the caller wraps (or clips) the result.
   * A projected read wants this, since the row's own UV adjust and wrap act after the steps.
   * \return false when a step's clipping axis puts the texel outside the square.
   */
  bool apply_unwrapped(const float u,
                       const float v,
                       float &r_u,
                       float &r_v,
                       float (*r_step_sides)[2] = nullptr) const
  {
    float cu = u;
    float cv = v;
    for (int i = 0; i < steps_num; i++) {
      const CompositeMappingTransform &step = steps[i];
      if (r_step_sides != nullptr) {
        /* The side signs are per-texel, taken where the step's fold acts: on the coordinate the
         * step reads, which is the previous step's output. */
        r_step_sides[i][0] = step.side_u(cu);
        r_step_sides[i][1] = step.side_v(cv);
      }
      if (!step.apply_fold_affine(cu, cv, cu, cv)) {
        return false;
      }
    }
    r_u = cu;
    r_v = cv;
    return true;
  }

  /**
   * The covariant counterpart of the composed read, for an encoded tangent normal: the inverse
   * Jacobian of the chain, applied from the last step to the first, each step's side signs the
   * ones \a step_sides recorded for this texel. The magnitudes do not turn a direction, only the
   * signs of the scales do.
   */
  void remap_tangent_normal(const float (*step_sides)[2], float r_xy[2]) const
  {
    float nx = r_xy[0];
    float ny = r_xy[1];
    for (int i = steps_num - 1; i >= 0; i--) {
      const CompositeMappingTransform &step = steps[i];
      const float fx = (step.scale_x < 0.0f ? -1.0f : 1.0f) * nx;
      const float fy = (step.scale_y < 0.0f ? -1.0f : 1.0f) * ny;
      /* The UV transform of a step is `S * R * fold`, whose inverse on a direction is
       * `side * R^T * S^-1`, the fold's side signs acting on the canvas axes after the rotation
       * back. Composing the steps, the last one's inverse acts first. */
      nx = step_sides[i][0] * (step.cos_rot * fx + step.sin_rot * fy);
      ny = step_sides[i][1] * (-step.sin_rot * fx + step.cos_rot * fy);
    }
    r_xy[0] = nx;
    r_xy[1] = ny;
  }
};

}  // namespace blender
