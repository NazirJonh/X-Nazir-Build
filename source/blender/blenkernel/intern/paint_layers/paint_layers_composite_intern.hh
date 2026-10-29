/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Internal helpers shared by the description -> image-layer bridge.
 */

#pragma once

#include "BKE_paint_layers.hh"
#include "BKE_paint_material_composite.hh"

namespace blender {

/**
 * The Height record's effective settings of \a layer in \a channel, as the evaluator reads them.
 *
 * The mode is #BKE_paint_layers_channel_height_blend_effective, so an Inherit row over Add,
 * Subtract or a colour blend is described as the step it really takes. The mode and the scale are
 * described on the Height channel alone, which is the one whose step is taken in delta space.
 */
void composite_height_settings_describe(const MaterialPaintLayer &layer,
                                        int channel,
                                        PaintMaterialCompositeRowParams &out);

}  // namespace blender
