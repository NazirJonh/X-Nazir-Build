/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bli
 */

#include "BLI_math_inline.h"
#include "BLI_sys_types.h"

namespace blender {

/******************** Blending Modes **********************
 * - byte function assume straight alpha
 * - float functions assume premultiplied alpha
 */

MINLINE void blend_color_mix_byte(unsigned char dst[4],
                                  const unsigned char src1[4],
                                  const unsigned char src2[4]);
MINLINE void blend_color_add_byte(unsigned char dst[4],
                                  const unsigned char src1[4],
                                  const unsigned char src2[4]);
MINLINE void blend_color_sub_byte(unsigned char dst[4],
                                  const unsigned char src1[4],
                                  const unsigned char src2[4]);
MINLINE void blend_color_mul_byte(unsigned char dst[4],
                                  const unsigned char src1[4],
                                  const unsigned char src2[4]);
MINLINE void blend_color_lighten_byte(unsigned char dst[4],
                                      const unsigned char src1[4],
                                      const unsigned char src2[4]);
MINLINE void blend_color_darken_byte(unsigned char dst[4],
                                     const unsigned char src1[4],
                                     const unsigned char src2[4]);
MINLINE void blend_color_erase_alpha_byte(unsigned char dst[4],
                                          const unsigned char src1[4],
                                          const unsigned char src2[4]);
MINLINE void blend_color_add_alpha_byte(unsigned char dst[4],
                                        const unsigned char src1[4],
                                        const unsigned char src2[4]);

MINLINE void blend_color_overlay_byte(unsigned char dst[4],
                                      const uchar src1[4],
                                      const uchar src2[4]);
MINLINE void blend_color_hardlight_byte(unsigned char dst[4],
                                        const uchar src1[4],
                                        const uchar src2[4]);
MINLINE void blend_color_burn_byte(unsigned char dst[4], const uchar src1[4], const uchar src2[4]);
MINLINE void blend_color_linearburn_byte(unsigned char dst[4],
                                         const uchar src1[4],
                                         const uchar src2[4]);
MINLINE void blend_color_dodge_byte(unsigned char dst[4],
                                    const uchar src1[4],
                                    const uchar src2[4]);
MINLINE void blend_color_screen_byte(unsigned char dst[4],
                                     const uchar src1[4],
                                     const uchar src2[4]);
MINLINE void blend_color_softlight_byte(unsigned char dst[4],
                                        const uchar src1[4],
                                        const uchar src2[4]);
MINLINE void blend_color_pinlight_byte(unsigned char dst[4],
                                       const uchar src1[4],
                                       const uchar src2[4]);
MINLINE void blend_color_linearlight_byte(unsigned char dst[4],
                                          const uchar src1[4],
                                          const uchar src2[4]);
MINLINE void blend_color_vividlight_byte(unsigned char dst[4],
                                         const uchar src1[4],
                                         const uchar src2[4]);
MINLINE void blend_color_difference_byte(unsigned char dst[4],
                                         const uchar src1[4],
                                         const uchar src2[4]);
MINLINE void blend_color_exclusion_byte(unsigned char dst[4],
                                        const uchar src1[4],
                                        const uchar src2[4]);
MINLINE void blend_color_color_byte(unsigned char dst[4],
                                    const uchar src1[4],
                                    const uchar src2[4]);
MINLINE void blend_color_hue_byte(unsigned char dst[4], const uchar src1[4], const uchar src2[4]);
MINLINE void blend_color_saturation_byte(unsigned char dst[4],
                                         const uchar src1[4],
                                         const uchar src2[4]);
MINLINE void blend_color_luminosity_byte(unsigned char dst[4],
                                         const uchar src1[4],
                                         const uchar src2[4]);

MINLINE void blend_color_interpolate_byte(unsigned char dst[4],
                                          const unsigned char src1[4],
                                          const unsigned char src2[4],
                                          float ft);

MINLINE void blend_color_mix_float(float dst[4], const float src1[4], const float src2[4]);
MINLINE void blend_color_add_float(float dst[4], const float src1[4], const float src2[4]);
MINLINE void blend_color_sub_float(float dst[4], const float src1[4], const float src2[4]);
MINLINE void blend_color_mul_float(float dst[4], const float src1[4], const float src2[4]);
MINLINE void blend_color_lighten_float(float dst[4], const float src1[4], const float src2[4]);
MINLINE void blend_color_darken_float(float dst[4], const float src1[4], const float src2[4]);
MINLINE void blend_color_erase_alpha_float(float dst[4], const float src1[4], const float src2[4]);
MINLINE void blend_color_add_alpha_float(float dst[4], const float src1[4], const float src2[4]);

MINLINE void blend_color_overlay_float(float dst[4], const float src1[4], const float src2[4]);
MINLINE void blend_color_hardlight_float(float dst[4], const float src1[4], const float src2[4]);
MINLINE void blend_color_burn_float(float dst[4], const float src1[4], const float src2[4]);
MINLINE void blend_color_linearburn_float(float dst[4], const float src1[4], const float src2[4]);
MINLINE void blend_color_dodge_float(float dst[4], const float src1[4], const float src2[4]);
MINLINE void blend_color_screen_float(float dst[4], const float src1[4], const float src2[4]);
MINLINE void blend_color_softlight_float(float dst[4], const float src1[4], const float src2[4]);
MINLINE void blend_color_pinlight_float(float dst[4], const float src1[4], const float src2[4]);
MINLINE void blend_color_linearlight_float(float dst[4], const float src1[4], const float src2[4]);
MINLINE void blend_color_vividlight_float(float dst[4], const float src1[4], const float src2[4]);
MINLINE void blend_color_difference_float(float dst[4], const float src1[4], const float src2[4]);
MINLINE void blend_color_exclusion_float(float dst[4], const float src1[4], const float src2[4]);
MINLINE void blend_color_color_float(float dst[4], const float src1[4], const float src2[4]);
MINLINE void blend_color_hue_float(float dst[4], const float src1[4], const float src2[4]);
MINLINE void blend_color_saturation_float(float dst[4], const float src1[4], const float src2[4]);
MINLINE void blend_color_luminosity_float(float dst[4], const float src1[4], const float src2[4]);

MINLINE void blend_color_interpolate_float(float dst[4],
                                           const float src1[4],
                                           const float src2[4],
                                           float t);

/**
 * One channel of the Color Burn blend at full factor. \a over is the upper colour already mixed
 * with the node's factor (`facm + fac * col`, exactly what #ramp_blend computes), so nothing is
 * weighted here: the result is the burned channel, clamped to 0..1.
 *
 * The CPU paint composite reduces Vivid Light to this step against a doubled upper colour, and the
 * GPU builds the same mode from the Mix node's own Burn branch, so both engines call this one
 * formula instead of carrying two that could drift.
 */
MINLINE float blend_color_burn_channel(const float below, const float over);

/**
 * One channel of the Color Dodge blend at full factor. \a over is `1 - fac * col`, exactly what
 * #ramp_blend computes: a burnt-out over returns one and a zero below stays put, like the Mix
 * node's own branch. See #blend_color_burn_channel for why one formula serves both engines.
 */
MINLINE float blend_color_dodge_channel(const float below, const float over);

}  // namespace blender

#if BLI_MATH_DO_INLINE
#  include "intern/math_color_blend_inline.cc"  // IWYU pragma: export
#endif
