/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The one list of the Paint Layers procedural generators. The CPU composite, the generated graph
 * and the bake predicates read it instead of naming a generator each, so the CPU and the GPU
 * halves of a generator are registered together and a new one is an entry rather than a branch per
 * consumer.
 */

#include "BKE_paint_layers_generator_registry.hh"

#include "BKE_node_legacy_types.hh"
#include "BKE_paint_layers_gradient.hh"
#include "BKE_paint_material_composite.hh"

#include "BLT_translation.hh"

#include "BLI_utildefines.h"

#include "DNA_node_types.h"

#include "paint_layers_generator_registry_intern.hh"
#include "paint_material_generator_group_ids.hh"

namespace blender {

/**
 * The Gradient's CPU factor through the composite's payload: the adapter unwraps the line the
 * composite carries and reads the one formula the generated group builds node by node. The
 * subtype rides the payload, so the composite's sample reaches this through the table without
 * naming the Gradient.
 */
static float paint_layer_generator_gradient_cpu_factor(
    const PaintMaterialCompositeGradient &gradient, const float3 &coordinate)
{
  return BKE_paint_layers_gradient_factor(gradient.payload, coordinate);
}

/* New generators are added here, subtype first: the entry names the CPU factor, the GPU emission
 * and every other half its consumers read, so the two (and the rest) cannot be registered apart
 * -- the parity the graph eval tests hold the module to. A generator that needs more than its
 * entry (a payload of its own, a bake policy) is a `TODO:` naming it, the way the kind registry
 * defers its own. */
static constexpr PaintLayerGeneratorInfo generators[] = {
    /* A ramp has no direction to read into: the Normal channel is skipped before the emission runs
     * (see #PaintLayerGeneratorFlag::PAINT_LAYER_GENERATOR_NO_NORMAL). Its pattern is its own factor
     * group, not one node: the build takes the Gradient row through the payload's wrapper. */
    {.subtype = MA_PAINT_LAYER_GENERATOR_GRADIENT,
     .name = "Gradient",
     .identifier = "GRADIENT",
     .menu_name = N_("Gradient Layer"),
     .description = N_("A colour and coverage ramp along a line or around a point"),
     .flags = PAINT_LAYER_GENERATOR_NO_NORMAL,
     .cpu_factor = &paint_layer_generator_gradient_cpu_factor,
     .gpu_emit = &bke::paint_layers::paint_layer_generator_gradient_gpu_emit},
    /* A parametric pattern's CPU factor reads the same node math the emission builds, so the pair
     * lands in one entry each. Every pattern is a height field like the Gradient. The factors live
     * in `paint_material_composite_generator.cc`, the node storage and the emissions beside the
     * build's dispatch. The Voronoi cell colour is the one setting under which a pattern lays a
     * colour of its own instead of an index into the row's band (see `own_color`). */
    {.subtype = MA_PAINT_LAYER_GENERATOR_NOISE,
     .name = "Noise",
     .identifier = "NOISE",
     .menu_name = N_("Noise Layer"),
     .description = N_("A Perlin-family noise over the placement coordinates, the base of procedural dirt"),
     .flags = PAINT_LAYER_GENERATOR_NO_NORMAL,
     .cpu_factor = &paint_layer_generator_noise_cpu_factor,
     .shader_node_type = SH_NODE_TEX_NOISE,
     .node_storage_init = &bke::paint_layers::paint_layer_generator_noise_storage_init,
     .gpu_emit = &bke::paint_layers::paint_layer_generator_noise_gpu_emit},
    {.subtype = MA_PAINT_LAYER_GENERATOR_VORONOI,
     .name = "Voronoi",
     .identifier = "VORONOI",
     .menu_name = N_("Voronoi Layer"),
     .description = N_("The cells of scattered points: their distance, their edge, or their own colour"),
     .flags = PAINT_LAYER_GENERATOR_NO_NORMAL | PAINT_LAYER_GENERATOR_OWN_COLOR,
     .cpu_factor = &paint_layer_generator_voronoi_cpu_factor,
     .own_color = &paint_layer_generator_voronoi_own_color,
     .shader_node_type = SH_NODE_TEX_VORONOI,
     .node_storage_init = &bke::paint_layers::paint_layer_generator_voronoi_storage_init,
     .gpu_emit = &bke::paint_layers::paint_layer_generator_voronoi_gpu_emit},
    /* The checker's storage holds nothing the row decides: its colours are the ramp's endpoints. */
    {.subtype = MA_PAINT_LAYER_GENERATOR_CHECKER,
     .name = "Checker",
     .identifier = "CHECKER",
     .menu_name = N_("Checker Layer"),
     .description = N_("The two-colour checkerboard of the placement grid"),
     .flags = PAINT_LAYER_GENERATOR_NO_NORMAL,
     .cpu_factor = &paint_layer_generator_checker_cpu_factor,
     .shader_node_type = SH_NODE_TEX_CHECKER,
     .gpu_emit = &bke::paint_layers::paint_layer_generator_checker_gpu_emit},
    {.subtype = MA_PAINT_LAYER_GENERATOR_BRICK,
     .name = "Brick",
     .identifier = "BRICK",
     .menu_name = N_("Brick Layer"),
     .description = N_("Stacked bricks with a mortar seam, offset and squashed per row"),
     .flags = PAINT_LAYER_GENERATOR_NO_NORMAL,
     .cpu_factor = &paint_layer_generator_brick_cpu_factor,
     .shader_node_type = SH_NODE_TEX_BRICK,
     .node_storage_init = &bke::paint_layers::paint_layer_generator_brick_storage_init,
     .gpu_emit = &bke::paint_layers::paint_layer_generator_brick_gpu_emit},
    {.subtype = MA_PAINT_LAYER_GENERATOR_WAVE,
     .name = "Wave",
     .identifier = "WAVE",
     .menu_name = N_("Wave Layer"),
     .description = N_("Bands or rings along an axis, through a sine, saw or triangle profile"),
     .flags = PAINT_LAYER_GENERATOR_NO_NORMAL,
     .cpu_factor = &paint_layer_generator_wave_cpu_factor,
     .shader_node_type = SH_NODE_TEX_WAVE,
     .node_storage_init = &bke::paint_layers::paint_layer_generator_wave_storage_init,
     .gpu_emit = &bke::paint_layers::paint_layer_generator_wave_gpu_emit},
    /* The compound generators: the CPU factor, the shared group of their kind and its interface
     * identifiers land in one entry, so the halves cannot be registered apart. The group is made
     * the first time a row of the kind asks for it, one per dimension. */
    {.subtype = MA_PAINT_LAYER_GENERATOR_SCRATCHES,
     .name = "Scratches",
     .identifier = "SCRATCHES",
     .menu_name = N_("Scratches Layer"),
     .description = N_("Stretched Voronoi edges thresholded to lines of a chosen width"),
     .flags = PAINT_LAYER_GENERATOR_NO_NORMAL | PAINT_LAYER_GENERATOR_COMPOUND,
     .cpu_factor = &paint_layer_generator_scratches_cpu_factor,
     .node_group_ensure = &BKE_paint_material_scratches_group_ensure,
     .group_coordinate_id = SCRATCHES_GROUP_ID_COORDINATE,
     .group_factor_id = SCRATCHES_GROUP_ID_FACTOR,
     .gpu_emit = &bke::paint_layers::paint_layer_generator_scratches_gpu_emit},
    {.subtype = MA_PAINT_LAYER_GENERATOR_WEAVE,
     .name = "Weave",
     .identifier = "WEAVE",
     .menu_name = N_("Weave Layer"),
     .description = N_("Two directions of threads crossing with a chosen over/under step"),
     .flags = PAINT_LAYER_GENERATOR_NO_NORMAL | PAINT_LAYER_GENERATOR_COMPOUND,
     .cpu_factor = &paint_layer_generator_weave_cpu_factor,
     .node_group_ensure = &BKE_paint_material_weave_group_ensure,
     .group_coordinate_id = WEAVE_GROUP_ID_COORDINATE,
     .group_factor_id = WEAVE_GROUP_ID_FACTOR,
     .gpu_emit = &bke::paint_layers::paint_layer_generator_weave_gpu_emit},
    {.subtype = MA_PAINT_LAYER_GENERATOR_GRUNGE,
     .name = "Grunge",
     .identifier = "GRUNGE",
     .menu_name = N_("Grunge Layer"),
     .description = N_("Noise modulated by a second, higher-frequency noise and thresholded, dirt in one row"),
     .flags = PAINT_LAYER_GENERATOR_NO_NORMAL | PAINT_LAYER_GENERATOR_COMPOUND,
     .cpu_factor = &paint_layer_generator_grunge_cpu_factor,
     .node_group_ensure = &BKE_paint_material_grunge_group_ensure,
     .group_coordinate_id = GRUNGE_GROUP_ID_COORDINATE,
     .group_factor_id = GRUNGE_GROUP_ID_FACTOR,
     .gpu_emit = &bke::paint_layers::paint_layer_generator_grunge_gpu_emit},
};

const PaintLayerGeneratorInfo *BKE_paint_layers_generator_find(const int16_t subtype)
{
  for (const PaintLayerGeneratorInfo &info : generators) {
    if (info.subtype == subtype) {
      return &info;
    }
  }
  /* A subtype this build does not know reads as no generator: the kind schema has no entry for it
   * either, so every consumer has already passed the row by. */
  return nullptr;
}

Span<const PaintLayerGeneratorInfo> BKE_paint_layers_generator_all()
{
  return Span<const PaintLayerGeneratorInfo>(generators, ARRAY_SIZE(generators));
}

}  // namespace blender
