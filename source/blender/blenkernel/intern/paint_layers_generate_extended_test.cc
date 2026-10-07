/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Paint Layers generator tests: mesh maps, snapshots, mapping rows and mask layout (plan 9.3).
 *
 * Fixture and shared helpers live in
 * `paint_layers_generate_test_fixture.hh` /
 * `paint_layers_generate_test_shared.hh`; this TU holds only its own tests.
 */

#include "intern/paint_layers_generate_test_fixture.hh"
#include "intern/paint_layers_generate_test_shared.hh"

namespace blender::bke::tests {

TEST_F(PaintLayersGenerateTest, mesh_map_row_builds_no_nodes_or_samplers)
{
  /* The row's map type has no atlas assigned yet (no slot image): the row reads nothing, so it
   * contributes nothing -- no Image Texture node, no sampler, no wire from the row (spec M2
   * item 4). With an atlas the row builds a chain; that is covered by the atlas tests. */
  const int samplers_before = BKE_paint_layers_sampler_count(*ma);
  MaterialPaintLayer *mesh_map = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MESH_MAP, "AO", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(mesh_map, nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  EXPECT_EQ(count_type(*ma->paint_layers_tree, SH_NODE_TEX_IMAGE), 0);
  EXPECT_EQ(BKE_paint_layers_sampler_count(*ma), samplers_before);
  EXPECT_EQ(interface_output_find("Result Base Color"), nullptr);
}

TEST_F(PaintLayersGenerateTest, mesh_map_type_moves_the_topology_hash)
{
  MaterialPaintLayer *mesh_map = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MESH_MAP, "AO", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(mesh_map, nullptr);
  const uint64_t hash_ao = paint_layers_layer_topology_hash(*ma, *mesh_map, Span<int>());

  ASSERT_TRUE(BKE_paint_layers_mesh_map_type_set(*ma, mesh_map, MA_MESH_MAP_EDGE));
  const uint64_t hash_edge = paint_layers_layer_topology_hash(*ma, *mesh_map, Span<int>());
  EXPECT_NE(hash_ao, hash_edge);
}

namespace {

/** The layer's float Non-Color atlas of \a size, blank content. */
Image *add_atlas_image(Main &bmain, const char *name, const int size)
{
  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  return BKE_image_add_generated(
      &bmain, size, size, name, 32, /*floatbuf=*/true, IMA_GENTYPE_BLANK, black, false, true, false);
}

/** Every TEX_IMAGE node of \a tree whose #Image is \a image, descending into layer groups. */
void collect_tex_images_of_image(const bNodeTree &tree, const Image *image, Vector<bNode *> &r_nodes)
{
  for (const bNode &node : tree.nodes) {
    if (node.type_legacy == SH_NODE_TEX_IMAGE && node.id == &image->id) {
      r_nodes.append(const_cast<bNode *>(&node));
    }
    if (node.is_group() && node.id != nullptr && GS(node.id->name) == ID_NT &&
        !BKE_paint_material_is_normal_combine_group(node))
    {
      collect_tex_images_of_image(*reinterpret_cast<const bNodeTree *>(node.id), image, r_nodes);
    }
  }
}

}  // namespace

/** Guard: the atlas chain is built by the MESH_MAP support in #paint_layers_tree_build (the
 * early-return removal) reading the shared resolver in paint_layers_intern.hh; revert either and
 * this finds no Extend Image Texture for the atlas. */
TEST_F(PaintLayersGenerateTest, mesh_map_row_atlas_builds_an_extend_tex_image)
{
  Image *atlas = add_atlas_image(*bmain, "Atlas", 8);
  ASSERT_NE(atlas, nullptr);

  MaterialPaintLayer *mesh_map = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MESH_MAP, "AO", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(mesh_map, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, mesh_map, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            nullptr);
  ASSERT_TRUE(BKE_mesh_maps_slot_image_set(*ma, MA_MESH_MAP_AO, atlas));

  const int samplers_before = BKE_paint_layers_sampler_count(*ma);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  /* The row paints the atlas: one Image Texture node carries it, sampled Linear + Extend, and the
   * row takes its sampler. */
  EXPECT_EQ(count_type(*ma->paint_layers_tree, SH_NODE_TEX_IMAGE), 1);
  Vector<bNode *> tex_images;
  collect_tex_images_of_image(*ma->paint_layers_tree, atlas, tex_images);
  ASSERT_EQ(tex_images.size(), 1);
  const NodeTexImage *storage = static_cast<const NodeTexImage *>(tex_images.first()->storage);
  ASSERT_NE(storage, nullptr);
  EXPECT_EQ(storage->extension, SHD_IMAGE_EXTENSION_EXTEND);
  EXPECT_EQ(storage->interpolation, SHD_INTERP_LINEAR);
  EXPECT_EQ(BKE_paint_layers_sampler_count(*ma), samplers_before + 1);
  EXPECT_NE(interface_output_find("Result Base Color"), nullptr);
}

/** Guard: the corrections' MESH_MAP support in #paint_layers_tree_build (content map node and the
 * mask item's Separate-X grey); revert it and neither element reads the atlas. */
TEST_F(PaintLayersGenerateTest, mesh_map_mask_and_effect_atlas_build_tex_images)
{
  Image *atlas = add_atlas_image(*bmain, "Atlas", 8);
  ASSERT_NE(atlas, nullptr);
  MaterialPaintLayer *owner = add_paint_layer("Paint", add_image("Paint"));

  MaterialPaintLayer *effect = BKE_paint_layers_correction_add(
      *ma, owner, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_MESH_MAP, "AO Effect");
  ASSERT_NE(effect, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, effect, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
  MaterialPaintLayer *mask_item = BKE_paint_layers_correction_add(
      *ma, owner, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_MESH_MAP, "AO Mask");
  ASSERT_NE(mask_item, nullptr);
  ASSERT_TRUE(BKE_mesh_maps_slot_image_set(*ma, MA_MESH_MAP_AO, atlas));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  /* The owner's Paint map plus one atlas read for the content correction and one for the mask
   * item; the mask reads its R through a Separate XYZ, so an extra one sits in the tree. */
  /* +warm: the real atlas corrections do not fit the warm slots, so both warm maps stay (5). */
  EXPECT_EQ(count_type(*ma->paint_layers_tree, SH_NODE_TEX_IMAGE), 5);
  Vector<bNode *> tex_images;
  collect_tex_images_of_image(*ma->paint_layers_tree, atlas, tex_images);
  ASSERT_EQ(tex_images.size(), 2);
  for (const bNode *tex : tex_images) {
    const NodeTexImage *storage = static_cast<const NodeTexImage *>(tex->storage);
    ASSERT_NE(storage, nullptr);
    EXPECT_EQ(storage->extension, SHD_IMAGE_EXTENSION_EXTEND);
  }
}

/** Guard: the atlas folds into the topology hash by Image identity (the material-aware
 * #topology_hash_layer / #topology_hash_correction) and into the sampler counter by Image; revert
 * either and a pixel edit or a slot re-point is seen wrong. */
TEST_F(PaintLayersGenerateTest, mesh_map_atlas_pixels_do_not_move_the_topology_hash)
{
  Image *atlas = add_atlas_image(*bmain, "Atlas", 8);
  ASSERT_NE(atlas, nullptr);
  MaterialPaintLayer *mesh_map = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MESH_MAP, "AO", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(mesh_map, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, mesh_map, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            nullptr);
  ASSERT_TRUE(BKE_mesh_maps_slot_image_set(*ma, MA_MESH_MAP_AO, atlas));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  const int wired[] = {PAINT_MATERIAL_CHANNEL_BASE_COLOR};
  const uint64_t hash_before = paint_layers_layer_topology_hash(
      *ma, *mesh_map, Span<int>(wired, 1));

  /* Repainting the atlas keeps the Image (its session UID) and so the tree: the hash stands. */
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(atlas, nullptr, &lock);
  ASSERT_NE(ibuf, nullptr);
  ASSERT_NE(ibuf->float_data(), nullptr);
  float *pixels = ibuf->float_data_for_write();
  pixels[0] = 0.75f;
  pixels[1] = 0.25f;
  BKE_image_release_ibuf(atlas, ibuf, lock);
  EXPECT_EQ(paint_layers_layer_topology_hash(*ma, *mesh_map, Span<int>(wired, 1)), hash_before);

  /* Pointing the slot at another Image is a structural edit: the hash moves. */
  Image *other = add_atlas_image(*bmain, "Other", 8);
  ASSERT_NE(other, nullptr);
  ASSERT_TRUE(BKE_mesh_maps_slot_image_set(*ma, MA_MESH_MAP_AO, other));
  EXPECT_NE(paint_layers_layer_topology_hash(*ma, *mesh_map, Span<int>(wired, 1)), hash_before);
}

/** Guard: the sampler counter deduplicates by Image (the material-aware enumeration in
 * #BKE_paint_layers_regenerate); revert it and a second row on the same atlas takes a sampler. */
TEST_F(PaintLayersGenerateTest, two_mesh_map_rows_share_one_atlas_sampler)
{
  Image *atlas = add_atlas_image(*bmain, "Atlas", 8);
  ASSERT_NE(atlas, nullptr);
  for (const int i : IndexRange(2)) {
    char name[32];
    BLI_snprintf(name, sizeof(name), "AO %d", i);
    MaterialPaintLayer *mesh_map = BKE_paint_layers_add(
        *ma, MA_PAINT_LAYER_SOURCE_MESH_MAP, name, nullptr, PaintLayerPlace::Above);
    ASSERT_NE(mesh_map, nullptr);
    ASSERT_NE(BKE_paint_layers_channel_add(*ma, mesh_map, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
              nullptr);
  }
  ASSERT_TRUE(BKE_mesh_maps_slot_image_set(*ma, MA_MESH_MAP_AO, atlas));

  const int samplers_before = BKE_paint_layers_sampler_count(*ma);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  /* Both rows read the same atlas Image, so the dedup by Image leaves one sampler. */
  EXPECT_EQ(count_type(*ma->paint_layers_tree, SH_NODE_TEX_IMAGE), 2);
  EXPECT_EQ(BKE_paint_layers_sampler_count(*ma), samplers_before + 1);
}

/** Guard: the structural resolver (`paint_layer_mesh_map_image` without the loaded-buffer check);
 * restore the check and an assigned-but-unloaded atlas loses its node, its hash mix and its
 * sampler. */
TEST_F(PaintLayersGenerateTest, mesh_map_atlas_without_a_loaded_buffer_still_builds)
{
  Image *atlas = add_atlas_image(*bmain, "Atlas", 8);
  ASSERT_NE(atlas, nullptr);
  MaterialPaintLayer *mesh_map = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MESH_MAP, "AO", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(mesh_map, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, mesh_map, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            nullptr);
  ASSERT_TRUE(BKE_mesh_maps_slot_image_set(*ma, MA_MESH_MAP_AO, atlas));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  /* The assigned, loaded atlas: one Image Texture node, one sampler. */
  EXPECT_EQ(count_type(*ma->paint_layers_tree, SH_NODE_TEX_IMAGE), 1);
  const int samplers_loaded = BKE_paint_layers_sampler_count(*ma);
  const int wired[] = {PAINT_MATERIAL_CHANNEL_BASE_COLOR};
  const uint64_t hash_loaded = paint_layers_layer_topology_hash(
      *ma, *mesh_map, Span<int>(wired, 1));

  /* Unload the buffer: the assignment (DNA) is untouched, so nothing structural may move. */
  BKE_image_free_buffers(atlas);
  ASSERT_FALSE(BKE_image_has_loaded_ibuf(atlas));

  EXPECT_EQ(paint_layers_layer_topology_hash(*ma, *mesh_map, Span<int>(wired, 1)), hash_loaded);
  EXPECT_EQ(BKE_paint_layers_sampler_count(*ma), samplers_loaded);

  /* A regenerate after the unload still builds the row's node, not an empty tree. */
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  Vector<bNode *> tex_images;
  collect_tex_images_of_image(*ma->paint_layers_tree, atlas, tex_images);
  EXPECT_EQ(tex_images.size(), 1);
}

/**
 * Phase 4: a Stack (content) correction's topology hash follows its children's structure, exactly
 * like #topology_hash_layer follows a Layer folder's own children -- adding a second child, or
 * changing a child's source, moves the owner row's hash; editing a child's *value* (opacity, a Fill
 * constant) does not, mirroring Spec-26 for every other row kind.
 */
TEST_F(PaintLayersGenerateTest, stack_effect_correction_hash_follows_subtree_structure)
{
  MaterialPaintLayer *owner = add_paint_layer("Owner", add_image("Owner"));
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, owner, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_STACK, "StackFX");
  ASSERT_NE(correction, nullptr);

  MaterialPaintLayer *child = add_paint_layer_into(correction, "Child", add_image("Child"));

  const int wired[] = {PAINT_MATERIAL_CHANNEL_BASE_COLOR};
  const uint64_t hash_before = paint_layers_layer_topology_hash(*ma, *owner, Span<int>(wired, 1));

  /* A value edit inside the subtree (opacity) must not move the owner's hash. */
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, child, 0.3f));
  EXPECT_EQ(paint_layers_layer_topology_hash(*ma, *owner, Span<int>(wired, 1)), hash_before);

  /* A structural edit inside the subtree -- a second child -- must move it. */
  add_paint_layer_into(correction, "Second", add_image("Second"));
  EXPECT_NE(paint_layers_layer_topology_hash(*ma, *owner, Span<int>(wired, 1)), hash_before);
}

/** Phase 4: a TEX_IMAGE inside a Stack correction's subtree is reachable from the material output
 * exactly like any other, so it is counted by the same generic, topology-only sampler walk
 * (#SamplerCounter) with no special-casing needed for a correction's own children. */
TEST_F(PaintLayersGenerateTest, stack_effect_correction_child_image_counts_as_a_sampler)
{
  MaterialPaintLayer *owner = add_paint_layer("Owner", add_image("Owner"));
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, owner, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_STACK, "StackFX");
  ASSERT_NE(correction, nullptr);

