/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * The registry of the Paint Layers procedural generators: one descriptor per
 * #eMaterialPaintLayerGenerator subtype, in the spirit of #PaintLayerKindInfo. The dispatch points
 * over a generator row -- the CPU factor in the composite, the GPU emission in the generated graph
 * -- read this table. A new generator becomes one entry here, a kind schema of its own, and the
 * two functions its entry names; the functions themselves are declared in the registry's internal
 * header (`paint_layers_generator_registry_intern.hh`), beside the table that names them.
 */

#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"

#include "BKE_paint_layers.hh"

#include "DNA_material_types.h"

namespace blender {

struct bNode;
struct bNodeTree;
struct Main;

/**
 * The composite's generator payload, defined in BKE_paint_material_composite.hh and never included
 * here: the factor signatures take it by reference, so the registry stays free of the composite's
 * own headers.
 */
struct PaintMaterialCompositeGradient;

namespace bke::paint_layers {

struct KindEmitResult;
class PaintLayersTreeBuilder;
struct RowEmitContext;

/**
 * The GPU emission of one generator: the nodes its row lays as the source of a channel, built into
 * the row's group by the generator's build translation unit. The builder argument is the internal
 * chain builder; the type is only complete to that build code, so this field is called from there
 * alone, through #PaintLayersTreeBuilder::generator_source_emit.
 */
using PaintLayerGeneratorGpuEmit = KindEmitResult (*)(PaintLayersTreeBuilder &builder,
                                                      const RowEmitContext &ctx);

}  // namespace bke::paint_layers

/**
 * The largest parameter schema a generator kind has: the longest of the `PAINT_LAYER_GEN_*_PARAM_NUM`
 * values the kind schemas lay out (`paint_layers_kind.hh` asserts each against it), and the length
 * of the composite's generator item (`BKE_paint_material_composite.hh` aliases it, the one array
 * that carries every kind's parameters). One constant, so a schema can never outgrow the storage
 * that carries it without both sides seeing it.
 */
inline constexpr int PAINT_LAYER_GENERATOR_PARAM_MAX = 20;

/** Bits of #PaintLayerGeneratorInfo::flags. */
enum PaintLayerGeneratorFlag : uint16_t {
  /**
   * The generator lays no colour of its own in the Normal channel: a height field has no direction
   * to read its ramp into. The dispatch skips the channel before the emission is called, so an
   * emission function never has to answer for it.
   */
  PAINT_LAYER_GENERATOR_NO_NORMAL = 1 << 0,
  /**
   * The generator's pattern is a graph of its own -- a shared node group the row instantiates,
   * built by #PaintLayerGeneratorInfo::node_group_ensure -- rather than one shader node the row
   * places (#PaintLayerGeneratorInfo::shader_node_type). The build and the group factory read the
   * bit instead of naming the compound kinds.
   */
  PAINT_LAYER_GENERATOR_COMPOUND = 1 << 1,
  /**
   * The generator has an own-colour answer under some choice of its parameters -- a colour no ramp
   * could express (the Voronoi cell colour). #PaintLayerGeneratorInfo::own_color is the CPU side;
   * the composite reads the bit before it calls the function.
   */
  PAINT_LAYER_GENERATOR_OWN_COLOR = 1 << 2,
};

/**
 * What one generator subtype is, as far as its consumers care. The CPU and the GPU halves share
 * the entry so they cannot be registered apart: adding a generator registers both at once, which
 * is the CPU/GPU parity this module lives on.
 */
struct PaintLayerGeneratorInfo {
  /** Registry key: #MaterialPaintLayer::kind_subtype of a #MA_PAINT_LAYER_SOURCE_GENERATOR row. */
  int16_t subtype;
  /**
   * The generator's name, as the kind schema spells it. Doubles as the UI label: the outliner slot
   * and the Add menu compose their items with it, and the RNA enum's item name is held to it by
   * the parity test.
   */
  const char *name;
  /**
   * The generator's identifier in caps ("NOISE"), the key of the Add menu's look entries and of
   * the RNA enum's item identifier, both held to it by the parity test.
   */
  const char *identifier;
  /**
   * The Add menu's layer entry name ("Noise Layer"), as one translatable unit: the outliner walks
   * the table for its items, so a ninth generator's menu entry is this field, not a row there.
   * Wrapped in #N_ here, where the extraction reads it.
   */
  const char *menu_name;
  /**
   * The one-line description the Add menu's layer entry shows. Wrapped in #N_ here, where the
   * extraction reads it; the menu translates at use.
   */
  const char *description;
  /** #PaintLayerGeneratorFlag bits. */
  uint16_t flags;
  /**
   * The CPU factor: the ramp position at \a coordinate, read from the composite's payload. The
   * parameter is the composite's own generator payload rather than the DNA one, so a factor can
   * read the scalar ends and the surface context beside the line without a second dispatch. The
   * one formula the CPU composite, the bake and the tests read; the generated group
   * (#BKE_paint_material_gradient_group_ensure) builds the same arithmetic node by node.
   */
  float (*cpu_factor)(const PaintMaterialCompositeGradient &gradient, const float3 &coordinate);
  /**
   * The colour the generator lays where no ramp could express it, in straight scene-linear RGBA,
   * or false when the row's current parameters give none (a switch of its own off, a feature
   * without a colour). Null for a generator that never answers one; read behind the
   * #PAINT_LAYER_GENERATOR_OWN_COLOR bit. Defined beside the CPU factors.
   */
  bool (*own_color)(const PaintMaterialCompositeGradient &gradient,
                    const float3 &coordinate,
                    float r_rgba[4]);
  /**
   * The one shader node the pattern is (#SH_NODE_TEX_NOISE and friends), for a single-node
   * generator; zero for a generator whose pattern is not one node (the Gradient's group, a
   * compound's shared group).
   */
  int shader_node_type;
  /**
   * Write the node's storage the row's schema decides (the noise type, the Voronoi feature, the
   * brick's offsets): the choices that are the node's own switches rather than socket values.
   * Called between the node's creation and its declaration update; null when the node's storage
   * needs nothing of the row.
   */
  void (*node_storage_init)(bNode &pattern, const MaterialPaintLayer &layer, bool is_2d);
  /**
   * The shared node group of a compound pattern, one per dimension, made in \a bmain the first
   * time a row of the kind asks for it. Null for every generator that is not
   * #PAINT_LAYER_GENERATOR_COMPOUND; the regen context hands the build a wrapper over it.
   */
  bNodeTree *(*node_group_ensure)(Main &bmain, bool is_2d);
  /**
   * The Coordinate input and the Factor output of the shared group, by the identifiers its
   * creation order fixed (`paint_material_generator_group_ids.hh`); null for a generator that is
   * not a compound one.
   */
  const char *group_coordinate_id;
  const char *group_factor_id;
  /** The GPU emission, as #PaintLayerGeneratorGpuEmit spells out. */
  bke::paint_layers::PaintLayerGeneratorGpuEmit gpu_emit;
};

/**
 * The generator of \a subtype, or null when this build does not know it (a file from a newer one).
 */
const PaintLayerGeneratorInfo *BKE_paint_layers_generator_find(int16_t subtype);

/**
 * The generator of \a row, or null when the row is not a generator or its subtype has no entry in
 * the table yet. The kind registry knows the parametric generators before their CPU and GPU halves
 * are registered here, so such a row is a valid one that simply lays nothing; the consumers skip
 * it, as they skip a subtype saved by a newer build.
 */
inline const PaintLayerGeneratorInfo *BKE_paint_layers_generator_of(const MaterialPaintLayer &row)
{
  if (row.source != MA_PAINT_LAYER_SOURCE_GENERATOR) {
    return nullptr;
  }
  return BKE_paint_layers_generator_find(row.kind_subtype);
}

/**
 * Every generator this build registers, subtype order (the Gradient first). The consumers that
 * list the generators -- the outliner's Add menu, the parity tests -- walk this instead of naming
 * them, so a ninth generator is one entry here and nothing beside.
 */
Span<const PaintLayerGeneratorInfo> BKE_paint_layers_generator_all();

}  // namespace blender
