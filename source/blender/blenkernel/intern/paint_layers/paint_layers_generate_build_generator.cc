/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The GPU emission of the Paint Layers generators: what a generator row lays as the source of a
 * channel, built inside the row's own group. The dispatch goes through the generator table
 * (#BKE_paint_layers_generator_of), so an emission here is one of the two functions a generator
 * registers; the CPU factor, its counterpart, lives beside the composite. The Gradient builds its
 * factor group node by node; the single-node patterns build the placement chain and one texture
 * node, and the shape the two answer is the same ramp tail.
 */

#include "BKE_paint_layers_generate.hh"
#include "BKE_paint_layers_generator.hh"

#include "BLI_math_vector.h"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_ustring.hh"
#include "BLI_uuid.h"

#include "BKE_lib_id.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_interface.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_generator_registry.hh"
#include "BKE_paint_material_composite.hh"

#include "NOD_socket.hh"

#include "DNA_material_types.h"
#include "DNA_node_tree_interface_types.h"
#include "DNA_node_types.h"

#include "paint_layers_generate_build_intern.hh"
#include "paint_layers_generate_intern.hh"
#include "paint_layers_generate_layout.hh"
#include "paint_layers_generator_registry_intern.hh"
#include "paint_layers_intern.hh"
#include "paint_material_generator_group_ids.hh"

namespace blender {
namespace bke::paint_layers {

/* -------------------------------------------------------------------- */
/** \name Generator dispatch
 *
 * The one place the chain asks which generator a row is. The table answers, so an emission below
 * never branches on a subtype and a new generator never adds one here: a parametric generator adds
 * its per-subtype emission under the Gradient one below, and no other build translation unit grows
 * a branch.
 * \{ */

KindEmitResult PaintLayersTreeBuilder::generator_source_emit(const RowEmitContext &ctx)
{
  const PaintLayerGeneratorInfo *generator = BKE_paint_layers_generator_of(ctx.layer);
  if (generator == nullptr) {
    return {};
  }
  /* The gates the emission need not answer for: a generator flagged out of a channel is skipped
   * before its nodes are asked for, exactly as the CPU description skips it. */
  if ((generator->flags & PAINT_LAYER_GENERATOR_NO_NORMAL) != 0 &&
      ctx.channel == PAINT_MATERIAL_CHANNEL_NORMAL)
  {
    return {};
  }
  if (generator->gpu_emit == nullptr) {
    /* The table knows the kind before its emission lands: a generator registered with the CPU
     * factor alone lays nothing in the graph yet. */
    return {};
  }
  return generator->gpu_emit(*this, ctx);
}

KindEmitResult paint_layer_generator_gradient_gpu_emit(PaintLayersTreeBuilder &builder,
                                                       const RowEmitContext &ctx)
{
  return builder.gradient_source_emit(ctx);
}

/* The five single-node patterns: each entry of the table names its own emission, and the five
 * share the shape of one -- the pattern's factor over the ramp the channel reads -- with only the
 * answer the pattern's own node gives differing. The Voronoi branches once more, for the cell
 * colour it lays instead of an index into the band. */

/** The emission of a pattern whose node answers with one float socket: the Noise, the Checker and
 * the Wave, whose `Fac` is the grey the channel's ramp is indexed with. */
static KindEmitResult generator_fac_pattern_gpu_emit(PaintLayersTreeBuilder &builder,
                                                     const RowEmitContext &ctx)
{
  const GeneratorFactor pattern = builder.generator_pattern_factor_ensure(
      ctx.tree, ctx.group, ctx.layer, "Fac");
  if (pattern.factor.socket == nullptr) {
    return {};
  }
  return builder.generator_ramp_emit(ctx, pattern);
}

KindEmitResult paint_layer_generator_noise_gpu_emit(PaintLayersTreeBuilder &builder,
                                                    const RowEmitContext &ctx)
{
  return generator_fac_pattern_gpu_emit(builder, ctx);
}

KindEmitResult paint_layer_generator_checker_gpu_emit(PaintLayersTreeBuilder &builder,
                                                      const RowEmitContext &ctx)
{
  return generator_fac_pattern_gpu_emit(builder, ctx);
}

KindEmitResult paint_layer_generator_wave_gpu_emit(PaintLayersTreeBuilder &builder,
                                                   const RowEmitContext &ctx)
{
  return generator_fac_pattern_gpu_emit(builder, ctx);
}

KindEmitResult paint_layer_generator_brick_gpu_emit(PaintLayersTreeBuilder &builder,
                                                    const RowEmitContext &ctx)
{
  const GeneratorFactor pattern = builder.generator_brick_factor_ensure(ctx.tree,
                                                                        ctx.group,
                                                                        ctx.layer);
  if (pattern.factor.socket == nullptr) {
    return {};
  }
  return builder.generator_ramp_emit(ctx, pattern);
}

KindEmitResult paint_layer_generator_voronoi_gpu_emit(PaintLayersTreeBuilder &builder,
                                                      const RowEmitContext &ctx)
{
  const GeneratorFactor pattern = builder.generator_voronoi_factor_ensure(ctx.tree,
                                                                         ctx.group,
                                                                         ctx.layer);
  if (pattern.factor.socket == nullptr) {
    return {};
  }
  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
      eMaterialPaintChannel(ctx.channel));
  if (info.is_color && pattern.color.socket != nullptr &&
      paint_layer_generator_has_own_color(ctx.layer))
  {
    return builder.generator_own_color_emit(ctx, pattern);
  }
  return builder.generator_ramp_emit(ctx, pattern);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Gradient emission
 *
 * The shared Gradient Factor group instance and the ColorRamp nodes the row's channels read. The
 * factor is shared by every channel of the row, the colour ramp by every colour channel and a
 * scalar channel keeps a value ramp of its own, so the nodes live in the builder's caches.
 * \{ */

NodeOutput PaintLayersTreeBuilder::gradient_factor_ensure(bNodeTree &tree,
                                                          LayerGroup &group,
                                                          const MaterialPaintLayer &layer)
{
  if (const NodeOutput *cached = gradient_factors_.lookup_ptr(&layer)) {
    return *cached;
  }
  BLI_assert(layer.gradient != nullptr);
  if (ctx_.gradient_group == nullptr || tree.typeinfo == nullptr ||
      tree.typeinfo->group_idname == nullptr)
  {
    return {};
  }
  bNode *instance = bke::node_add_node(nullptr, tree, tree.typeinfo->group_idname);
  if (instance == nullptr) {
    return {};
  }
  instance->id = &ctx_.gradient_group->id;
  id_us_plus(&ctx_.gradient_group->id);
  STRNCPY_UTF8(instance->label, "Gradient Factor");
  nodes::update_node_declaration_and_sockets(tree, *instance);
  grid_.place(*instance, layout::Column::Source, 0);

  const auto discard = [&]() -> NodeOutput {
    bke::node_remove_node(nullptr, tree, *instance, true, false);
    return {};
  };

  /* The group's Coordinate input reads the Texture Coordinate output the row's space names: UV
   * rows pass (u, v, 0), Object rows object-space coordinates, Generated rows normalized ones. The
   * caller has already checked the payload (#gradient_source_emit). */
  bNode *coord = this->uv_coordinate_ensure(tree);
  bNodeSocket *coordinate = (coord != nullptr) ?
                                this->gradient_coordinate_socket(*coord, *layer.gradient) :
                                nullptr;
  bNodeSocket *coordinate_in = bke::node_find_socket(
      *instance, SOCK_IN, UString::from_ptr_noinline(GRADIENT_GROUP_ID_COORDINATE));
  if (coord == nullptr || coordinate == nullptr || coordinate_in == nullptr) {
    return discard();
  }
  bke::node_add_link(tree, *coord, *coordinate, *instance, *coordinate_in);

  struct GradientInput {
    Map<const MaterialPaintLayer *, bNodeTreeInterfaceSocket *> *inputs;
    const char *identifier;
  };
  const GradientInput inputs[5] = {
      {&gradient_start_inputs_, GRADIENT_GROUP_ID_START},
      {&gradient_end_inputs_, GRADIENT_GROUP_ID_END},
      {&gradient_midpoint_inputs_, GRADIENT_GROUP_ID_MIDPOINT},
      {&gradient_type_inputs_, GRADIENT_GROUP_ID_TYPE},
      {&gradient_repeat_inputs_, GRADIENT_GROUP_ID_REPEAT},
  };
  for (const GradientInput &input : inputs) {
    bNodeTreeInterfaceSocket *const *iface = input.inputs->lookup_ptr(&layer);
    bNodeSocket *source = (iface != nullptr) ?
                              this->group_input_socket(group, **iface) :
                              nullptr;
    bNodeSocket *target = bke::node_find_socket(
        *instance, SOCK_IN, UString::from_ptr_noinline(input.identifier));
    if (source == nullptr || target == nullptr) {
      return discard();
    }
    bke::node_add_link(tree, *group.group_input, *source, *instance, *target);
  }

  bNodeSocket *factor = bke::node_find_socket(
      *instance, SOCK_OUT, UString::from_ptr_noinline(GRADIENT_GROUP_ID_FACTOR));
  if (factor == nullptr) {
    return discard();
  }
  const NodeOutput out{instance, factor};
  gradient_factors_.add(&layer, out);
  return out;
}

/** A ColorRamp node holding a copy of \a band, or null when the node cannot be made. */
static bNode *generator_ramp_node_add(bNodeTree &tree, const ColorBand &band)
{
  bNode *ramp = bke::node_add_static_node(nullptr, tree, SH_NODE_VALTORGB);
  if (ramp == nullptr || ramp->storage == nullptr) {
    return nullptr;
  }
  *static_cast<ColorBand *>(ramp->storage) = band;
  return ramp;
}

bNode *PaintLayersTreeBuilder::generator_ramp_ensure(bNodeTree &tree,
                                                     const MaterialPaintLayer &layer,
                                                     const int channel)
{
  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
      eMaterialPaintChannel(channel));
  /* Every colour channel shares the row's ramp node; a scalar channel has one of its own. */
  if (info.is_color) {
    if (bNode **cached = generator_ramps_.lookup_ptr(&layer)) {
      return *cached;
    }
    /* A Gradient carries its band in its payload; the others in the row's ramp, which the kind's
     * Ramp parameter ensured. The kind decides, not the payload a row may keep from a Gradient it
     * was switched away from -- the same rule the CPU's band lookup goes by. Without either, the
     * row lays no colour and the CPU lays a grey of the factor, so the row is left out of the
     * graph rather than disagreeing. */
    const ColorBand *band = (paint_layer_generator_kind_of(layer) ==
                             MA_PAINT_LAYER_GENERATOR_GRADIENT) ?
                                ((layer.gradient != nullptr) ? &layer.gradient->ramp : nullptr) :
                                BKE_paint_layers_ramp_get(layer);
    if (band == nullptr) {
      return nullptr;
    }
    bNode *ramp = generator_ramp_node_add(tree, *band);
    if (ramp == nullptr) {
      return nullptr;
    }
    ramp_node_key_set(*ramp, layer, -1);
    grid_.place(*ramp, layout::Column::Source, 1);
    generator_ramps_.add(&layer, ramp);
    return ramp;
  }

  if (Map<int, bNode *> *by_channel = generator_scalar_ramps_.lookup_ptr(&layer)) {
    if (bNode **cached = by_channel->lookup_ptr(channel)) {
      return *cached;
    }
  }
  const MaterialPaintLayerChannel *record = paint_layer_channel_find(layer, channel);
  if (record == nullptr || record->gradient_ramp == nullptr) {
    return nullptr;
  }
  bNode *ramp = generator_ramp_node_add(tree, *record->gradient_ramp);
  if (ramp == nullptr) {
    return nullptr;
  }
  ramp_node_key_set(*ramp, layer, channel);
  grid_.place_extra(*ramp);
  generator_scalar_ramps_.lookup_or_add_default(&layer).add(channel, ramp);
  return ramp;
}

KindEmitResult PaintLayersTreeBuilder::gradient_source_emit(const RowEmitContext &ctx)
{
  bNodeTree &tree = ctx.tree;
  LayerGroup &group = ctx.group;
  const MaterialPaintLayerGradient *gradient = ctx.layer.gradient;
  if (gradient == nullptr) {
    return {};
  }
  if (ctx.channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
    return {};
  }
  const NodeOutput factor = this->gradient_factor_ensure(tree, group, ctx.layer);
  if (factor.socket == nullptr) {
    return {};
  }
  return this->generator_ramp_emit(ctx, {factor, {}});
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Generator coordinates
 *
 * The Texture Coordinate output a generator's space names. The Gradient's factor group reads it
 * through the payload's own wrapper, the patterns through the space their schema stores, so the
 * one switch answers both.
 * \{ */

bNodeSocket *PaintLayersTreeBuilder::generator_coordinate_socket(bNode &coord, const int space)
{
  switch (eMaterialPaintLayerGradientSpace(space)) {
    case MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT:
      return socket_out(coord, "Object");
    case MA_PAINT_LAYER_GRADIENT_SPACE_GENERATED:
      return socket_out(coord, "Generated");
    case MA_PAINT_LAYER_GRADIENT_SPACE_UV:
      break;
  }
  /* An unknown space is the safe UV default, not a discarded row. */
  return socket_out(coord, "UV");
}

bNodeSocket *PaintLayersTreeBuilder::gradient_coordinate_socket(
    bNode &coord, const MaterialPaintLayerGradient &gradient)
{
  /* The Gradient keeps the payload's space against the shared switch, so its factor group reads
   * the coordinate its own wording names; the graph it builds is unchanged. */
  return this->generator_coordinate_socket(coord, int(gradient.space));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Pattern emission
 *
 * The single-node generators: a Noise, a Voronoi, a Checker, a Brick or a Wave reads its placed
 * coordinate through one texture node, whose parameters the row group's value inputs feed. The
 * build and the ramp tail are shared; what a generator registers is its emission, the one function
 * that answers its node's own shape of a factor, and the storage the row's schema decides.
 * \{ */

/**
 * The storage of the pattern's node, from the row's schema: the choices that are the node's own
 * switches rather than socket values, written between the node's creation and its declaration
 * update. The registry entries name these, one per single-node kind that has storage to write.
 */

void paint_layer_generator_noise_storage_init(bNode &pattern,
                                              const MaterialPaintLayer &layer,
                                              const bool is_2d)
{
  NodeTexNoise &tex = *static_cast<NodeTexNoise *>(pattern.storage);
  tex.dimensions = is_2d ? 2 : 3;
  tex.type = uint8_t(int(paint_layer_kind_param_get(layer, PAINT_LAYER_GEN_NOISE_TYPE)));
  tex.normalize = uint8_t(paint_layer_kind_param_get(layer, PAINT_LAYER_GEN_NOISE_NORMALIZE) !=
                          0.0f);
}

void paint_layer_generator_voronoi_storage_init(bNode &pattern,
                                                const MaterialPaintLayer &layer,
                                                const bool is_2d)
{
  NodeTexVoronoi &tex = *static_cast<NodeTexVoronoi *>(pattern.storage);
  tex.dimensions = is_2d ? 2 : 3;
  tex.feature = int(paint_layer_kind_param_get(layer, PAINT_LAYER_GEN_VORONOI_FEATURE));
  tex.distance = int(paint_layer_kind_param_get(layer, PAINT_LAYER_GEN_VORONOI_DISTANCE));
}

void paint_layer_generator_brick_storage_init(bNode &pattern,
                                              const MaterialPaintLayer &layer,
                                              const bool /*is_2d*/)
{
  NodeTexBrick &tex = *static_cast<NodeTexBrick *>(pattern.storage);
  /* The two offsets are the node's own switches, not sockets: they choose the shape of the
   * answer rather than weigh it. */
  tex.offset = paint_layer_kind_param_get(layer, PAINT_LAYER_GEN_BRICK_OFFSET);
  tex.offset_freq = int(paint_layer_kind_param_get(layer, PAINT_LAYER_GEN_BRICK_OFFSET_FREQUENCY));
  tex.squash = paint_layer_kind_param_get(layer, PAINT_LAYER_GEN_BRICK_SQUASH);
  tex.squash_freq = int(paint_layer_kind_param_get(layer, PAINT_LAYER_GEN_BRICK_SQUASH_FREQUENCY));
}

void paint_layer_generator_wave_storage_init(bNode &pattern,
                                             const MaterialPaintLayer &layer,
                                             const bool /*is_2d*/)
{
  NodeTexWave &tex = *static_cast<NodeTexWave *>(pattern.storage);
  tex.wave_type = int(paint_layer_kind_param_get(layer, PAINT_LAYER_GEN_WAVE_TYPE));
  tex.bands_direction = int(
      paint_layer_kind_param_get(layer, PAINT_LAYER_GEN_WAVE_BANDS_DIRECTION));
  tex.rings_direction = int(
      paint_layer_kind_param_get(layer, PAINT_LAYER_GEN_WAVE_RINGS_DIRECTION));
  tex.wave_profile = int(paint_layer_kind_param_get(layer, PAINT_LAYER_GEN_WAVE_PROFILE));
}

/**
 * The pattern's texture node of \a info's kind, its storage already holding the choices the row's
 * schema makes and its sockets re-derived from the storage, so a socket the signature leaves out
 * for the feature chosen now is absent rather than dead. \a r_vector receives the Vector input the
 * placed coordinate links into. Null when the node cannot be made.
 */
static bNode *generator_pattern_node_add(bNodeTree &tree,
                                         const MaterialPaintLayer &layer,
                                         const PaintLayerGeneratorInfo &info,
                                         const int space,
                                         bNodeSocket **r_vector)
{
  bNode *pattern = bke::node_add_static_node(nullptr, tree, info.shader_node_type);
  if (pattern == nullptr || pattern->storage == nullptr) {
    return nullptr;
  }
  if (info.node_storage_init != nullptr) {
    info.node_storage_init(*pattern, layer, space == MA_PAINT_LAYER_GRADIENT_SPACE_UV);
  }
  nodes::update_node_declaration_and_sockets(tree, *pattern);
  *r_vector = socket_in(*pattern, "Vector");
  return *r_vector != nullptr ? pattern : nullptr;
}

/**
 * The grey the pattern's colour answer carries, exactly the value the node writes into every
 * channel of it: the red of the colour, which is what the CPU composite indexes its ramp with. A
 * Separate Color node rather than a RGB to BW one, because the sum of that node's own coefficients
 * is not one in a float and would drift from the texel by an ulp.
 */
static NodeOutput generator_grey_of_color(bNodeTree &tree,
                                          layout::LayoutGrid &grid,
                                          Vector<bNode *> &r_nodes,
                                          const NodeOutput color)
{
  bNode *separate = bke::node_add_static_node(nullptr, tree, SH_NODE_SEPARATE_COLOR);
  bNodeSocket *in = (separate != nullptr) ? socket_in(*separate, "Color") : nullptr;
  bNodeSocket *red = (separate != nullptr) ? socket_out(*separate, "Red") : nullptr;
  if (separate == nullptr || in == nullptr || red == nullptr) {
    if (separate != nullptr) {
      bke::node_remove_node(nullptr, tree, *separate, true, false);
    }
    return {};
  }
  grid.place_extra(*separate);
  r_nodes.append(separate);
  bke::node_add_link(tree, *color.node, *color.socket, *separate, *in);
  return {separate, red};
}

/** A constant one -- the coverage a pattern that lays a colour of its own always has. */
static bNode *constant_one_node_add(bNodeTree &tree,
                                    layout::LayoutGrid &grid,
                                    bNodeSocket **r_value)
{
  *r_value = nullptr;
  bNode *one = bke::node_add_static_node(nullptr, tree, SH_NODE_VALUE);
  bNodeSocket *out = (one != nullptr) ? socket_out(*one, "Value") : nullptr;
  if (out == nullptr) {
    if (one != nullptr) {
      bke::node_remove_node(nullptr, tree, *one, true, false);
    }
    return nullptr;
  }
  if (out->default_value != nullptr) {
    static_cast<bNodeSocketValueFloat *>(out->default_value)->value = 1.0f;
  }
  grid.place_extra(*one);
  *r_value = out;
  return one;
}

bNode *PaintLayersTreeBuilder::generator_placed_coordinate_ensure(
    bNodeTree &tree,
    LayerGroup &group,
    const MaterialPaintLayer &layer,
    const int space,
    Vector<bNode *> &r_nodes_made)
{
  const auto placement_socket = [&](const Map<const MaterialPaintLayer *,
                                              bNodeTreeInterfaceSocket *> &inputs) -> bNodeSocket *
  {
    const bNodeTreeInterfaceSocket *const *iface = inputs.lookup_ptr(&layer);
    return (iface != nullptr && *iface != nullptr) ? this->group_input_socket(group, **iface) :
                                                     nullptr;
  };
  /* The coordinate the pattern reads: the space's own output, turned, sheared and moved by the
   * placement, then shifted by the seed. The mapping chain is the row mapping's own shape, so the
   * turn is a Vector Rotate in front of a Point Mapping and the two add in that order -- the order
   * #paint_layer_generator_coordinate applies them. The seed rides after, because it is a third
   * term of the same sum and the CPU adds it last. */
  bNode *coord = this->uv_coordinate_ensure(tree);
  bNodeSocket *coordinate = (coord != nullptr) ? this->generator_coordinate_socket(*coord, space) :
                                                 nullptr;
  bNodeSocket *offset = placement_socket(generator_offset_inputs_);
  bNodeSocket *scale = placement_socket(generator_scale_inputs_);
  bNodeSocket *rotation = placement_socket(generator_rotation_inputs_);
  if (coord == nullptr || coordinate == nullptr || offset == nullptr || scale == nullptr ||
      rotation == nullptr)
  {
    return nullptr;
  }
  const MappingChain chain = mapping_chain_add(
      tree, *coord, *coordinate, group.group_input, offset, scale, rotation);
  if (chain.rotate == nullptr || chain.mapping == nullptr) {
    return nullptr;
  }
  grid_.place_extra(*chain.rotate);
  grid_.place_extra(*chain.mapping);
  r_nodes_made.append(chain.rotate);
  r_nodes_made.append(chain.mapping);
  if (socket_out(*chain.mapping, "Vector") == nullptr) {
    return nullptr;
  }

  const float3 seed = paint_layer_generator_seed_offset(
      int(paint_layer_kind_param_get(layer, PAINT_LAYER_GEN_SEED)));
  bNode *shifted = vector_math_node_add(tree,
                                        grid_,
                                        r_nodes_made,
                                        NODE_VECTOR_MATH_ADD,
                                        {chain.mapping, socket_out(*chain.mapping, "Vector")},
                                        {});
  bNodeSocket *seed_in = (shifted != nullptr) ? socket_in(*shifted, "Vector_001") : nullptr;
  if (shifted == nullptr || seed_in == nullptr || seed_in->default_value == nullptr) {
    return nullptr;
  }
  const float seed_value[3] = {seed.x, seed.y, seed.z};
  copy_v3_v3(static_cast<bNodeSocketValueVector *>(seed_in->default_value)->value, seed_value);
  return shifted;
}

bNode *PaintLayersTreeBuilder::generator_pattern_build(bNodeTree &tree,
                                                       LayerGroup &group,
                                                       const MaterialPaintLayer &layer,
                                                       Vector<bNode *> &r_nodes_made)
{
  /* A row whose kind schema this build does not know has no pattern to make: the same skip the
   * composite applies, and the same "visible but empty" state. */
  const PaintLayerKindInfo *schema = paint_layer_kind_of(layer);
  const PaintLayerGeneratorInfo *info = BKE_paint_layers_generator_of(layer);
  if (schema == nullptr || info == nullptr) {
    return nullptr;
  }
  const int space = paint_layer_generator_space_of(layer);

  bNode *placed_node = this->generator_placed_coordinate_ensure(
      tree, group, layer, space, r_nodes_made);
  bNodeSocket *placed = (placed_node != nullptr) ? socket_out(*placed_node, "Vector") : nullptr;
  if (placed == nullptr) {
    return nullptr;
  }

  bNodeSocket *vector_in = nullptr;
  bNode *pattern = generator_pattern_node_add(tree, layer, *info, space, &vector_in);
  if (pattern == nullptr) {
    return nullptr;
  }
  grid_.place(*pattern, layout::Column::Source, 0);
  r_nodes_made.append(pattern);
  bke::node_add_link(tree, *placed_node, *placed, *pattern, *vector_in);

  /* Every parameter the node reads through a socket of its own comes from the row group, so an
   * edit of one is a value the sync writes in place. */
  Map<int, bNodeTreeInterfaceSocket *> *by_param = generator_param_inputs_.lookup_ptr(&layer);
  if (by_param == nullptr) {
    return nullptr;
  }
  for (const int i : IndexRange(schema->params_num)) {
    const char *socket_name = paint_layer_generator_param_socket(layer, i);
    if (socket_name == nullptr) {
      continue;
    }
    bNodeTreeInterfaceSocket *const *iface = by_param->lookup_ptr(i);
    bNodeSocket *value = (iface != nullptr && *iface != nullptr) ?
                            this->group_input_socket(group, **iface) :
                            nullptr;
    bNodeSocket *target = socket_in(*pattern, socket_name);
    if (value == nullptr || target == nullptr) {
      return nullptr;
    }
    bke::node_add_link(tree, *group.group_input, *value, *pattern, *target);
  }
  return pattern;
}

GeneratorFactor PaintLayersTreeBuilder::generator_pattern_factor_ensure(
    bNodeTree &tree, LayerGroup &group, const MaterialPaintLayer &layer, const char *factor_socket)
{
  if (const GeneratorFactor *cached = generator_factors_.lookup_ptr(&layer)) {
    return *cached;
  }
  Vector<bNode *> nodes_made;
  const auto discard = [&]() -> GeneratorFactor {
    this->nodes_discard(tree, nodes_made);
    return {};
  };

  bNode *pattern = this->generator_pattern_build(tree, group, layer, nodes_made);
  if (pattern == nullptr) {
    return discard();
  }
  GeneratorFactor out;
  out.factor = {pattern, socket_out(*pattern, factor_socket)};
  if (out.factor.socket == nullptr) {
    return discard();
  }
  generator_factors_.add(&layer, out);
  return out;
}

GeneratorFactor PaintLayersTreeBuilder::generator_brick_factor_ensure(bNodeTree &tree,
                                                                      LayerGroup &group,
                                                                      const MaterialPaintLayer
                                                                          &layer)
{
  if (const GeneratorFactor *cached = generator_factors_.lookup_ptr(&layer)) {
    return *cached;
  }
  Vector<bNode *> nodes_made;
  const auto discard = [&]() -> GeneratorFactor {
    this->nodes_discard(tree, nodes_made);
    return {};
  };

  bNode *pattern = this->generator_pattern_build(tree, group, layer, nodes_made);
  if (pattern == nullptr) {
    return discard();
  }
  bNodeSocket *brick_color = socket_out(*pattern, "Color");
  bNodeSocket *color1 = socket_in(*pattern, "Color1");
  bNodeSocket *color2 = socket_in(*pattern, "Color2");
  bNodeSocket *mortar = socket_in(*pattern, "Mortar");
  if (brick_color == nullptr || color1 == nullptr || color2 == nullptr || mortar == nullptr) {
    return discard();
  }
  /* The three colours are not parameters of the generator: they are the shape of the answer the
   * CPU evaluates, so the node is made to give it. */
  if (color1->default_value != nullptr) {
    copy_v4_fl4(static_cast<bNodeSocketValueRGBA *>(color1->default_value)->value,
                1.0f,
                1.0f,
                1.0f,
                1.0f);
  }
  for (bNodeSocket *black : {color2, mortar}) {
    if (black->default_value != nullptr) {
      zero_v4(static_cast<bNodeSocketValueRGBA *>(black->default_value)->value);
    }
  }
  GeneratorFactor out;
  out.factor = generator_grey_of_color(tree, grid_, nodes_made, {pattern, brick_color});
  if (out.factor.socket == nullptr) {
    return discard();
  }
  generator_factors_.add(&layer, out);
  return out;
}

GeneratorFactor PaintLayersTreeBuilder::generator_voronoi_factor_ensure(
    bNodeTree &tree, LayerGroup &group, const MaterialPaintLayer &layer)
{
  if (const GeneratorFactor *cached = generator_factors_.lookup_ptr(&layer)) {
    return *cached;
  }
  Vector<bNode *> nodes_made;
  const auto discard = [&]() -> GeneratorFactor {
    this->nodes_discard(tree, nodes_made);
    return {};
  };

  bNode *pattern = this->generator_pattern_build(tree, group, layer, nodes_made);
  if (pattern == nullptr) {
    return discard();
  }
  const int feature = int(paint_layer_kind_param_get(layer, PAINT_LAYER_GEN_VORONOI_FEATURE));
  GeneratorFactor out;
  if (feature == SHD_VORONOI_N_SPHERE_RADIUS) {
    out.factor = {pattern, socket_out(*pattern, "Radius")};
  }
  else if (paint_layer_generator_has_own_color(layer)) {
    bNodeSocket *cell = socket_out(*pattern, "Color");
    if (cell == nullptr) {
      return discard();
    }
    out.color = {pattern, cell};
    /* A scalar channel still indexes its ramp, with the luminance the CPU takes of the colour. */
    bNode *bw = bke::node_add_static_node(nullptr, tree, SH_NODE_RGBTOBW);
    bNodeSocket *bw_color = (bw != nullptr) ? socket_in(*bw, "Color") : nullptr;
    bNodeSocket *bw_val = (bw != nullptr) ? socket_out(*bw, "Val") : nullptr;
    if (bw == nullptr || bw_color == nullptr || bw_val == nullptr) {
      if (bw != nullptr) {
        bke::node_remove_node(nullptr, tree, *bw, true, false);
      }
      return discard();
    }
    grid_.place_extra(*bw);
    nodes_made.append(bw);
    bke::node_add_link(tree, *pattern, *cell, *bw, *bw_color);
    out.factor = {bw, bw_val};
  }
  else {
    out.factor = {pattern, socket_out(*pattern, "Distance")};
  }
  if (out.factor.socket == nullptr) {
    return discard();
  }
  generator_factors_.add(&layer, out);
  return out;
}

KindEmitResult PaintLayersTreeBuilder::generator_ramp_emit(const RowEmitContext &ctx,
                                                           const GeneratorFactor &pattern)
{
  bNodeTree &tree = ctx.tree;
  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
      eMaterialPaintChannel(ctx.channel));
  /* A colour channel reads the row's shared ramp; a scalar channel reads its own value ramp, whose
   * grey stops are the values along the pattern (the same ramp the CPU evaluates). */
  bNode *ramp = this->generator_ramp_ensure(tree, ctx.layer, ctx.channel);
  if (ramp == nullptr) {
    return {};
  }
  bNodeSocket *fac = socket_in(*ramp, "Fac");
  bNodeSocket *color = socket_out(*ramp, "Color");
  bNodeSocket *alpha = socket_out(*ramp, "Alpha");
  if (fac == nullptr || color == nullptr || alpha == nullptr) {
    return {};
  }
  /* A ramp node can be shared by several channels: the factor is linked once. */
  if (fac->link == nullptr) {
    bke::node_add_link(tree, *pattern.factor.node, *pattern.factor.socket, *ramp, *fac);
  }
  KindEmitResult out;
  out.color = {ramp, color};
  if (info.is_color) {
    out.coverage = {ramp, alpha};
  }
  out.content_alpha = {ramp, alpha};
  return out;
}

KindEmitResult PaintLayersTreeBuilder::generator_own_color_emit(const RowEmitContext &ctx,
                                                                const GeneratorFactor &pattern)
{
  /* The cell colour a Voronoi row lays instead of an index into its band: the node's own answer,
   * opaque everywhere, exactly what the CPU's own-colour branch writes. */
  bNodeSocket *one_out = nullptr;
  bNode *one = constant_one_node_add(ctx.tree, grid_, &one_out);
  if (one == nullptr) {
    return {};
  }
  KindEmitResult out;
  out.color = pattern.color;
  out.coverage = {one, one_out};
  out.content_alpha = out.coverage;
  return out;
}

/** \} */

GeneratorFactor PaintLayersTreeBuilder::generator_compound_factor_ensure(
    bNodeTree &tree, LayerGroup &group, const MaterialPaintLayer &layer)
{
  const PaintLayerGeneratorInfo *info = BKE_paint_layers_generator_of(layer);
  if (info == nullptr || !paint_layer_generator_is_compound(info->subtype)) {
    return {};
  }
  if (const GeneratorFactor *cached = generator_factors_.lookup_ptr(&layer)) {
    return *cached;
  }
  const int space = paint_layer_generator_space_of(layer);
  const bool is_2d = space == MA_PAINT_LAYER_GRADIENT_SPACE_UV;
  if (!ctx_.generator_group_get) {
    return {};
  }
  bNodeTree *pattern_group = ctx_.generator_group_get(info->subtype, is_2d);
  if (pattern_group == nullptr || tree.typeinfo == nullptr ||
      tree.typeinfo->group_idname == nullptr)
  {
    return {};
  }
  const char *coordinate_id = info->group_coordinate_id;
  const char *factor_id = info->group_factor_id;
  if (coordinate_id == nullptr || factor_id == nullptr) {
    return {};
  }
  const PaintLayerKindInfo *schema = paint_layer_kind_of(layer);
  if (schema == nullptr) {
    return {};
  }

  Vector<bNode *> nodes_made;
  const auto discard = [&]() -> GeneratorFactor {
    this->nodes_discard(tree, nodes_made);
    return {};
  };

  /* The same placed coordinate a single-node pattern reads: the group's own Coordinate input is what
   * the pattern's Vector socket would have been, so a row of a compound kind and a row of a node
   * kind stand in the same place in the row's graph. */
  bNode *placed_node = this->generator_placed_coordinate_ensure(
      tree, group, layer, space, nodes_made);
  bNodeSocket *placed = (placed_node != nullptr) ? socket_out(*placed_node, "Vector") : nullptr;
  if (placed == nullptr) {
    return discard();
  }

  bNode *instance = bke::node_add_node(nullptr, tree, tree.typeinfo->group_idname);
  if (instance == nullptr) {
    return discard();
  }
  instance->id = &pattern_group->id;
  id_us_plus(&pattern_group->id);
  STRNCPY_UTF8(instance->label, schema->name);
  nodes::update_node_declaration_and_sockets(tree, *instance);
  grid_.place(*instance, layout::Column::Source, 0);
  nodes_made.append(instance);

  const auto instance_socket = [&](const eNodeSocketInOut in_out,
                                   const char *identifier) -> bNodeSocket * {
    return bke::node_find_socket(*instance, in_out, UString::from_ptr_noinline(identifier));
  };

  bNodeSocket *coordinate_in = instance_socket(SOCK_IN, coordinate_id);
  if (coordinate_in == nullptr) {
    return discard();
  }
  bke::node_add_link(tree, *placed_node, *placed, *instance, *coordinate_in);

  /* Every number the pattern reads is an input of the group and one of the row group, so an edit of
   * it is a value the sync writes in place -- the same rule the single nodes follow, with the
   * group's interface identifiers in the socket table in place of a node's socket names. */
  Map<int, bNodeTreeInterfaceSocket *> *by_param = generator_param_inputs_.lookup_ptr(&layer);
  if (by_param == nullptr) {
    return discard();
  }
  for (const int i : IndexRange(schema->params_num)) {
    const char *identifier = paint_layer_generator_param_socket(layer, i);
    if (identifier == nullptr) {
      continue;
    }
    bNodeTreeInterfaceSocket *const *iface = by_param->lookup_ptr(i);
    bNodeSocket *value = (iface != nullptr && *iface != nullptr) ?
                             this->group_input_socket(group, **iface) :
                             nullptr;
    bNodeSocket *target = instance_socket(SOCK_IN, identifier);
    if (value == nullptr || target == nullptr) {
      return discard();
    }
    bke::node_add_link(tree, *group.group_input, *value, *instance, *target);
  }

  bNodeSocket *factor = instance_socket(SOCK_OUT, factor_id);
  if (factor == nullptr) {
    return discard();
  }
  GeneratorFactor out;
  out.factor = {instance, factor};
  generator_factors_.add(&layer, out);
  return out;
}

/* The emission of the compound generators: the row instantiates the shared node group of its kind
 * and the channel's ramp reads the group's Factor, the same tail every pattern shares. */
static KindEmitResult generator_compound_gpu_emit(PaintLayersTreeBuilder &builder,
                                                  const RowEmitContext &ctx)
{
  const GeneratorFactor pattern = builder.generator_compound_factor_ensure(
      ctx.tree, ctx.group, ctx.layer);
  if (pattern.factor.socket == nullptr) {
    return {};
  }
  return builder.generator_ramp_emit(ctx, pattern);
}

KindEmitResult paint_layer_generator_scratches_gpu_emit(PaintLayersTreeBuilder &builder,
                                                        const RowEmitContext &ctx)
{
  return generator_compound_gpu_emit(builder, ctx);
}

KindEmitResult paint_layer_generator_weave_gpu_emit(PaintLayersTreeBuilder &builder,
                                                    const RowEmitContext &ctx)
{
  return generator_compound_gpu_emit(builder, ctx);
}

KindEmitResult paint_layer_generator_grunge_gpu_emit(PaintLayersTreeBuilder &builder,
                                                     const RowEmitContext &ctx)
{
  return generator_compound_gpu_emit(builder, ctx);
}

}  // namespace bke::paint_layers
}  // namespace blender