  const int samplers_before = BKE_paint_layers_sampler_count(*ma);
  add_paint_layer_into(correction, "Child", add_image("Child"));

  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_EQ(report.sampler_estimate, BKE_paint_layers_sampler_count(*ma));

  /* The owner's own map plus the correction child's map: two samplers over the baseline (zero
   * rows). #SamplerCounter follows the tree from its output nodes regardless of which row's
   * subtree a TEX_IMAGE node was built for. */
  /* +warm: and the two shared warm images of the owner. */
  EXPECT_EQ(BKE_paint_layers_sampler_count(*ma), samplers_before + 4);
}

/* -------------------------------------------------------------------- */
/** \name C3 refactor guard: deterministic generated-tree snapshots
 *
 * These tests lock the exact generated graph (root + every reachable group tree) so the C3
 * decomposition of #paint_layers_tree_build cannot silently reorder node/link/socket creation,
 * which would move socket identifiers and the stored topology/root hashes even while the
 * behaviour-only tests stay green. The serialization is order-based and pointer-free; the only
 * non-deterministic input, the random UUID markers stamped on value sockets, is normalized to the
 * order each marker first appears (u0, u1, ...).
 * \{ */

namespace {

const IDProperty *snapshot_iprop(const IDProperty *props, const char *key, const int type)
{
  if (props == nullptr) {
    return nullptr;
  }
  return IDP_GetPropertyTypeFromGroup(props, key, type);
}

void snapshot_uuid(std::string &out,
                   const char *uuid_text,
                   Map<std::string, std::string> &seen,
                   int &next)
{
  if (uuid_text == nullptr || uuid_text[0] == '\0') {
    out += "-";
    return;
  }
  const std::string key(uuid_text);
  if (const std::string *found = seen.lookup_ptr(key)) {
    out += *found;
    return;
  }
  const std::string norm = "u" + std::to_string(next++);
  seen.add(key, norm);
  out += norm;
}

void snapshot_interface(std::string &out,
                        bNodeTree &tree,
                        Map<std::string, std::string> &markers,
                        int &next_marker)
{
  tree.ensure_interface_cache();
  tree.tree_interface.foreach_item([&](bNodeTreeInterfaceItem &item) {
    if (item.item_type != NodeTreeInterfaceItemType::Socket) {
      return true;
    }
    bNodeTreeInterfaceSocket &socket = reinterpret_cast<bNodeTreeInterfaceSocket &>(item);
    const IDProperty *props = socket.properties;
    const IDProperty *role = snapshot_iprop(props, "pbr_paint_layers_role", IDP_STRING);
    const IDProperty *channel = snapshot_iprop(props, "pbr_paint_layers_channel", IDP_INT);
    const IDProperty *mirror = snapshot_iprop(props, "pbr_paint_layers_mirror", IDP_INT);
    const IDProperty *marker = snapshot_iprop(props, "pbr_paint_layers_layer", IDP_STRING);
    out += "  IFACE ";
    out += ((socket.flag & NODE_INTERFACE_SOCKET_INPUT) != 0) ? "in" : "out";
    /* Why no name/id: the interface name is uniquified ("Factor 2") and the identifier is a
     * creation-order label, so both move when another socket is added; the type, direction and the
     * paint-layer role properties are what the shader and the value sync actually read. */
    out += " type=" + std::string((socket.socket_type != nullptr) ? socket.socket_type : "-");
    out += " role=" + std::string((role != nullptr) ? IDP_string_get(role) : "-");
    out += " channel=" + std::to_string((channel != nullptr) ? IDP_int_get(channel) : -1);
    out += " mirror=" + std::to_string((mirror != nullptr) ? IDP_int_get(mirror) : 0);
    out += " marker=";
    snapshot_uuid(out, (marker != nullptr) ? IDP_string_get(marker) : nullptr, markers, next_marker);
    out += "\n";
    return true;
  });
}

/** A float rounded to the same precision the snapshot prints, so the hash is not at the mercy of
 * the last bits of a float computation. */
std::string snapshot_f(const float value)
{
  char buf[40];
  BLI_snprintf(buf, sizeof(buf), "%.5f", value);
  return buf;
}

/** The default value of an unlinked input socket, by type; "-" for outputs and types not tracked. */
std::string snapshot_socket_default(const bNodeSocket &sock)
{
  switch (sock.type) {
    case SOCK_FLOAT: {
      const auto *v = static_cast<const bNodeSocketValueFloat *>(sock.default_value);
      return snapshot_f(v != nullptr ? v->value : 0.0f);
    }
    case SOCK_INT: {
      const auto *v = static_cast<const bNodeSocketValueInt *>(sock.default_value);
      return std::to_string(v != nullptr ? v->value : 0);
    }
    case SOCK_BOOLEAN: {
      const auto *v = static_cast<const bNodeSocketValueBoolean *>(sock.default_value);
      return std::to_string((v != nullptr && v->value) ? 1 : 0);
    }
    case SOCK_VECTOR: {
      const auto *v = static_cast<const bNodeSocketValueVector *>(sock.default_value);
      return "{" + snapshot_f(v != nullptr ? v->value[0] : 0.0f) + "," +
             snapshot_f(v != nullptr ? v->value[1] : 0.0f) + "," +
             snapshot_f(v != nullptr ? v->value[2] : 0.0f) + "}";
    }
    case SOCK_RGBA: {
      const auto *v = static_cast<const bNodeSocketValueRGBA *>(sock.default_value);
      return "{" + snapshot_f(v != nullptr ? v->value[0] : 0.0f) + "," +
             snapshot_f(v != nullptr ? v->value[1] : 0.0f) + "," +
             snapshot_f(v != nullptr ? v->value[2] : 0.0f) + "," +
             snapshot_f(v != nullptr ? v->value[3] : 0.0f) + "}";
    }
    default:
      return "-";
  }
}

/** The storage fields the generator actually writes, for the node types it creates. */
std::string snapshot_node_storage(const bNode &node)
{
  switch (node.type_legacy) {
    case SH_NODE_MIX: {
      const auto *s = static_cast<const NodeShaderMix *>(node.storage);
      if (s == nullptr) {
        return "mix storage=null";
      }
      return "mix data_type=" + std::to_string(int(s->data_type)) +
             " factor_mode=" + std::to_string(int(s->factor_mode)) +
             " blend_type=" + std::to_string(int(s->blend_type)) +
             " clamp_factor=" + std::to_string(int(s->clamp_factor)) +
             " clamp_result=" + std::to_string(int(s->clamp_result));
    }
    case SH_NODE_TEX_IMAGE: {
      const auto *s = static_cast<const NodeTexImage *>(node.storage);
      if (s == nullptr) {
        return "teximage storage=null";
      }
      return "teximage interpolation=" + std::to_string(s->interpolation) +
             " extension=" + std::to_string(s->extension) +
             " projection=" + std::to_string(s->projection);
    }
    case SH_NODE_UVMAP: {
      const auto *s = static_cast<const NodeShaderUVMap *>(node.storage);
      return std::string("uvmap uv=") + ((s != nullptr) ? s->uv_map : "-");
    }
    /* Math and Vector Math keep their operation/use_clamp in custom1/custom2 (serialized with the
     * params), and the XYZ/Constant/Color nodes the generator makes carry no storage fields of
     * their own; label them so a type change is still visible. */
    case SH_NODE_MATH:
      return "math";
    case SH_NODE_VECTOR_MATH:
      return "vecmath";
    case SH_NODE_SEPXYZ:
      return "sepxyz";
    case SH_NODE_COMBXYZ:
      return "combxyz";
    case SH_NODE_RGB:
      return "rgb";
    case SH_NODE_VALUE:
      return "value";
    case SH_NODE_RGBTOBW:
      return "rgbtobw";
    case SH_NODE_COMPOSE_COLOR_ALPHA:
      return "composealpha";
    default:
      return node.is_group() ? "group" : "-";
  }
}

void snapshot_tree(std::string &out,
                   bNodeTree &tree,
                   Set<const bNodeTree *> &visited,
                   Map<std::string, std::string> &markers,
                   int &next_marker)
{
  if (!visited.add(&tree)) {
    out += "  (repeated tree)\n";
    return;
  }
  out += "TREE " + std::string(tree.id.name + 2) + "\n";
  tree.ensure_topology_cache();

  /* The stored topology/root hashes are deliberately NOT serialized: they fold in each layer's
   * random UUID marker, so they differ every run and would defeat the snapshot. The node/link/
   * socket order and the node parameters below are what actually pin the graph; each scenario also
   * re-regenerates unchanged and checks the snapshot is byte-identical, so a moved hash is caught
   * indirectly through the "root was not rebuilt" check. */

  Map<const bNode *, int> index;
  int i = 0;
  for (bNode &node : tree.nodes) {
    /* Why skip frames: a NODE_FRAME only groups nodes for the editor and carries no shader code,
     * and the shader inlines every nested group. Leaving it out of the index keeps it out of the
     * links too, since a frame never owns a socket. The node's own `name` and `label` are likewise
     * editor cosmetics, so they are not serialized. */
    if (node.type_legacy == NODE_FRAME) {
      continue;
    }
    index.add(&node, i);
    out += "  NODE " + std::to_string(i) + " type=" + std::to_string(node.type_legacy) +
           " idname=" + std::string(node.idname) +
           " id=" + std::string((node.id != nullptr) ? node.id->name + 2 : "-") +
           " custom1=" + std::to_string(node.custom1) +
           " custom2=" + std::to_string(node.custom2) +
           " custom3=" + std::to_string(node.custom3) +
           " custom4=" + std::to_string(node.custom4) +
           " muted=" + std::to_string((node.flag & NODE_MUTED) != 0 ? 1 : 0) + "\n";
    out += "    STORAGE " + snapshot_node_storage(node) + "\n";
    i++;
  }
  for (bNode &node : tree.nodes) {
    if (node.type_legacy == NODE_FRAME) {
      continue;
    }
    int si = 0;
    for (bNodeSocket *sock : node.input_sockets()) {
      /* Why no inline name/id: a socket identifier is a creation-order label and moves when an
       * unrelated node is added, while a socket's position and its unlinked default are what the
       * generated code reads. */
      out += "    IN " + std::to_string(si);
      if (sock->directly_linked_links().is_empty()) {
        out += " default=" + snapshot_socket_default(*sock);
      }
      out += "\n";
      si++;
    }
    si = 0;
    for (bNodeSocket *sock : node.output_sockets()) {
      out += "    OUT " + std::to_string(si) + "\n";
      si++;
    }
  }

  snapshot_interface(out, tree, markers, next_marker);

  for (bNodeLink &link : tree.links) {
    const int fi = index.lookup_default(link.fromnode, -1);
    const int ti = index.lookup_default(link.tonode, -1);
    int fs = -1;
    int ts = -1;
    int k = 0;
    if (link.fromnode != nullptr) {
      for (bNodeSocket *sock : link.fromnode->output_sockets()) {
        if (sock == link.fromsock) {
          fs = k;
          break;
        }
        k++;
      }
    }
    k = 0;
    if (link.tonode != nullptr) {
      for (bNodeSocket *sock : link.tonode->input_sockets()) {
        if (sock == link.tosock) {
          ts = k;
          break;
        }
        k++;
      }
    }
    out += "  LINK " + std::to_string(fi) + ":" + std::to_string(fs) + " -> " +
           std::to_string(ti) + ":" + std::to_string(ts) + "\n";
  }

  /* Depth-first into the group instances, in node order, so a nested tree's serialization sits
   * right after the instance that reaches it. */
  for (bNode &node : tree.nodes) {
    if (!node.is_group() || node.id == nullptr || GS(node.id->name) != ID_NT) {
      continue;
    }
    if (bNodeTree *group = id_cast<bNodeTree *>(node.id)) {
      snapshot_tree(out, *group, visited, markers, next_marker);
    }
  }
}

uint64_t snapshot_hash(const Material &ma)
{
  std::string text;
  if (ma.nodetree != nullptr) {
    Set<const bNodeTree *> visited;
    Map<std::string, std::string> markers;
    int next_marker = 0;
    snapshot_tree(text, *ma.nodetree, visited, markers, next_marker);
  }
  uint64_t h = 1469598103934665603ull;
  for (const unsigned char c : text) {
    h ^= c;
    h *= 1099511628211ull;
  }
  return h;
}

/**
 * Lock \a ma's generated graph: compare its hash to \a expected, then regenerate once more without
 * any description edit and require the root to be kept and the snapshot to be byte-identical. That
 * second half is what replaces a stored-hash assertion: a hash the user cannot see moves exactly
 * when the root topology moves, which this catches.
 */
void snapshot_expect(Material &ma, const uint64_t expected, const char *label)
{
  Main *bmain = G_MAIN;
  ASSERT_NE(bmain, nullptr);
  bNodeTree *const root = ma.paint_layers_tree;
  Vector<bNode *> nodes_before;
  if (root != nullptr) {
    for (bNode &node : root->nodes) {
      nodes_before.append(&node);
    }
  }
  const uint64_t first = snapshot_hash(ma);
  std::printf("SNAPSHOT %s %016llx\n", label, static_cast<unsigned long long>(first));
  EXPECT_EQ(first, expected) << "snapshot " << label;

  /* Unchanged re-regeneration: the root must be kept (its stored hash matched) and nothing may
   * have moved -- node order, links, interface, params and unlinked-input values alike. */
  EXPECT_TRUE(BKE_paint_layers_regenerate(*bmain, ma)) << label;
  EXPECT_EQ(ma.paint_layers_tree, root) << label << " root was rebuilt on an unchanged regenerate";
  bool same_nodes = root != nullptr && ma.paint_layers_tree == root;
  if (same_nodes) {
    int i = 0;
    for (bNode &node : root->nodes) {
      if (i >= int(nodes_before.size()) || nodes_before[i] != &node) {
        same_nodes = false;
        break;
      }
      i++;
    }
    if (i != int(nodes_before.size())) {
      same_nodes = false;
    }
  }
  EXPECT_TRUE(same_nodes) << label << " root node list changed on an unchanged regenerate";
  EXPECT_EQ(snapshot_hash(ma), first)
      << label << " second regenerate changed the snapshot (root topology moved)";
}

}  // namespace

