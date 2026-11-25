/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The template trees the node-group kinds start from: a shader node tree per kind with the
 * convention's sockets (`BKE_paint_layers_node_group.hh`) and the Group Input/Output nodes the
 * user builds inside, so a new row never lands on an empty graph. The Add operator creates one for
 * every row it makes; a row that reaches a node-group kind by any other path (a script, the RNA
 * slot) keeps no definition until one is assigned, and reads as definition-less meanwhile.
 *
 * The templates are authored here with the other BKE tree builders (the generated source group,
 * `paint_layers_generate_source_group.cc`) rather than in the operator: the kind switch seeds them
 * from BKE, which has no editor to ask. A template is a standalone tree in the main database,
 * shared by every instance that reads it.
 */

#include <iterator>

#include "BLI_span.hh"
#include "BLI_string.h"
#include "BLI_ustring.hh"
#include "BLI_utildefines.h"

#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_node.hh"
#include "BKE_node_tree_interface.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers_node_group.hh"

#include "DNA_node_tree_interface_types.h"
#include "DNA_node_types.h"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name The template builder
 * \{ */

/** One interface socket of the template, as the convention spells it. */
struct TemplateSocket {
  const char *name;
  const char *description;
  const char *socket_type;
  eNodeSocketInOut in_out;
  float default_float = 0.0f;
};

/**
 * A shader tree with the convention's sockets and the Group Input/Output nodes the user starts
 * from. The sockets are created in the order they are given, which is the order the row's dynamic
 * schema reads them in.
 */
static bNodeTree *node_group_template_tree_create(Main &bmain,
                                                  const char *name,
                                                  const Span<const TemplateSocket> sockets)
{
  bNodeTree *tree = bke::node_tree_add_tree(&bmain, name, "ShaderNodeTree");
  if (tree == nullptr) {
    return nullptr;
  }
  bke::node_add_node(nullptr, *tree, "NodeGroupInput"_ustr);
  bke::node_add_node(nullptr, *tree, "NodeGroupOutput"_ustr);
  for (const TemplateSocket &socket : sockets) {
    bNodeTreeInterfaceSocket *iface = tree->tree_interface.add_socket(
        socket.name,
        socket.description,
        socket.socket_type,
        socket.in_out == SOCK_IN ? NODE_INTERFACE_SOCKET_INPUT : NODE_INTERFACE_SOCKET_OUTPUT,
        nullptr);
    if (iface == nullptr) {
      /* A half-built tree would answer a shape the convention does not have: it leaves instead of
       * being assigned to the row. */
      BKE_id_free(&bmain, &tree->id);
      return nullptr;
    }
    if (socket.in_out == SOCK_IN && STREQ(socket.socket_type, "NodeSocketFloat")) {
      if (bNodeSocketValueFloat *value = static_cast<bNodeSocketValueFloat *>(
              iface->socket_data))
      {
        value->value = socket.default_float;
      }
    }
  }
  return tree;
}

/** \} */

bNodeTree *BKE_paint_layers_node_group_template_ensure(Main &bmain,
                                                       const PaintLayerNodeGroupTemplateKind kind,
                                                       const char *name)
{
  char tree_name[MAX_NAME];
  if (name == nullptr || name[0] == '\0') {
    STRNCPY(tree_name, "Node Group");
  }
  else {
    STRNCPY(tree_name, name);
  }
  switch (kind) {
    case PaintLayerNodeGroupTemplateKind::Filter: {
      static const TemplateSocket sockets[] = {
          {PAINT_NG_SOCKET_INPUT,
           "The colour the filter works on: the layer's channel content, or the mask's grey",
           "NodeSocketColor",
           SOCK_IN},
          {PAINT_NG_SOCKET_UV,
           "The UV the layer reads at; without it the group reads the texture coordinates itself",
           "NodeSocketVector",
           SOCK_IN},
          {"Strength",
           "A parameter: every input that is not one of the reserved names is one",
           "NodeSocketFloat",
           SOCK_IN,
           1.0f},
          {PAINT_NG_SOCKET_RESULT, "The colour the filter lays", "NodeSocketColor", SOCK_OUT},
      };
      bNodeTree *tree = node_group_template_tree_create(
          bmain, tree_name, Span<const TemplateSocket>(sockets, std::size(sockets)));
      /* The template is the convention's own shape: failing the validator would be a bug in this
       * file, so a debug build stops here, and a release build keeps the tree -- whose assignment
       * the validator refuses anyway. */
      BLI_assert(tree == nullptr || BKE_paint_layers_node_group_supports_filter(tree));
      return tree;
    }
    case PaintLayerNodeGroupTemplateKind::SourceLayer: {
      /* No `Result` beside the named channel on purpose: a tree that names any paint channel is a
       * per-channel source, where `Result` is never read -- a template carrying both would invite
       * wiring the one the layer ignores. A single-answer tree (a `Result` and no channel names)
       * is the convention's other half; renaming this output to `Result` gets there. */
      static const TemplateSocket sockets[] = {
          {PAINT_NG_SOCKET_UV,
           "The UV the layer reads at; without it the group reads the texture coordinates itself",
           "NodeSocketVector",
           SOCK_IN},
          {"Base Color",
           "A paint-channel output: name an output after a paint channel to fill that channel",
           "NodeSocketColor",
           SOCK_OUT},
      };
      bNodeTree *tree = node_group_template_tree_create(
          bmain, tree_name, Span<const TemplateSocket>(sockets, std::size(sockets)));
      BLI_assert(tree == nullptr || BKE_paint_layers_node_group_supports_source(tree));
      return tree;
    }
    case PaintLayerNodeGroupTemplateKind::Generator: {
      static const TemplateSocket sockets[] = {
          {PAINT_NG_SOCKET_COORDINATE,
           "The placed coordinate the placement chain feeds (space, scale, rotation, offsets)",
           "NodeSocketVector",
           SOCK_IN},
          {"Detail",
           "A parameter: every input that is not one of the reserved names is one",
           "NodeSocketFloat",
           SOCK_IN,
           2.0f},
          {PAINT_NG_SOCKET_FAC, "The pattern value the ramp indexes", "NodeSocketFloat", SOCK_OUT},
          {PAINT_NG_SOCKET_COLOR,
           "The colour the generator lays where no ramp could express the answer",
           "NodeSocketColor",
           SOCK_OUT},
      };
      bNodeTree *tree = node_group_template_tree_create(
          bmain, tree_name, Span<const TemplateSocket>(sockets, std::size(sockets)));
      BLI_assert(tree == nullptr || BKE_paint_layers_node_group_supports_generator(tree));
      return tree;
    }
  }
  /* A code outside the enum names no template: the caller's own picker never offers one. */
  BLI_assert_unreachable();
  return nullptr;
}

/* -------------------------------------------------------------------- */
/** \name The source channels
 * \{ */

bool BKE_paint_layers_node_group_source_channels(const bNodeTree *tree,
                                                 MutableSpan<bool> r_channels)
{
  r_channels.fill(false);
  if (tree == nullptr) {
    return false;
  }
  bool named = false;
  tree->tree_interface.root_panel.foreach_item(
      [&](const bNodeTreeInterfaceItem &item) {
        if (item.item_type != NodeTreeInterfaceItemType::Socket) {
          return true;
        }
        const bNodeTreeInterfaceSocket &socket = reinterpret_cast<const bNodeTreeInterfaceSocket &>(item);
        if ((socket.flag & NODE_INTERFACE_SOCKET_OUTPUT) == 0) {
          return true;
        }
        for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
          /* A channel without a Principled socket to feed (a Height, an AO, a Custom) is never
           * answered: the bake prints what the Principled carries, so naming it would promise a
           * map no render lands in. */
          if (StringRef(socket.name) == info.ui_name && info.socket_name != nullptr) {
            if (info.channel < int(r_channels.size())) {
              r_channels[int(info.channel)] = true;
              named = true;
            }
            break;
          }
        }
        return true;
      },
      /*include_self=*/false);
  return named;
}

/** \} */

}  // namespace blender
