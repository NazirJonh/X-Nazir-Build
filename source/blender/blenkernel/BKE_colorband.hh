/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

/** \file
 * \ingroup bke
 */

#include "BLI_math_base.h"

#include "DNA_colorband_types.h"

namespace blender {

struct IDTypeForeachColorFunctionCallback;

/** #ColorBand.data length. */
#define MAXCOLORBAND 32

void BKE_colorband_init(ColorBand *coba, bool rangetype);
/**
 * Default ramp of the paint gradient tools (Image Editor and Sculpt): black to white with
 * B-Spline interpolation, the black stop moved to the middle of the gradient.
 */
void BKE_colorband_init_gradient_default(ColorBand *coba);
void BKE_colorband_init_from_table_rgba(ColorBand *coba,
                                        const float (*array)[4],
                                        int array_len,
                                        bool filter_samples);
ColorBand *BKE_colorband_add(bool rangetype);
bool BKE_colorband_evaluate(const ColorBand *coba, float in, float out[4]);
void BKE_colorband_evaluate_table_rgba(const ColorBand *coba, float **array, int *size);
CBData *BKE_colorband_element_add(ColorBand *coba, float position);
bool BKE_colorband_element_remove(ColorBand *coba, int index);
void BKE_colorband_update_sort(ColorBand *coba);
void BKE_colorband_foreach_working_space_color(ColorBand *coba,
                                               const IDTypeForeachColorFunctionCallback &fn);

/**
 * The mul/bias pair `gpu_shader_valtorgb` feeds the two-stop linear and ease functions of a
 * ColorRamp node: `mul` stretches the factor over the stop positions and `bias` shifts it back.
 * The one definition keeps the node's GPU code and the in-place uniform update
 * (#GPU_material_values_update) bit-for-bit identical.
 */
inline void BKE_colorband_valtorgb_mul_bias(const ColorBand &coba, float r_mul_bias[2])
{
  r_mul_bias[0] = 1.0f / (coba.data[1].pos - coba.data[0].pos);
  r_mul_bias[1] = -r_mul_bias[0] * coba.data[0].pos;
}

/**
 * The edge position `gpu_shader_valtorgb` feeds the two-stop constant function of a ColorRamp
 * node: the last position where the first color still shows.
 */
inline float BKE_colorband_valtorgb_const_pos(const ColorBand &coba)
{
  return max_ff(coba.data[0].pos, coba.data[1].pos);
}

}  // namespace blender