TEST_F(PaintLayersGenerateTest, generated_tree_snapshot_is_stable_simple_layer)
{
  add_paint_layer("Bottom", add_image("Bottom"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  /* +warm: root layers carry warm chains. */
  /* Snapshot signature is code only (no label/frame/names/ids); the mask is packed into .PL Mask. */
  snapshot_expect(*ma, 0xa338c95bf0e23c59ull, "simple_layer"); /* +warm: slots and spare names. */
}

TEST_F(PaintLayersGenerateTest, generated_tree_snapshot_is_stable_fill_layer)
{
  MaterialPaintLayer *fill = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_CONSTANT, "Fill", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(fill, nullptr);
  BKE_paint_layers_default_channels_apply(*ma, *fill);
  const float green[4] = {0.0f, 1.0f, 0.0f, 1.0f};
  ASSERT_TRUE(BKE_paint_layers_set_fill_color(*ma, fill, green));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  /* The Fill default set is five channels now: the extra Normal and AO selectors are the only
   * change to the serialized tree (see the report's fill_layer diff). */
  /* Snapshot signature is code only (no label/frame/names/ids); the mask is packed into .PL Mask. */
  snapshot_expect(*ma, 0xc485ecea1b70df87ull, "fill_layer"); /* +warm, +Normal/AO default */
}

TEST_F(PaintLayersGenerateTest, generated_tree_snapshot_is_stable_folder_isolating)
{
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Folder", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Child", folder, PaintLayerPlace::Into);
  ASSERT_NE(child, nullptr);
  add_channel(*child, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("Child"));
  /* Opacity below one keeps the folder isolating. */
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 0.5f));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  /* Snapshot signature is code only (no label/frame/names/ids); the mask is packed into .PL Mask. */
  snapshot_expect(*ma, 0x1963da51706e9920ull, "folder_isolating"); /* +warm */
}

TEST_F(PaintLayersGenerateTest, generated_tree_snapshot_is_stable_folder_pass_through)
{
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Folder", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Child", folder, PaintLayerPlace::Into);
  ASSERT_NE(child, nullptr);
  add_channel(*child, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("Child"));
  /* Default opacity and MIX blend make the folder pass through (children inlined). */
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  /* Snapshot signature is code only (no label/frame/names/ids); the mask is packed into .PL Mask. */
  snapshot_expect(*ma, 0xacad6511920367daull, "folder_pass_through"); /* +warm: rows inside a Pass Through folder carry warm chains. */
}

TEST_F(PaintLayersGenerateTest, generated_tree_snapshot_is_stable_nested_folder_value_mirrors)
{
  MaterialPaintLayer *outer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Outer", nullptr, PaintLayerPlace::Above);
  MaterialPaintLayer *inner = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Inner", outer, PaintLayerPlace::Into);
  MaterialPaintLayer *leaf = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Leaf", inner, PaintLayerPlace::Into);
  ASSERT_NE(outer, nullptr);
  ASSERT_NE(inner, nullptr);
  ASSERT_NE(leaf, nullptr);
  add_channel(*leaf, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("Leaf"));
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, outer, 0.5f));
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, inner, 0.5f));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  /* Snapshot signature is code only (no label/frame/names/ids); the mask is packed into .PL Mask. */
  snapshot_expect(*ma, 0xa1a2acfa0b365ca1ull, "nested_folder_value_mirrors"); /* +warm */
}

TEST_F(PaintLayersGenerateTest, generated_tree_snapshot_is_stable_content_correction_image)
{
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_image("Bottom"));
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, bottom, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_IMAGE, "C");
  ASSERT_NE(correction, nullptr);
  add_channel(*correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("Correction"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  /* Snapshot signature is code only (no label/frame/names/ids); the mask is packed into .PL Mask. */
  snapshot_expect(*ma, 0x759a5f1c02229ed0ull, "content_correction_image"); /* +warm */
}

TEST_F(PaintLayersGenerateTest, generated_tree_snapshot_is_stable_stack_correction)
{
  MaterialPaintLayer *owner = add_paint_layer("Owner", add_image("Owner"));
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, owner, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_STACK, "StackFX");
  ASSERT_NE(correction, nullptr);
  add_paint_layer_into(correction, "Child", add_image("Child"));
  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_EQ(report.sampler_estimate, BKE_paint_layers_sampler_count(*ma));
  /* Snapshot signature is code only (no label/frame/names/ids); the mask is packed into .PL Mask. */
  snapshot_expect(*ma, 0xef806788062af624ull, "stack_correction"); /* +warm */
}

TEST_F(PaintLayersGenerateTest, generated_tree_snapshot_is_stable_mask_image)
{
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_image("Bottom"));
  MaterialPaintLayer *mask = BKE_paint_layers_correction_add(
      *ma, bottom, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_IMAGE, "M");
  ASSERT_NE(mask, nullptr);
  add_channel(*mask, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("Mask"));
  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_EQ(report.sampler_estimate, BKE_paint_layers_sampler_count(*ma));
  /* Snapshot signature is code only (no label/frame/names/ids); the mask is packed into .PL Mask. */
  snapshot_expect(*ma, 0xafcbf0dc03d66cceull, "mask_image"); /* +warm */
}

TEST_F(PaintLayersGenerateTest, generated_tree_snapshot_is_stable_stack_mask_channel)
{
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_image("Bottom"));
  MaterialPaintLayer *mask = BKE_paint_layers_correction_add(
      *ma, bottom, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_STACK, "StackMask");
  ASSERT_NE(mask, nullptr);
  mask->mask_channel = int8_t(PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  add_paint_layer_into(mask, "Child", add_image("Child"));
  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_EQ(report.sampler_estimate, BKE_paint_layers_sampler_count(*ma));
  /* Snapshot signature is code only (no label/frame/names/ids); the mask is packed into .PL Mask. */
  snapshot_expect(*ma, 0xcf9007e0ac9a1c1aull, "stack_mask_channel"); /* +warm */
}

TEST_F(PaintLayersGenerateTest, generated_tree_snapshot_is_stable_normal_layer)
{
  MaterialPaintLayer *bottom = add_paint_layer("Bottom", add_image("Bottom"));
  add_channel(*bottom, PAINT_MATERIAL_CHANNEL_NORMAL, add_image("Normal"));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  /* Snapshot signature is code only (no label/frame/names/ids); the mask is packed into .PL Mask. */
  snapshot_expect(*ma, 0xce452a20c3d113a0ull, "normal_layer"); /* +warm */
}

TEST_F(PaintLayersGenerateTest, generated_tree_snapshot_is_stable_material_row_hybrid)
{
  Material *source = add_principled_source("HybridSource", 0.42f);
  add_paint_layer("Bottom", add_image("Bottom"));
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  /* Restore the pre-filter topology: the source carries Specular, Alpha and Emission. */
  extend_channel_set(channel_bit(PAINT_MATERIAL_CHANNEL_SPECULAR) |
                     channel_bit(PAINT_MATERIAL_CHANNEL_ALPHA) |
                     channel_bit(PAINT_MATERIAL_CHANNEL_EMISSION));
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(
      *ma, *row, PAINT_MATERIAL_CHANNEL_ROUGHNESS, add_image("HybridBaked")));
  BKE_paint_layers_active_set(*ma, row->marker);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  /* Snapshot signature is code only (no label/frame/names/ids); the mask is packed into .PL Mask. */
  snapshot_expect(*ma, 0xde2501a0313a272bull, "material_row_hybrid"); /* +warm */
}

TEST_F(PaintLayersGenerateTest, generated_tree_snapshot_is_stable_material_row_source_group)
{
  Material *source = add_principled_source("GroupSource", 0.42f);
  source_set_noise_base_color(*bmain, *source);
  add_paint_layer("Bottom", add_image("Bottom"));
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MATERIAL, "Source", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_set_material(*ma, row, source));
  /* Restore the pre-filter topology: the source carries Specular, Alpha and Emission. */
  extend_channel_set(channel_bit(PAINT_MATERIAL_CHANNEL_SPECULAR) |
                     channel_bit(PAINT_MATERIAL_CHANNEL_ALPHA) |
                     channel_bit(PAINT_MATERIAL_CHANNEL_EMISSION));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  /* Snapshot signature is code only (no label/frame/names/ids); the mask is packed into .PL Mask. */
  snapshot_expect(*ma, 0x6c50977783ee991bull, "material_row_source_group"); /* +warm */
}

