/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The shared Grunge group: the shader-tree half of the Grunge generator. The CPU composite calls
 * #BKE_paint_material_grunge_factor, which builds the same arithmetic; every node and every constant
 * below has a counterpart there.
 *
 * A grunge is a patch of dirt rather than a texture: one noise says how dirty the place is and a
 * second, much finer one breaks that up, and the threshold is where the patch ends. Two noise nodes
 * and three Math nodes make it, which is why the pattern is a group and not a node -- the pair, and
 * the arithmetic between them, is the look.
 */

#include <iterator>

#include "BKE_paint_material_composite.hh"

#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_tree_update.hh"

#include "BLI_math_base.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_noise.hh"
#include "BLI_span.hh"
#include "BLI_string.h"

#include "BLT_translation.hh"

#include "DNA_node_types.h"

#include "paint_layers/paint_material_generator_group_ids.hh"
#include "paint_material_composite_internal.hh"
#include "paint_material_group_builder.hh"
#include "paint_layers/paint_layers_kind.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Grunge group
 * \{ */

/** Marker values of the Grunge groups; the two dimensions are two groups. */
static constexpr const char *GRUNGE_MARKER_2D = "GRUNGE_2D";
static constexpr const char *GRUNGE_MARKER_3D = "GRUNGE_3D";

/** Names of the shared group data-blocks. The leading dot keeps them out of every ID picker. */
static const char *GRUNGE_TREE_NAME_2D = ".PBR Grunge 2D";
static const char *GRUNGE_TREE_NAME_3D = ".PBR Grunge 3D";

bool BKE_paint_material_is_grunge_group(const bNode &node)
{
  return paint_material_group_has_marker(node, GRUNGE_MARKER_2D) ||
         paint_material_group_has_marker(node, GRUNGE_MARKER_3D);
}

namespace {

/**
 * The base noise's own inputs, then the two of the one that breaks it up, then the threshold's pair.
 * In the order they become the group's inputs, which is the order the identifiers of
 * #paint_material_composite_internal.hh say they are.
 */
static const GroupFloatInput grunge_inputs[] = {
    {"Scale", 2.0f, 0.01f, 1000.0f},
    {"Detail", 2.0f, 0.0f, 15.0f},
    {"Roughness", 0.5f, 0.0f, 1.0f},
    {"Lacunarity", 2.0f, 1.0f, 8.0f},
    {"Distortion", 0.0f, 0.0f, 4.0f},
    {"Variation Strength", 0.3f, 0.0f, 1.0f},
    /* On top of the generator's own scale, which is what makes the speckle fine at any zoom. */
    {"Variation Scale", 15.0f, 0.1f, 100.0f},
    {"Threshold", 0.0f, 0.0f, 1.0f},
    {"Soften", 0.05f, 0.0f, 0.5f},
};

/** The variation noise's own numbers: a single step of fine breaking-up, pinned so that the graph
 * and the CPU mirror state them once. */
constexpr float GRUNGE_VARIATION_DETAIL = 1.0f;
constexpr float GRUNGE_VARIATION_ROUGHNESS = 0.5f;
constexpr float GRUNGE_VARIATION_LACUNARITY = 2.0f;

}  // namespace

bNodeTree *BKE_paint_material_grunge_group_ensure(Main &bmain, const bool is_2d)
{
  const char *marker = is_2d ? GRUNGE_MARKER_2D : GRUNGE_MARKER_3D;
  const char *tree_name = is_2d ? GRUNGE_TREE_NAME_2D : GRUNGE_TREE_NAME_3D;
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

  if (!paint_material_group_interface_add(
          *group,
          "Coordinate",
          Span<const GroupFloatInput>(grunge_inputs, std::size(grunge_inputs)),
          "Factor"))
  {
    BKE_id_free(&bmain, &group->id);
    return nullptr;
  }

  PaintMaterialGroupBuilder builder(*group);
  if (!builder.ok()) {
    BKE_id_free(&bmain, &group->id);
    return nullptr;
  }
  const auto parameter = [&](const char *identifier) -> GroupSocket {
    return {builder.group_input(), identifier};
  };

  /* How dirty the place is, in the group's own words: a normalized FBM, so its answer is a position
   * between the two edges of the threshold rather than a value whose range the user would have to
   * know. The type and the normalize are the node's storage, which a socket cannot drive; that is
   * why the kind's schema holds neither, and why the two dimensions are two groups. */
  bNode *base = builder.noise(is_2d, SHD_NOISE_FBM, true);
  builder.link_input(GRUNGE_GROUP_ID_COORDINATE, base, "Vector");
  const struct
  {
    const char *identifier;
    const char *socket;
  } base_inputs[5] = {
      {GRUNGE_GROUP_ID_SCALE, "Scale"},
      {GRUNGE_GROUP_ID_DETAIL, "Detail"},
      {GRUNGE_GROUP_ID_ROUGHNESS, "Roughness"},
      {GRUNGE_GROUP_ID_LACUNARITY, "Lacunarity"},
      {GRUNGE_GROUP_ID_DISTORTION, "Distortion"},
  };
  for (const auto &input : base_inputs) {
    builder.link_input(input.identifier, base, input.socket);
  }
  if (!builder.ok()) {
    BKE_id_free(&bmain, &group->id);
    return nullptr;
  }

  /* The breaking-up: the same coordinate at a scale of its own, which is the generator's multiplied
   * by the variation's so that the speckle stays fine however far the dirt is zoomed. */
  bNode *variation_scale = builder.binary(
      NODE_MATH_MULTIPLY, parameter(GRUNGE_GROUP_ID_SCALE), parameter(GRUNGE_GROUP_ID_VARIATION_SCALE));
  bNode *variation = builder.noise(is_2d, SHD_NOISE_FBM, true);
  builder.link_input(GRUNGE_GROUP_ID_COORDINATE, variation, "Vector");
  builder.link(builder.value(variation_scale), {variation, "Scale"});
  builder.set_float(variation, "Detail", GRUNGE_VARIATION_DETAIL);
  builder.set_float(variation, "Roughness", GRUNGE_VARIATION_ROUGHNESS);
  builder.set_float(variation, "Lacunarity", GRUNGE_VARIATION_LACUNARITY);
  builder.set_float(variation, "Distortion", 0.0f);
  if (!builder.ok()) {
    BKE_id_free(&bmain, &group->id);
    return nullptr;
  }

  /* The modulation: the fine noise moves the coarse one about its middle, by as much as the strength
   * allows. A normalized noise's middle is one half, which is why the subtract reads that constant
   * rather than the noise's own answer. */
  bNode *centered = builder.binary_const(NODE_MATH_SUBTRACT, builder.fac(variation), 0.5f);
  const GroupSocket weighted = builder.multiply(
      builder.value(centered), parameter(GRUNGE_GROUP_ID_VARIATION_STRENGTH));
  const GroupSocket modulated = builder.add(builder.fac(base), weighted);

  /* The patch: everything under the threshold, faded out over the softening -- the same threshold a
   * scratch's line and a weave's thread are built from, read against the modulated noise. */
  const GroupSocket hi = builder.add(parameter(GRUNGE_GROUP_ID_THRESHOLD),
                                     parameter(GRUNGE_GROUP_ID_SOFTEN));
  const GroupSocket inside = builder.smooth_step(
      modulated, parameter(GRUNGE_GROUP_ID_THRESHOLD), hi);
  const GroupSocket covered = builder.invert(inside);
  if (!builder.ok() || covered.node == nullptr) {
    BKE_id_free(&bmain, &group->id);
    return nullptr;
  }
  if (!builder.link_output(covered, GRUNGE_GROUP_ID_FACTOR)) {
    BKE_id_free(&bmain, &group->id);
    return nullptr;
  }
  builder.place_output();

  BKE_ntree_update_tag_all(group);
  BKE_ntree_update_after_single_tree_change(bmain, *group);
  return group;
}

/* -------------------------------------------------------------------- */
/** \name Grunge factor, the CPU's copy of the group
 *
 * One statement per node of the group above, in the same order. \a placed is the coordinate the row's
 * mapping chain made, which is what the group's Coordinate input is linked to; the caller has already
 * applied the placement and the seed. \{ */

/** The normalized FBM of a node of the group, at \a position: the node's own body with its own
 * clamps, its offset and gain left at the defaults of sockets a type as fixed as this one does not
 * have. */
static float grunge_fbm(const float3 &position,
                        const float detail,
                        const float roughness,
                        const float lacunarity,
                        const float distortion,
                        const bool is_2d)
{
  if (is_2d) {
    return noise::perlin_fractal_distorted(float2(position.x, position.y),
                                           math::clamp(detail, 0.0f, 15.0f),
                                           math::max(roughness, 0.0f),
                                           lacunarity,
                                           0.0f,
                                           1.0f,
                                           distortion,
                                           SHD_NOISE_FBM,
                                           true);
  }
  return noise::perlin_fractal_distorted(position,
                                         math::clamp(detail, 0.0f, 15.0f),
                                         math::max(roughness, 0.0f),
                                         lacunarity,
                                         0.0f,
                                         1.0f,
                                         distortion,
                                         SHD_NOISE_FBM,
                                         true);
}

float BKE_paint_material_grunge_factor(const float3 &placed,
                                       const PaintMaterialGeneratorParams &params,
                                       const bool is_2d)
{
  const float scale = params[PAINT_LAYER_GEN_SCALE];
  const float base = grunge_fbm(placed * scale,
                                params[PAINT_LAYER_GEN_GRUNGE_DETAIL],
                                params[PAINT_LAYER_GEN_GRUNGE_ROUGHNESS],
                                params[PAINT_LAYER_GEN_GRUNGE_LACUNARITY],
                                params[PAINT_LAYER_GEN_GRUNGE_DISTORTION],
                                is_2d);

  const float variation_scale = scale * params[PAINT_LAYER_GEN_GRUNGE_VARIATION_SCALE];
  const float variation = grunge_fbm(placed * variation_scale,
                                     GRUNGE_VARIATION_DETAIL,
                                     GRUNGE_VARIATION_ROUGHNESS,
                                     GRUNGE_VARIATION_LACUNARITY,
                                     0.0f,
                                     is_2d);

  const float shifted = (variation - 0.5f) * params[PAINT_LAYER_GEN_GRUNGE_VARIATION_STRENGTH];
  const float modulated = base + shifted;

  const float threshold = params[PAINT_LAYER_GEN_GRUNGE_THRESHOLD];
  return 1.0f - paint_material_group_smooth_step(
                   modulated, threshold, threshold + params[PAINT_LAYER_GEN_GRUNGE_SOFTEN]);
}

/** \} */

/** \} */

}  // namespace blender
