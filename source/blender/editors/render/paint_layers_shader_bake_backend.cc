/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edrender
 *
 * The shader-bake backend: turns a `PaintLayerShaderBakeRequest` into pixels, by way of the same
 * machinery the source-graph cache bakes with. A temporary material is built around the user's
 * tree -- the tree instanced, its parameters bound from the row's properties, its `Result` into
 * the Principled's Base Color -- and #material_bake_to_images runs it to completion on the
 * calling thread, over the unit UV square, into a temporary bake target. The pixels are then the
 * caller's.
 *
 * The binding contract is the node-group convention's: the group's `UV` input (when the tree has
 * one) takes the bake's UV; every other input but the reserved ones is bound by identifier from
 * #PaintLayerShaderBakeRequest::parameters (the row's system properties, keyed by the interface
 * socket identifier, the same keys the dynamic kind schema reads). The `Input` texture of a
 * filter kernel is a temporary image the request's buffer is wrapped into, sampled closest so a
 * canvas-sized input passes through texel for texel.
 *
 * A generator ask (#PaintLayerShaderBakeRequest::generator, the node-group generator's pattern
 * emission) builds the placement chain instead of the `UV` feed: the space's own Texture
 * Coordinate output through the turn, the scale and the offsets the request's placement
 * properties name, plus the seed's hash offset -- the same chain the generated graph builds
 * (#PaintLayersTreeBuilder::generator_placed_coordinate_ensure), node for node -- into the tree's
 * `Coordinate`, and lays the `Fac` through each channel's ramp tail, the context the outputs
 * carry (#PaintLayerShaderBakeOutput::ramp). Both sides render the same node chain; the bake
 * just does it through the backend instead of the build.
 *
 * The backend runs on the main thread: the bake workers reach the same machinery through the
 * effect cache instead (a result the preview computed once serves every later bake), and a
 * worker-local bake of its own would race the image writes the landing does on the main thread.
 */

#include <algorithm>
#include <cmath>
#include <cstring>

#include "BLI_index_range.hh"
#include "BLI_listbase_iterator.hh"
#include "BLI_math_base.h"
#include "BLI_math_vector.h"
#include "BLI_span.hh"
#include "BLI_string.h"
#include "BLI_string_ref.hh"
#include "BLI_threads.h"
#include "BLI_ustring.hh"
#include "BLI_utildefines.h"

#include "BKE_context.hh"
#include "BKE_global.hh"
#include "BKE_idprop.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_node_group.hh"
#include "BKE_paint_layers_shader_bake.hh"
#include "BKE_paint_layers_tile.hh"

#include "DNA_ID.h"
#include "DNA_color_types.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"

#include "ED_material_bake.hh"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "NOD_socket.hh"

#include "RNA_prototypes.hh"

#include "paint_layers_shader_bake_backend_intern.hh" /* own include */

namespace blender::ed::render {

/* -------------------------------------------------------------------- */
/** \name The binding
 * \{ */

/** The interface input named \a identifier, as the instance socket is found by name. */
static const bNodeTreeInterfaceSocket *backend_interface_input(const bNodeTree &tree,
                                                               const StringRef identifier)
{
  const bNodeTreeInterfaceSocket *found = nullptr;
  tree.tree_interface.root_panel.foreach_item(
      [&](const bNodeTreeInterfaceItem &item) {
        if (item.item_type != NodeTreeInterfaceItemType::Socket) {
          return true;
        }
        const bNodeTreeInterfaceSocket &socket = reinterpret_cast<const bNodeTreeInterfaceSocket &>(item);
        if ((socket.flag & NODE_INTERFACE_SOCKET_INPUT) != 0 &&
            StringRef(socket.identifier) == identifier)
        {
          found = &socket;
          return false;
        }
        return true;
      },
      /*include_self=*/false);
  return found;
}

/** #bke::node_find_socket with the convention's `const char *` names, without a UString temporary
 * at every call site. */
static bNodeSocket *backend_find_socket(bNode &node,
                                        const eNodeSocketInOut in_out,
                                        const char *name)
{
  return bke::node_find_socket(node, in_out, UString::from_ptr_noinline(name));
}

static bNode *backend_add_node(bNodeTree &tree, const int type)
{
  return bke::node_add_static_node(nullptr, tree, type);
}

/** Write \a prop's value into the socket \a instance names \a socket_name, typed as the socket is.
 * False when the socket is missing or the types disagree. */
static bool backend_socket_value_bind(bNode &instance,
                                      const char *socket_name,
                                      const IDProperty &prop)
{
  bNodeSocket *socket = backend_find_socket(instance, SOCK_IN, socket_name);
  if (socket == nullptr) {
    return false;
  }
  switch (socket->type) {
    case SOCK_FLOAT: {
      if (prop.type != IDP_FLOAT && prop.type != IDP_INT) {
        return false;
      }
      static_cast<bNodeSocketValueFloat *>(socket->default_value)->value =
          (prop.type == IDP_FLOAT) ? IDP_float_get(&prop) : float(IDP_int_get(&prop));
      return true;
    }
    case SOCK_INT: {
      if (prop.type != IDP_INT && prop.type != IDP_FLOAT) {
        return false;
      }
      static_cast<bNodeSocketValueInt *>(socket->default_value)->value =
          (prop.type == IDP_INT) ? IDP_int_get(&prop) : int(IDP_float_get(&prop));
      return true;
    }
    case SOCK_BOOLEAN: {
      if (prop.type != IDP_BOOLEAN) {
        return false;
      }
      static_cast<bNodeSocketValueBoolean *>(socket->default_value)->value =
          IDP_bool_get(&prop) ? 1 : 0;
      return true;
    }
    case SOCK_MENU: {
      /* A menu parameter stores the item's persistent int identifier, which is what the socket's
       * own default_value carries; a schema that lost the choice reads as no override. */
      if (prop.type != IDP_INT) {
        return false;
      }
      static_cast<bNodeSocketValueMenu *>(socket->default_value)->value = IDP_int_get(&prop);
      return true;
    }
    case SOCK_RGBA: {
      if (prop.type != IDP_ARRAY || prop.subtype != IDP_FLOAT || prop.len != 4) {
        return false;
      }
      const float *stored = IDP_array_float_get(&prop);
      copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(socket->default_value)->value, stored);
      return true;
    }
    case SOCK_VECTOR: {
      if (prop.type != IDP_ARRAY || prop.subtype != IDP_FLOAT || prop.len != 3) {
        return false;
      }
      const float *stored = IDP_array_float_get(&prop);
      copy_v3_v3(static_cast<bNodeSocketValueVector *>(socket->default_value)->value, stored);
      return true;
    }
    default:
      /* A menu or a resource socket the schema does not carry: its default stands. */
      return false;
  }
}

/** Bind every parameter the row stores into the instance, by identifier through the interface. */
static void backend_parameters_bind(bNodeTree &tree, bNode &instance, const IDProperty *parameters)
{
  if (parameters == nullptr || parameters->type != IDP_GROUP) {
    return;
  }
  /* The request's group is read-only: the iterator walks it without writing through it. */
  for (const IDProperty &prop : parameters->data.group) {
    const bNodeTreeInterfaceSocket *interface = backend_interface_input(tree, prop.name);
    if (interface == nullptr) {
      /* A stored value the tree no longer names: the socket default stands. */
      continue;
    }
    backend_socket_value_bind(instance, interface->identifier, prop);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name The generator ask
 *
 * The node-group generator's pattern emission: the placement chain the graph's
 * #PaintLayersTreeBuilder::generator_placed_coordinate_ensure builds, node for node, with the
 * values read from the request's IDProperties by the schema's own names -- the same keys the
 * dynamic kind schema stores them under -- and the ramp tail the graph's
 * #PaintLayersTreeBuilder::generator_ramp_emit builds after the tree's `Fac`. Parity by
 * construction: both sides render the same node chain, the bake just does it through the backend
 * instead of the build.
 * \{ */

/** The placement values the backend's chain reads, with the schema's defaults where the row
 * carries nothing (a property the schema seeded is the rule; a null group is the fallback). */
struct BakePlacement {
  /** #eMaterialPaintLayerGradientSpace, the space's own texture-coordinate output. */
  int space = MA_PAINT_LAYER_GRADIENT_SPACE_UV;
  float scale_x = 5.0f;
  float scale_y = 5.0f;
  bool scale_lock = true;
  /** Degrees, as the schema stores the turn. */
  float rotation = 0.0f;
  float offset_x = 0.0f;
  float offset_y = 0.0f;
  int seed = 0;
};

/** Read \a prop as a float whatever scalar type it landed as, or \a fallback. */
static float backend_placement_float(const IDProperty *prop, const float fallback)
{
  if (prop == nullptr) {
    return fallback;
  }
  if (prop->type == IDP_FLOAT) {
    return IDP_float_get(prop);
  }
  if (prop->type == IDP_INT) {
    return float(IDP_int_get(prop));
  }
  if (prop->type == IDP_BOOLEAN) {
    return IDP_bool_get(prop) ? 1.0f : 0.0f;
  }
  return fallback;
}

/** Read \a prop as an int whatever scalar type it landed as, or \a fallback. */
static int backend_placement_int(const IDProperty *prop, const int fallback)
{
  if (prop == nullptr) {
    return fallback;
  }
  if (prop->type == IDP_INT) {
    return IDP_int_get(prop);
  }
  if (prop->type == IDP_FLOAT) {
    return int(IDP_float_get(prop));
  }
  return fallback;
}

/** The placement of a generator ask, from the request's IDProperties by the schema's names. */
static BakePlacement backend_placement_read(const IDProperty *parameters)
{
  BakePlacement placement;
  if (parameters == nullptr || parameters->type != IDP_GROUP) {
    return placement;
  }
  const auto prop_of = [&](const char *name) {
    return IDP_GetPropertyFromGroup(parameters, name);
  };
  placement.space = backend_placement_int(prop_of("space"), MA_PAINT_LAYER_GRADIENT_SPACE_UV);
  placement.scale_x = backend_placement_float(prop_of("scale"), 5.0f);
  placement.scale_y = backend_placement_float(prop_of("scale_y"), 5.0f);
  placement.scale_lock = backend_placement_float(prop_of("scale_lock"), 1.0f) != 0.0f;
  placement.rotation = backend_placement_float(prop_of("rotation"), 0.0f);
  placement.offset_x = backend_placement_float(prop_of("offset_x"), 0.0f);
  placement.offset_y = backend_placement_float(prop_of("offset_y"), 0.0f);
  placement.seed = backend_placement_int(prop_of("seed"), 0);
  return placement;
}

/**
 * The group-instance socket the convention's kind names: by identifier first -- a user's group
 * names its sockets `Coordinate` and `Fac` but their identifiers are the generated `Socket_<n>` --
 * then by name, so both spellings work. The same order the graph's own instance lookup takes
 * (#PaintLayersTreeBuilder::generator_compound_factor_ensure).
 */
static bNodeSocket *backend_instance_socket(bNode &instance,
                                            const eNodeSocketInOut in_out,
                                            const char *identifier_or_name)
{
  if (bNodeSocket *socket = backend_find_socket(instance, in_out, identifier_or_name)) {
    return socket;
  }
  /* The node's own socket list, not the topology cache's span: the bake builds a fresh tree and
   * wires it before any cache is ensured, and a span read there is empty (or stale). This is the
   * same list #bke::node_find_socket walks. */
  ListBaseT<bNodeSocket> &sockets = (in_out == SOCK_IN) ? instance.inputs : instance.outputs;
  for (bNodeSocket &socket : sockets) {
    if (StringRef(socket.name) == identifier_or_name) {
      return &socket;
    }
  }
  return nullptr;
}

/**
 * The placed coordinate of a generator ask: the Texture Coordinate output the row's `space`
 * names, through a Vector Rotate (the turn, in radians, around Z), a Point Mapping (the Y ratio
 * of the locked scales, then the two offsets), and one Vector Math Add carrying the seed's hash
 * offset as a constant -- `seed + scale * (R * uv) + seed_offset`, the exact chain
 * #generator_placed_coordinate_ensure builds and #paint_layer_generator_coordinate evaluates.
 * The values ride the nodes' socket defaults: the bake re-renders on every placement edit, so no
 * group input carries them. Returns the chain's output socket, or null when a node could not be
 * made (the ask then renders nothing, the way a failed chain leaves the row out of the graph).
 */
static bNodeSocket *backend_generator_coordinate_build(bNodeTree &tree,
                                                       const BakePlacement &placement)
{
  const auto space_output = [&]() -> const char * {
    switch (eMaterialPaintLayerGradientSpace(placement.space)) {
      case MA_PAINT_LAYER_GRADIENT_SPACE_OBJECT:
        return "Object";
      case MA_PAINT_LAYER_GRADIENT_SPACE_GENERATED:
        return "Generated";
      case MA_PAINT_LAYER_GRADIENT_SPACE_UV:
        break;
    }
    /* An unknown space is the safe UV default, not a discarded row -- the same answer the
     * graph's #generator_coordinate_socket gives. */
    return "UV";
  };

  bNode *coord = backend_add_node(tree, SH_NODE_TEX_COORD);
  bNode *rotate = backend_add_node(tree, SH_NODE_VECTOR_ROTATE);
  bNode *mapping = backend_add_node(tree, SH_NODE_MAPPING);
  bNode *shifted = backend_add_node(tree, SH_NODE_VECTOR_MATH);
  if (coord == nullptr || rotate == nullptr || mapping == nullptr || shifted == nullptr) {
    return nullptr;
  }
  const auto remove_made = [&]() {
    for (bNode *made : {coord, rotate, mapping, shifted}) {
      if (made != nullptr) {
        bke::node_remove_node(nullptr, tree, *made, true, false);
      }
    }
  };

  bNodeSocket *coordinate = backend_find_socket(*coord, SOCK_OUT, space_output());
  bNodeSocket *rotate_vector = backend_find_socket(*rotate, SOCK_IN, "Vector");
  bNodeSocket *rotate_rotation = backend_find_socket(*rotate, SOCK_IN, "Rotation");
  bNodeSocket *rotate_out = backend_find_socket(*rotate, SOCK_OUT, "Vector");
  bNodeSocket *map_vector = backend_find_socket(*mapping, SOCK_IN, "Vector");
  bNodeSocket *map_location = backend_find_socket(*mapping, SOCK_IN, "Location");
  bNodeSocket *map_scale = backend_find_socket(*mapping, SOCK_IN, "Scale");
  bNodeSocket *map_out = backend_find_socket(*mapping, SOCK_OUT, "Vector");
  bNodeSocket *shift_a = backend_find_socket(*shifted, SOCK_IN, "Vector");
  bNodeSocket *shift_b = backend_find_socket(*shifted, SOCK_IN, "Vector_001");
  bNodeSocket *shift_out = backend_find_socket(*shifted, SOCK_OUT, "Vector");
  if (coordinate == nullptr || rotate_vector == nullptr || rotate_rotation == nullptr ||
      rotate_out == nullptr || map_vector == nullptr || map_location == nullptr ||
      map_scale == nullptr || map_out == nullptr || shift_a == nullptr || shift_b == nullptr ||
      shift_out == nullptr)
  {
    remove_made();
    return nullptr;
  }
  /* A Vector Rotate in front of the Mapping node gives `offset + scale * (R * uv)`: the Mapping
   * node alone would scale first and shear the pattern when the ratio is not one -- the same
   * order the row mapping's chain states in #mapping_chain_add. */
  rotate->custom1 = NODE_VECTOR_ROTATE_TYPE_EULER_XYZ;
  mapping->custom1 = NODE_MAPPING_TYPE_POINT;
  shifted->custom1 = NODE_VECTOR_MATH_ADD;
  if (bNodeSocketValueVector *rotation = static_cast<bNodeSocketValueVector *>(
          rotate_rotation->default_value))
  {
    /* The turn travels in radians, what a Vector Rotate turns by; the schema stores degrees. */
    rotation->value[0] = 0.0f;
    rotation->value[1] = 0.0f;
    rotation->value[2] = DEG2RADF(placement.rotation);
  }
  if (bNodeSocketValueVector *scale = static_cast<bNodeSocketValueVector *>(
          map_scale->default_value))
  {
    /* The Y ratio the lock folds: locked, one slider scales both axes; unlocked, the Y scale is
     * its own. The depth never scales, so a 3D pattern's third term stays the coordinate's. */
    const float ratio = (placement.scale_lock || placement.scale_x <= 0.0f) ?
                            1.0f :
                            placement.scale_y / placement.scale_x;
    scale->value[0] = 1.0f;
    scale->value[1] = ratio;
    scale->value[2] = 1.0f;
  }
  if (bNodeSocketValueVector *location = static_cast<bNodeSocketValueVector *>(
          map_location->default_value))
  {
    location->value[0] = placement.offset_x;
    location->value[1] = placement.offset_y;
    location->value[2] = 0.0f;
  }
  if (bNodeSocketValueVector *seed = static_cast<bNodeSocketValueVector *>(
          shift_b->default_value))
  {
    /* The hash offset the seed maps the coordinate by, the one formula
     * #paint_layer_generator_seed_offset runs: the two irrational multiples of the seed, each
     * folded back into its unit interval. A third term of the same sum, added last the way the
     * CPU adds it. */
    const float s = float(placement.seed);
    float seed_x = s * 0.6180339887498948482f;
    seed_x -= floorf(seed_x);
    float seed_y = s * 0.7548776662466927361f;
    seed_y -= floorf(seed_y);
    seed->value[0] = seed_x;
    seed->value[1] = seed_y;
    seed->value[2] = 0.0f;
  }
  bke::node_add_link(tree, *coord, *coordinate, *rotate, *rotate_vector);
  bke::node_add_link(tree, *rotate, *rotate_out, *mapping, *map_vector);
  bke::node_add_link(tree, *mapping, *map_out, *shifted, *shift_a);
  return shift_out;
}

/**
 * One channel's ramp tail, as the graph's #generator_ramp_emit builds it: the ask's band copied
 * into a ColorRamp node the tree's `Fac` indexes, or -- with a null band, the ask's linear pass --
 * a Map Range carrying the raw `Fac` from \a output.scalar_start to \a output.scalar_end. The
 * colour answer is the composed vec4 the dump reads whole: the band's colour in rgb and its alpha
 * in the fourth channel, the coverage the graph lays and the composite reads as the map's own. A
 * scalar answer feeds the socket straight, the implicit color-to-float conversion standing in
 * where a band answers a colour for a float channel -- the same conversion the graph's scalar
 * chain applies -- and the dump's alpha stays opaque, which is the scalar row's coverage by its
 * opacity alone. Returns the socket to feed the Principled with, or null when the channel has no
 * Principled socket to carry it.
 */
static bNodeSocket *backend_generator_channel_build(bNodeTree &tree,
                                                    bNode &principled,
                                                    const PaintLayerShaderBakeOutput &output,
                                                    const MaterialPaintChannelInfo &info,
                                                    bNodeSocket &fac_socket)
{
  const char *socket_name = info.socket_name;
  if (socket_name == nullptr) {
    /* A Height, an AO or a Custom has no Principled input to feed: the output stays transparent,
     * the way the per-channel source's unnamed channel does. */
    return nullptr;
  }
  bNodeSocket *socket_in = backend_find_socket(principled, SOCK_IN, socket_name);
  if (socket_in == nullptr) {
    return nullptr;
  }

  /* The tail: a band the graph copies, or the linear pass the null band falls back to. */
  bNode *tail = nullptr;
  bNodeSocket *tail_color = nullptr;
  bNodeSocket *tail_alpha = nullptr;
  if (output.ramp != nullptr) {
    tail = backend_add_node(tree, SH_NODE_VALTORGB);
    if (tail == nullptr || tail->storage == nullptr) {
      if (tail != nullptr) {
        bke::node_remove_node(nullptr, tree, *tail, true, false);
      }
      return nullptr;
    }
    *static_cast<ColorBand *>(tail->storage) = *output.ramp;
    tail_color = backend_find_socket(*tail, SOCK_OUT, "Color");
    tail_alpha = backend_find_socket(*tail, SOCK_OUT, "Alpha");
    bNodeSocket *fac_in = backend_find_socket(*tail, SOCK_IN, "Fac");
    if (tail_color == nullptr || tail_alpha == nullptr || fac_in == nullptr) {
      bke::node_remove_node(nullptr, tree, *tail, true, false);
      return nullptr;
    }
    bke::node_add_link(tree, fac_socket.owner_node(), fac_socket, *tail, *fac_in);
  }
  else {
    tail = backend_add_node(tree, SH_NODE_MAP_RANGE);
    if (tail == nullptr) {
      return nullptr;
    }
    bNodeSocket *value_in = backend_find_socket(*tail, SOCK_IN, "Value");
    bNodeSocket *to_min = backend_find_socket(*tail, SOCK_IN, "To Min");
    bNodeSocket *to_max = backend_find_socket(*tail, SOCK_IN, "To Max");
    bNodeSocket *result = backend_find_socket(*tail, SOCK_OUT, "Result");
    if (value_in == nullptr || to_min == nullptr || to_max == nullptr || result == nullptr) {
      bke::node_remove_node(nullptr, tree, *tail, true, false);
      return nullptr;
    }
    /* The linear pass the null band stands for: the raw `Fac`, from the channel's stored start
     * at 0 to its stored end at 1 -- exactly the fallback
     * #BKE_paint_material_generator_sample_coordinate lays when a channel has no band. The
     * node's own defaults give the rest of it: From 0..1, linear, clamped, which is the range
     * the pattern's factor is clamped into before the ramp reads it. */
    if (bNodeSocketValueFloat *start = static_cast<bNodeSocketValueFloat *>(to_min->default_value))
    {
      start->value = output.scalar_start;
    }
    if (bNodeSocketValueFloat *end = static_cast<bNodeSocketValueFloat *>(to_max->default_value))
    {
      end->value = output.scalar_end;
    }
    bke::node_add_link(tree, fac_socket.owner_node(), fac_socket, *tail, *value_in);
    tail_color = result;
    /* A linear pass carries no alpha of its own: the fourth channel stays at the socket's one,
     * which is the coverage a band-less row lays. */
    tail_alpha = nullptr;
  }

  bNodeSocket *fed = nullptr;
  if (info.is_color) {
    /* The Compose node is the carrier, the same mechanism the alpha ask uses: straight colour in,
     * the tail's alpha into the vec4's fourth channel. The bake dumps this socket's value whole,
     * so the map comes back with the coverage beside it -- the band's alpha, which is both the
     * coverage the graph lays and the alpha the CPU's band sample gives. */
    bNode *compose = backend_add_node(tree, SH_NODE_COMPOSE_COLOR_ALPHA);
    bNodeSocket *compose_color_in =
        (compose != nullptr) ? backend_find_socket(*compose, SOCK_IN, "Color") : nullptr;
    bNodeSocket *compose_alpha_in =
        (compose != nullptr) ? backend_find_socket(*compose, SOCK_IN, "Alpha") : nullptr;
    bNodeSocket *compose_color_out =
        (compose != nullptr) ? backend_find_socket(*compose, SOCK_OUT, "Color") : nullptr;
    if (compose_color_in != nullptr && compose_alpha_in != nullptr && compose_color_out != nullptr)
    {
      bke::node_add_link(tree, *tail, *tail_color, *compose, *compose_color_in);
      if (tail_alpha != nullptr) {
        bke::node_add_link(tree, *tail, *tail_alpha, *compose, *compose_alpha_in);
      }
      fed = compose_color_out;
    }
    else if (compose != nullptr) {
      /* A compose that could not be built lays the colour alone: the alpha stays the socket's
       * own, and the map's coverage falls back to the row's opacity -- the answer a tail without
       * an alpha output would have laid. */
      bke::node_remove_node(nullptr, tree, *compose, true, false);
      fed = tail_color;
    }
    else {
      fed = tail_color;
    }
  }
  else {
    /* A scalar channel reads the band's grey straight: the implicit color-to-float conversion
     * stands in where the tail answers a colour for a float socket, the same one the graph's
     * scalar chain applies. The dump's alpha stays opaque, so the row covers by its opacity --
     * the scalar answer the CPU sample gives. */
    fed = tail_color;
  }
  if (fed == nullptr) {
    return nullptr;
  }
  bke::node_add_link(tree, fed->owner_node(), *fed, principled, *socket_in);
  return fed;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name The graph
 * \{ */

/**
 * The temporary material the bake renders: the tree instanced, the UV fed where the tree asks for
 * it, the parameters bound, the `Result` into the Principled's Base Color. The material lives in
 * \a bmain for the length of the render (the bake copies and localizes it) and is the caller's to
 * free.
 *
 * With \a request.result_has_alpha (a #PAINT_LAYER_KIND_RESULT_HAS_ALPHA filter, a tree that
 * exposes an `Alpha` output) the alpha is wired beside the colour: the tree's `Alpha` output into
 * the Principled's own `Alpha` input, and -- through the Compose node that feeds Base Color --
 * into the fourth channel of the vec4 the bake dumps. The dump reads the one socket that reaches
 * Base Color, so the Compose node is the mechanism that carries a second answer beside the colour
 * without a second render target.
 *
 * With \a request.generator the wiring is the pattern emission instead: the placement chain into
 * the tree's `Coordinate`, the tree's `Fac` into each channel's ramp tail
 * (#backend_generator_channel_build), and the `Result`/alpha contract below does not apply -- a
 * generator's answer is its tail, not a tree output named after a channel.
 */
Material *paint_layers_shader_bake_backend_material_build(
    Main &bmain,
    const PaintLayerShaderBakeRequest &request,
    Image *input_image,
    const Span<const PaintLayerShaderBakeOutput> outputs)
{
  Material *material = BKE_material_add(&bmain, "PL NG Bake");
  if (material == nullptr || material->nodetree == nullptr) {
    if (material != nullptr) {
      BKE_id_free(&bmain, material);
    }
    return nullptr;
  }
  bNodeTree &tree = *material->nodetree;

  bNode *instance = bke::node_add_node(nullptr, tree, tree.typeinfo->group_idname);
  bNode *principled = backend_add_node(tree, SH_NODE_BSDF_PRINCIPLED);
  bNode *output = backend_add_node(tree, SH_NODE_OUTPUT_MATERIAL);
  if (instance == nullptr || principled == nullptr || output == nullptr) {
    BKE_id_free(&bmain, material);
    return nullptr;
  }
  /* The instance takes a user on the definition: the request hands it const because a render
   * never edits the tree, but the user count is the node's own. */
  instance->id = const_cast<ID *>(&request.tree->id);
  id_us_plus(instance->id);
  nodes::update_node_declaration_and_sockets(tree, *instance);

  /* The UV the bake renders over, into the tree's `UV` input when the tree exposes one; a tree
   * without it reads the texture coordinates itself, and the bake's UV is what those are. */
  if (backend_instance_socket(*instance, SOCK_IN, PAINT_NG_SOCKET_UV) != nullptr) {
    if (bNode *texcoord = backend_add_node(tree, SH_NODE_TEX_COORD)) {
      if (bNodeSocket *uv_out = backend_find_socket(*texcoord, SOCK_OUT, "UV")) {
        bNodeSocket *uv_in = backend_instance_socket(*instance, SOCK_IN, PAINT_NG_SOCKET_UV);
        if (uv_in != nullptr) {
          bke::node_add_link(tree, *texcoord, *uv_out, *instance, *uv_in);
        }
      }
    }
  }

  /* The pattern emission's placement: the chain into the tree's `Coordinate`, built before the
   * parameters bind so the bind below still reaches the tree's own inputs (a generator's schema
   * carries them beside the placement prefix, keyed the same way). The chain reads the placement
   * values from the request's IDProperties itself. */
  if (request.generator) {
    if (bNodeSocket *coordinate_in = backend_instance_socket(*instance,
                                                             SOCK_IN,
                                                             PAINT_NG_SOCKET_COORDINATE))
    {
      const BakePlacement placement = backend_placement_read(request.parameters);
      if (bNodeSocket *placed = backend_generator_coordinate_build(tree, placement)) {
        bke::node_add_link(tree,
                           placed->owner_node(),
                           *placed,
                           *instance,
                           *coordinate_in);
      }
    }
  }

  backend_parameters_bind(tree, *instance, request.parameters);

  /* The filter kernel's input: a temporary image the buffer is wrapped into, sampled closest so a
   * canvas-sized buffer passes through texel for texel. */
  if (input_image != nullptr) {
    if (bNode *image_node = backend_add_node(tree, SH_NODE_TEX_IMAGE)) {
      if (NodeTexImage *storage = static_cast<NodeTexImage *>(image_node->storage)) {
        storage->interpolation = SHD_INTERP_CLOSEST;
        storage->extension = SHD_IMAGE_EXTENSION_EXTEND;
      }
      image_node->id = &input_image->id;
      id_us_plus(&input_image->id);
      if (bNodeSocket *color_out = backend_find_socket(*image_node, SOCK_OUT, "Color")) {
        bNodeSocket *input_in = backend_instance_socket(*instance, SOCK_IN, PAINT_NG_SOCKET_INPUT);
        if (input_in != nullptr) {
          bke::node_add_link(tree, *image_node, *color_out, *instance, *input_in);
        }
      }
    }
  }

  /* The one-answer contract (a -1 channel: a filter kernel) lays `Result` into the Base Color;
   * a per-channel source lays each named output into its own Principled socket, the implicit
   * color-to-float conversion standing in where the tree answers a colour for a scalar channel.
   * A channel with no Principled socket (a Height, an AO) has no link to make -- its output is
   * filled transparent below, so the reader never takes another channel's pixels for it. A
   * generator ask runs the wiring above instead and skips this loop: its answer is the ramp tail
   * of `Fac`, not a tree output named after a channel. */
  bool wired_base_color = false;
  /* The alpha a #PAINT_LAYER_KIND_RESULT_HAS_ALPHA filter's result covers by: present only when
   * the ask names it and the tree exposes the output (a source tree's `Alpha` output answers
   * nothing -- its coverage is its map's own alpha -- so the wiring stays filter-only). */
  const bool alpha_asked = request.result_has_alpha &&
                           backend_instance_socket(*instance, SOCK_OUT, PAINT_NG_SOCKET_ALPHA) !=
                               nullptr;
  if (request.generator) {
    /* The tree's `Fac` indexes every channel's ramp tail: one chain, one tail per channel, laid
     * into the socket of that channel the way the per-channel source lays its named outputs. The
     * tree's `Color` output, where the tree carries one, is left to the graph's own-colour shape:
     * the bake answers the ramp tail, the same non-own-colour shape a tree without the output
     * rides, and the own-colour form stays the viewport's until the bake grows the branch. A
     * channel whose tail could not be built (no Principled socket, a failed node) stays
     * transparent -- the output the loop below fills, never another channel's pixels. */
    /* A tree that answers `Color` lays it whole instead of the ramp tail, the cell-colour shape
     * the generated graph's own-colour branch takes; a tree without it rides the ramp. The
     * coverage of the own-colour form is the opaque one the graph uses, so no Compose/alpha
     * carrier is made here. */
    bNodeSocket *own_color = backend_instance_socket(*instance, SOCK_OUT, PAINT_NG_SOCKET_COLOR);
    if (own_color != nullptr) {
      for (const PaintLayerShaderBakeOutput &output : outputs) {
        if (output.channel < 0) {
          continue;
        }
        const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
            eMaterialPaintChannel(output.channel));
        if (info.socket_name == nullptr) {
          continue;
        }
        bNodeSocket *socket_in = backend_find_socket(*principled, SOCK_IN, info.socket_name);
        if (socket_in == nullptr) {
          continue;
        }
        bke::node_add_link(tree, own_color->owner_node(), *own_color, *principled, *socket_in);
        if (output.channel == PAINT_MATERIAL_CHANNEL_BASE_COLOR) {
          wired_base_color = true;
        }
      }
    }
    else if (bNodeSocket *fac_out = backend_instance_socket(*instance,
                                                            SOCK_OUT,
                                                            PAINT_NG_SOCKET_FAC))
    {
      for (const PaintLayerShaderBakeOutput &output : outputs) {
        if (output.channel < 0) {
          continue;
        }
        const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
            eMaterialPaintChannel(output.channel));
        const bNodeSocket *fed = backend_generator_channel_build(
            tree, *principled, output, info, *fac_out);
        if (fed != nullptr && output.channel == PAINT_MATERIAL_CHANNEL_BASE_COLOR) {
          wired_base_color = true;
        }
      }
    }
  }
  else if (alpha_asked) {
    /* The Principled takes the same alpha its Base Color will carry: the surface renders what the
     * map lays. The dump does not read this input -- the AOV carries the socket the wiring below
     * reaches Base Color through -- so the link only keeps the temporary material honest. */
    if (bNodeSocket *alpha_out = backend_instance_socket(*instance, SOCK_OUT,
                                                         PAINT_NG_SOCKET_ALPHA)) {
      if (bNodeSocket *alpha_in = backend_find_socket(*principled, SOCK_IN, "Alpha")) {
        bke::node_add_link(tree, *instance, *alpha_out, *principled, *alpha_in);
      }
    }
  }
  /* The channels the ask names, in the order the outputs carry them: the contract wiring reads
   * them. A generator ask runs the tail wiring above and no contract loop -- `Result` names
   * nothing a generator's tree answers by. */
  if (!request.generator) {
    Vector<int> channels;
    for (const PaintLayerShaderBakeOutput &output : outputs) {
      channels.append(output.channel);
    }
    for (const int channel : channels) {
      if (channel < 0) {
        if (bNodeSocket *result_out = backend_instance_socket(*instance, SOCK_OUT,
                                                              PAINT_NG_SOCKET_RESULT))
        {
          if (bNodeSocket *base_in = backend_find_socket(*principled, SOCK_IN, "Base Color")) {
            if (alpha_asked) {
              /* The Compose node is the carrier: straight colour in from `Result`, the tree's
               * `Alpha` output into the vec4's fourth channel. The bake dumps this socket's value
               * whole, so the buffer comes back with the alpha beside it -- the form
               * #shader_bake_backend_render premultiplies into the contract's. A compose that
               * could not be built falls back to the colour alone, the answer a tree without the
               * output would have laid. */
              bNode *compose = backend_add_node(tree, SH_NODE_COMPOSE_COLOR_ALPHA);
              bNodeSocket *compose_color_in =
                  (compose != nullptr) ? backend_find_socket(*compose, SOCK_IN, "Color") : nullptr;
              bNodeSocket *compose_alpha_in =
                  (compose != nullptr) ? backend_find_socket(*compose, SOCK_IN, "Alpha") : nullptr;
              bNodeSocket *compose_color_out =
                  (compose != nullptr) ? backend_find_socket(*compose, SOCK_OUT, "Color") : nullptr;
              if (compose_color_in != nullptr && compose_alpha_in != nullptr &&
                  compose_color_out != nullptr)
              {
                bke::node_add_link(tree, *instance, *result_out, *compose, *compose_color_in);
                if (bNodeSocket *alpha_out = backend_instance_socket(*instance, SOCK_OUT,
                                                                     PAINT_NG_SOCKET_ALPHA))
                {
                  bke::node_add_link(tree, *instance, *alpha_out, *compose, *compose_alpha_in);
                }
                bke::node_add_link(tree, *compose, *compose_color_out, *principled, *base_in);
                wired_base_color = true;
              }
            }
            if (!wired_base_color) {
              bke::node_add_link(tree, *instance, *result_out, *principled, *base_in);
              wired_base_color = true;
            }
          }
        }
        break;
      }
      const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
          eMaterialPaintChannel(channel));
      if (info.socket_name == nullptr) {
        continue;
      }
      bNodeSocket *out = backend_instance_socket(*instance, SOCK_OUT, info.ui_name);
      bNodeSocket *in = backend_find_socket(*principled, SOCK_IN, info.socket_name);
      if (out != nullptr && in != nullptr) {
        bke::node_add_link(tree, *instance, *out, *principled, *in);
        if (channel == PAINT_MATERIAL_CHANNEL_BASE_COLOR) {
          wired_base_color = true;
        }
      }
    }
  }
  /* A contract answer the wiring could not feed still needs a black Base Color, or the bake's
   * own resolve would call the channel a constant and print nothing. */
  if (!wired_base_color) {
    if (bNode *black = backend_add_node(tree, SH_NODE_RGB)) {
      if (bNodeSocket *black_out = backend_find_socket(*black, SOCK_OUT, "Color")) {
        if (bNodeSocketValueRGBA *value = static_cast<bNodeSocketValueRGBA *>(
                black_out->default_value))
        {
          zero_v4(value->value);
        }
        if (bNodeSocket *base_in = backend_find_socket(*principled, SOCK_IN, "Base Color")) {
          bke::node_add_link(tree, *black, *black_out, *principled, *base_in);
        }
      }
    }
  }
  if (bNodeSocket *surface_out = backend_find_socket(*principled, SOCK_OUT, "BSDF")) {
    if (bNodeSocket *surface_in = backend_find_socket(*output, SOCK_IN, "Surface")) {
      bke::node_add_link(tree, *principled, *surface_out, *output, *surface_in);
    }
  }

  BKE_ntree_update_after_single_tree_change(bmain, tree);
  return material;
}

/**
 * The temporary image the kernel's input buffer is sampled from. Data colorspace, like every
 * buffer the stack reads raw; freed by the caller once the render is done with it.
 */
static Image *backend_input_image_build(Main &bmain,
                                        const int width,
                                        const int height,
                                        const float *pixels)
{
  ImBuf *ibuf = IMB_allocImBuf(unsigned(width), unsigned(height), ImBufFlags::FloatData);
  if (ibuf == nullptr || ibuf->float_buffer.data == nullptr) {
    if (ibuf != nullptr) {
      IMB_freeImBuf(ibuf);
    }
    return nullptr;
  }
  /* The buffer was allocated above and is unshared: the only writer is this copy. */
  memcpy(const_cast<float *>(ibuf->float_buffer.data),
         pixels,
         sizeof(float) * size_t(width) * size_t(height) * 4);
  Image *image = BKE_image_add_from_imbuf(&bmain, ibuf, "PL NG Input");
  IMB_freeImBuf(ibuf);
  if (image != nullptr) {
    STRNCPY(image->colorspace_settings.name,
            IMB_colormanagement_role_colorspace_name_get(COLOR_ROLE_DATA));
  }
  return image;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name The render
 * \{ */

/** Fill \a pixels (an RGBA float map of \a pixels_num texels) with transparent black: the answer
 * a channel the backend could not feed lays, so a reader never takes another channel's pixels. */
static void backend_pixels_fill_transparent(float *pixels, const size_t pixels_num)
{
  for (const size_t i : IndexRange(pixels_num * 4)) {
    pixels[i] = 0.0f;
  }
}

/**
 * Copy \a source's texels into \a pixels, premultiplied: the colour times the alpha it covers by,
 * clamped to [0, 1] -- the weight the Drop Shadow's kernel clamps its own layer alpha to, and the
 * range the composite and the graph read the map's coverage at. This is the one form change
 * between the render and the buffer contract a #PAINT_LAYER_KIND_RESULT_HAS_ALPHA kind answers
 * for: the bake machinery's convention is a straight map (the dumped colour in rgb, the tree's
 * `Alpha` output in the fourth channel, see
 * #paint_layers_shader_bake_backend_material_build), while the effect buffer the answer lands in
 * is premultiplied -- "premultiplied colour and the alpha it covers by", the way every kernel
 * with the flag leaves it.
 */
static void backend_pixels_copy_premultiplied(float *pixels,
                                             const float *source,
                                             const size_t texels_num)
{
  for (const size_t i : IndexRange(texels_num)) {
    const float alpha = std::clamp(source[i * 4 + 3], 0.0f, 1.0f);
    pixels[i * 4 + 0] = source[i * 4 + 0] * alpha;
    pixels[i * 4 + 1] = source[i * 4 + 1] * alpha;
    pixels[i * 4 + 2] = source[i * 4 + 2] * alpha;
    pixels[i * 4 + 3] = alpha;
  }
}

static bool shader_bake_backend_render(const PaintLayerShaderBakeRequest &request,
                                       const MutableSpan<PaintLayerShaderBakeOutput> outputs)
{
  Main *bmain = BKE_paint_layers_shader_bake_main();
  if (bmain == nullptr || request.tree == nullptr || request.width <= 0 || request.height <= 0 ||
      outputs.is_empty())
  {
    return false;
  }
  /* Main thread only: the landing writes the pixels into the images on the thread the bake ends
   * on, which the composite and the sweep are on. A bake worker reaches the same answer through
   * the effect cache (see the file comment). */
  if (!BLI_thread_is_main()) {
    return false;
  }
  /* The bake is square: the target's side is one number (#MaterialBakeToImagesParams::size), and
   * the current stack maps are square (#paint_layers_image_size) -- a non-square ask has no canvas
   * the bake could land on, so refuse it rather than stretch the answer into a wrong-sized map. */
  if (request.width != request.height) {
    return false;
  }

  Image *input_image = nullptr;
  if (request.input_pixels != nullptr) {
    input_image = backend_input_image_build(
        *bmain, request.width, request.height, request.input_pixels);
    if (input_image == nullptr) {
      return false;
    }
  }
  /* The channels the outputs name, in the order they came: the wiring and the targets both read
   * them. The outputs carry the ramp context a generator ask lays its tails from; the build reads
   * them from the span directly. */
  Vector<int> channels;
  for (const PaintLayerShaderBakeOutput &output : outputs) {
    channels.append(output.channel);
  }
  Material *material = paint_layers_shader_bake_backend_material_build(
      *bmain, request, input_image, outputs.as_span().cast<const PaintLayerShaderBakeOutput>());
  if (material == nullptr) {
    if (input_image != nullptr) {
      BKE_id_free(bmain, input_image);
    }
    return false;
  }

  /* Every answer is the caller's buffer by way of a temporary target image the bake lands into;
   * the pixels are copied out and the images freed, so the caller owns nothing but the spans.
   * One bake answers all of them -- the render routes each linked channel into its own AOV, the
   * same single pass the source cache's multi-channel bake runs. */
  Vector<Image *> targets;
  Vector<ed::material_bake::BakeTargetSpec> targets_spec;
  const size_t pixels_num = size_t(request.width) * size_t(request.height);
  for (const int channel : channels) {
    const eMaterialPaintChannel bake_channel = (channel < 0) ? PAINT_MATERIAL_CHANNEL_BASE_COLOR :
                                                               eMaterialPaintChannel(channel);
    Image *target = ed::material_bake::bake_target_image_create(*bmain,
                                                               *material,
                                                               bake_channel,
                                                               request.width,
                                                               "",
                                                               0,
                                                               ed::material_bake::BakeTargetKind::
                                                                   Row,
                                                               /*tiled=*/false);
    targets.append(target);
    if (target != nullptr) {
      targets_spec.append(ed::material_bake::BakeTargetSpec{bake_channel, target});
    }
  }

  bool rendered = false;
  if (!targets_spec.is_empty()) {
    ed::material_bake::MaterialBakeToImagesParams params;
    params.material = material;
    params.targets = targets_spec;
    params.size = request.width;
    /* Run to completion here: the caller is the composite's effect cache or the sweep, both of
     * which can afford one EEVEE pass; a job of its own would only move the stall. */
    params.blocking = true;
    const ed::material_bake::MaterialBakeToImagesResult result =
        ed::material_bake::material_bake_to_images(
        *bmain, nullptr, nullptr, params);
    rendered = result.ok;
  }
  for (const int i : outputs.index_range()) {
    Image *target = targets[i];
    if (!rendered || target == nullptr) {
      /* Unanswered is transparent, never another channel's answer: a per-channel source that a
       * channel the Principled cannot carry named lays nothing there. */
      if (outputs[i].pixels != nullptr) {
        backend_pixels_fill_transparent(outputs[i].pixels, pixels_num);
      }
    }
    else if (outputs[i].pixels == nullptr) {
      /* A caller naming no buffer asks for nothing into it; the target still goes away below. */
    }
    else {
      void *lock = nullptr;
      ImBuf *buffer = BKE_image_acquire_ibuf(target, nullptr, &lock);
      if (buffer != nullptr && buffer->float_buffer.data != nullptr) {
        const size_t to_copy = std::min(pixels_num, size_t(buffer->x) * size_t(buffer->y));
        if (request.result_has_alpha) {
          /* The ask said the result is a layer with an alpha of its own: the tree's `Alpha`
           * output rode the dump's fourth channel (straight, the bake machinery's own form), and
           * the premultiply here is what makes the answer the contract's -- the kernel copies it
           * over the effect buffer whole, and every reader of the kind flag mixes it as it mixes
           * a Drop Shadow's. A mask item's ask carries no flag and lands on the memcpy below. */
          backend_pixels_copy_premultiplied(outputs[i].pixels, buffer->float_buffer.data, to_copy);
        }
        else {
          memcpy(outputs[i].pixels, buffer->float_buffer.data, sizeof(float) * to_copy * 4);
        }
        if (to_copy < pixels_num) {
          backend_pixels_fill_transparent(outputs[i].pixels + to_copy * 4, pixels_num - to_copy);
        }
      }
      else if (outputs[i].pixels != nullptr) {
        backend_pixels_fill_transparent(outputs[i].pixels, pixels_num);
      }
      BKE_image_release_ibuf(target, buffer, lock);
    }
    if (target != nullptr) {
      BKE_id_free(bmain, target);
    }
  }

  BKE_id_free(bmain, material);
  if (input_image != nullptr) {
    BKE_id_free(bmain, input_image);
  }
  return rendered;
}

/** \} */

}  // namespace blender::ed::render

namespace blender::ed::material_bake {

/* -------------------------------------------------------------------- */
/** \name The sweep
 * \{ */

void paint_layers_shader_bake_rows_ensure(Main &bmain, Material &ma)
{
  if (BKE_paint_layers_shader_bake_main() == nullptr) {
    /* No backend: nothing to compute, the rows serve their stale answer. */
    return;
  }
  const int size = ma.paint_layers_image_size;
  const PaintLayerTileContext tile{.painted_object = nullptr,
                                   .tile_number = PAINT_TILE_NUMBER_BASE};
  /* One due-check-and-render per tree-reading row, shared by the stack rows and the elements laid
   * over them: the walk covers the corrections and the mask items, which the stack walk alone
   * never visits, and which describe from the same bake maps -- a node-group correction or a
   * node-group generator mask item lays its map only if a render lands in it. */
  const auto row_ensure = [&](MaterialPaintLayer &row) {
    /* The tree-reading rows are what the sweep serves: a node-group source (a stack row, or the
     * same kind laid as a correction), and the node-group generator whose CPU answer is the bake
     * (the composite reads the bake's map, the same way a source row's is read). A filter's
     * kernel renders through the effect cache on its own schedule and takes no sweep here. */
    const bool node_group_row =
        row.source == MA_PAINT_LAYER_SOURCE_NODE_GROUP ||
        (row.source == MA_PAINT_LAYER_SOURCE_GENERATOR &&
         row.kind_subtype == MA_PAINT_LAYER_GENERATOR_NODE_GROUP);
    if (!node_group_row || row.kind_tree == nullptr ||
        (row.flag & MA_PAINT_LAYER_ENABLED) == 0)
    {
      return;
    }
    if (!BKE_paint_layers_shader_bake_due(ma, row, tile, size, size)) {
      return;
    }
    /* One render per row, blocking: a sweep that finds several due rows renders them one by one,
     * and the wanted policy of the caller keeps an unseen material from paying for any. */
    BKE_paint_layers_shader_bake_compute(bmain, ma, row, tile, size, size);
  };
  BKE_paint_layers_foreach(ma, [&](MaterialPaintLayer &row, MaterialPaintLayer * /*parent*/) {
    row_ensure(row);
    for (MaterialPaintLayer &effect : row.effects) {
      row_ensure(effect);
    }
    for (MaterialPaintLayer &item : row.mask_stack) {
      row_ensure(item);
    }
    return true;
  });
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name The registration
 * \{ */

void ED_paint_layers_shader_bake_backend_register()
{
  PaintLayerShaderBakeHooks hooks;
  hooks.render = &blender::ed::render::shader_bake_backend_render;
  hooks.main = G_MAIN;
  BKE_paint_layers_shader_bake_hooks_set(hooks);
}

/** \} */

}  // namespace blender::ed::material_bake