TEST_F(PaintLayersGenerateTest, generated_tree_snapshot_is_stable_folder_with_mask_and_effect)
{
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Folder", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  MaterialPaintLayer *child = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Child", folder, PaintLayerPlace::Into);
  ASSERT_NE(child, nullptr);
  add_channel(*child, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("Child"));
  ASSERT_TRUE(BKE_paint_layers_set_opacity(*ma, folder, 0.5f));

  MaterialPaintLayer *effect = BKE_paint_layers_correction_add(
      *ma, folder, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_IMAGE, "FX");
  ASSERT_NE(effect, nullptr);
  add_channel(*effect, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("FX"));
  MaterialPaintLayer *mask = BKE_paint_layers_correction_add(
      *ma, folder, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_IMAGE, "MK");
  ASSERT_NE(mask, nullptr);
  add_channel(*mask, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("MK"));

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  /* Snapshot signature is code only (no label/frame/names/ids); the mask is packed into .PL Mask. */
  snapshot_expect(*ma, 0x66f4b804f9ac0f02ull, "folder_with_mask_and_effect"); /* +warm */
}

TEST_F(PaintLayersGenerateTest, generated_tree_snapshot_is_stable_disabled_row)
{
  add_paint_layer("OnLayer", add_image("On"));
  MaterialPaintLayer *off = add_paint_layer("OffLayer", add_image("Off"));
  ASSERT_NE(off, nullptr);
  /* The row is hidden past the tier, so the first rebuild drops it: the snapshot is the graph
   * without it. A row hidden for less than the tier would still be in the graph (the cold-tier
   * tests cover that side). */
  disable_and_age(off);
  /* snapshot_expect's own unchanged regeneration must then keep the root (D1: the stored hash
   * describes what was built). */
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  /* Snapshot signature is code only (no label/frame/names/ids); the mask is packed into .PL Mask. */
  snapshot_expect(*ma, 0x47f3715b125acb5full, "disabled_row"); /* +warm */
}

TEST_F(PaintLayersGenerateTest, generated_tree_snapshot_is_stable_multi_channel_overrides)
{
  MaterialPaintLayer *layer = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Multi", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(layer, nullptr);
  add_channel(*layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("Base"));
  add_channel(*layer, PAINT_MATERIAL_CHANNEL_ROUGHNESS, add_image("Rough"));
  add_channel(*layer, PAINT_MATERIAL_CHANNEL_METALLIC, add_image("Metal"));
  /* Per-channel blend/opacity overrides (channel_settings). */
  layer->channel_settings[PAINT_MATERIAL_CHANNEL_ROUGHNESS].blend =
      MA_PAINT_LAYER_BLEND_MULTIPLY;
  layer->channel_settings[PAINT_MATERIAL_CHANNEL_ROUGHNESS].opacity = 0.5f;
  layer->channel_settings[PAINT_MATERIAL_CHANNEL_METALLIC].blend = MA_PAINT_LAYER_BLEND_ADD;
  layer->channel_settings[PAINT_MATERIAL_CHANNEL_METALLIC].opacity = 0.25f;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  /* Snapshot signature is code only (no label/frame/names/ids); the mask is packed into .PL Mask. */
  snapshot_expect(*ma, 0x9f9a12f6fbfebc8cull, "multi_channel_overrides"); /* +warm */
}

TEST_F(PaintLayersGenerateTest, generated_tree_snapshot_is_stable_custom_node_group_row)
{
  MaterialPaintLayer *owner = add_paint_layer("Owner", add_image("OwnerMap"));
  MaterialPaintLayer *correction = BKE_paint_layers_correction_add(
      *ma, owner, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_NODE_GROUP, "NGC");
  ASSERT_NE(correction, nullptr);
  Image *bake_map = add_image("NodeGroupBakeMap");
  Image *coverage = add_image("NodeGroupBakeCoverage");
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(
      *ma, *correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR, bake_map));
  ASSERT_TRUE(BKE_paint_layers_bake_set_map(*ma, *correction, -1, coverage));
  BKE_paint_layers_bake_finalize(*ma, *correction);
  PaintLayersRegenerateReport report;
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma, &report));
  EXPECT_EQ(report.sampler_estimate, BKE_paint_layers_sampler_count(*ma));
  /* Snapshot signature is code only (no label/frame/names/ids); the mask is packed into .PL Mask. */
  snapshot_expect(*ma, 0x69145f30a6792f4cull, "custom_node_group_row"); /* +warm */
}

TEST_F(PaintLayersGenerateTest, generated_tree_snapshot_is_stable_mesh_map_row)
{
  Image *atlas = add_atlas_image(*bmain, "Atlas", 8);
  ASSERT_NE(atlas, nullptr);
  MaterialPaintLayer *mesh_map = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_MESH_MAP, "AO", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(mesh_map, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, mesh_map, PAINT_MATERIAL_CHANNEL_BASE_COLOR),
            nullptr);
  ASSERT_TRUE(BKE_mesh_maps_slot_image_set(*ma, MA_MESH_MAP_AO, atlas));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  /* Snapshot signature is code only (no label/frame/names/ids); the mask is packed into .PL Mask. */
  snapshot_expect(*ma, 0x2bbb938ca6f55d18ull, "mesh_map_row");
}

/** A Fill layer carrying a map and a Fill mask carrying a map, with a bake that stands in for the
 * row until a mapping goes live on it. */
TEST_F(PaintLayersGenerateTest, mapping_row_stays_live_and_slider_drag_is_value_only)
{
  MaterialPaintLayer *fill = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_CONSTANT, "FillLive", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(fill, nullptr);
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
      *ma, fill, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(record, nullptr);
  record->image = add_image("FillLiveMap");
  record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  MaterialPaintLayer *mask = BKE_paint_layers_mask_add(*ma, fill, 1.0f);
  ASSERT_NE(mask, nullptr);
  MaterialPaintLayerChannel *mask_record = BKE_paint_layers_channel_add(
      *ma, mask, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(mask_record, nullptr);
  mask_record->image = add_image("FillLiveMaskMap");
  mask_record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  MaterialPaintLayerBake *bake = BKE_paint_layers_bake_struct_ensure(*fill);
  ASSERT_NE(bake, nullptr);
  bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR] = add_image("FillLiveBakedColor");
  bake->coverage = add_image("FillLiveBakedCoverage");
  const auto store_hash = [&]() {
    uint32_t hash[2];
    BKE_paint_layers_bake_hash(*ma, *fill, hash);
    bake->hash[0] = hash[0];
    bake->hash[1] = hash[1];
  };
  store_hash();
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  Image *baked = nullptr;
  EXPECT_TRUE(BKE_paint_layers_bake_substitute(
      *ma, *fill, PAINT_MATERIAL_CHANNEL_BASE_COLOR, &baked));
  EXPECT_FALSE(BKE_paint_layers_mapping_blocks_bake(*ma, *fill));

  /* A mapping on the mask alone is enough to keep the owner live. */
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, mask, true));
  EXPECT_TRUE(BKE_paint_layers_mapping_blocks_bake(*ma, *fill));
  EXPECT_FALSE(BKE_paint_layers_bake_substitute(
      *ma, *fill, PAINT_MATERIAL_CHANNEL_BASE_COLOR, &baked));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, fill, true));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *group = layer_tree_find(*bmain, "FillLive");
  ASSERT_NE(group, nullptr);
  EXPECT_GE(count_type(*group, SH_NODE_MAPPING), 1);

  /* Even with a hash-matching bake the row is live: nothing is substituted. */
  store_hash();
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *fill));
  EXPECT_FALSE(BKE_paint_layers_bake_substitute(
      *ma, *fill, PAINT_MATERIAL_CHANNEL_BASE_COLOR, &baked));
  EXPECT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  ma->paint_layers_flag &= ~(MA_PAINT_LAYERS_REGEN | MA_PAINT_LAYERS_BAKE_STALE);

  const auto root_vector = [&](const bUUID &marker, const char *role, float r_value[3]) {
    char marker_text[UUID_STRING_SIZE];
    BLI_uuid_format(marker_text, marker);
    ma->paint_layers_tree->ensure_interface_cache();
    bNode *instance = instance_find();
    if (instance == nullptr) {
      return false;
    }
    for (bNodeTreeInterfaceSocket *socket : ma->paint_layers_tree->interface_inputs()) {
      if (socket->properties == nullptr) {
        continue;
      }
      const IDProperty *role_prop = IDP_GetPropertyTypeFromGroup(
          socket->properties, "pbr_paint_layers_role", IDP_STRING);
      const IDProperty *marker_prop = IDP_GetPropertyTypeFromGroup(
          socket->properties, "pbr_paint_layers_layer", IDP_STRING);
      if (role_prop == nullptr || marker_prop == nullptr || !STREQ(IDP_string_get(role_prop), role) ||
          !STREQ(IDP_string_get(marker_prop), marker_text))
      {
        continue;
      }
      bNodeSocket *input = bke::node_find_socket(
          *instance, SOCK_IN, UString::from_ptr_noinline(socket->identifier));
      if (input == nullptr) {
        return false;
      }
      copy_v3_v3(r_value, static_cast<bNodeSocketValueVector *>(input->default_value)->value);
      return true;
    }
    return false;
  };

  uint32_t hash_before[2];
  BKE_paint_layers_bake_hash(*ma, *fill, hash_before);
  for (int i = 1; i <= 5; i++) {
    const float offset[2] = {0.1f * float(i), 0.05f * float(i)};
    const float scale[2] = {1.0f + 0.5f * float(i), 1.0f + 0.5f * float(i)};
    const float mask_offset[2] = {0.2f * float(i), 0.0f};
    ASSERT_TRUE(BKE_paint_layers_mapping_set_offset(*ma, fill, offset));
    ASSERT_TRUE(BKE_paint_layers_mapping_set_scale(*ma, fill, scale));
    ASSERT_TRUE(BKE_paint_layers_mapping_set_rotation(*ma, fill, 0.1f * float(i)));
    ASSERT_TRUE(BKE_paint_layers_mapping_set_offset(*ma, mask, mask_offset));

    EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN, 0);
    EXPECT_EQ(ma->paint_layers_flag & MA_PAINT_LAYERS_BAKE_STALE, 0);
    uint32_t hash_now[2];
    BKE_paint_layers_bake_hash(*ma, *fill, hash_now);
    EXPECT_EQ(hash_now[0], hash_before[0]);
    EXPECT_EQ(hash_now[1], hash_before[1]);
    EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *fill));

    float value[3] = {};
    ASSERT_TRUE(root_vector(fill->marker, bke::paint_layers::ROLE_MAPPING_OFFSET, value));
    EXPECT_FLOAT_EQ(value[0], offset[0]);
    EXPECT_FLOAT_EQ(value[1], offset[1]);
    ASSERT_TRUE(root_vector(fill->marker, bke::paint_layers::ROLE_MAPPING_ROTATION, value));
    EXPECT_FLOAT_EQ(value[2], 0.1f * float(i));
    ASSERT_TRUE(root_vector(mask->marker, bke::paint_layers::ROLE_MAPPING_OFFSET, value));
    EXPECT_FLOAT_EQ(value[0], mask_offset[0]);
  }
}

/** Stage 1 mapping: a Fill layer whose valid bake stands in for it is rebuilt live, with its
 * Mapping node, once the mapping is enabled; the row is never substituted while it applies. */
