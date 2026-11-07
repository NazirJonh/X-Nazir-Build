/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The shared Weave group: the shader-tree half of the Weave generator. The CPU composite calls
 * #BKE_paint_material_weave_factor, which builds the same arithmetic; every node and every constant
 * below has a counterpart there.
 *
 * A weave is periodic, so its group is arithmetic rather than a texture node: the cell a coordinate
 * falls in says which of the two thread directions lies on top, and the cell's own position says how
 * far inside the thread it is. Only the jitter is a noise, and it shifts the coordinate before the
 * cells are counted, so the threads leave their grid without the grid itself changing.
 */

#include <cmath>
#include <iterator>

#include "BKE_paint_material_composite.hh"

#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_tree_update.hh"

#include "BLI_index_range.hh"
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
/** \name Weave group
 * \{ */

/** Marker values of the Weave groups; the two dimensions are two groups. */
static constexpr const char *WEAVE_MARKER_2D = "WEAVE_2D";
static constexpr const char *WEAVE_MARKER_3D = "WEAVE_3D";

/** Names of the shared group data-blocks. The leading dot keeps them out of every ID picker. */
static const char *WEAVE_TREE_NAME_2D = ".PBR Weave 2D";
static const char *WEAVE_TREE_NAME_3D = ".PBR Weave 3D";

bool BKE_paint_material_is_weave_group(const bNode &node)
{
  return paint_material_group_has_marker(node, WEAVE_MARKER_2D) ||
         paint_material_group_has_marker(node, WEAVE_MARKER_3D);
}

namespace {

/**
 * The group's float inputs, in the order they become the interface's sockets: the grid's own
 * frequency, the choice of plain or twill, the thread's width, the gap that shrinks it, the softening
 * of its edges and how far a noise may push it off the grid.
 */
static const GroupFloatInput weave_inputs[] = {
    {"Scale", 8.0f, 0.01f, 1000.0f},
    {"Pattern", 0.0f, 0.0f, 1.0f},
    {"Thread Width", 0.7f, 0.05f, 1.0f},
    {"Gap", 0.05f, 0.0f, 0.5f},
    {"Soften", 0.05f, 0.0f, 0.5f},
    {"Jitter", 0.0f, 0.0f, 1.0f},
};

}  // namespace

