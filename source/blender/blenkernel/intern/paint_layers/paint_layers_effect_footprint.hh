/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The pure half of the effect-bake footprint: the preimage of the unit UV square under a row's
 * composed mapping chain (the transform effects' steps, then the row's own mapping). The public
 * entry that walks a described input and returns a pixel rectangle is
 * #BKE_paint_layers_effect_input_footprint_region in `BKE_paint_layers_composite.hh`; this header
 * names the UV-space step so the geometry stays testable without a material.
 */

#pragma once

#include "BLI_rect.h"
#include "BLI_span.hh"

#include "BKE_paint_material_composite.hh"

namespace blender::bke::paint_layers::footprint {

/**
 * Whether the composed read \a mapping describes (the \a input_steps transform effects first, the
 * row's own mapping last) bounds the content it lays to a rectangle of the canvas UV, and the
 * rectangle. The rectangle is the preimage of the unit square: content exists exactly where the
 * composed lookup lands inside the square, because a clipping axis reads transparent outside it.
 *
 * False when the read cannot bound the content: a mapping that is off or repeats an axis (the
 * composed lookup floors at the end, which tiles the content over the whole canvas), a projected
 * or Decal step (no UV form to invert here), or a chain the description truncated. A conservative
 * false is always safe -- the caller computes the whole canvas instead.
 */
bool uv_rect(const PaintMaterialCompositeMapping &mapping,
             blender::Span<PaintMaterialCompositeMapping> input_steps,
             rctf &r_uv);

/**
 * Whether the chain below \a effect (it included) carries a whole-map kind in \a channel. Such a
 * kind reads the whole canvas (a flood's labels, a reduction's sums), so no rectangle of its input
 * bounds its result: the planner must keep the whole canvas rather than fall back to a band.
 */
bool chain_has_whole_map(const Material &ma,
                         const MaterialPaintLayer &row,
                         const MaterialPaintLayer &effect,
                         int channel);

/**
 * The halo the whole chain below \a effect (it included) reaches in \a channel, at the canvas \a
 * width by \a height: the sum of every filtering effect's own halo, the growth around the content
 * footprint that keeps a neighbourhood kernel's reads on content the region path rendered. The
 * caller narrows only when every kind in the chain has a finite reach
 * (#BKE_paint_layers_effect_reach), so a whole-map kind never gets here.
 */
int paint_layer_effect_chain_halo(const Material &ma,
                                  const MaterialPaintLayer &row,
                                  const MaterialPaintLayer &effect,
                                  const int channel,
                                  const int width,
                                  const int height);

/**
 * The region of a \a size by \a size bake canvas, in bake pixels, where \a row's own bake render
 * can lay content: the preimage of the unit UV square under the row's composed read -- the
 * transform effects' steps first, the row's own mapping last, the same chain the bake render
 * composes -- grown by the halo of the row's filtering effects, whose results the bake render lays
 * over the row's content. The max edges are exclusive, padded by one texel and clamped to the
 * canvas.
 *
 * False when nothing bounds the content: a folder, a row the planner keeps live (its mapping is
 * applied by the graph, not laid into the map), a row framed off the UV plane, a reader, a
 * generator or Mesh Map row, a row with no own content, a laid correction (its own frame is not
 * this chain -- v1), a whole-map effect in the chain, or a mapping that does not bound the read
 * (off, repeating, projected). A conservative false is always safe -- the caller renders the whole
 * canvas; a wrong rectangle would keep stale content in the maps.
 */
bool row_source_region(const Material &ma,
                       const MaterialPaintLayer &row,
                       const PaintLayerTileContext &context,
                       int size,
                       rcti &r_region);

}  // namespace blender::bke::paint_layers::footprint