TEST_F(PaintLayersGenerateTest, mapping_fill_layer_with_valid_bake_rebuilds_with_mapping)
{
  MaterialPaintLayer *fill = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_CONSTANT, "FillBaked", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(fill, nullptr);
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
      *ma, fill, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(record, nullptr);
  record->image = add_image("FillMap");
  record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;

  MaterialPaintLayerBake *bake = BKE_paint_layers_bake_struct_ensure(*fill);
  ASSERT_NE(bake, nullptr);
  bake->images[PAINT_MATERIAL_CHANNEL_BASE_COLOR] = add_image("FillBakedColor");
  bake->coverage = add_image("FillBakedCoverage");
  uint32_t hash_before[2];
  BKE_paint_layers_bake_hash(*ma, *fill, hash_before);
  bake->hash[0] = hash_before[0];
  bake->hash[1] = hash_before[1];
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *group1 = layer_tree_find(*bmain, "FillBaked");
  ASSERT_NE(group1, nullptr);
  /* The valid bake stands in for the row: no Mapping node while the mapping is off. */
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *fill));
  EXPECT_EQ(count_type(*group1, SH_NODE_MAPPING), 0);

  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, fill, true));
  uint32_t hash_after[2];
  BKE_paint_layers_bake_hash(*ma, *fill, hash_after);
  /* Only the fact that a mapping is enabled is in the bake hash (never its values), and the row
   * is live whether or not a bake matches that hash. */
  EXPECT_TRUE(hash_before[0] != hash_after[0] || hash_before[1] != hash_after[1]);
  EXPECT_FALSE(BKE_paint_layers_bake_is_valid(*ma, *fill));
  EXPECT_TRUE(BKE_paint_layers_mapping_blocks_bake(*ma, *fill));
  bake->hash[0] = hash_after[0];
  bake->hash[1] = hash_after[1];
  EXPECT_TRUE(BKE_paint_layers_bake_is_valid(*ma, *fill));
  Image *substitute = nullptr;
  EXPECT_FALSE(BKE_paint_layers_bake_substitute(
      *ma, *fill, PAINT_MATERIAL_CHANNEL_BASE_COLOR, &substitute));
  const float moved[2] = {0.3f, 0.3f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_offset(*ma, fill, moved));
  uint32_t hash_moved[2];
  BKE_paint_layers_bake_hash(*ma, *fill, hash_moved);
  EXPECT_EQ(hash_moved[0], hash_after[0]);
  EXPECT_EQ(hash_moved[1], hash_after[1]);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *group2 = layer_tree_find(*bmain, "FillBaked");
  ASSERT_NE(group2, nullptr);
  const int mappings = count_type(*group2, SH_NODE_MAPPING);
  EXPECT_EQ(mappings, 1);
  bNode *mapping = find_type(*group2, SH_NODE_MAPPING);
  if (mapping != nullptr) {
    bNodeSocket *vec = bke::node_find_socket(*mapping, SOCK_IN, "Vector"_ustr);
    ASSERT_NE(vec, nullptr);
    ASSERT_FALSE(vec->directly_linked_links().is_empty());
    const bNode *src = vec->directly_linked_links()[0]->fromnode;
    EXPECT_TRUE(src->type_legacy == SH_NODE_UVMAP || src->type_legacy == SH_NODE_TEX_COORD);
    bool repeat = false;
    for (bNode &node : group2->nodes) {
      if (node.type_legacy == SH_NODE_TEX_IMAGE && node.storage != nullptr) {
        bNodeSocket *v = bke::node_find_socket(node, SOCK_IN, "Vector"_ustr);
        if (v != nullptr && !v->directly_linked_links().is_empty() &&
            v->directly_linked_links()[0]->fromnode == mapping)
        {
          repeat = static_cast<NodeTexImage *>(node.storage)->extension ==
                   SHD_IMAGE_EXTENSION_REPEAT;
        }
      }
    }
    EXPECT_TRUE(repeat);
  }
}

/** Stage 1 mapping: a Fill layer reading a map builds a Mapping node for it. */
TEST_F(PaintLayersGenerateTest, mapping_fill_layer_map_builds_mapping_node)
{
  MaterialPaintLayer *fill = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_CONSTANT, "FillMapped", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(fill, nullptr);
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
      *ma, fill, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(record, nullptr);
  record->image = add_image("FillMap2");
  record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, fill, true));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *group = layer_tree_find(*bmain, "FillMapped");
  ASSERT_NE(group, nullptr);
  EXPECT_EQ(count_type(*group, SH_NODE_MAPPING), 1);
}

enum class MappingRowKind { FillLayer, FillCorrection, FillMask };

/** Builds a row of \a kind that carries a map (the mapping enabled when \a mapped) into a fresh
 * tree and returns the row group. No topology update runs, so `bNodeSocket::link` stays stale;
 * the checks below read `bNodeTree::links` instead. */
static bNodeTree *mapping_row_build(PaintLayersGenerateTest &test,
                                    const MappingRowKind kind,
                                    const char *uv_name,
                                    const bool mapped,
                                    const char *row_name)
{
  Material &ma = *test.ma;
  BLI_strncpy(ma.paint_layers_uv_map, uv_name, sizeof(ma.paint_layers_uv_map));

  MaterialPaintLayer *row = nullptr;
  if (kind == MappingRowKind::FillLayer) {
    row = BKE_paint_layers_add(
        ma, MA_PAINT_LAYER_SOURCE_CONSTANT, row_name, nullptr, PaintLayerPlace::Above);
  }
  else {
    MaterialPaintLayer *bottom = test.add_paint_layer(row_name, test.add_image("Bottom"));
    row = kind == MappingRowKind::FillCorrection ?
              BKE_paint_layers_correction_add(
                  ma, bottom, MA_PAINT_LAYER_ROLE_EFFECT, MA_PAINT_LAYER_SOURCE_CONSTANT, "C") :
              BKE_paint_layers_mask_add(ma, bottom, 1.0f);
  }
  EXPECT_NE(row, nullptr);
  if (row == nullptr) {
    return nullptr;
  }
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
      ma, row, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  EXPECT_NE(record, nullptr);
  if (record == nullptr) {
    return nullptr;
  }
  record->image = test.add_image("RowMap");
  record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  if (mapped) {
    EXPECT_TRUE(BKE_paint_layers_mapping_set_enabled(ma, row, true));
  }

  bNodeTree *tree = test.make_tree("PBR Layers Vector Links");
  PaintLayersBuildContext ctx;
  auto layer_tree_factory = [&test](const MaterialPaintLayer &layer) {
    return test.layer_tree_for(layer);
  };
  ctx.layer_tree_get = layer_tree_factory;
  auto subgroup_tree_factory = [&test](const MaterialPaintLayer &row, StringRef kind) {
    return test.subgroup_tree_for(row, kind);
  };
  ctx.subgroup_tree_get = subgroup_tree_factory;
  paint_layers_tree_build(ma, *tree, ctx);
  return PaintLayersGenerateTest::layer_tree_find(*test.bmain, row_name);
}

/** The nodes linked into \a texture's Vector input, read from the tree's own link list. */
static Vector<const bNode *> vector_link_sources(const bNodeTree &tree, bNode &texture)
{
  Vector<const bNode *> sources;
  const bNodeSocket *vector = bke::node_find_socket(texture, SOCK_IN, "Vector"_ustr);
  for (const bNodeLink &link : tree.links) {
    if (link.tosock == vector) {
      sources.append(link.fromnode);
    }
  }
  return sources;
}

/** Every Image Texture in \a group reads its Vector through exactly one link, from a Mapping node
 * that the tree's one coordinate source feeds (no coordinate source reaches a texture directly).
 * A packed `.PL Mask` panel holds its own texture and Mapping, and the panel's Group Input stands
 * for the row's coordinate source. */
static void expect_maps_read_through_mapping(bNodeTree &group, const bool named)
{
  const int source_type = named ? SH_NODE_UVMAP : SH_NODE_TEX_COORD;
  int textures = 0;
  int direct_sources = 0;
  PaintLayersGenerateTest::walk_nodes_recursive(
      group, [&](bNodeTree &owner, bNode &node) {
        if (node.type_legacy == SH_NODE_TEX_IMAGE && node.id != nullptr &&
            STRPREFIX(node.id->name + 2, "RowMap"))
        {
          textures++;
          const Vector<const bNode *> sources = vector_link_sources(owner, node);
          ASSERT_EQ(sources.size(), 1);
          EXPECT_EQ(sources[0]->type_legacy, SH_NODE_MAPPING);
        }
        else if (node.type_legacy == SH_NODE_MAPPING) {
          const Vector<const bNode *> sources = vector_link_sources(owner, node);
          ASSERT_EQ(sources.size(), 1);
          if (sources[0]->type_legacy == source_type) {
            direct_sources++;
            return;
          }
          /* Inside a packed panel the Mapping reads its UV from that panel's Group Input, which
           * the row group wires to the row's own coordinate source. */
          EXPECT_TRUE(sources[0]->is_group_input());
        }
      });
  EXPECT_GE(textures, 1);

  if (direct_sources == 0) {
    /* The packed mask's coordinate source still stands in the row group, feeding the panel. */
    EXPECT_GT(PaintLayersGenerateTest::count_type(group, source_type), 0);
  }
}

/** A map read through a Mapping node never gets a second link on its Vector input: a stale
 * `bNodeSocket::link` once let the UV wire add a coordinate source beside the Mapping. */
TEST_F(PaintLayersGenerateTest, mapping_vector_has_one_link_from_mapping_for_every_row_kind)
{
  const MappingRowKind kinds[] = {
      MappingRowKind::FillLayer, MappingRowKind::FillCorrection, MappingRowKind::FillMask};
  int case_index = 0;
  for (const MappingRowKind kind : kinds) {
    for (const bool named : {true, false}) {
      SCOPED_TRACE(::testing::Message()
                   << "kind=" << int(kind) << " named=" << named);
      /* A fresh material per case keeps the rows from piling up in one stack. */
      ma = BKE_material_add(bmain, "LayeredCase");
      ma->paint_layers_flag |= MA_PAINT_LAYERED;
      const std::string row_name = "Row" + std::to_string(case_index++);
      bNodeTree *group = mapping_row_build(
          *this, kind, named ? "UVMap" : "", true, row_name.c_str());
      ASSERT_NE(group, nullptr);
      expect_maps_read_through_mapping(*group, named);
    }
  }
}

/** A row without a mapping keeps the plain wire: the UV Map feeds the texture directly when a
 * name is set, and nothing feeds it when the name is empty. */
TEST_F(PaintLayersGenerateTest, unmapped_row_keeps_the_plain_uv_wire)
{
  bNodeTree *named_group = mapping_row_build(
      *this, MappingRowKind::FillLayer, "UVMap", false, "RowNamed");
  ASSERT_NE(named_group, nullptr);
  int textures = 0;
  for (bNode &node : named_group->nodes) {
    if (node.type_legacy != SH_NODE_TEX_IMAGE) {
      continue;
    }
    textures++;
    const Vector<const bNode *> sources = vector_link_sources(*named_group, node);
    ASSERT_EQ(sources.size(), 1);
    EXPECT_EQ(sources[0]->type_legacy, SH_NODE_UVMAP);
  }
  EXPECT_GE(textures, 1);
  EXPECT_EQ(count_type(*named_group, SH_NODE_MAPPING), 0);

  ma = BKE_material_add(bmain, "LayeredEmpty");
  ma->paint_layers_flag |= MA_PAINT_LAYERED;
  bNodeTree *empty_group = mapping_row_build(
      *this, MappingRowKind::FillLayer, "", false, "RowEmpty");
  ASSERT_NE(empty_group, nullptr);
  for (bNode &node : empty_group->nodes) {
    if (node.type_legacy == SH_NODE_TEX_IMAGE) {
      EXPECT_TRUE(vector_link_sources(*empty_group, node).is_empty());
    }
  }
}

/** Regenerating again must not stack more links on the Vector of a mapped map. */
TEST_F(PaintLayersGenerateTest, regenerate_keeps_one_mapping_link_on_each_vector)
{
  BLI_strncpy(ma->paint_layers_uv_map, "UVMap", sizeof(ma->paint_layers_uv_map));
  MaterialPaintLayer *fill = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_CONSTANT, "Row", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(fill, nullptr);
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
      *ma, fill, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(record, nullptr);
  record->image = add_image("RowMap");
  record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, fill, true));

  for (int pass = 0; pass < 3; pass++) {
    ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
    bNodeTree *group = layer_tree_find(*bmain, "Row");
    ASSERT_NE(group, nullptr);
    expect_maps_read_through_mapping(*group, true);
  }
}

/** Nodes of \a type in \a tree (a Mapping of \a mapping_type when given, a Vector Math of that
 * operation otherwise): the Normal Remap is told apart from the row's own Point Mapping by it. */
static int count_nodes_with_custom1(const bNodeTree &tree, const int type, const int custom1)
{
  int count = 0;
  for (const bNode &node : tree.nodes) {
    if (node.type_legacy == type && node.custom1 == custom1) {
      count++;
    }
  }
  return count;
}

