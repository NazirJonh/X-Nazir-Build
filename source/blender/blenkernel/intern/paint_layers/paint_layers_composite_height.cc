/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The Height half of the description -> image-layer bridge: what a row's Height record asks of the
 * evaluator.
 */

#include "paint_layers_composite_intern.hh"

#include "BKE_paint_layers.hh"
#include "BKE_paint_material_composite.hh"

namespace blender {

void composite_height_settings_describe(const MaterialPaintLayer &layer,
                                        const int channel,
                                        PaintMaterialCompositeRowParams &out)
{
  if (channel != PAINT_MATERIAL_CHANNEL_HEIGHT) {
    return;
  }
  /* The evaluator reads the mode the row really lays, not the raw override: an Inherit row over
   * Add, Subtract or a colour blend is a step of its own. */
  out.height_blend = BKE_paint_layers_channel_height_blend_effective(layer, channel);
  out.height_scale = BKE_paint_layers_channel_height_scale(layer, channel);
}

}  // namespace blender