bNodeTree *BKE_paint_material_weave_group_ensure(Main &bmain, const bool is_2d)
{
  const char *marker = is_2d ? WEAVE_MARKER_2D : WEAVE_MARKER_3D;
  const char *tree_name = is_2d ? WEAVE_TREE_NAME_2D : WEAVE_TREE_NAME_3D;
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
          Span<const GroupFloatInput>(weave_inputs, std::size(weave_inputs)),
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

  /* p = Coordinate * Scale: the thread grid's own units, which is also what the jitter's noise
   * reads, so a cell is one thread wide whatever the scale. */
  bNode *scaled = builder.vector_math(NODE_VECTOR_MATH_MULTIPLY);
  builder.link_input(WEAVE_GROUP_ID_COORDINATE, scaled, "Vector");
  builder.link_input(WEAVE_GROUP_ID_SCALE, scaled, "Vector_001");
  const GroupSocket p = builder.vector(scaled);

  /* One noise, three channels: the jitter of the two axes and, in a 3D group, of the third. Its own
   * scale is one because the coordinate already carries the grid's; the rest of its numbers are
   * pinned here rather than left to the node's defaults, so the constants the CPU mirror passes and
   * the ones the graph reads are written down once, in the same words. */
  bNode *jitter_noise = builder.noise(is_2d, SHD_NOISE_FBM, true);
  builder.link(p, {jitter_noise, "Vector"});
  builder.set_float(jitter_noise, "Scale", 1.0f);
  builder.set_float(jitter_noise, "Detail", 2.0f);
  builder.set_float(jitter_noise, "Roughness", 0.5f);
  builder.set_float(jitter_noise, "Lacunarity", 2.0f);
  builder.set_float(jitter_noise, "Distortion", 0.0f);
  GroupSocket rgb[3];
  if (!builder.separate_color({jitter_noise, "Color"}, rgb)) {
    BKE_id_free(&bmain, &group->id);
    return nullptr;
  }
  GroupSocket shifted[3];
  for (const int i : IndexRange(3)) {
    bNode *centered = builder.binary_const(NODE_MATH_SUBTRACT, rgb[i], 0.5f);
    shifted[i] = builder.multiply(builder.value(centered), parameter(WEAVE_GROUP_ID_JITTER));
  }
  const GroupSocket offset = builder.combine_xyz(shifted[0], shifted[1], shifted[2]);
  bNode *moved = builder.vector_math(NODE_VECTOR_MATH_ADD);
  builder.link(p, {moved, "Vector"});
  builder.link(offset, {moved, "Vector_001"});
  if (!builder.ok()) {
    BKE_id_free(&bmain, &group->id);
    return nullptr;
  }

  /* The cell the moved coordinate sits in, and its position within: the two numbers a thread needs,
   * the index to know which way it runs and the axis to know how far along it the point is. */
  GroupSocket q[3];
  if (!builder.separate_xyz(builder.vector(moved), q)) {
    BKE_id_free(&bmain, &group->id);
    return nullptr;
  }
  const GroupSocket ix = builder.floor(q[0]);
  const GroupSocket iy = builder.floor(q[1]);
  const GroupSocket fx = builder.subtract(q[0], ix);
  const GroupSocket fy = builder.subtract(q[1], iy);

  /* Which thread lies on top. Plain weave turns that over in every cell; twill keeps it the same for
   * two cells before it turns, which is the diagonal of the name. The step is a float the mix below
   * reads, so the two spellings only have to agree on the parity of a cell. */
  bNode *cell_sum = builder.math(NODE_MATH_ADD);
  builder.link(ix, {cell_sum, "Value"});
  builder.link(iy, {cell_sum, "Value_001"});
  const GroupSocket sum = builder.value(cell_sum);
  const GroupSocket plain = builder.floored_modulo(sum, 2.0f);
  bNode *half_pair = builder.binary_const(NODE_MATH_DIVIDE, sum, 2.0f);
  const GroupSocket pair = builder.floor(builder.value(half_pair));
  const GroupSocket twill = builder.floored_modulo(pair, 2.0f);
  bNode *is_twill = builder.compare(parameter(WEAVE_GROUP_ID_PATTERN), 1.0f);
  const GroupSocket on_top = builder.result(
      builder.mix(plain, twill, builder.value(is_twill)));

  /* A thread: the part of its cell it fills, risen and fallen over the softening. The gap shrinks
   * it from both sides, so it starts halfway between the cell's edge and the gap's end. */
  const auto band = [&](const GroupSocket t) -> GroupSocket {
    const GroupSocket width = parameter(WEAVE_GROUP_ID_THREAD_WIDTH);
    const GroupSocket gap = parameter(WEAVE_GROUP_ID_GAP);
    const GroupSocket soften = parameter(WEAVE_GROUP_ID_SOFTEN);
    bNode *from_edge = builder.const_binary(NODE_MATH_SUBTRACT, 1.0f, width);
    const GroupSocket with_gap = builder.add(builder.value(from_edge), gap);
    const GroupSocket start = builder.value(
        builder.binary_const(NODE_MATH_MULTIPLY, with_gap, 0.5f));
    const GroupSocket rise = builder.smooth_step(t, start, builder.add(start, soften));
    const GroupSocket end = builder.subtract(builder.add(start, width), gap);
    const GroupSocket fall = builder.smooth_step(t, builder.subtract(end, soften), end);
    return builder.multiply(rise, builder.invert(fall));
  };
  const GroupSocket warp = band(fx);
  const GroupSocket weft = band(fy);

  /* Which of the two the point is inside: the thread that lies on top of this cell, read along its
   * own axis. A cell of plain weave whose sum is even runs along X, so the mix takes the warp's band
   * at zero and the weft's at one. */
  const GroupSocket factor = builder.result(builder.mix(warp, weft, on_top));
  if (!builder.ok() || factor.node == nullptr) {
    BKE_id_free(&bmain, &group->id);
    return nullptr;
  }
  if (!builder.link_output(factor, WEAVE_GROUP_ID_FACTOR)) {
    BKE_id_free(&bmain, &group->id);
    return nullptr;
  }
  builder.place_output();

  BKE_ntree_update_tag_all(group);
  BKE_ntree_update_after_single_tree_change(bmain, *group);
  return group;
}