/** One Fill row with a map on each of \a channels, the mapping enabled when \a mapped. */
static bNodeTree *normal_remap_row_build(PaintLayersGenerateTest &test,
                                         const Span<eMaterialPaintChannel> channels,
                                         const bool mapped,
                                         const char *row_name,
                                         MaterialPaintLayer **r_row = nullptr)
{
  Material &ma = *test.ma;
  MaterialPaintLayer *row = BKE_paint_layers_add(
      ma, MA_PAINT_LAYER_SOURCE_CONSTANT, row_name, nullptr, PaintLayerPlace::Above);
  EXPECT_NE(row, nullptr);
  if (row == nullptr) {
    return nullptr;
  }
  for (const eMaterialPaintChannel channel : channels) {
    test.add_channel(*row, channel, test.add_image("RowMap"));
  }
  if (mapped) {
    EXPECT_TRUE(BKE_paint_layers_mapping_set_enabled(ma, row, true));
  }
  if (r_row != nullptr) {
    *r_row = row;
  }
  EXPECT_TRUE(BKE_paint_layers_regenerate(*test.bmain, ma));
  return PaintLayersGenerateTest::layer_tree_find(*test.bmain, row_name);
}

/**
 * A mapped Normal map gets the fixed Normal Remap chain (one Sign, one Texture-type Mapping, beside
 * the row's own Point Mapping); a mapped Base Color map and an unmapped Normal map get none.
 */
TEST_F(PaintLayersGenerateTest, mapped_normal_row_builds_the_remap_and_other_rows_do_not)
{
  const eMaterialPaintChannel normal_only[] = {PAINT_MATERIAL_CHANNEL_NORMAL};
  bNodeTree *mapped_normal = normal_remap_row_build(*this, Span<eMaterialPaintChannel>(normal_only, 1), true, "NormalMapped");
  ASSERT_NE(mapped_normal, nullptr);
  EXPECT_EQ(count_nodes_with_custom1(*mapped_normal, SH_NODE_VECTOR_MATH, NODE_VECTOR_MATH_SIGN), 1);
  EXPECT_EQ(count_nodes_with_custom1(*mapped_normal, SH_NODE_MAPPING, NODE_MAPPING_TYPE_POINT), 1);
  EXPECT_EQ(count_nodes_with_custom1(*mapped_normal, SH_NODE_MAPPING, NODE_MAPPING_TYPE_TEXTURE), 1);

  ma = BKE_material_add(bmain, "NormalRemapBase");
  ma->paint_layers_flag |= MA_PAINT_LAYERED;
  const eMaterialPaintChannel base_only[] = {PAINT_MATERIAL_CHANNEL_BASE_COLOR};
  bNodeTree *mapped_base = normal_remap_row_build(*this, Span<eMaterialPaintChannel>(base_only, 1), true, "BaseMapped");
  ASSERT_NE(mapped_base, nullptr);
  EXPECT_EQ(count_nodes_with_custom1(*mapped_base, SH_NODE_VECTOR_MATH, NODE_VECTOR_MATH_SIGN), 0);
  EXPECT_EQ(count_nodes_with_custom1(*mapped_base, SH_NODE_MAPPING, NODE_MAPPING_TYPE_TEXTURE), 0);

  ma = BKE_material_add(bmain, "NormalRemapOff");
  ma->paint_layers_flag |= MA_PAINT_LAYERED;
  bNodeTree *unmapped_normal = normal_remap_row_build(*this, Span<eMaterialPaintChannel>(normal_only, 1), false, "NormalPlain");
  ASSERT_NE(unmapped_normal, nullptr);
  EXPECT_EQ(
      count_nodes_with_custom1(*unmapped_normal, SH_NODE_VECTOR_MATH, NODE_VECTOR_MATH_SIGN), 0);
  EXPECT_EQ(count_nodes_with_custom1(*unmapped_normal, SH_NODE_MAPPING, NODE_MAPPING_TYPE_POINT), 0);
}

/**
 * The remap rides the Mapping's own group inputs: its Rotation and the Sign's input come from the
 * same Group Input sockets as the Point Mapping's, and editing the values (a negative scale, a
 * rotation) leaves the node and link counts alone.
 */
TEST_F(PaintLayersGenerateTest, normal_remap_shares_the_mapping_inputs_and_ignores_value_edits)
{
  const eMaterialPaintChannel normal_only[] = {PAINT_MATERIAL_CHANNEL_NORMAL};
  MaterialPaintLayer *row = nullptr;
  bNodeTree *group = normal_remap_row_build(*this, Span<eMaterialPaintChannel>(normal_only, 1), true, "NormalShared", &row);
  ASSERT_NE(group, nullptr);
  ASSERT_NE(row, nullptr);

  auto source_of = [&](const bNode &node, const char *socket_name) -> const bNodeSocket * {
    const bNodeSocket *input = bke::node_find_socket(
        const_cast<bNode &>(node), SOCK_IN, UString::from_ptr_noinline(socket_name));
    for (const bNodeLink &link : group->links) {
      if (link.tosock == input) {
        return link.fromsock;
      }
    }
    return nullptr;
  };
  const bNode *point = nullptr;
  const bNode *texture = nullptr;
  const bNode *sign = nullptr;
  for (const bNode &node : group->nodes) {
    if (node.type_legacy == SH_NODE_MAPPING && node.custom1 == NODE_MAPPING_TYPE_POINT) {
      point = &node;
    }
    else if (node.type_legacy == SH_NODE_MAPPING && node.custom1 == NODE_MAPPING_TYPE_TEXTURE) {
      texture = &node;
    }
    else if (node.type_legacy == SH_NODE_VECTOR_MATH && node.custom1 == NODE_VECTOR_MATH_SIGN) {
      sign = &node;
    }
  }
  ASSERT_NE(point, nullptr);
  ASSERT_NE(texture, nullptr);
  ASSERT_NE(sign, nullptr);
  EXPECT_NE(source_of(*point, "Rotation"), nullptr);
  EXPECT_EQ(source_of(*texture, "Rotation"), source_of(*point, "Rotation"));
  EXPECT_NE(source_of(*sign, "Vector"), nullptr);
  EXPECT_EQ(source_of(*sign, "Vector"), source_of(*point, "Scale"));

  const int nodes_before = BLI_listbase_count(&group->nodes);
  const int links_before = BLI_listbase_count(&group->links);
  const float mirror[2] = {-3.0f, 0.5f};
  ASSERT_TRUE(BKE_paint_layers_mapping_set_scale(*ma, row, mirror));
  ASSERT_TRUE(BKE_paint_layers_mapping_set_rotation(*ma, row, 1.2f));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *after = layer_tree_find(*bmain, "NormalShared");
  ASSERT_NE(after, nullptr);
  EXPECT_EQ(BLI_listbase_count(&after->nodes), nodes_before);
  EXPECT_EQ(BLI_listbase_count(&after->links), links_before);
}

/**
 * Step 0 layout: the densest row (Fill mask plus Image mask plus Effect) places
 * every node of its group without overlaps. Frames and the group
 * input/output are packing only and are skipped.
 */
TEST_F(PaintLayersGenerateTest, row_group_nodes_do_not_overlap)
{
  ASSERT_NE(build_row_with_two_masks_and_effect(), nullptr);

  bNodeTree *tree = make_tree("PBR Layers Layout");
  ASSERT_NE(tree, nullptr);
  PaintLayersBuildContext ctx;
  auto layer_tree_factory = [this](const MaterialPaintLayer &layer) {
    return layer_tree_for(layer);
  };
  ctx.layer_tree_get = layer_tree_factory;
  auto subgroup_tree_factory = [this](const MaterialPaintLayer &row, StringRef kind) {
    return subgroup_tree_for(row, kind);
  };
  ctx.subgroup_tree_get = subgroup_tree_factory;
  paint_layers_tree_build(*ma, *tree, ctx);

  bNodeTree *group = find_row_group("Row");
  ASSERT_NE(group, nullptr);
  /* Why nested: the mask stack packs into its own `.PL Mask` group with its own origin, so each
   * tree is checked in its own space. */
  Vector<bNodeTree *> nested;
  collect_group_instances(*group, nested);
  ASSERT_EQ(nested.size(), 1);
  bNodeTree *mask_group = nested[0];
  ASSERT_NE(mask_group, nullptr);
  EXPECT_GT(tree_nodes_do_not_overlap(*group), 0);
  /* The dense row really builds something inside: source, grey chains, two Mixes. */
  EXPECT_GT(tree_nodes_do_not_overlap(*mask_group), 5);
}

/**
 * M1/R5: a Fill mask that carries a map packs its Mapping node one column left of
 * its Image Texture, so neither the row group nor the nested `.PL Mask` tree has
 * overlaps. The same kind of row without a mapping passes the older test above;
 * this one is the regression that catches two nodes sharing the Mask base+0 slot.
 */
TEST_F(PaintLayersGenerateTest, row_group_mapped_mask_nodes_do_not_overlap)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_image("RowBase"));
  ASSERT_NE(row, nullptr);
  MaterialPaintLayer *fill_mask = BKE_paint_layers_mask_add(*ma, row, 0.5f);
  ASSERT_NE(fill_mask, nullptr);
  MaterialPaintLayer *image_mask = BKE_paint_layers_mask_add(*ma, row, 1.0f);
  ASSERT_NE(image_mask, nullptr);
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
      *ma, image_mask, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  ASSERT_NE(record, nullptr);
  record->image = add_image("RowMask");
  record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  /* Only a Fill source that carries a map is remappable, so the Constant source
   * stays and the record's image makes it the mapped item. */
  ASSERT_TRUE(BKE_paint_layers_mapping_set_enabled(*ma, image_mask, true));

  bNodeTree *tree = make_tree("PBR Layers Mapped Mask Layout");
  ASSERT_NE(tree, nullptr);
  PaintLayersBuildContext ctx;
  auto layer_tree_factory = [this](const MaterialPaintLayer &layer) {
    return layer_tree_for(layer);
  };
  ctx.layer_tree_get = layer_tree_factory;
  auto subgroup_tree_factory = [this](const MaterialPaintLayer &row, StringRef kind) {
    return subgroup_tree_for(row, kind);
  };
  ctx.subgroup_tree_get = subgroup_tree_factory;
  paint_layers_tree_build(*ma, *tree, ctx);

  bNodeTree *group = find_row_group("Row");
  ASSERT_NE(group, nullptr);
  /* Why nested: the mask stack packs into its own `.PL Mask` group with its own origin, so each
   * tree is checked in its own space. */
  Vector<bNodeTree *> nested;
  collect_group_instances(*group, nested);
  ASSERT_EQ(nested.size(), 1);
  bNodeTree *mask_group = nested[0];
  ASSERT_NE(mask_group, nullptr);
  /* The mapping really is built, so the check below exercises the two-node slot. */
  EXPECT_GE(count_type(*mask_group, SH_NODE_MAPPING), 1);
  EXPECT_GT(tree_nodes_do_not_overlap(*group), 0);
  EXPECT_GT(tree_nodes_do_not_overlap(*mask_group), 5);
}

/**
 * R5: a stack of three Fill masks lays its Mix nodes out in stack order, element 0
 * at the top; each item owns kMaskItemRows rows, so their y coordinates strictly
 * fall by kMaskItemRows * kRowPitch per element.
 */
TEST_F(PaintLayersGenerateTest, mask_stack_mix_rows_follow_stack_order)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_image("RowBase"));
  ASSERT_NE(row, nullptr);
  for (int i = 0; i < 3; i++) {
    ASSERT_NE(BKE_paint_layers_mask_add(*ma, row, 1.0f), nullptr);
  }

  bNodeTree *tree = make_tree("PBR Layers Mask Stack Order");
  ASSERT_NE(tree, nullptr);
  PaintLayersBuildContext ctx;
  auto layer_tree_factory = [this](const MaterialPaintLayer &layer) {
    return layer_tree_for(layer);
  };
  ctx.layer_tree_get = layer_tree_factory;
  auto subgroup_tree_factory = [this](const MaterialPaintLayer &row, StringRef kind) {
    return subgroup_tree_for(row, kind);
  };
  ctx.subgroup_tree_get = subgroup_tree_factory;
  paint_layers_tree_build(*ma, *tree, ctx);

  bNodeTree *group = find_row_group("Row");
  ASSERT_NE(group, nullptr);
  Vector<bNodeTree *> nested;
  collect_group_instances(*group, nested);
  ASSERT_EQ(nested.size(), 1);
  bNodeTree *mask_group = nested[0];
  ASSERT_NE(mask_group, nullptr);

  Vector<const bNode *> mixes;
  for (bNode &node : mask_group->nodes) {
    if (node.type_legacy == SH_NODE_MIX &&
        (STREQ(node.label, "Mask: Fill") || STREQ(node.label, "Mask: Image")))
    {
      mixes.append(&node);
    }
  }
  ASSERT_EQ(mixes.size(), 3);
  /* Order the Mixes by their row: element 0 is the highest, so its y is the largest. */
  std::sort(mixes.begin(), mixes.end(), [](const bNode *a, const bNode *b) {
    return a->location[1] > b->location[1];
  });
  for (int i = 0; i < mixes.size(); i++) {
    const float expected = -float(i * bke::paint_layers::layout::kMaskItemRows) *
                           bke::paint_layers::layout::kRowPitch;
    EXPECT_FLOAT_EQ(mixes[i]->location[1], expected)
        << "element " << i << " should sit " << i << " item blocks below the top";
  }
  EXPECT_GT(mixes[0]->location[1], mixes[1]->location[1]);
  EXPECT_GT(mixes[1]->location[1], mixes[2]->location[1]);
}

