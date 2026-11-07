/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The shared Scratches group: the shader-tree half of the Scratches generator. The CPU composite
 * calls #BKE_paint_material_scratches_factor, which builds the same arithmetic; every node and every
 * constant below has a counterpart there.
 *
 * A scratch is a Voronoi edge that has been pulled along X, so the group is a stretched coordinate,
 * one distance-to-edge node, and a threshold wide enough to give the edge a line of its own. The
 * threshold is arithmetic rather than a Color Band because a scratch's width is a parameter the user
 * reads in the cells' own distance units, not a position along a ramp.
 */

#include <algorithm>
#include <iterator>

#include "BKE_paint_material_composite.hh"

#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_tree_update.hh"

#include "BLI_math_vector_types.hh"
#include "BLI_noise.hh"
#include "BLI_string.h"

#include "BLT_translation.hh"

#include "DNA_node_tree_interface_types.h"
#include "DNA_node_types.h"

#include "paint_layers/paint_material_generator_group_ids.hh"
#include "paint_material_composite_internal.hh"
#include "paint_material_group_builder.hh"
#include "paint_layers/paint_layers_kind.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Scratches group
 * \{ */

/** Marker values of the Scratches groups; the two dimensions are two groups. */
static constexpr const char *SCRATCHES_MARKER_2D = "SCRATCHES_2D";
static constexpr const char *SCRATCHES_MARKER_3D = "SCRATCHES_3D";

/** Names of the shared group data-blocks. The leading dot keeps them out of every ID picker. */
static const char *SCRATCHES_TREE_NAME_2D = ".PBR Scratches 2D";
static const char *SCRATCHES_TREE_NAME_3D = ".PBR Scratches 3D";

bool BKE_paint_material_is_scratches_group(const bNode &node)
{
  return paint_material_group_has_marker(node, SCRATCHES_MARKER_2D) ||
         paint_material_group_has_marker(node, SCRATCHES_MARKER_3D);
}

namespace {

/**
 * The parameters the Voronoi node reads through a socket of its own, then the three of the line. In
 * the order they become the group's inputs, which is the order the identifiers in
 * #paint_material_composite_internal.hh say they are.
 */
static const GroupFloatInput scratches_inputs[] = {
    {"Scale", 5.0f, 0.01f, 1000.0f},
    {"Detail", 0.0f, 0.0f, 15.0f},
    {"Roughness", 0.5f, 0.0f, 1.0f},
    {"Lacunarity", 2.0f, 1.0f, 8.0f},
    {"Randomness", 1.0f, 0.0f, 1.0f},
    {"Width", 0.02f, 0.0f, 0.5f},
    {"Soften", 0.02f, 0.0f, 0.25f},
    {"Stretch", 3.0f, 1.0f, 20.0f},
};

}  // namespace

bNodeTree *BKE_paint_material_scratches_group_ensure(Main &bmain, const bool is_2d)
{
  const char *marker = is_2d ? SCRATCHES_MARKER_2D : SCRATCHES_MARKER_3D;
  const char *tree_name = is_2d ? SCRATCHES_TREE_NAME_2D : SCRATCHES_TREE_NAME_3D;
  if (bNodeTree *existing = paint_material_group_find(bmain, marker)) {
    if (!STREQ(existing->id.name + 2, tree_name)) {
      BKE_id_rename(bmain, existing->id, tree_name);
    }
    return existing;
  }

  bNodeTree *group = bke::node_tree_add_tree(&bmain, tree_name, "ShaderNodeTree");
  if (group == nullptr) {
    return nullptr;
  }
  paint_material_group_marker_set(*group, marker);

  if (!paint_material_group_interface_add(*group, "Coordinate", Span<const GroupFloatInput>(scratches_inputs, std::size(scratches_inputs)), "Factor")) {
    BKE_id_free(&bmain, &group->id);
    return nullptr;
  }

  PaintMaterialGroupBuilder builder(*group);
  if (!builder.ok()) {
    BKE_id_free(&bmain, &group->id);
    return nullptr;
  }

  /* p = (x, y / stretch, z): the cells squash along Y, so a cell's edge reads as a long scratch
   * along X. The divide is guarded because a stored file's float is not a schema's clamped one. */
  GroupSocket xyz[3];
  if (!builder.separate_xyz({builder.group_input(), SCRATCHES_GROUP_ID_COORDINATE}, xyz)) {
    BKE_id_free(&bmain, &group->id);
    return nullptr;
  }
  bNode *safe_stretch = builder.binary_const(
      NODE_MATH_MAXIMUM, {builder.group_input(), SCRATCHES_GROUP_ID_STRETCH}, 1e-6f);
  if (safe_stretch == nullptr) {
    BKE_id_free(&bmain, &group->id);
    return nullptr;
  }
  const GroupSocket stretched = builder.divide(xyz[1], builder.value(safe_stretch));
  const GroupSocket p = builder.combine_xyz(xyz[0], stretched, xyz[2]);

  bNode *voronoi = builder.voronoi_edge(is_2d);
  if (voronoi == nullptr) {
    BKE_id_free(&bmain, &group->id);
    return nullptr;
  }
  if (!builder.link(p, {voronoi, "Vector"})) {
    BKE_id_free(&bmain, &group->id);
    return nullptr;
  }
  /* Every other number the pattern needs is an input of the group, so an edit of one is a value the
   * row's own sync writes in place. */
  struct PatternInput {
    const char *identifier;
    const char *socket;
  };
  const PatternInput inputs[5] = {
      {SCRATCHES_GROUP_ID_SCALE, "Scale"},
      {SCRATCHES_GROUP_ID_DETAIL, "Detail"},
      {SCRATCHES_GROUP_ID_ROUGHNESS, "Roughness"},
      {SCRATCHES_GROUP_ID_LACUNARITY, "Lacunarity"},
      {SCRATCHES_GROUP_ID_RANDOMNESS, "Randomness"},
  };
  for (const PatternInput &input : inputs) {
    if (!builder.link_input(input.identifier, voronoi, input.socket)) {
      BKE_id_free(&bmain, &group->id);
      return nullptr;
    }
  }

  /* The line: everything within half its width of the edge, faded out over the softening. */
  const GroupSocket distance{voronoi, "Distance"};
  bNode *edge = builder.math(NODE_MATH_ADD);
  if (edge == nullptr ||
      !builder.link({builder.group_input(), SCRATCHES_GROUP_ID_WIDTH}, {edge, "Value"}) ||
      !builder.link({builder.group_input(), SCRATCHES_GROUP_ID_SOFTEN}, {edge, "Value_001"}))
  {
    BKE_id_free(&bmain, &group->id);
    return nullptr;
  }
  const GroupSocket inside = builder.smooth_step(
      distance, {builder.group_input(), SCRATCHES_GROUP_ID_WIDTH}, builder.value(edge));
  const GroupSocket covered = builder.invert(inside);
  if (!builder.ok() || covered.node == nullptr) {
    BKE_id_free(&bmain, &group->id);
    return nullptr;
  }
  if (!builder.link_output(covered, SCRATCHES_GROUP_ID_FACTOR)) {
    BKE_id_free(&bmain, &group->id);
    return nullptr;
  }
  builder.place_output();

  BKE_ntree_update_tag_all(group);
  BKE_ntree_update_after_single_tree_change(bmain, *group);
  return group;
}

/* -------------------------------------------------------------------- */
/** \name Scratches factor, the CPU's copy of the group
 *
 * One statement per node of the group above, in the same order. \a placed is the coordinate the row
 * group's mapping chain made, which is what the group's Coordinate input is linked to; the caller
 * has already applied the placement and the seed. \{ */

float BKE_paint_material_scratches_factor(const float3 &placed,
                                          const PaintMaterialGeneratorParams &params,
                                          const bool is_2d)
{
  const float scale = params[PAINT_LAYER_GEN_SCALE];
  const float width = params[PAINT_LAYER_GEN_SCRATCHES_WIDTH];
  const float stretch = params[PAINT_LAYER_GEN_SCRATCHES_STRETCH];
  const float randomness = params[PAINT_LAYER_GEN_SCRATCHES_RANDOMNESS];

  const float safe_stretch = std::max(stretch, 1e-6f);
  const float3 stretched{placed.x, placed.y / safe_stretch, placed.z};

  noise::VoronoiParams voronoi;
  voronoi.feature = SHD_VORONOI_DISTANCE_TO_EDGE;
  voronoi.metric = SHD_VORONOI_EUCLIDEAN;
  voronoi.normalize = false;
  voronoi.scale = scale;
  voronoi.detail = params[PAINT_LAYER_GEN_SCRATCHES_DETAIL];
  voronoi.roughness = params[PAINT_LAYER_GEN_SCRATCHES_ROUGHNESS];
  voronoi.lacunarity = params[PAINT_LAYER_GEN_SCRATCHES_LACUNARITY];
  /* The node's own clamp, applied before the reach it sets: the group links the value straight
   * through, so the same clamp has to be here. */
  voronoi.randomness = std::min(std::max(randomness, 0.0f), 1.0f);
  voronoi.smoothness = 0.0f;
  voronoi.exponent = 0.0f;
  voronoi.max_distance = 0.5f + 0.5f * voronoi.randomness;

  const float distance = is_2d ?
                             noise::fractal_voronoi_distance_to_edge<float2>(
                                 voronoi, float2{stretched.x * scale, stretched.y * scale}) :
                             noise::fractal_voronoi_distance_to_edge<float3>(
                                 voronoi, stretched * scale);

  const float hi = width + params[PAINT_LAYER_GEN_SCRATCHES_SOFTEN];
  return 1.0f - paint_material_group_smooth_step(distance, width, hi);
}

/** \} */

/** \} */

}  // namespace blender
