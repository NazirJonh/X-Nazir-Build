/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"

namespace blender {

struct PaintMaterialCompositeGradient;
struct PaintMaterialCompositeImageLayer;
struct PaintMaterialCompositeMapping;
struct PaintMaterialCompositeProjection;

/**
 * How many generator payloads \a image_layers lays (a row, a correction and a mask item each count
 * one, folders recursed). Defined in `paint_material_composite_generator.cc`, beside the per-texel
 * sample the count prices.
 */
int composite_stack_generator_count(Span<PaintMaterialCompositeImageLayer> image_layers);

/**
 * Fold a generator payload into \a hash: every parameter moves the composited pixels. Defined in
 * `paint_material_composite_generator.cc`, beside the sample whose inputs it guards.
 */
uint64_t composite_generator_hash(uint64_t hash, const PaintMaterialCompositeGradient &gradient);

/**
 * Fold \a projection into \a hash: every value of the frame and both transforms move the pixels a
 * projected read bakes. Shared by a map's mapping hash and a projected generator line's.
 */
uint64_t composite_projection_hash(uint64_t hash, const PaintMaterialCompositeProjection &projection);

/**
 * Fold a 2D mapping (UV adjust and projection) into \a hash: every slider moves the pixels a
 * mapped read produces. Shared by the stack hash and the effect input hash.
 */
uint64_t composite_mapping_hash(uint64_t hash, const PaintMaterialCompositeMapping &mapping);

struct ID;
struct Main;
struct bNode;
struct bNodeTree;

/**
 * The marker shared by the node groups this module owns (Normal Combine, Blend, Gradient): one
 * IDProperty key, told apart by \a marker_value. Found by marker, so a rename never loses a group.
 */
bool paint_material_group_has_marker(const bNode &node, const char *marker_value);

/** The group of \a bmain carrying \a marker_value, or null. */
bNodeTree *paint_material_group_find(Main &bmain, const char *marker_value);

/** Stamp \a group with \a marker_value. */
void paint_material_group_marker_set(bNodeTree &group, const char *marker_value);

/**
 * The interface-socket identifiers the groups above (and the Scratches, Weave and Grunge ones)
 * answer for live in `paint_layers/paint_material_generator_group_ids.hh`, the light header the
 * groups' readers share -- this module's own translation units included, which read instances'
 * sockets by identifier and so include it too.
 */

/**
 * Test hooks. Both count work a caller can pay once per stack or, by mistake, once per region of
 * one, which no assertion on pixels can tell apart.
 */

/** How many times a linear composite acquired its layer buffers since the last reset. */
int paint_material_composite_acquire_call_count();
void paint_material_composite_acquire_call_count_reset();

/** How many Compare masks were resampled onto another grid since the last reset. */
int paint_material_composite_height_won_resample_count();
void paint_material_composite_height_won_resample_count_reset();

}  // namespace blender