/** Step 0 labels: the Fill mask Mix, the grey Divide and the factor Multiply. */
TEST_F(PaintLayersGenerateTest, row_group_mask_labels)
{
  ASSERT_NE(build_row_with_two_masks_and_effect(), nullptr);

  bNodeTree *tree = make_tree("PBR Layers Labels");
  ASSERT_NE(tree, nullptr);
  PaintLayersBuildContext ctx;
  auto layer_tree_factory = [this](const MaterialPaintLayer &layer) {
    return layer_tree_for(layer);
  };
  ctx.layer_tree_get = layer_tree_factory;
  auto subgroup_tree_factory = [this](const MaterialPaintLayer &row, StringRef kind) {
    return subgroup_tree_for(row, kind);
  };
  ctx.subgroup_tree_get = subgroup_tree_factory;
  paint_layers_tree_build(*ma, *tree, ctx);

  bNodeTree *group = find_row_group("Row");
  ASSERT_NE(group, nullptr);
  /* Why nested: the mask nodes live in the packed `.PL Mask` group now, not flat in the row. */
  Vector<bNodeTree *> nested;
  collect_group_instances(*group, nested);
  ASSERT_EQ(nested.size(), 1);
  bNodeTree *mask_group = nested[0];
  ASSERT_NE(mask_group, nullptr);
  bool has_fill = false;
  bool has_grey = false;
  bool has_opacity = false;
  for (bNode &node : mask_group->nodes) {
    const char *label = node.label;
    if (label[0] == '\0') {
      continue;
    }
    if (STREQ(label, "Mask: Fill")) {
      EXPECT_EQ(node.type_legacy, SH_NODE_MIX);
      has_fill = true;
    }
    if (STREQ(label, "Mask grey")) {
      EXPECT_EQ(node.type_legacy, SH_NODE_MATH);
      EXPECT_EQ(node.custom1, NODE_MATH_DIVIDE);
      has_grey = true;
    }
    if (STREQ(label, "Mask opacity")) {
      EXPECT_EQ(node.type_legacy, SH_NODE_MATH);
      EXPECT_EQ(node.custom1, NODE_MATH_MULTIPLY);
      has_opacity = true;
    }
  }
  EXPECT_TRUE(has_fill);
  EXPECT_TRUE(has_grey);
  EXPECT_TRUE(has_opacity);
}

/**
 * Step 1 frames: the same dense row packs each mask item into its own `Mask <N>` frame,
 * and the shared Normal Combine group holds no frames of its own.
 */
TEST_F(PaintLayersGenerateTest, row_group_mask_frames)
{
  ASSERT_NE(build_row_with_two_masks_and_effect(), nullptr);

  bNodeTree *tree = make_tree("PBR Layers Mask Frames");
  ASSERT_NE(tree, nullptr);
  PaintLayersBuildContext ctx;
  auto layer_tree_factory = [this](const MaterialPaintLayer &layer) {
    return layer_tree_for(layer);
  };
  ctx.layer_tree_get = layer_tree_factory;
  auto subgroup_tree_factory = [this](const MaterialPaintLayer &row, StringRef kind) {
    return subgroup_tree_for(row, kind);
  };
  ctx.subgroup_tree_get = subgroup_tree_factory;
  paint_layers_tree_build(*ma, *tree, ctx);

  bNodeTree *group = find_row_group("Row");
  ASSERT_NE(group, nullptr);
  /* Why nested: `Mask <N>` frames live inside the packed group now; the row itself keeps none. */
  for (bNode &node : group->nodes) {
    if (node.type_legacy == NODE_FRAME && StringRef(node.label).startswith("Mask")) {
      EXPECT_TRUE(false) << "row group keeps a Mask frame '" << node.label << "'";
    }
  }
  Vector<bNodeTree *> nested;
  collect_group_instances(*group, nested);
  ASSERT_EQ(nested.size(), 1);
  bNodeTree *mask_group = nested[0];
  ASSERT_NE(mask_group, nullptr);
  Vector<bNode *> mask_frames;
  for (bNode &node : mask_group->nodes) {
    if (node.type_legacy == NODE_FRAME && StringRef(node.label).startswith("Mask")) {
      mask_frames.append(&node);
    }
  }
  /* The Fill item and the Image item, one frame each, in stack order. */
  ASSERT_EQ(mask_frames.size(), 2);
  for (bNode *frame : mask_frames) {
    bool has_mix = false;
    for (bNode &node : mask_group->nodes) {
      if (node.parent == frame && node.type_legacy == SH_NODE_MIX) {
        has_mix = true;
      }
    }
    EXPECT_TRUE(has_mix) << "frame '" << frame->label << "' parents no Mix";
  }
  /* Only instances hang under frames: the shared combine tree itself stays frameless. */
  for (bNode &node : group->nodes) {
    if (!BKE_paint_material_is_normal_combine_group(node)) {
      continue;
    }
    ASSERT_NE(node.id, nullptr);
    const bNodeTree &combine = *reinterpret_cast<const bNodeTree *>(node.id);
    for (const bNode &inner : combine.nodes) {
      EXPECT_NE(inner.type_legacy, NODE_FRAME)
          << "Normal Combine tree holds a frame '" << inner.label << "'";
    }
  }
}

/**
 * Step 0 version: the row hash is stable across identical builds, while the
 * layout version itself moves the hash, so the one-time grid rebuild happens
 * and later passes keep unchanged groups.
 */
TEST_F(PaintLayersGenerateTest, layout_version_moves_topology_hash)
{
  MaterialPaintLayer *row = add_paint_layer("Bottom", add_image("Bottom"));
  ASSERT_NE(row, nullptr);
  const int wired[] = {PAINT_MATERIAL_CHANNEL_BASE_COLOR};
  const uint64_t hash_first = paint_layers_layer_topology_hash(*ma, *row, Span<int>(wired, 1));
  EXPECT_EQ(paint_layers_layer_topology_hash(*ma, *row, Span<int>(wired, 1)), hash_first);
  uint64_t without_version = hash_first;
  /* The version participates through the same mixer the hash uses. */
  EXPECT_NE(bke::paint_layers::topology_hash_mix(without_version,
                                                 uint64_t(bke::paint_layers::layout::kLayoutVersion)),
            bke::paint_layers::topology_hash_mix(
                without_version, uint64_t(bke::paint_layers::layout::kLayoutVersion - 1)));
  EXPECT_GT(bke::paint_layers::layout::kLayoutVersion, 0);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *group = find_row_group("Bottom");
  ASSERT_NE(group, nullptr);
  ASSERT_TRUE(group_io_sentinel_set(*group, 0.5f));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  group = find_row_group("Bottom");
  ASSERT_NE(group, nullptr);
  EXPECT_TRUE(group_io_sentinel_get(*group, 0.5f));
}

/* Task 4 Step 2: the mask stack packs into one nested group per row. */
TEST_F(PaintLayersGenerateTest, mask_stack_is_packed_into_a_nested_group)
{
  ASSERT_NE(build_row_with_two_masks_and_effect(), nullptr);

  bNodeTree *tree = make_tree("PBR Layers Mask Packed");
  ASSERT_NE(tree, nullptr);
  PaintLayersBuildContext ctx;
  auto layer_tree_factory = [this](const MaterialPaintLayer &layer) {
    return layer_tree_for(layer);
  };
  ctx.layer_tree_get = layer_tree_factory;
  auto subgroup_tree_factory = [this](const MaterialPaintLayer &row, StringRef kind) {
    return subgroup_tree_for(row, kind);
  };
  ctx.subgroup_tree_get = subgroup_tree_factory;
  paint_layers_tree_build(*ma, *tree, ctx);

  bNodeTree *group = find_row_group("Row");
  ASSERT_NE(group, nullptr);
  Vector<bNodeTree *> nested;
  collect_group_instances(*group, nested);
  ASSERT_EQ(nested.size(), 1);
  bNodeTree *mask_group = nested[0];
  ASSERT_NE(mask_group, nullptr);
  EXPECT_TRUE(StringRef(mask_group->id.name + 2).startswith(".PL Mask "));
  EXPECT_TRUE(interface_has_socket(*mask_group, "Factor", false));
  EXPECT_TRUE(interface_has_socket(*mask_group, "Factor", true));
}

/* R2: a pure build packs the mask subgroup without any G_MAIN, so no code path depends on the
 * global Main any more. */
TEST_F(PaintLayersGenerateTest, mask_subgroup_build_needs_no_g_main)
{
  ASSERT_NE(build_row_with_two_masks_and_effect(), nullptr);
  bNodeTree *tree = make_tree("PBR Layers No GMain");
  ASSERT_NE(tree, nullptr);
  PaintLayersBuildContext ctx;
  auto layer_tree_factory = [this](const MaterialPaintLayer &layer) {
    return layer_tree_for(layer);
  };
  auto subgroup_tree_factory = [this](const MaterialPaintLayer &row, StringRef kind) {
    return subgroup_tree_for(row, kind);
  };
  ctx.layer_tree_get = layer_tree_factory;
  ctx.subgroup_tree_get = subgroup_tree_factory;
  Main *saved_main = G_MAIN;
  G_MAIN = nullptr;
  paint_layers_tree_build(*ma, *tree, ctx);
  G_MAIN = saved_main;
  bNodeTree *group = find_row_group("Row");
  ASSERT_NE(group, nullptr);
  Vector<bNodeTree *> nested;
  collect_group_instances(*group, nested);
  EXPECT_EQ(nested.size(), 1);
}

/* Task 4 Step 2 lifecycle: dropping the mask or the row deletes its `.PL Mask` tree, and an
 * unchanged regen creates none. */
TEST_F(PaintLayersGenerateTest, mask_nested_group_lifecycle)
{
  MaterialPaintLayer *folder = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_STACK, "Folder", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(folder, nullptr);
  MaterialPaintLayer *top = add_paint_layer_into(folder, "Top", add_image("TopMap"));
  ASSERT_NE(top, nullptr);
  MaterialPaintLayer *mask = BKE_paint_layers_mask_add(*ma, top, 0.5f);
  ASSERT_NE(mask, nullptr);

  auto mask_tree_find = [&]() -> bNodeTree * {
    for (bNodeTree &candidate : bmain->nodetrees) {
      if (STREQ(candidate.id.name + 2, ".PL Mask Top")) {
        return &candidate;
      }
    }
    return nullptr;
  };
  auto nodetree_count = [&]() {
    int count = 0;
    for (bNodeTree &candidate : bmain->nodetrees) {
      (void)candidate;
      count++;
    }
    return count;
  };

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_NE(mask_tree_find(), nullptr);

  /* User decision (TZ-5): removing one mask leaves the `.PL Mask` tree standing while the row's
   * `Warm Mask` spare is there, because that spare is what the next added mask reuses. The tree
   * goes only with the whole row. */
  ASSERT_TRUE(BKE_paint_layers_remove(*ma, mask));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_NE(mask_tree_find(), nullptr);
  EXPECT_NE(paint_layers_warm_item(*ma, *top, WarmKind::Mask), nullptr);

  ASSERT_TRUE(BKE_paint_layers_remove(*ma, top));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(mask_tree_find(), nullptr);

  const int before = nodetree_count();
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  EXPECT_EQ(nodetree_count(), before);
}

