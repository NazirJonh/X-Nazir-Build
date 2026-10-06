/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "paint_layers_generate_subgroup.hh"

#include "paint_layers_generate_intern.hh"
#include "paint_layers_generate_layout.hh"

#include "BKE_lib_id.hh"
#include "BKE_node.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_update.hh"

#include "BLI_string.h"
#include "BLI_ustring.hh"

#include "DNA_ID.h"
#include "DNA_node_types.h"

namespace blender::bke::paint_layers {

void subgroup_refresh(SubGroup &sub)
{
  /* Why only inner: the instance lives in the parent row group, which this refresh cannot see,
   * so it only redeclares the Group Input/Output; the caller redeclares the instance in its own
   * tree once the interface batch is complete. */
  if (sub.tree == nullptr) {
    return;
  }
  if (sub.group_input != nullptr) {
    nodes::update_node_declaration_and_sockets(*sub.tree, *sub.group_input);
  }
  if (sub.group_output != nullptr) {
    nodes::update_node_declaration_and_sockets(*sub.tree, *sub.group_output);
  }
}

static void subgroup_refresh_with_parent(SubGroup &sub, bNodeTree &parent_tree)
{
  /* Why separate: the instance lives in the parent, so its declaration update needs the parent
   * tree, which the plain refresh cannot see. */
  if (sub.tree != nullptr && sub.group_input != nullptr) {
    nodes::update_node_declaration_and_sockets(*sub.tree, *sub.group_input);
  }
  if (sub.tree != nullptr && sub.group_output != nullptr) {
    nodes::update_node_declaration_and_sockets(*sub.tree, *sub.group_output);
  }
  if (sub.instance != nullptr) {
    nodes::update_node_declaration_and_sockets(parent_tree, *sub.instance);
  }
}

bNodeTreeInterfaceSocket *subgroup_add_input(SubGroup &sub,
                                             const char *base,
                                             const StringRef socket_type)
{
  /* Why unique: two mask items may share their DNA name, so a second identical base must not
   * claim the first item's socket. */
  if (sub.tree == nullptr) {
    return nullptr;
  }
  char name[256];
  interface_name_unique(sub.tree->tree_interface, base, name, sizeof(name));
  bNodeTreeInterfaceSocket *socket = sub.tree->tree_interface.add_socket(
      name, "", socket_type, NODE_INTERFACE_SOCKET_INPUT, nullptr);
  /* Why only inner: the parent instance is redeclared by the caller once the whole interface
   * stands, so adding one socket at a time never leaves it half-built. */
  subgroup_refresh(sub);
  return socket;
}

bNodeTreeInterfaceSocket *subgroup_add_output(SubGroup &sub,
                                              const char *base,
                                              const StringRef socket_type)
{
  if (sub.tree == nullptr) {
    return nullptr;
  }
  /* Why no unique name: a subgroup has a single output and an output may share its name with an
   * input, so the Factor output keeps its plain name instead of "Factor 2". */
  bNodeTreeInterfaceSocket *socket = sub.tree->tree_interface.add_socket(
      base, "", socket_type, NODE_INTERFACE_SOCKET_OUTPUT, nullptr);
  subgroup_refresh(sub);
  return socket;
}

SubGroup subgroup_ensure(const PaintLayersBuildContext &ctx,
                         const MaterialPaintLayer &row,
                         const char *kind,
                         bNodeTree &parent_tree,
                         const int channel)
{
  SubGroup sub;
  /* Why only Mask: Content and Source packing belong to a later task, so any other kind answers
   * empty instead of building half a group. */
  if (kind == nullptr || !STREQ(kind, TREE_SUBKIND_MASK)) {
    return sub;
  }
  if (parent_tree.typeinfo == nullptr || parent_tree.typeinfo->group_idname == nullptr) {
    return sub;
  }
  const bUUID owner = tree_owner_uid_get(parent_tree);
  /* Why per channel: the row's stack is one subgroup tree instantiated once per channel it takes
   * part in, so the parent may already hold several instances of the same tree. The exact channel
   * is found by `custom1`; any other instance of the same subgroup proves the tree was already
   * built in this pass. */
  bNodeTree *existing_tree = nullptr;
  for (bNode &node : parent_tree.nodes) {
    if (!node.is_group() || node.id == nullptr || GS(node.id->name) != ID_NT) {
      continue;
    }
    bNodeTree *group = id_cast<bNodeTree *>(node.id);
    if (group == nullptr) {
      continue;
    }
    if (!BLI_uuid_equal(tree_owner_uid_get(*group), owner)) {
      continue;
    }
    bUUID marker = BLI_uuid_nil();
    if (!layer_tree_marker_get(*group, marker) || !BLI_uuid_equal(marker, row.marker)) {
      continue;
    }
    const char *subkind = prop_string_get(group->id.properties, TREE_SUBKIND_PROP);
    if (subkind == nullptr || !STREQ(subkind, TREE_SUBKIND_MASK)) {
      continue;
    }
    if (existing_tree == nullptr) {
      existing_tree = group;
    }
    if (int(node.custom1) != channel) {
      continue;
    }
    /* This exact channel is already instantiated: reuse its instance as is and do not rebuild the
     * shared interface or nodes. */
    sub.tree = group;
    sub.instance = &node;
    for (bNode &inner : group->nodes) {
      if (inner.is_group_input()) {
        sub.group_input = &inner;
      }
      else if (inner.is_group_output()) {
        sub.group_output = &inner;
      }
    }
    sub.built = false;
    return sub;
  }
  if (existing_tree != nullptr) {
    /* The tree is already built in this build: add a second instance for this channel and let the
     * caller bind only its sockets, never the interface or the nodes again. */
    sub.tree = existing_tree;
    for (bNode &inner : existing_tree->nodes) {
      if (inner.is_group_input()) {
        sub.group_input = &inner;
      }
      else if (inner.is_group_output()) {
        sub.group_output = &inner;
      }
    }
    bNode *instance = bke::node_add_node(
        nullptr, parent_tree, parent_tree.typeinfo->group_idname);
    if (instance != nullptr) {
      instance->id = &existing_tree->id;
      id_us_plus(&existing_tree->id);
      /* Why custom1: a group node ignores it, so it is free to carry the channel number; the
       * label is only a human hint and must never be read by code. */
      instance->custom1 = int16_t(channel);
      char label[32];
      SNPRINTF(label, "Mask ch%d", channel);
      STRNCPY_UTF8(instance->label, label);
      /* Why Mask column by channel: every channel's instance owns its own row, so the four
       * instances of a four-channel row never pile up on the tree origin. */
      layout::place(*instance, layout::Column::Mask, 0);
      sub.instance = instance;
    }
    if (sub.instance != nullptr) {
      nodes::update_node_declaration_and_sockets(parent_tree, *sub.instance);
    }
    sub.built = false;
    return sub;
  }
  /* The factory hands out a tree already cleared and tagged with the row's owner, marker and
   * subkind; this module only wires the instance and the Group Input/Output. */
  bNodeTree *tree = ctx.subgroup_tree_get ? ctx.subgroup_tree_get(row, kind) : nullptr;
  if (tree == nullptr) {
    return sub;
  }
  sub.tree = tree;
  for (bNode &node : sub.tree->nodes) {
    if (node.is_group_input()) {
      sub.group_input = &node;
    }
    else if (node.is_group_output()) {
      sub.group_output = &node;
    }
  }
  if (sub.group_input == nullptr) {
    sub.group_input = bke::node_add_node(nullptr, *sub.tree, "NodeGroupInput"_ustr);
  }
  if (sub.group_output == nullptr) {
    sub.group_output = bke::node_add_node(nullptr, *sub.tree, "NodeGroupOutput"_ustr);
  }
  bNode *instance = bke::node_add_node(
      nullptr, parent_tree, parent_tree.typeinfo->group_idname);
  if (instance != nullptr) {
    instance->id = &sub.tree->id;
    id_us_plus(&sub.tree->id);
    instance->custom1 = int16_t(channel);
    char label[32];
    SNPRINTF(label, "Mask ch%d", channel);
    STRNCPY_UTF8(instance->label, label);
    /* Why Mask column by channel: every channel's instance owns its own row, so the four
     * instances of a four-channel row never pile up on the tree origin. */
    layout::place(*instance, layout::Column::Mask, 0);
    sub.instance = instance;
  }
  subgroup_refresh_with_parent(sub, parent_tree);
  /* Why true: the interface is empty (the factory cleared the tree), so the caller fills it. */
  sub.built = true;
  return sub;
}

}  // namespace blender::bke::paint_layers
