/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

/** \file
 * \ingroup bke
 */

namespace blender {

struct CBData;
struct ColorBand;
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

}  // namespace blender