/* Task 4 Step 2 warm: a Paint mask taking the Warm Mask slot keeps the nested interface shape. */
TEST_F(PaintLayersGenerateTest, mask_nested_group_warm_slot_keeps_interface)
{
  MaterialPaintLayer *row = add_paint_layer("Bottom", add_image("Bottom"));
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  auto mask_tree_find = [&]() -> bNodeTree * {
    for (bNodeTree &candidate : bmain->nodetrees) {
      if (STREQ(candidate.id.name + 2, ".PL Mask Bottom")) {
        return &candidate;
      }
    }
    return nullptr;
  };
  auto socket_count = [](bNodeTree *tree) {
    if (tree == nullptr) {
      return -1;
    }
    tree->ensure_interface_cache();
    return int(tree->interface_inputs().size() + tree->interface_outputs().size());
  };
  auto input_count = [](bNodeTree *tree) {
    if (tree == nullptr) {
      return -1;
    }
    tree->ensure_interface_cache();
    return int(tree->interface_inputs().size());
  };
  auto output_count = [](bNodeTree *tree) {
    if (tree == nullptr) {
      return -1;
    }
    tree->ensure_interface_cache();
    return int(tree->interface_outputs().size());
  };
  /* Links landing on the `.PL Mask` instance's own inputs in the row group: the interface itself
   * stays the same, so the wiring of this instance's sockets must too. */
  auto instance_input_links = [&](bNodeTree *tree) {
    if (tree == nullptr) {
      return -1;
    }
    bNodeTree *row_group = layer_tree_find(*bmain, "Bottom");
    if (row_group == nullptr) {
      return -1;
    }
    for (bNode &node : row_group->nodes) {
      if (node.is_group() && node.id == &tree->id) {
        int links = 0;
        for (bNodeSocket *socket : node.input_sockets()) {
          for (const bNodeLink *link : socket->directly_linked_links()) {
            UNUSED_VARS(link);
            links++;
          }
        }
        return links;
      }
    }
    return -1;
  };

  bNodeTree *before_tree = mask_tree_find();
  ASSERT_NE(before_tree, nullptr);
  const int before_sockets = socket_count(before_tree);
  const int before_inputs = input_count(before_tree);
  const int before_outputs = output_count(before_tree);
  const int before_links = instance_input_links(before_tree);
  EXPECT_GT(before_sockets, 0);
  EXPECT_GT(before_inputs, 0);
  EXPECT_EQ(before_outputs, 1);
  EXPECT_GE(before_links, 0);

  MaterialPaintLayer *real = BKE_paint_layers_correction_add(
      *ma, row, MA_PAINT_LAYER_ROLE_MASK_ITEM, MA_PAINT_LAYER_SOURCE_IMAGE, "Mask");
  ASSERT_NE(real, nullptr);
  ASSERT_NE(BKE_paint_layers_channel_add(*ma, real, PAINT_MATERIAL_CHANNEL_BASE_COLOR), nullptr);
  ASSERT_TRUE(BKE_paint_layers_channel_set_image(
      *ma, real, PAINT_MATERIAL_CHANNEL_BASE_COLOR, add_image("RealMaskMap")));
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *after_tree = mask_tree_find();
  ASSERT_NE(after_tree, nullptr);
  EXPECT_EQ(socket_count(after_tree), before_sockets);
  EXPECT_EQ(input_count(after_tree), before_inputs);
  EXPECT_EQ(output_count(after_tree), before_outputs);
  EXPECT_EQ(instance_input_links(after_tree), before_links);
}

/* Task 4 Step 2 normal: a Normal row with a mask still packs exactly one group besides the
 * Normal Combine instances. */
TEST_F(PaintLayersGenerateTest, mask_nested_group_with_normal_row)
{
  MaterialPaintLayer *row = BKE_paint_layers_add(
      *ma, MA_PAINT_LAYER_SOURCE_IMAGE, "Top", nullptr, PaintLayerPlace::Above);
  ASSERT_NE(row, nullptr);
  MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
      *ma, row, PAINT_MATERIAL_CHANNEL_NORMAL);
  ASSERT_NE(record, nullptr);
  record->image = add_image("TopNormal");
  record->state = MA_PAINT_LAYER_CHANNEL_ENABLED;
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, row, 0.5f), nullptr);

  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNodeTree *group = find_row_group("Top");
  ASSERT_NE(group, nullptr);
  Vector<bNodeTree *> nested;
  collect_group_instances(*group, nested);
  ASSERT_EQ(nested.size(), 1);
  EXPECT_TRUE(StringRef(nested[0]->id.name + 2).startswith(".PL Mask "));
}

/* Task 4: a row whose mask takes part in four channels keeps one `.PL Mask` tree with one instance
 * per channel, told apart by `custom1`; each instance reads its own channel's factor and the shared
 * interface never grows a second Factor per channel. */
TEST_F(PaintLayersGenerateTest, mask_row_in_four_channels_keeps_one_group_and_one_instance_each)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_image("RowBase"));
  ASSERT_NE(row, nullptr);
  for (const eMaterialPaintChannel channel : {PAINT_MATERIAL_CHANNEL_METALLIC,
                                              PAINT_MATERIAL_CHANNEL_ROUGHNESS,
                                              PAINT_MATERIAL_CHANNEL_NORMAL})
  {
    ASSERT_NE(add_channel(*row, channel, add_image("RowChannel")), nullptr);
  }
  /* The base mask is the Fill item: one grey constant the shared subgroup reduces on every
   * channel. */
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, row, 0.5f), nullptr);

  /* Why regenerate: the real factory tags the shared `.PL Mask` tree (owner, marker, subkind), so
   * the second channel reuses it; a pure build's bare factory would make one tree per channel and
   * hide the bug this test guards. */
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  bNodeTree *group = find_row_group("Row");
  ASSERT_NE(group, nullptr);

  bNodeTree *mask_group = nullptr;
  Vector<bNode *> instances;
  for (bNode &node : group->nodes) {
    if (!node.is_group() || node.id == nullptr || GS(node.id->name) != ID_NT) {
      continue;
    }
    bNodeTree *nested = id_cast<bNodeTree *>(node.id);
    if (nested == nullptr || !StringRef(nested->id.name + 2).startswith(".PL Mask ")) {
      continue;
    }
    if (mask_group == nullptr) {
      mask_group = nested;
    }
    /* Every instance must reference the one shared tree. */
    EXPECT_EQ(nested, mask_group);
    instances.append(&node);
  }
  ASSERT_NE(mask_group, nullptr);
  /* One tree, one instance per participating channel. */
  ASSERT_EQ(instances.size(), 4);
  uint16_t seen = 0;
  for (bNode *instance : instances) {
    seen |= uint16_t(1) << int(instance->custom1);
  }
  EXPECT_NE(seen & (uint16_t(1) << int(PAINT_MATERIAL_CHANNEL_BASE_COLOR)), 0);
  EXPECT_NE(seen & (uint16_t(1) << int(PAINT_MATERIAL_CHANNEL_METALLIC)), 0);
  EXPECT_NE(seen & (uint16_t(1) << int(PAINT_MATERIAL_CHANNEL_ROUGHNESS)), 0);
  EXPECT_NE(seen & (uint16_t(1) << int(PAINT_MATERIAL_CHANNEL_NORMAL)), 0);

  /* Every instance's Factor input is fed from a different upstream chain. */
  mask_group->ensure_interface_cache();
  bNodeTreeInterfaceSocket *factor_in = nullptr;
  for (bNodeTreeInterfaceSocket *socket : mask_group->interface_inputs()) {
    if (socket->name != nullptr && STREQ(socket->name, "Factor")) {
      factor_in = socket;
    }
  }
  ASSERT_NE(factor_in, nullptr);
  int factor_links = 0;
  Vector<const bNode *> from_nodes;
  for (bNode *instance : instances) {
    bNodeSocket *input = bke::node_find_socket(
        *instance, SOCK_IN, UString::from_ptr_noinline(factor_in->identifier));
    ASSERT_NE(input, nullptr);
    for (const bNodeLink *link : input->directly_linked_links()) {
      factor_links++;
      if (!from_nodes.contains(link->fromnode)) {
        from_nodes.append(link->fromnode);
      }
    }
  }
  EXPECT_EQ(factor_links, 4);
  EXPECT_EQ(from_nodes.size(), 4);

  /* The interface did not grow a second Factor per channel. */
  int factor_inputs = 0;
  int factor_outputs = 0;
  for (bNodeTreeInterfaceSocket *socket : mask_group->interface_inputs()) {
    if (socket->name != nullptr && STREQ(socket->name, "Factor")) {
      factor_inputs++;
    }
  }
  for (bNodeTreeInterfaceSocket *socket : mask_group->interface_outputs()) {
    if (socket->name != nullptr && STREQ(socket->name, "Factor")) {
      factor_outputs++;
    }
  }
  EXPECT_EQ(factor_inputs, 1);
  EXPECT_EQ(factor_outputs, 1);

  /* Task 4: each instance carries its channel number in `custom1` and sits on its own Y lane, so no
   * two instances share a row. */
  for (const int i : instances.index_range()) {
    EXPECT_TRUE(ELEM(instances[i]->custom1,
                     int16_t(PAINT_MATERIAL_CHANNEL_BASE_COLOR),
                     int16_t(PAINT_MATERIAL_CHANNEL_METALLIC),
                     int16_t(PAINT_MATERIAL_CHANNEL_ROUGHNESS),
                     int16_t(PAINT_MATERIAL_CHANNEL_NORMAL)))
        << "instance custom1 " << instances[i]->custom1 << " is not a wired channel";
    for (const int j : instances.index_range()) {
      if (i < j) {
        EXPECT_NE(instances[i]->location[1], instances[j]->location[1])
            << "Mask ch" << instances[i]->custom1 << " and ch" << instances[j]->custom1
            << " share a lane row";
      }
    }
  }

  /* Task 3: the four `Mask chN` instances sit on their own Mask-column rows, one per channel, so
   * no two instances and no row Mix run into each other; the packed tree is checked in its own
   * space too. */
  EXPECT_GT(tree_nodes_do_not_overlap(*group), 0);
  EXPECT_GT(tree_nodes_do_not_overlap(*mask_group), 0);
}

/* Task 4: the lane bases are per-pass state, so a second rebuild with the same description lays the
 * channel instances exactly where the first pass did; growing lane 0 (more mask rows) must move
 * lane 1 down, proving the base is recomputed rather than kept from the previous pass. */
TEST_F(PaintLayersGenerateTest, channel_lanes_reset_between_regenerations)
{
  MaterialPaintLayer *row = add_paint_layer("Row", add_image("RowBase"));
  ASSERT_NE(row, nullptr);
  ASSERT_NE(add_channel(*row, PAINT_MATERIAL_CHANNEL_METALLIC, add_image("RowMetal")), nullptr);
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, row, 1.0f), nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));

  /* The `.PL Mask` instance of \a channel in the row group, or null. */
  auto instance_of = [&](const int channel) -> bNode * {
    bNodeTree *group = find_row_group("Row");
    if (group == nullptr) {
      return nullptr;
    }
    for (bNode &node : group->nodes) {
      if (node.is_group() && node.id != nullptr && GS(node.id->name) == ID_NT &&
          StringRef(id_cast<bNodeTree *>(node.id)->id.name + 2).startswith(".PL Mask ") &&
          int(node.custom1) == channel)
      {
        return &node;
      }
    }
    return nullptr;
  };

  bNode *first_metallic = instance_of(int(PAINT_MATERIAL_CHANNEL_METALLIC));
  ASSERT_NE(first_metallic, nullptr);
  const float first_metallic_y = first_metallic->location[1];

  /* Forcing a rebuild with the same description must not move the lane: a stale base would. */
  BKE_paint_layers_generate_runtime_free(*ma);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNode *rebuilt_metallic = instance_of(int(PAINT_MATERIAL_CHANNEL_METALLIC));
  ASSERT_NE(rebuilt_metallic, nullptr);
  EXPECT_EQ(rebuilt_metallic->location[1], first_metallic_y);

  /* Two more Fill masks make lane 0 taller, so lane 1 starts lower. */
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, row, 1.0f), nullptr);
  ASSERT_NE(BKE_paint_layers_mask_add(*ma, row, 1.0f), nullptr);
  ASSERT_TRUE(BKE_paint_layers_regenerate(*bmain, *ma));
  bNode *grown_metallic = instance_of(int(PAINT_MATERIAL_CHANNEL_METALLIC));
  ASSERT_NE(grown_metallic, nullptr);
  EXPECT_LT(grown_metallic->location[1], first_metallic_y);
}

}  // namespace blender::bke::tests
