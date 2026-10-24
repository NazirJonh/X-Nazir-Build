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

#include "BLI_math_vector_types.hh"

#include "BKE_paint_layers.hh"

#include "DNA_color_types.h"
#include "DNA_material_types.h"
#include "DNA_scene_types.h"

namespace blender {

struct Material;
struct MaterialPaintLayer;
struct Mesh;
struct Object;

/**
 * Mixed first into every hash of a Gradient payload (topology and bake). Bump it when the generated
 * formula or the payload layout changes, so every cached graph and bake is rebuilt once.
 */
constexpr uint64_t BKE_PAINT_LAYERS_GRADIENT_HASH_VERSION = 0x700;

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
 * starts with a black-to-white value ramp spanning its whole range.
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
 * The ramp whose stops the row's gizmo shows for \a channel: the shared colour ramp for a colour
 * channel, the scalar record's own value ramp for a value channel, or null when the row has no
 * gradient or the channel has no ramp.
 */
const ColorBand *BKE_paint_layers_gradient_channel_ramp_get(const MaterialPaintLayer &layer,
                                                           int channel);
ColorBand *BKE_paint_layers_gradient_channel_ramp_get(MaterialPaintLayer &layer, int channel);

/**
 * Write the channel values \a start and \a end into the first and last stop of a scalar channel's
 * value ramp, as grey in 0..1 over the channel's range. The inner stops are left alone. Not a
 * rebuild: the caller tags the edit.
 */
void BKE_paint_layers_gradient_channel_ramp_ends_set(ColorBand &ramp,
                                                     eMaterialPaintChannel channel,
                                                     float start,
                                                     float end);

/** The channel value of the first (or, with \a end, the last) stop of a value ramp. */
float BKE_paint_layers_gradient_channel_ramp_end_get(const ColorBand &ramp,
                                                     eMaterialPaintChannel channel,
                                                     bool end);

/**
 * Choose the channel whose ramp the gizmo shows. A viewport aid: it never rebuilds the graph.
 * False for Normal, a row without a gradient, or a value channel with no ramp to show.
 */
bool BKE_paint_layers_gradient_set_gizmo_channel(Material &ma,
                                                 MaterialPaintLayer &layer,
                                                 eMaterialPaintChannel channel);

/**
 * Fold a raw gradient parameter into the ramp position: repeat first, then the Hermite midpoint
 * bend. The one formula the generated nodes, the CPU composite and the tests read.
 */
float BKE_paint_layers_gradient_ramp_position(const MaterialPaintLayerGradient &gradient,
                                              float t);

/**
 * The ramp position at \a coordinate: the type's line/radial parameter, the degenerate guard, the
 * repeat and the midpoint. UV rows pass `(u, v, 0)`, Object rows object-space coordinates and
 * Generated rows normalized 0..1 coordinates. A degenerate line returns the start of the ramp, not
 * a NaN.
 */
float BKE_paint_layers_gradient_factor(const MaterialPaintLayerGradient &gradient,
                                       const float3 &coordinate);

/**
 * The Z of a gradient line's start or end as the graph reads it: a UV line lives in 2D, so its Z is
 * zero whatever the payload kept from another space. Object and Generated lines keep \a z.
 */
float BKE_paint_layers_gradient_line_z(const MaterialPaintLayerGradient &gradient, float z);

/** Whether \a layer is a Gradient row whose result can be shared between objects (UV only). */
bool BKE_paint_layers_gradient_is_shared_bakeable(const MaterialPaintLayer &layer);

/**
 * Whether \a layer and everything below it can be shared between objects: false when the row, a
 * child, a correction or a mask item is an Object/Generated Gradient. A folder holding one cannot
 * be baked into a shared map either.
 */
bool BKE_paint_layers_gradient_subtree_is_shared_bakeable(const MaterialPaintLayer &layer);

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

/**
 * The midpoint the ramp position is bent by: the stored one in the Midpoint distribution, and the
 * unbent 0.5 in the Ramp distribution, where the ramp's stops alone place the colours. The one
 * place the two are told apart, so the CPU, the generated graph and the hashes cannot disagree.
 */
float BKE_paint_layers_gradient_midpoint_effective(const MaterialPaintLayerGradient &gradient);

/** Choose what spreads the colours along the line; a value edit. False for an unknown value. */
bool BKE_paint_layers_gradient_set_distribution(
    Material &ma, MaterialPaintLayer &layer, eMaterialPaintLayerGradientDistribution distribution);

/** Choose when the handles are shown. A viewport aid: it never rebuilds the tree. */
bool BKE_paint_layers_gradient_set_gizmo_display(
    Material &ma, MaterialPaintLayer &layer, eMaterialPaintLayerGradientGizmoDisplay display);

/** Set the ramp's midpoint (clamped to the usable range); a value edit. */
bool BKE_paint_layers_gradient_set_midpoint(Material &ma,
                                            MaterialPaintLayer &layer,
                                            float midpoint);

/** Set the space the line is defined in; a topology edit. False for an unknown value. */
bool BKE_paint_layers_gradient_set_space(Material &ma,
                                         MaterialPaintLayer &layer,
                                         eMaterialPaintLayerGradientSpace space);

/**
 * Switch the row's space while keeping the line where it is on the surface of \a object.
 *
 * \a mesh is the evaluated mesh of \a object, or null when there is none. Each end is converted
 * through object space: UV is mapped on the render-active UV map (a point outside every island
 * lands on the nearest one), and object/generated use the shader's box transform. A point off the
 * surface loses its distance from it when converted to UV.
 *
 * When \a mesh is null, or an end cannot be placed, the numbers are left as they are and only the
 * space changes; \a r_converted tells the caller. The space always ends up set.
 */
bool BKE_paint_layers_gradient_convert_space(Material &ma,
                                             MaterialPaintLayer &layer,
                                             const Object *object,
                                             const Mesh *mesh,
                                             eMaterialPaintLayerGradientSpace space,
                                             bool &r_converted);

/**
 * Move one ramp stop along the ramp. The value is clamped to 0..1 and to the neighbouring stops,
 * so the ramp's order never changes. A ramp edit rebuilds the generated ColorRamp node, as its
 * storage is the node itself. False for an out-of-range index or a non-finite position.
 */
bool BKE_paint_layers_gradient_set_stop_position(Material &ma,
                                                 MaterialPaintLayer &layer,
                                                 int index,
                                                 float position);

/**
 * Move one stop of the ramp the gizmo shows for \a channel. Same clamping and rebuild rules as
 * #BKE_paint_layers_gradient_set_stop_position.
 */
bool BKE_paint_layers_gradient_set_channel_stop_position(Material &ma,
                                                         MaterialPaintLayer &layer,
                                                         eMaterialPaintChannel channel,
                                                         int index,
                                                         float position);

/**
 * Show or hide the row's line gizmo. A viewport aid only: the flag is not read by the generated
 * graph or the CPU composite, so toggling it never rebuilds the tree.
 */
bool BKE_paint_layers_gradient_set_gizmo_shown(Material &ma,
                                               MaterialPaintLayer &layer,
                                               bool shown);

/** Set the line's start; a value edit. False for a non-finite value. */
bool BKE_paint_layers_gradient_set_start(Material &ma,
                                         MaterialPaintLayer &layer,
                                         const float start[3]);

/** Set the line's end; a value edit. False for a non-finite value. */
bool BKE_paint_layers_gradient_set_end(Material &ma,
                                       MaterialPaintLayer &layer,
                                       const float end[3]);

/** Set both ends of the line; a value edit. A degenerate line is stored as given. */
bool BKE_paint_layers_gradient_set_line(Material &ma,
                                        MaterialPaintLayer &layer,
                                        const float start[3],
                                        const float end[3]);

/** Set a scalar channel's two ends; a value edit. Clamped to the channel's own range. Also keeps
 * the first and last stops of the channel's value ramp in step. */
bool BKE_paint_layers_gradient_set_channel_scalars(Material &ma,
                                                   MaterialPaintLayer &layer,
                                                   eMaterialPaintChannel channel,
                                                   float start,
                                                   float end);

/**
 * The value ramp of the scalar channel \a record, allocated on first use as black at 0 and white at
 * 1. The stops store the channel's normalized value in grey RGB (0..1). Null for a colour channel,
 * a Normal, or a record that is not part of a Gradient generator.
 */
ColorBand *BKE_paint_layers_gradient_channel_ramp_ensure(MaterialPaintLayerChannel &record);

/**
 * Give the scalar channel \a channel the same stop count and positions as the row's Base Color
 * ramp. The values of the existing stops are sampled at the new positions, so a user who first lays
 * out colours and then tunes a channel keeps the tune. The interpolation modes follow Base Color.
 * A topology edit. False for a colour channel, a missing record or a row without a gradient.
 */
bool BKE_paint_layers_gradient_sync_channel_ramp(Material &ma,
                                                 MaterialPaintLayer &layer,
                                                 eMaterialPaintChannel channel);

}  // namespace blender
