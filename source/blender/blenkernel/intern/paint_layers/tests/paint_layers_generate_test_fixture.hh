/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * Fixture of the Paint Layers generator tests: a layered material on a fresh #Main, plus the
 * helpers that add rows backed by maps and look the generated groups up again.
 */

#include "testing/testing.h"

#include "BKE_global.hh"
#include "BKE_gtest_base.hh"
#include "BKE_idprop.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_generate.hh"
#include "BKE_paint_material_composite.hh"
#include "BKE_paint_material_resolve.hh"

#include "../paint_layers_generate_intern.hh"
#include "../../paint_material_composite_internal.hh"

#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_string_ref.hh"
#include "BLI_ustring.hh"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_scene_types.h"

#include <algorithm>
#include <string>

namespace blender::bke::tests {

class PaintLayersGenerateTest : public bke::BlenderGTestBase {
 public:
  Main *bmain = nullptr;
  Material *ma = nullptr;

  void SetUp() override
  {
    /* The runtime blend tier is file-static state: a record from an earlier test must not decide
     * whether this test's first blend edit is a tier transition. */
    BKE_paint_layers_blend_tier_clear();
    bmain = BKE_main_new();
    G_MAIN = bmain;
    ma = BKE_material_add(bmain, "Layered");
    ma->paint_layers_flag |= MA_PAINT_LAYERED;
  }

  void TearDown() override
  {
    BKE_paint_layers_blend_tier_clear();
    BKE_main_free(bmain);
    G_MAIN = nullptr;
  }

  Image *add_image(const char *name)
  {
    const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    return BKE_image_add_generated(
        bmain, 8, 8, name, 32, false, IMA_GENTYPE_BLANK, color, false, false, false);
  }

  /** A row on top of the stack with a map on its enabled \a channel. */
  MaterialPaintLayer *add_paint_layer(const char *name,
                                      Image *image,
                                      const eMaterialPaintChannel channel =
                                          PAINT_MATERIAL_CHANNEL_BASE_COLOR)
  {
    MaterialPaintLayer *layer = BKE_paint_layers_add(*ma, name, nullptr, PaintLayerPlace::Above);
    EXPECT_NE(layer, nullptr);
    add_channel(*layer, channel, image);
    return layer;
  }

  /** An enabled \a channel of \a layer backed by \a image. */
  void add_channel(MaterialPaintLayer &layer, const eMaterialPaintChannel channel, Image *image)
  {
    EXPECT_NE(BKE_paint_layers_channel_add(*ma, &layer, channel), nullptr);
    EXPECT_TRUE(BKE_paint_layers_channel_set_image(*ma, &layer, channel, image));
  }

  /** An enabled \a channel of \a layer without a map and with the flat \a value. */
  void add_constant_channel(MaterialPaintLayer &layer,
                            const eMaterialPaintChannel channel,
                            const float value[4])
  {
    EXPECT_NE(BKE_paint_layers_channel_add(*ma, &layer, channel), nullptr);
    EXPECT_TRUE(BKE_paint_layers_channel_set_value(*ma, &layer, channel, value));
  }

  bNodeTree *make_tree(const char *name)
  {
    return bke::node_tree_add_tree(bmain, name, "ShaderNodeTree");
  }

  /** The layer-tree factory the build requires: an empty tree per row on the fixture's #Main. */
  bNodeTree *layer_tree_for(const MaterialPaintLayer &layer)
  {
    char name[64];
    BLI_snprintf(
        name, sizeof(name), ".PL Layer %s", layer.name[0] != '\0' ? layer.name : "Layer");
    return make_tree(name);
  }

  /** The pure build of the fixture's material into \a tree, with the fixture's factory. */
  void build_into(bNodeTree &tree, bNodeTree *normal_combine_group = nullptr)
  {
    PaintLayersBuildContext ctx;
    ctx.normal_combine_group = normal_combine_group;
    auto factory = [this](const MaterialPaintLayer &layer) { return layer_tree_for(layer); };
    ctx.layer_tree_get = factory;
    BKE_paint_layers_tree_build(*ma, tree, ctx);
  }

  /** The `.PL Layer <name>` group in \a bmain, or null. */
  static bNodeTree *layer_tree_find(Main &bmain, const char *layer_name)
  {
    char full[96];
    BLI_snprintf(full, sizeof(full), ".PL Layer %s", layer_name);
    for (bNodeTree &tree : bmain.nodetrees) {
      if (STREQ(tree.id.name + 2, full)) {
        return &tree;
      }
    }
    return nullptr;
  }

  static int layer_tree_count(Main &bmain)
  {
    int count = 0;
    for (bNodeTree &tree : bmain.nodetrees) {
      if (StringRef(tree.id.name + 2).startswith(".PL Layer ")) {
        count++;
      }
    }
    return count;
  }

  /** The instance of \a group directly in \a parent, or null. */
  static bNode *group_instance_find(bNodeTree &parent, bNodeTree &group)
  {
    for (bNode &node : parent.nodes) {
      if (node.is_group() && node.id == &group.id) {
        return &node;
      }
    }
    return nullptr;
  }

  static bool interface_has_socket(bNodeTree &tree, const char *name, const bool output)
  {
    tree.ensure_interface_cache();
    const Span<bNodeTreeInterfaceSocket *> sockets = output ? tree.interface_outputs() :
                                                              tree.interface_inputs();
    for (const bNodeTreeInterfaceSocket *socket : sockets) {
      if (socket->name != nullptr && STREQ(socket->name, name)) {
        return true;
      }
    }
    return false;
  }

  /** The interface socket named \a name of \a tree's inputs, or null. */
  static const bNodeTreeInterfaceSocket *interface_input_find(bNodeTree &tree, const char *name)
  {
    tree.ensure_interface_cache();
    for (const bNodeTreeInterfaceSocket *socket : tree.interface_inputs()) {
      if (socket->name != nullptr && STREQ(socket->name, name)) {
        return socket;
      }
    }
    return nullptr;
  }

  /** Count nodes of \a type, descending into nested layer groups so the shape is seen whole. */
  static int count_type(const bNodeTree &tree, const int type)
  {
    int count = 0;
    for (const bNode &node : tree.nodes) {
      if (node.type_legacy == type) {
        count++;
      }
      if (node.is_group() && node.id != nullptr && GS(node.id->name) == ID_NT &&
          !BKE_paint_material_is_normal_combine_group(node))
      {
        count += count_type(*reinterpret_cast<const bNodeTree *>(node.id), type);
      }
    }
    return count;
  }

  /** The Principled input \a socket_name of \a ma's Principled BSDF, or null. */
  bNodeSocket *principled_input(const char *socket_name)
  {
    ChannelUnavailableReason reason = ChannelUnavailableReason::None;
    const bNode *principled = BKE_paint_material_principled_find(*ma, reason);
    if (principled == nullptr) {
      return nullptr;
    }
    return bke::node_find_socket(
        const_cast<bNode &>(*principled), SOCK_IN, UString::from_ptr_noinline(socket_name));
  }

  /** The instance of the material's generated group in its embedded tree, or null. */
  bNode *instance_find()
  {
    if (ma->nodetree == nullptr || ma->paint_layers_tree == nullptr) {
      return nullptr;
    }
    return group_instance_find(*ma->nodetree, *ma->paint_layers_tree);
  }
};

}  // namespace blender::bke::tests
