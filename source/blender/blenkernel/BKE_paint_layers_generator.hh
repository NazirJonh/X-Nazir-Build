/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The placement of a procedural generator's pattern, shared by the two consumers that must agree on
 * where a texel sits inside it: the CPU composite, which reads the coordinate here before its math,
 * and the generated shader tree, whose node chain this describes. The tree builds the same four
 * steps out of nodes -- a scale along Y, a turn around Z, an offset, and the seed's move -- and the
 * seed enters it as a constant computed here, so a hash the CPU made up cannot differ from the one
 * the graph carries.
 *
 * The Gradient keeps its own placement in #MaterialPaintLayerGradient; this is for the generators
 * whose parameters live in the row's kind schema. The same header answers what any generator's bake
 * can be cached as, since the answer follows the coordinate space rather than the kind.
 */

#pragma once

#include "BLI_math_vector_types.hh"

#include "BKE_paint_layers.hh"

#include "DNA_color_types.h"
#include "DNA_material_types.h"

namespace blender {

/**
 * The move a seed gives the pattern, in the coordinate's own units: a pair of the irrational
 * low-discrepancy sequence, so neighbouring seeds land far apart without a table or a random
 * generator that the CPU and the graph could disagree about.
 */
float3 paint_layer_generator_seed_offset(int seed);

/**
 * The Y ratio the placement shears by, from the absolute X and Y scales the schema stores: one while
 * \a locked (the pattern reads the X scale along both axes), else `scale_y / scale_x`. The scale
 * bounds of the schema keep \a scale_x away from zero.
 */
float paint_layer_generator_scale_y_ratio(float scale_x, float scale_y, bool locked);

/**
 * The coordinate of \a coordinate under a generator's placement: turned about Z by
 * #rotation_degrees, stretched along Y by \a scale_y, then moved by the offset and the seed's own
 * move.
 *
 * The pattern's own scale is not part of this: it reaches the node's Scale socket, which multiplies
 * the result. A placement applied before it keeps the offset in the coordinate's units, so a slider
 * of 0.1 moves the pattern by a tenth of the UV square whatever the scale. \a scale_y is the ratio
 * #paint_layer_generator_scale_y_ratio answers, not the absolute scale.
 */
float3 paint_layer_generator_coordinate(float3 coordinate,
                                        float scale_y,
                                        float rotation_degrees,
                                        float offset_x,
                                        float offset_y,
                                        int seed);

/**
 * Whether the placement leaves the coordinate as it is: the tree then skips its whole chain, which
 * is what keeps an untouched generator's graph the same shape as one with no placement at all. A
 * seed of zero is the only seed that places nothing.
 */
bool paint_layer_generator_placement_is_identity(float scale_y,
                                                 float rotation_degrees,
                                                 float offset_x,
                                                 float offset_y,
                                                 int seed);

/* -------------------------------------------------------------------- */
/** \name What a generator's bake can be cached as
 *
 * The same questions the row bake asks of a Gradient before there were other generators, answered
 * of every kind: whether the pattern can be evaluated into a map at all, and whether one map can
 * serve every object of the material or each object needs its own. The bake, the planner and the
 * preview all read these, so none of them has to know which kind it is looking at.
 * \{ */

/** Whether \a layer is a generator row, correction or mask item of any kind. */
bool BKE_paint_layers_generator_supported(const MaterialPaintLayer &layer);

/**
 * The #eMaterialPaintLayerGradientSpace \a layer's pattern reads its coordinate in: the payload's
 * for a Gradient, the schema's for every other kind. UV for a row that generates nothing, or for a
 * Gradient whose payload is missing.
 */
int BKE_paint_layers_generator_space_of(const MaterialPaintLayer &layer);

/**
 * Whether \a layer's own pattern can be evaluated into a bake at all: a Gradient row needs its
 * payload, which is where its line and its ramp live, and every other generator carries its
 * parameters in the row itself. A row that is not a generator answers true; it asks no bake.
 */
bool BKE_paint_layers_generator_is_bakeable(const MaterialPaintLayer &layer);

/** Whether \a layer's pattern reads the texel's own UV, so one map serves every object. */
bool BKE_paint_layers_generator_is_shared_bakeable(const MaterialPaintLayer &layer);

/** Whether \a layer and everything below it can be baked at all (see
 * #BKE_paint_layers_generator_is_bakeable). */
bool BKE_paint_layers_generator_subtree_is_bakeable(const MaterialPaintLayer &layer);

/** Whether \a layer and everything below it can be shared between the objects of the material:
 * false when the row, a child, a correction or a mask item reads an Object or a Generated space. */
bool BKE_paint_layers_generator_subtree_is_shared_bakeable(const MaterialPaintLayer &layer);

/**
 * Whether an enabled generator is laid over \a layer with no cache of its own to collapse it: one of
 * the row's corrections or mask items generates, or a folder below it holds one. A generator child
 * row does not count: the child is a bake candidate itself, and its cache already stands in for it.
 * The row itself is not asked either, for the same reason.
 */
bool BKE_paint_layers_generator_subtree_holds_laid_generator(const MaterialPaintLayer &layer);

/**
 * Whether a generator reading the Generated space is laid over \a layer -- in the row, one of its
 * corrections or mask items, or in a folder below it. Only such a subtree reads the texture-space
 * orco grid, which the surface of a bake then has to carry; a bake without one passes it over.
 */
bool BKE_paint_layers_generator_subtree_holds_generated(const MaterialPaintLayer &layer);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Adding a generator and switching its kind
 *
 * The row-building half of the kind registry: one Add that takes any generator, and the switch that
 * turns a generator row, correction or mask item into another kind. Both leave the row ready to
 * evaluate -- payload or schema parameters, the colour band, and a channel record for the row to
 * start in -- so no caller has to know which of the two storages a kind uses. \{ */

/**
 * Add a generator row of \a generator: a procedural pattern with no maps, evaluated by the CPU with
 * the same formula the generated shader nodes carry. The row starts in the first channel of the
 * material's set, with the kind's own defaults and a black-to-white band.
 *
 * \param name: the row's name, or null for a unique "<kind> N".
 * \return the new row, or null when the stack refuses the Add or \a generator names no kind.
 */
MaterialPaintLayer *BKE_paint_layers_generator_add(Material &ma,
                                                   eMaterialPaintLayerGenerator generator,
                                                   const char *name,
                                                   MaterialPaintLayer *anchor,
                                                   PaintLayerPlace place);

/**
 * Make \a row's generator \a generator: the same row, its role, its name and its channel records
 * kept, with the new kind's parameters initialized to their schema defaults. A Gradient's payload is
 * left on the row (which only reads it while its kind is Gradient), so switching back restores the
 * line the user drew. False when \a generator names no kind, or the kind does not allow the row's
 * role; a row that is not a generator at all cannot be switched.
 */
bool BKE_paint_layers_generator_kind_set(Material &ma,
                                         MaterialPaintLayer &row,
                                         eMaterialPaintLayerGenerator generator);

/** \} */

/* -------------------------------------------------------------------- */
/** \name The ramps and the value ends of every generator
 *
 * A generator's colour channels read one band and each scalar channel its own value ramp; where the
 * colour band lives is the only thing that tells a Gradient from the kinds whose parameters are a
 * kind schema. These answer the question for both, so the panel, the bake and the composite need one
 * set of accessors rather than a branch on the kind. \{ */

/**
 * The ramp a generator row's \a channel reads: the colour band, which is the Gradient payload's or
 * the row's own #MaterialPaintLayerRamp, and for a scalar channel the record's value ramp -- null
 * when the row generates nothing, is a Normal, or has no record for \a channel yet.
 */
const ColorBand *BKE_paint_layers_generator_channel_ramp_get(const MaterialPaintLayer &row,
                                                             int channel);
ColorBand *BKE_paint_layers_generator_channel_ramp_get(MaterialPaintLayer &row, int channel);

/**
 * Whether the row's kind still reads the kind parameter named \a name under the choices made now: a
 * generator hides the parameter whose socket the node's signature leaves out (a Minkowski-only
 * Exponent, the Offset of a noise type that reads none) and the Wave direction its type does not
 * ask. Every other kind, and every choice the node keeps in its storage, reads whatever the state
 * is, so the answer is true there.
 */
bool BKE_paint_layers_generator_param_is_active(const MaterialPaintLayer &row, const char *name);

/**
 * The value ramp of the scalar channel \a record, allocated on first use and seeded from the two
 * ends the record already keeps. Null for a colour channel or a Normal.
 */
ColorBand *BKE_paint_layers_generator_channel_ramp_ensure(MaterialPaintLayerChannel &record);

/**
 * Write the channel values \a start and \a end into the first and last stop of a value ramp, as grey
 * in 0..1 over the channel's range. The inner stops are left alone. Not a rebuild: the caller tags
 * the edit.
 */
void BKE_paint_layers_generator_channel_ramp_ends_set(ColorBand &ramp,
                                                      eMaterialPaintChannel channel,
                                                      float start,
                                                      float end);

/** The channel value of the first (or, with \a end, the last) stop of a value ramp. */
float BKE_paint_layers_generator_channel_ramp_end_get(const ColorBand &ramp,
                                                      eMaterialPaintChannel channel,
                                                      bool end);

/**
 * Set the two ends a generator row's scalar channel \a channel spans, clamped to the channel's own
 * range, and keep the outer stops of its value ramp in step with them. A value edit; false for a
 * colour channel, a Normal, a row that generates nothing or a row without a record for the channel.
 */
bool BKE_paint_layers_generator_set_channel_scalars(Material &ma,
                                                    MaterialPaintLayer &row,
                                                    eMaterialPaintChannel channel,
                                                    float start,
                                                    float end);

/**
 * Whether \a data is a generator's colour band, a scalar channel's value ramp or one of their stops,
 * tagging \a ma for a rebuild and a re-bake when it is. The ColorRamp RNA callbacks receive either
 * the band itself or a #CBData inside it, so this is the one place that recognizes both.
 */
bool BKE_paint_layers_generator_ramp_edited(Material &ma, const void *data);

/** \} */

}  // namespace blender
