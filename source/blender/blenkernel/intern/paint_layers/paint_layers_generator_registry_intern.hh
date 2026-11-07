/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * The functions the generator table names: the CPU factors, defined beside the composite's
 * per-texel sample in `paint_material_composite_generator.cc`, and the GPU emissions, defined
 * beside the build dispatch in `paint_layers_generate_build_generator.cc`. Internal to the module
 * -- a consumer reads the table (#BKE_paint_layers_generator_find), never these -- and declared
 * here so the table's entries cannot name a function whose signature has drifted.
 */

#include "BKE_paint_layers_generator_registry.hh"

namespace blender {

/**
 * The CPU factor of one parametric generator, as the generator table's #cpu_factor names it: the
 * pattern's ramp position at \a coordinate, read from the composite's payload. Defined in
 * `paint_material_composite_generator.cc`, beside the node math they spell out; the registry's
 * entries point here.
 */
float paint_layer_generator_noise_cpu_factor(const PaintMaterialCompositeGradient &gradient,
                                             const float3 &coordinate);
float paint_layer_generator_voronoi_cpu_factor(const PaintMaterialCompositeGradient &gradient,
                                               const float3 &coordinate);
float paint_layer_generator_checker_cpu_factor(const PaintMaterialCompositeGradient &gradient,
                                               const float3 &coordinate);
float paint_layer_generator_brick_cpu_factor(const PaintMaterialCompositeGradient &gradient,
                                             const float3 &coordinate);
float paint_layer_generator_wave_cpu_factor(const PaintMaterialCompositeGradient &gradient,
                                            const float3 &coordinate);
float paint_layer_generator_scratches_cpu_factor(const PaintMaterialCompositeGradient &gradient,
                                                 const float3 &coordinate);
float paint_layer_generator_weave_cpu_factor(const PaintMaterialCompositeGradient &gradient,
                                             const float3 &coordinate);
float paint_layer_generator_grunge_cpu_factor(const PaintMaterialCompositeGradient &gradient,
                                              const float3 &coordinate);

/**
 * The registry's #PaintLayerGeneratorInfo::own_color for the Voronoi: the cell colour the pattern
 * lays where its ramp would otherwise index, when its Output switch asks for it and its feature
 * has a colour to give. Defined beside the factors; false for every other setting.
 */
bool paint_layer_generator_voronoi_own_color(const PaintMaterialCompositeGradient &gradient,
                                             const float3 &coordinate,
                                             float r_rgba[4]);

namespace bke::paint_layers {

/**
 * The storage of the one shader node a single-node pattern is, from the row's schema: the node's
 * own switches (the noise type, the Voronoi feature and metric, the brick's offsets, the wave's
 * directions and profile) that are storage rather than socket values. Defined in
 * `paint_layers_generate_build_generator.cc`, beside the node's creation; the registry's
 * #PaintLayerGeneratorInfo::node_storage_init entries point here.
 */
void paint_layer_generator_noise_storage_init(bNode &pattern,
                                              const MaterialPaintLayer &layer,
                                              bool is_2d);
void paint_layer_generator_voronoi_storage_init(bNode &pattern,
                                                const MaterialPaintLayer &layer,
                                                bool is_2d);
void paint_layer_generator_brick_storage_init(bNode &pattern,
                                              const MaterialPaintLayer &layer,
                                              bool is_2d);
void paint_layer_generator_wave_storage_init(bNode &pattern,
                                             const MaterialPaintLayer &layer,
                                             bool is_2d);

/**
 * The GPU emission of the Gradient generator: the shared Gradient Factor group and the ColorRamp
 * nodes, built by the builder's own #PaintLayersTreeBuilder::gradient_source_emit.
 */
KindEmitResult paint_layer_generator_gradient_gpu_emit(PaintLayersTreeBuilder &builder,
                                                       const RowEmitContext &ctx);

/**
 * The GPU emissions of the five single-node patterns: each table entry names its own, and each
 * builds the placement chain and the one texture node its kind is, answering the ramp tail the
 * emissions share. A pattern is a height field like the Gradient, so every entry carries
 * #PAINT_LAYER_GENERATOR_NO_NORMAL the dispatch reads.
 */
KindEmitResult paint_layer_generator_noise_gpu_emit(PaintLayersTreeBuilder &builder,
                                                    const RowEmitContext &ctx);
KindEmitResult paint_layer_generator_voronoi_gpu_emit(PaintLayersTreeBuilder &builder,
                                                      const RowEmitContext &ctx);
KindEmitResult paint_layer_generator_checker_gpu_emit(PaintLayersTreeBuilder &builder,
                                                      const RowEmitContext &ctx);
KindEmitResult paint_layer_generator_brick_gpu_emit(PaintLayersTreeBuilder &builder,
                                                    const RowEmitContext &ctx);
KindEmitResult paint_layer_generator_wave_gpu_emit(PaintLayersTreeBuilder &builder,
                                                   const RowEmitContext &ctx);

/** The compound generators' emission: an instance of the kind's shared node group. */
KindEmitResult paint_layer_generator_scratches_gpu_emit(PaintLayersTreeBuilder &builder,
                                                        const RowEmitContext &ctx);
KindEmitResult paint_layer_generator_weave_gpu_emit(PaintLayersTreeBuilder &builder,
                                                    const RowEmitContext &ctx);
KindEmitResult paint_layer_generator_grunge_gpu_emit(PaintLayersTreeBuilder &builder,
                                                     const RowEmitContext &ctx);

}  // namespace bke::paint_layers
}  // namespace blender
