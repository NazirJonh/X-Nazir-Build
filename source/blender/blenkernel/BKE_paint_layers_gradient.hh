/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * The Gradient generator of the Paint Layers stack: its payload, its parameters, and the one
 * evaluation function the generated shader nodes and the CPU composite share.
 */

#include "BLI_math_vector.hh"

#include "BKE_paint_layers.hh"

#include "DNA_color_types.h"
#include "DNA_material_types.h"
#include "DNA_scene_types.h"

namespace blender {

struct Material;
struct MaterialPaintLayer;

/** Whether \a layer is a Gradient generator row, correction or mask item. */
bool BKE_paint_layers_gradient_supported(const MaterialPaintLayer &layer);

/**
 * Allocate \a layer's gradient payload and set its defaults when it has none; an existing payload is
 * returned untouched. Returns null for a row whose kind is not a gradient.
 */
MaterialPaintLayerGradient *BKE_paint_layers_gradient_ensure(MaterialPaintLayer &layer);

/** Free \a layer's gradient payload. Safe on a null payload. */
void BKE_paint_layers_gradient_free(MaterialPaintLayer &layer);

/**
 * Give \a dst a deep copy of \a src's gradient payload (or none when \a src has none). \a dst's
 * pointer is overwritten without being freed, as it is a shallow copy of \a src's row.
 */
void BKE_paint_layers_gradient_copy(MaterialPaintLayer &dst, const MaterialPaintLayer &src);

/**
 * Add a Gradient generator row: a procedural ramp in UV space, built from Shader Editor nodes and
 * evaluated by the CPU with the same formula. The row gets its gradient payload, its default
 * Base Color channel and no map; another channel is switched on like a Fill's, and a scalar channel
 * starts spanning its whole range.
 *
 * \param name: the row's name, or null for a unique "Gradient N".
 * \return the new row, or null when the stack refuses the Add.
 */
MaterialPaintLayer *BKE_paint_layers_gradient_add(Material &ma,
                                                  const char *name,
                                                  MaterialPaintLayer *anchor,
                                                  PaintLayerPlace place);

/** Set \a gradient to the defaults: a linear black-to-white ramp across the unit square. */
void BKE_paint_layers_gradient_init(MaterialPaintLayerGradient &gradient);

/**
 * Fold a raw gradient parameter into the ramp position: repeat first, then the Hermite midpoint
 * bend. The one formula the generated nodes, the CPU composite and the tests read.
 */
float BKE_paint_layers_gradient_ramp_position(const MaterialPaintLayerGradient &gradient,
                                              float t);

/**
 * The ramp position at \a coordinate (a UV with Z = 0 in the UV space): the type's line/radial
 * parameter, the degenerate guard, the repeat and the midpoint. A degenerate line returns the
 * start of the ramp, not a NaN.
 */
float BKE_paint_layers_gradient_factor(const MaterialPaintLayerGradient &gradient,
                                       const float3 &coordinate);

/** Evaluate the ramp at \a t into straight scene-linear RGBA. */
void BKE_paint_layers_gradient_color(const MaterialPaintLayerGradient &gradient,
                                     float t,
                                     float r_rgba[4]);

/**
 * Hash of everything in \a ramp that decides its colors: the stop count, the interpolation modes
 * and every stop. Order-sensitive. The bake, topology and composite hashes fold it in, so a ramp
 * edit invalidates all three by the same rule.
 */
uint64_t BKE_paint_layers_gradient_ramp_hash(const ColorBand &ramp);

/** Whether any row of \a ma, or any correction or mask item laid over one, is a Gradient. */
bool BKE_paint_layers_gradient_any(const Material &ma);

/**
 * The row of \a ma whose gradient ramp is \a data or contains the stop \a data, or null. The
 * ColorRamp RNA callbacks receive either the band itself or a #CBData inside it.
 */
MaterialPaintLayer *BKE_paint_layers_gradient_ramp_row_from_data(Material &ma, const void *data);

/**
 * Tag \a ma for regeneration when \a data is a gradient's ColorBand or one of its stops. The
 * ColorRamp RNA update callback receives either the band itself or a #CBData inside it, so this is
 * the one place that recognizes both. \return whether a gradient ramp owns the address.
 */
bool BKE_paint_layers_gradient_ramp_edited(Material &ma, const void *data);

/** Set the gradient's type; a value edit. */
bool BKE_paint_layers_gradient_set_type(Material &ma,
                                        MaterialPaintLayer &layer,
                                        eMaterialPaintLayerGradientType type);

/** Set how the parameter outside 0..1 is folded; a value edit. */
bool BKE_paint_layers_gradient_set_repeat(Material &ma,
                                          MaterialPaintLayer &layer,
                                          eMaterialPaintLayerGradientRepeat repeat);

/** Set the ramp's midpoint (clamped to the usable range); a value edit. */
bool BKE_paint_layers_gradient_set_midpoint(Material &ma,
                                            MaterialPaintLayer &layer,
                                            float midpoint);

/** Set the gradient-space line (Z forced to 0 in the UV space); a value edit. A degenerate line
 * is stored as given. */
bool BKE_paint_layers_gradient_set_line(Material &ma,
                                        MaterialPaintLayer &layer,
                                        const float start[3],
                                        const float end[3]);

/** Set a scalar channel's two ends; a value edit. Clamped to the channel's own range. */
bool BKE_paint_layers_gradient_set_channel_scalars(Material &ma,
                                                   MaterialPaintLayer &layer,
                                                   eMaterialPaintChannel channel,
                                                   float start,
                                                   float end);

}  // namespace blender