/* -------------------------------------------------------------------- */
/** \name Weave factor, the CPU's copy of the group
 *
 * One statement per node of the group above, in the same order. \a placed is the coordinate the row's
 * mapping chain made, which is what the group's Coordinate input is linked to. \{ */

/** The jitter noise of the group, at \a p: the three channels of the node's Color output, which are
 * three noises of one coordinate read at three seeds. The node's own scale is one, so the position
 * carries the grid's; a UV group's noise is the two-dimensional one, whose third channel still
 * shifts the third axis the cells of a 2D weave never look at. */
static float3 weave_jitter(const float3 &p, const bool is_2d)
{
  /* The FBM body reads neither offset nor gain; the two are the sockets' defaults, kept here so the
   * call is the node's argument list. */
  if (is_2d) {
    return noise::perlin_float3_fractal_distorted(
        float2(p.x, p.y), 2.0f, 0.5f, 2.0f, 0.0f, 1.0f, 0.0f, SHD_NOISE_FBM, true);
  }
  return noise::perlin_float3_fractal_distorted(
      p, 2.0f, 0.5f, 2.0f, 0.0f, 1.0f, 0.0f, SHD_NOISE_FBM, true);
}

/** The thread \a t of its cell stands for: risen at \a start, fallen at \a end, each edge faded over
 * the softening. The two thresholds are the group's, in the group's order. */
static float weave_band(const float t,
                        const float thread_width,
                        const float gap,
                        const float soften)
{
  const float from_edge = 1.0f - thread_width;
  const float with_gap = from_edge + gap;
  const float start = with_gap * 0.5f;
  const float rise = paint_material_group_smooth_step(t, start, start + soften);
  const float end = (start + thread_width) - gap;
  const float fall = paint_material_group_smooth_step(t, end - soften, end);
  return rise * (1.0f - fall);
}

/** `x - floor(x / period) * period`, the Floored Modulo node's own body, which keeps a negative cell
 * index on the same side of the step as the positive one across it. */
static float weave_floored_modulo(const float x, const float period)
{
  if (period == 0.0f) {
    return 0.0f;
  }
  return x - std::floor(x / period) * period;
}

float BKE_paint_material_weave_factor(const float3 &placed,
                                      const PaintMaterialGeneratorParams &params,
                                      const bool is_2d)
{
  const float3 p = placed * params[PAINT_LAYER_GEN_SCALE];

  const float3 jitter = weave_jitter(p, is_2d);
  const float jitter_amount = params[PAINT_LAYER_GEN_WEAVE_JITTER];
  const float3 q{p.x + (jitter.x - 0.5f) * jitter_amount,
                 p.y + (jitter.y - 0.5f) * jitter_amount,
                 p.z + (jitter.z - 0.5f) * jitter_amount};

  const float ix = std::floor(q.x);
  const float iy = std::floor(q.y);
  const float sum = ix + iy;
  const float plain = weave_floored_modulo(sum, 2.0f);
  const float twill = weave_floored_modulo(std::floor(sum / 2.0f), 2.0f);
  /* The Compare node: one when the pattern parameter is within half of twill's code, which is the
   * only value that answers it -- the schema's enum holds the two codes. */
  const float is_twill = (std::abs(params[PAINT_LAYER_GEN_WEAVE_PATTERN] -
                                   float(PAINT_LAYER_GEN_WEAVE_TWILL)) <=
                          0.5f) ?
                             1.0f :
                             0.0f;
  const float on_top = math::interpolate(plain, twill, is_twill);

  const float thread_width = params[PAINT_LAYER_GEN_WEAVE_THREAD_WIDTH];
  const float gap = params[PAINT_LAYER_GEN_WEAVE_GAP];
  const float soften = params[PAINT_LAYER_GEN_WEAVE_SOFTEN];
  const float warp = weave_band(q.x - ix, thread_width, gap, soften);
  const float weft = weave_band(q.y - iy, thread_width, gap, soften);
  return math::interpolate(warp, weft, on_top);
}

/** \} */

/** \} */

}  // namespace blender
