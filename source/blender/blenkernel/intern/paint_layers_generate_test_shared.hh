/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** Shared file-static helpers for the split generate-test TUs (plan 9.3).
 *
 * Moved verbatim from `paint_layers_generate_test.cc`: `static` keeps one copy per
 * TU, so there is no ODR risk. The anonymous block keeps its linkage as before.
 */

#include "intern/paint_layers_generate_test_fixture.hh"

namespace blender::bke::tests {


/** Give \a image the data colorspace, so a correction map is read as colour data. */
static void make_generate_image_data(Image &image)
{
  BLI_strncpy(image.colorspace_settings.name,
              IMB_colormanagement_role_colorspace_name_get(COLOR_ROLE_DATA),
              sizeof(image.colorspace_settings.name));
}

namespace {

/** The `pbr_paint_layers_role` of output \a name in \a group, or null. */
const char *output_role(bNodeTree &group, const char *name)
{
  group.ensure_interface_cache();
  for (bNodeTreeInterfaceSocket *socket : group.interface_outputs()) {
    if (socket->name == nullptr || !STREQ(socket->name, name) || socket->properties == nullptr) {
      continue;
    }
    const IDProperty *role = IDP_GetPropertyTypeFromGroup(
        socket->properties, "pbr_custom_role", IDP_STRING);
    if (role != nullptr) {
      return IDP_string_get(role);
    }
  }
  return nullptr;
}

/** The output interface identifiers of \a group, sorted; equal sets mean a preserved interface. */
Vector<std::string> output_identifiers(bNodeTree &group)
{
  group.ensure_interface_cache();
  Vector<std::string> ids;
  for (bNodeTreeInterfaceSocket *socket : group.interface_outputs()) {
    if (socket->identifier != nullptr) {
      ids.append(std::string(socket->identifier));
    }
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

/** The wrapper tree named `.PL Source <name>`, or null. */
bNodeTree *source_wrapper_find(Main &bmain, const char *source_name)
{
  char full[128];
  BLI_snprintf(full, sizeof(full), ".PL Source %s", source_name);
  for (bNodeTree &tree : bmain.nodetrees) {
    if (STREQ(tree.id.name + 2, full)) {
      return &tree;
    }
  }
  return nullptr;
}

/**
 * A source whose Principled lives inside a nested group and whose Base Color is a Noise graph, so
 * the mode is SourceGroup but the wrapper factory refuses it (PrincipledInGroup).
 */
bNodeTreeInterfaceSocket *tree_interface_output_find(bNodeTree &tree, const char *name)
{
  tree.ensure_interface_cache();
  for (bNodeTreeInterfaceSocket *socket : tree.interface_outputs()) {
    if (socket->name != nullptr && STREQ(socket->name, name)) {
      return socket;
    }
  }
  return nullptr;
}

/** A group tree holding a Principled (Base Color from Noise) feeding a "Surface" output. */
bNodeTree *make_principled_group(Main &bmain, const char *tree_name)
{
  bNodeTree *group = bke::node_tree_add_tree(&bmain, tree_name, "ShaderNodeTree");
  group->tree_interface.add_socket(
      "Surface", "", "NodeSocketShader", NODE_INTERFACE_SOCKET_OUTPUT, nullptr);
  bNode *principled = bke::node_add_static_node(nullptr, *group, SH_NODE_BSDF_PRINCIPLED);
  bNode *noise = bke::node_add_static_node(nullptr, *group, SH_NODE_TEX_NOISE);
  bNode *output = bke::node_add_node(nullptr, *group, "NodeGroupOutput"_ustr);
  bke::node_add_link(*group,
                     *noise,
                     *bke::node_find_socket(*noise, SOCK_OUT, "Fac"_ustr),
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr));
  BKE_ntree_update_tag_all(group);
  BKE_ntree_update_after_single_tree_change(bmain, *group);
  bNodeTreeInterfaceSocket *surface = tree_interface_output_find(*group, "Surface");
  BLI_assert(surface != nullptr);
  bke::node_add_link(
      *group,
      *principled,
      *bke::node_find_socket(*principled, SOCK_OUT, "BSDF"_ustr),
      *output,
      *bke::node_find_socket(*output, SOCK_IN, UString::from_ptr_noinline(surface->identifier)));
  return group;
}

/** Wrap \a inner in a new group that passes its "Surface" through. */
bNodeTree *wrap_principled_group(Main &bmain, const char *tree_name, bNodeTree &inner)
{
  bNodeTree *outer = bke::node_tree_add_tree(&bmain, tree_name, "ShaderNodeTree");
  outer->tree_interface.add_socket(
      "Surface", "", "NodeSocketShader", NODE_INTERFACE_SOCKET_OUTPUT, nullptr);
  bNode *instance = bke::node_add_node(nullptr, *outer, inner.typeinfo->group_idname);
  instance->id = &inner.id;
  id_us_plus(&inner.id);
  nodes::update_node_declaration_and_sockets(*outer, *instance);
  bNode *output = bke::node_add_node(nullptr, *outer, "NodeGroupOutput"_ustr);
  BKE_ntree_update_tag_all(outer);
  BKE_ntree_update_after_single_tree_change(bmain, *outer);
  bNodeTreeInterfaceSocket *outer_surface = tree_interface_output_find(*outer, "Surface");
  bNodeTreeInterfaceSocket *inner_surface = tree_interface_output_find(inner, "Surface");
  BLI_assert(outer_surface != nullptr && inner_surface != nullptr);
  bke::node_add_link(
      *outer,
      *instance,
      *bke::node_find_socket(
          *instance, SOCK_OUT, UString::from_ptr_noinline(inner_surface->identifier)),
      *output,
      *bke::node_find_socket(
          *output, SOCK_IN, UString::from_ptr_noinline(outer_surface->identifier)));
  return outer;
}

/** A source whose Principled is \a depth groups deep, with a Noise on Base Color. */
Material *make_nested_principled_source_depth(Main &bmain, const char *name, const int depth)
{
  bNodeTree *group = make_principled_group(bmain, "NestedSourceBase");
  for (int i = 1; i < depth; i++) {
    char tree_name[64];
    BLI_snprintf(tree_name, sizeof(tree_name), "NestedSourceLevel%d", i);
    group = wrap_principled_group(bmain, tree_name, *group);
  }
  Material *source = BKE_material_add(&bmain, name);
  bNode *instance = bke::node_add_node(nullptr, *source->nodetree, group->typeinfo->group_idname);
  instance->id = &group->id;
  id_us_plus(&group->id);
  nodes::update_node_declaration_and_sockets(*source->nodetree, *instance);
  bNode *output_node = bke::node_add_static_node(nullptr, *source->nodetree, SH_NODE_OUTPUT_MATERIAL);
  BKE_ntree_update_tag_all(source->nodetree);
  BKE_ntree_update_after_single_tree_change(bmain, *source->nodetree);
  bNodeTreeInterfaceSocket *surface = tree_interface_output_find(*group, "Surface");
  BLI_assert(surface != nullptr);
  bke::node_add_link(
      *source->nodetree,
      *instance,
      *bke::node_find_socket(
          *instance, SOCK_OUT, UString::from_ptr_noinline(surface->identifier)),
      *output_node,
      *bke::node_find_socket(*output_node, SOCK_IN, "Surface"_ustr));
  return source;
}

Material *make_nested_principled_source(Main &bmain, const char *name)
{
  return make_nested_principled_source_depth(bmain, name, 1);
}

/** The output interface names of \a tree, sorted; equal sets mean a preserved interface. */
Vector<std::string> output_names(bNodeTree &tree)
{
  tree.ensure_interface_cache();
  Vector<std::string> names;
  for (bNodeTreeInterfaceSocket *socket : tree.interface_outputs()) {
    if (socket->name != nullptr) {
      names.append(std::string(socket->name));
    }
  }
  std::sort(names.begin(), names.end());
  return names;
}

/** How many source wrapper or path-copy trees live in \a bmain. */
int source_wrapper_tree_count(Main &bmain)
{
  int count = 0;
  for (bNodeTree &tree : bmain.nodetrees) {
    if (StringRef(tree.id.name + 2).startswith(".PL Source ")) {
      count++;
    }
  }
  return count;
}

/** Every source wrapper or path-copy tree in \a bmain. */
Vector<bNodeTree *> source_wrapper_trees(Main &bmain)
{
  Vector<bNodeTree *> trees;
  for (bNodeTree &tree : bmain.nodetrees) {
    if (StringRef(tree.id.name + 2).startswith(".PL Source ")) {
      trees.append(&tree);
    }
  }
  return trees;
}

/** Whether every node of \a tree whose parent is set has that parent inside the same tree. */
bool parents_are_local(const bNodeTree &tree)
{
  for (const bNode &node : tree.nodes) {
    if (node.parent == nullptr) {
      continue;
    }
    bool found = false;
    for (const bNode &candidate : tree.nodes) {
      if (&candidate == node.parent) {
        found = true;
        break;
      }
    }
    if (!found) {
      return false;
    }
  }
  return true;
}

/** Give \a source's Base Color a Noise graph, so its row goes to SourceGroup mode. */
void source_set_noise_base_color(Main &bmain, Material &source)
{
  bNode *principled = nullptr;
  for (bNode &node : source.nodetree->nodes) {
    if (node.type_legacy == SH_NODE_BSDF_PRINCIPLED) {
      principled = &node;
      break;
    }
  }
  BLI_assert(principled != nullptr);
  bNode *noise = bke::node_add_static_node(nullptr, *source.nodetree, SH_NODE_TEX_NOISE);
  bke::node_add_link(*source.nodetree,
                     *noise,
                     *bke::node_find_socket(*noise, SOCK_OUT, "Fac"_ustr),
                     *principled,
                     *bke::node_find_socket(*principled, SOCK_IN, "Base Color"_ustr));
  BKE_ntree_update_tag_all(source.nodetree);
  BKE_ntree_update_after_single_tree_change(bmain, *source.nodetree);
}

/** The node of \a tree named \a name, or null; a copied wrapper preserves node names. */
bNode *node_by_name(bNodeTree &tree, const StringRefNull name)
{
  for (bNode &node : tree.nodes) {
    if (node.name == name) {
      return &node;
    }
  }
  return nullptr;
}

/** The node feeding the Vector input of \a tex, or null when nothing is linked. */
bNode *vector_source(bNode &tex)
{
  bNodeSocket *vector = bke::node_find_socket(tex, SOCK_IN, "Vector"_ustr);
  if (vector == nullptr) {
    return nullptr;
  }
  const Span<const bNodeLink *> links = vector->directly_linked_links();
  if (links.is_empty()) {
    return nullptr;
  }
  return links[0]->fromnode;
}

/** The UV layer name a UV Map node reads, or "". */
const char *uv_node_layer(const bNode &node)
{
  const NodeShaderUVMap *storage = static_cast<const NodeShaderUVMap *>(node.storage);
  return (storage != nullptr) ? storage->uv_map : "";
}

/* Stage 2 mapping helpers: the wrapper's row mapping lives on shared trees, so the tests read
 * links from the trees themselves (the per-socket cache can be stale mid-build). */
bNodeSocket *sock_in(bNode &node, const char *name)
{
  return bke::node_find_socket(node, SOCK_IN, UString::from_ptr_noinline(name));
}

bNodeSocket *sock_out(bNode &node, const char *name)
{
  return bke::node_find_socket(node, SOCK_OUT, UString::from_ptr_noinline(name));
}

/** A group node's sockets carry the interface's own identifiers, so they are found by name. */
bNodeSocket *sock_in_named(bNode &node, const char *name)
{
  for (bNodeSocket &socket : node.inputs) {
    if (STREQ(socket.name, name)) {
      return &socket;
    }
  }
  return nullptr;
}

bNodeSocket *sock_out_named(bNode &node, const char *name)
{
  for (bNodeSocket &socket : node.outputs) {
    if (STREQ(socket.name, name)) {
      return &socket;
    }
  }
  return nullptr;
}

/** The links leaving \a node's \a output socket, read from the tree. */
Vector<const bNodeLink *> outgoing_links(bNodeTree &tree, const bNode &node, const char *output)
{
  Vector<const bNodeLink *> out;
  bNodeSocket *socket = sock_out(const_cast<bNode &>(node), output);
  if (socket == nullptr) {
    return out;
  }
  for (const bNodeLink &link : tree.links) {
    if (link.fromnode == &node && link.fromsock == socket) {
      out.append(&link);
    }
  }
  return out;
}

/** The wrapper interface input whose role marks it, or null. */
bNodeTreeInterfaceSocket *wrapper_mapping_input(bNodeTree &tree, const char *role)
{
  tree.ensure_interface_cache();
  for (bNodeTreeInterfaceSocket *socket : tree.interface_inputs()) {
    const IDProperty *marker = socket->properties != nullptr ?
                                   IDP_GetPropertyTypeFromGroup(
                                       socket->properties, bke::paint_layers::INPUT_ROLE_PROP, IDP_STRING) :
                                   nullptr;
    if (marker != nullptr && STREQ(IDP_string_get(marker), role)) {
      return socket;
    }
  }
  return nullptr;
}

/** The one wrapper \a bmain holds for \a source_name, or null. */
bNodeTree *find_source_wrapper(Main &bmain, const char *source_name)
{
  char name[MAX_ID_NAME - 2];
  SNPRINTF(name, ".PL Source %s", source_name);
  for (bNodeTree &tree : bmain.nodetrees) {
    if (STREQ(tree.id.name + 2, name)) {
      return &tree;
    }
  }
  return nullptr;
}

/** The node of \a group instantiating \a wrapper, or null. */
bNode *group_instance_of(bNodeTree &group, const bNodeTree &wrapper)
{
  for (bNode &node : group.nodes) {
    if (node.is_group() && node.id == &const_cast<bNodeTree &>(wrapper).id) {
      return &node;
    }
  }
  return nullptr;
}

/** The instance socket of the wrapper input \a name (".PL Mapping Offset"/Scale/Rotation) names,
 * found through the wrapper interface role: the identifier is the wrapper's own. */
bNodeSocket *instance_mapping_socket(bNode &instance, const char *name)
{
  if (instance.id == nullptr) {
    return nullptr;
  }
  const StringRef suffix = StringRef(name).drop_prefix(StringRef(".PL Mapping ").size());
  const char *role = suffix == "Offset" ? bke::paint_layers::SOURCE_GROUP_ROLE_MAPPING_OFFSET :
                     suffix == "Scale"  ? bke::paint_layers::SOURCE_GROUP_ROLE_MAPPING_SCALE :
                                          bke::paint_layers::SOURCE_GROUP_ROLE_MAPPING_ROTATION;
  const bNodeTreeInterfaceSocket *iface = wrapper_mapping_input(*id_cast<bNodeTree *>(instance.id),
                                                                role);
  if (iface == nullptr) {
    return nullptr;
  }
  return bke::node_find_socket(instance, SOCK_IN, UString::from_ptr_noinline(iface->identifier));
}

/** Whether the wrapper instance's mapping input \a name is fed by the row group's Group Input. */
bool instance_mapping_input_wired(bNodeTree &group, bNode &instance, const char *name)
{
  bNodeSocket *dst = instance_mapping_socket(instance, name);
  if (dst == nullptr) {
    return false;
  }
  bool wired_from_group_input = false;
  for (const bNodeLink &link : group.links) {
    if (link.tosock == dst) {
      if (link.fromnode->is_group_input()) {
        wired_from_group_input = true;
      }
      else {
        /* Something else feeds it: the wiring must never do that. */
        return false;
      }
    }
  }
  return wired_from_group_input;
}

}  // namespace


/**
 * Build a source shaped like a real material: the Principled inside a group whose interface names
 * the channels the wrapper routes, a second group off the path to it that the wrapper's copy also
 * instances (the "shared" group), a Frame the group instance is parented to, and a Reroute.
 *
 * \param r_shared receives the off-path group, \a r_on_path the group holding the Principled.
 */
static Material *make_hash_source(Main &bmain,
                                  bNodeTree **r_shared,
                                  bNodeTree **r_on_path)
{
  bNodeTree *on_path = make_principled_group(bmain, "HashInner");
  on_path->tree_interface.add_socket(
      "Roughness", "", "NodeSocketFloat", NODE_INTERFACE_SOCKET_INPUT, nullptr);
  on_path->tree_interface.add_socket(
      "Specular", "", "NodeSocketFloat", NODE_INTERFACE_SOCKET_INPUT, nullptr);
  BKE_ntree_update_tag_all(on_path);
  BKE_ntree_update_after_single_tree_change(bmain, *on_path);

  bNodeTree *shared = bke::node_tree_add_tree(&bmain, "HashShared", "ShaderNodeTree");
  shared->tree_interface.add_socket(
      "SharedOut", "", "NodeSocketFloat", NODE_INTERFACE_SOCKET_OUTPUT, nullptr);
  bNode *shared_value = bke::node_add_static_node(nullptr, *shared, SH_NODE_VALUE);
  BKE_ntree_update_tag_all(shared);
  BKE_ntree_update_after_single_tree_change(bmain, *shared);
  bNode *shared_output = bke::node_add_node(nullptr, *shared, "NodeGroupOutput"_ustr);
  bNodeTreeInterfaceSocket *shared_out = tree_interface_output_find(*shared, "SharedOut");
  bke::node_add_link(*shared,
                     *shared_value,
                     *bke::node_find_socket(*shared_value, SOCK_OUT, "Value"_ustr),
                     *shared_output,
                     *bke::node_find_socket(
                         *shared_output, SOCK_IN, UString::from_ptr_noinline(shared_out->identifier)));

  Material *source = BKE_material_add(&bmain, "HashSource");
  bNode *on_path_instance = bke::node_add_node(
      nullptr, *source->nodetree, on_path->typeinfo->group_idname);
  on_path_instance->id = &on_path->id;
  id_us_plus(&on_path->id);
  nodes::update_node_declaration_and_sockets(*source->nodetree, *on_path_instance);
  bNode *shared_instance = bke::node_add_node(
      nullptr, *source->nodetree, shared->typeinfo->group_idname);
  shared_instance->id = &shared->id;
  id_us_plus(&shared->id);
  nodes::update_node_declaration_and_sockets(*source->nodetree, *shared_instance);
  bNode *output_node = bke::node_add_static_node(
      nullptr, *source->nodetree, SH_NODE_OUTPUT_MATERIAL);
  BKE_ntree_update_tag_all(source->nodetree);
  BKE_ntree_update_after_single_tree_change(bmain, *source->nodetree);
  bNodeTreeInterfaceSocket *surface = tree_interface_output_find(*on_path, "Surface");
  bke::node_add_link(*source->nodetree,
                     *on_path_instance,
                     *bke::node_find_socket(
                         *on_path_instance, SOCK_OUT, UString::from_ptr_noinline(surface->identifier)),
                     *output_node,
                     *bke::node_find_socket(*output_node, SOCK_IN, "Surface"_ustr));

  /* A Frame and a Reroute, and a value node parented to the frame: none of the parent/location/
   * selection state may enter the hash. */
  bNode *frame = bke::node_add_static_node(nullptr, *source->nodetree, NODE_FRAME);
  bNode *value = bke::node_add_static_node(nullptr, *source->nodetree, SH_NODE_VALUE);
  value->parent = frame;
  bNode *reroute = bke::node_add_static_node(nullptr, *source->nodetree, NODE_REROUTE);
  bke::node_add_link(*source->nodetree,
                     *value,
                     *bke::node_find_socket(*value, SOCK_OUT, "Value"_ustr),
                     *reroute,
                     *bke::node_find_socket(*reroute, SOCK_IN, "Input"_ustr));
  BKE_ntree_update_tag_all(source->nodetree);
  BKE_ntree_update_after_single_tree_change(bmain, *source->nodetree);

  *r_shared = shared;
  *r_on_path = on_path;
  return source;
}


/** Group \a member into a fresh folder that takes its slot, as the Outliner's Group does. */
static MaterialPaintLayer *group_one(Material &ma, MaterialPaintLayer *member)
{
  MaterialPaintLayer *members[1] = {member};
  return BKE_paint_layers_group(ma, Span<MaterialPaintLayer *>(members, 1));
}


/**
 * What EEVEE compiles from a group: node types, the operation/blend a node carries and the wiring
 * between node kinds -- never a value, a name or a data-block choice. Two groups with the same
 * signature generate the same shader code, which is the acceptance rule of the warm slots.
 */
static uint64_t code_shape_signature(const bNodeTree &tree)
{
  const auto mix = [](uint64_t h, const uint64_t v) { return (h ^ v) * 1099511628211ull; };
  /* Why here: a nested `.PL Mask` group is reached only through its instance, so no regenerate
   * step has built its topology cache, and the socket/link accessors below read it. */
  const_cast<bNodeTree &>(tree).ensure_topology_cache();
  Vector<uint64_t> keys;
  for (const bNode &node : tree.nodes) {
    uint64_t key = 1469598103934665603ull;
    key = mix(key, uint64_t(node.type_legacy));
    key = mix(key, uint64_t(uint16_t(node.custom1)));
    key = mix(key, uint64_t(uint16_t(node.custom2)));
    if (node.is_group()) {
      /* The interface order of an instance is not part of the code: count, do not sequence. */
      int linked = 0;
      int unlinked = 0;
      for (const bNodeSocket *socket : node.input_sockets()) {
        (socket->is_directly_linked() ? linked : unlinked)++;
      }
      key = mix(key, uint64_t(linked));
      key = mix(key, uint64_t(unlinked));
    }
    else {
      for (const bNodeSocket *socket : node.input_sockets()) {
        key = mix(key, socket->is_directly_linked() ? 1 : 2);
      }
    }
    if (node.type_legacy == SH_NODE_TEX_IMAGE && node.id != nullptr && GS(node.id->name) == ID_IM) {
      /* How the map is decoded changes the code: a data map is straightened, a colour one is not. */
      const Image &image = *reinterpret_cast<const Image *>(node.id);
      key = mix(key, IMB_colormanagement_space_name_is_data(image.colorspace_settings.name) ? 3 : 4);
      key = mix(key, uint64_t(image.alpha_mode));
      key = mix(key, (image.flag & IMA_GPU_LINEAR_PREMUL) ? 5 : 6);
    }
    keys.append(key);
  }
  for (const bNodeLink *link : tree.all_links()) {
    uint64_t key = 7919;
    key = mix(key, uint64_t(link->fromnode->type_legacy));
    /* A Group Input socket is a uniform the parent fills in: which slot of the interface it sits in
     * changes no generated code, and a real item's sockets legitimately land after the spare's. */
    key = mix(key,
              link->fromnode->is_group_input() ? uint64_t(0) : uint64_t(link->fromsock->index()));
    key = mix(key, uint64_t(link->tonode->type_legacy));
    /* Likewise a group instance's input slot: the shader inlines the group, so the interface order
     * of its uniforms is not part of the code. */
    key = mix(key, link->tonode->is_group() ? uint64_t(0) : uint64_t(link->tosock->index()));
    keys.append(key);
  }
  std::sort(keys.begin(), keys.end());
  uint64_t signature = 1469598103934665603ull;
  for (const uint64_t key : keys) {
    signature = mix(signature, key);
  }
  return signature;
}


/**
 * #code_shape_signature of \a tree combined with that of every group it instances, recursively and
 * without regard to the instance order or names: the shape of the code the shader inlines.
 */
static uint64_t code_shape_deep(const bNodeTree &tree, const int depth = 0)
{
  Vector<uint64_t> parts;
  parts.append(code_shape_signature(tree));
  if (depth < 8) {
    for (const bNode &node : tree.nodes) {
      if (node.is_group() && node.id != nullptr && GS(node.id->name) == ID_NT &&
          !BKE_paint_material_is_normal_combine_group(node))
      {
        parts.append(code_shape_deep(*reinterpret_cast<const bNodeTree *>(node.id), depth + 1));
      }
    }
  }
  std::sort(parts.begin() + 1, parts.end());
  uint64_t signature = 1469598103934665603ull;
  for (const uint64_t part : parts) {
    signature = (signature ^ part) * 1099511628211ull;
  }
  return signature;
}


/** A map like the ones the UI creates for a mask: Non-Color data, straight alpha, linear premul. */
static void make_generate_image_mask_like(Image &image)
{
  make_generate_image_data(image);
  image.alpha_mode = IMA_ALPHA_STRAIGHT;
  image.flag |= IMA_GPU_LINEAR_PREMUL;
}


/** An Image Texture node reading \a image with the given sampling, linked by the caller. */
static bNode *add_tex_image_node(bNodeTree &tree,
                          Image &image,
                          const int interpolation,
                          const int projection)
{
  bNode *node = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
  if (node == nullptr) {
    return nullptr;
  }
  node->id = &image.id;
  id_us_plus(&image.id);
  NodeTexImage *storage = static_cast<NodeTexImage *>(node->storage);
  storage->extension = SHD_IMAGE_EXTENSION_REPEAT;
  storage->interpolation = interpolation;
  storage->projection = projection;
  return node;
}

static bNodeSocket *out_socket(bNode &node, const char *name)
{
  return bke::node_find_socket(node, SOCK_OUT, UString::from_ptr_noinline(name));
}

static bNodeSocket *in_socket(bNode &node, const char *name)
{
  return bke::node_find_socket(node, SOCK_IN, UString::from_ptr_noinline(name));
}

/** Three distinct image samplers feeding a Principled's Base Color: a live-worthy source. */
static void source_set_three_image_base_color(Material &source,
                                       Image &a,
                                       Image &b,
                                       Image &c)
{
  bNodeTree &tree = *source.nodetree;
  bNode *principled = nullptr;
  for (bNode &node : tree.nodes) {
    if (node.type_legacy == SH_NODE_BSDF_PRINCIPLED) {
      principled = &node;
      break;
    }
  }
  bNodeSocket *base = in_socket(*principled, "Base Color");
  bNode *na = add_tex_image_node(tree, a, SHD_INTERP_LINEAR, SHD_PROJ_BOX);
  bNode *nb = add_tex_image_node(tree, b, SHD_INTERP_LINEAR, SHD_PROJ_BOX);
  bNode *nc = add_tex_image_node(tree, c, SHD_INTERP_LINEAR, SHD_PROJ_BOX);
  bke::node_add_link(tree, *na, *out_socket(*na, "Color"), *principled, *base);
  bke::node_add_link(tree, *nb, *out_socket(*nb, "Color"), *na, *in_socket(*na, "Vector"));
  bke::node_add_link(tree, *nc, *out_socket(*nc, "Color"), *nb, *in_socket(*nb, "Vector"));
}

}  // namespace blender::bke::tests
