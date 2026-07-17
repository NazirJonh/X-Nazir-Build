/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include <string>

#include "BKE_gtest_base.hh"
#include "BKE_lib_id.hh"
#include "BKE_mesh.h"
#include "BKE_sculpt_layers.hh"

#include "BLI_array.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_set.hh"
#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

#include "DNA_mesh_types.h"

namespace blender::bke::sculpt_layers::tests {

/**
 * A #Mesh cannot be allocated before #BKE_idtype_init has run, so these tests need the shared
 * blenkernel fixture rather than a bare #TEST. The fixture name is snake_case so the gtest group
 * stays `sculpt_layers_tree`; #keyframes_paste makes the same trade-off.
 */
struct sculpt_layers_tree : public BlenderGTestBase {
  Mesh *mesh = nullptr;

  void SetUp() override
  {
    /* The tree carries no per-element data in these tests — every layer below is created with
     * `totelem = 0` and never allocates a buffer — so an empty mesh is enough. */
    mesh = BKE_mesh_new_nomain(0, 0, 0, 0);
  }

  void TearDown() override
  {
    BKE_id_free(nullptr, mesh);
  }
};

/**
 * The children list *is* the sibling order, so reading it back in list order is the only way to
 * assert that order.
 */
static Vector<SculptLayerTreeNode *> children_of(SculptLayerGroup &group)
{
  Vector<SculptLayerTreeNode *> children;
  for (SculptLayerTreeNode &node : group.children) {
    children.append(&node);
  }
  return children;
}

/**
 * The node base is the first member of both concrete types (the DNA invariant documented on
 * #SculptLayerTreeNode), which is what makes the cast legal. Null rather than a wrong pointer when
 * the node is a layer, so a mistaken lookup fails an assertion instead of reading a layer as a
 * folder.
 */
static SculptLayerGroup *node_as_group(SculptLayerTreeNode *node)
{
  if (node == nullptr || node->type != SCULPT_LAYER_TREE_NODE_TYPE_GROUP) {
    return nullptr;
  }
  return reinterpret_cast<SculptLayerGroup *>(node);
}

/** Depth-first walk of every node below \a group, \a group itself excluded. */
static void gather_nodes(SculptLayerGroup &group, Vector<SculptLayerTreeNode *> &r_nodes)
{
  for (SculptLayerTreeNode &node : group.children) {
    r_nodes.append(&node);
    if (node.type == SCULPT_LAYER_TREE_NODE_TYPE_GROUP) {
      gather_nodes(*node_as_group(&node), r_nodes);
    }
  }
}

/**
 * #build_tree_mesh cannot hand pointers back, and a node's uid is assigned rather than predictable,
 * so the tests address the nodes it made by their name — which the model already guarantees to be
 * unique among siblings.
 */
static SculptLayerTreeNode *child_by_name(SculptLayerGroup &group, const StringRef name)
{
  for (SculptLayerTreeNode &node : group.children) {
    if (name == node.name) {
      return &node;
    }
  }
  return nullptr;
}

/* Builds a mesh whose root group holds a folder and a layer, with a layer nested in the folder. */
static void build_tree_mesh(Mesh &mesh)
{
  /* Created folder-first, then the layers, which reproduces the one arrangement the two flat lists
   * could express: the UI drew every folder ahead of every layer. Starting from it is what lets
   * #move_into_interleaves_kinds_and_sets_parent assert an interleaving the old model could not
   * represent at all. */
  SculptLayerGroup *folder = group_add(mesh, "Folder", 0);
  add(mesh, "Outer", SCULPT_LAYER_DOMAIN_VERT, 0);
  SculptLayer *nested = add(mesh, "Nested", SCULPT_LAYER_DOMAIN_VERT, 0);
  node_move_into(mesh, nested->base, *folder, nullptr);
}

TEST_F(sculpt_layers_tree, uid_is_unique_across_both_kinds)
{
  /* Layer uids and group uids used to be separate counters that both started at 1, so the first
   * layer and the first folder collided on 1. node_unique_uid draws from one counter: creating
   * layers and folders in any interleaving yields all-distinct uids, and none is 0 (the root). */
  build_tree_mesh(*mesh);
  /* A folder created *after* the layers is the other half of the old collision: under separate
   * counters it repeats the second layer's number rather than the first's, so a counter that only
   * avoided the first clash would still fail here. */
  group_add(*mesh, "Late Folder", 0);
  add(*mesh, "Late Layer", SCULPT_LAYER_DOMAIN_VERT, 0);

  ASSERT_NE(root_group(*mesh), nullptr);
  SculptLayerGroup &root = *root_group(*mesh);
  EXPECT_EQ(root.base.uid, 0) << "uid 0 is the root group";

  Vector<SculptLayerTreeNode *> nodes;
  gather_nodes(root, nodes);
  ASSERT_EQ(nodes.size(), 5);

  Set<int> uids;
  for (const SculptLayerTreeNode *node : nodes) {
    EXPECT_NE(node->uid, 0) << "node '" << node->name << "' took the root's uid";
    EXPECT_TRUE(uids.add(node->uid)) << "uid " << node->uid << " is held by more than one node";
  }

  /* The counter must also be able to name a uid no node holds yet — that is the number the next
   * node created on this mesh takes. */
  EXPECT_NE(node_unique_uid(*mesh), 0);
  EXPECT_FALSE(uids.contains(node_unique_uid(*mesh)));
}

TEST_F(sculpt_layers_tree, find_by_uid_resolves_either_kind)
{
  /* node_find_by_uid replaces the two kind-specific overloads: one call finds a layer or a folder
   * at any depth, and returns null for an unknown uid. Uid 0 resolves to the root group. */
  build_tree_mesh(*mesh);

  ASSERT_NE(root_group(*mesh), nullptr);
  SculptLayerGroup &root = *root_group(*mesh);
  SculptLayerTreeNode *outer = child_by_name(root, "Outer");
  ASSERT_NE(outer, nullptr);
  SculptLayerGroup *folder = node_as_group(child_by_name(root, "Folder"));
  ASSERT_NE(folder, nullptr);
  SculptLayerTreeNode *nested = child_by_name(*folder, "Nested");
  ASSERT_NE(nested, nullptr);

  /* A layer and a folder sharing the root, then a layer one level down: one call, either kind, any
   * depth. */
  EXPECT_EQ(node_find_by_uid(*mesh, outer->uid), outer);
  EXPECT_EQ(node_find_by_uid(*mesh, folder->base.uid), &folder->base);
  EXPECT_EQ(node_find_by_uid(*mesh, nested->uid), nested);

  EXPECT_EQ(node_find_by_uid(*mesh, 0), &root.base);

  /* node_unique_uid names the number the *next* node would take, so nothing holds it yet. */
  EXPECT_EQ(node_find_by_uid(*mesh, node_unique_uid(*mesh)), nullptr);
}

TEST_F(sculpt_layers_tree, active_get_is_null_when_no_layer_is_active)
{
  /* The uid-0 collision between the two conventions that meet in #active_get: node_find_by_uid
   * resolves 0 to the *root group*, while Mesh::sculpt_layers_active_uid uses 0 for "no active
   * layer" (uids start at 1). An active_get written over node_find_by_uid without its own zero
   * check hands back the root, and every caller then reads a SculptLayerGroup as a SculptLayer —
   * `influence` lands on the group's child list and `data` reads past the smaller allocation. So
   * this pins the null, not just the absence of a crash. */
  build_tree_mesh(*mesh);

  ASSERT_NE(root_group(*mesh), nullptr);
  /* Only meaningful while the root really does answer to uid 0 — the collision this guards against
   * exists exactly because that is true. */
  ASSERT_EQ(node_find_by_uid(*mesh, 0), &root_group(*mesh)->base);

  /* #add made its layer active, so clearing it is what creates the "no active layer" state; a mesh
   * that never had one would pass even against an implementation that ignores active_set. */
  ASSERT_NE(active_get(*mesh), nullptr);
  active_set(*mesh, nullptr);
  EXPECT_EQ(mesh->sculpt_layers_active_uid, 0) << "clearing the active layer stores uid 0";

  EXPECT_EQ(active_get(*mesh), nullptr) << "no active layer must not resolve to the root group";
}

TEST_F(sculpt_layers_tree, is_descendant_of_spans_depth_and_rejects_self)
{
  /* The drop target's real constraint: a folder may not move into its own subtree. True for a
   * nested child at any depth, false for a sibling, false for the node against itself. */
  build_tree_mesh(*mesh);

  ASSERT_NE(root_group(*mesh), nullptr);
  SculptLayerGroup &root = *root_group(*mesh);
  SculptLayerTreeNode *outer = child_by_name(root, "Outer");
  ASSERT_NE(outer, nullptr);
  SculptLayerGroup *folder = node_as_group(child_by_name(root, "Folder"));
  ASSERT_NE(folder, nullptr);
  SculptLayerTreeNode *nested = child_by_name(*folder, "Nested");
  ASSERT_NE(nested, nullptr);

  /* Another level below "Folder", so answering has to climb more than one link and a walk that
   * only checked the direct parent would fail. */
  SculptLayerGroup *inner = group_add(*mesh, "Inner", 0);
  ASSERT_NE(inner, nullptr);
  node_move_into(*mesh, inner->base, *folder, nullptr);
  SculptLayer *deep = add(*mesh, "Deep", SCULPT_LAYER_DOMAIN_VERT, 0);
  node_move_into(*mesh, deep->base, *inner, nullptr);

  EXPECT_TRUE(node_is_descendant_of(*nested, *folder));
  EXPECT_TRUE(node_is_descendant_of(inner->base, *folder));
  EXPECT_TRUE(node_is_descendant_of(deep->base, *folder));

  /* A sibling shares the parent but sits outside the subtree. */
  EXPECT_FALSE(node_is_descendant_of(*outer, *folder));
  /* "Deep" is below "Inner" but "Nested" is not: descendancy is the subtree, not the whole mesh. */
  EXPECT_FALSE(node_is_descendant_of(*nested, *inner));

  /* Strictly below: a node is not its own descendant. */
  EXPECT_FALSE(node_is_descendant_of(folder->base, *folder));

  /* The root deserves its own case: it is the one ancestor whose `parent` is null, so the walk
   * (a plain Floyd cycle-detecting parent walk, see sculpt_layers.cc:273-303, with no special
   * case for uid 0) terminates against it by running out of parent links rather than by matching
   * some other node first. Drop-onto-root is a real UI path, so every depth below it must still
   * read as a descendant of the root. */
  EXPECT_TRUE(node_is_descendant_of(*outer, root));
  EXPECT_TRUE(node_is_descendant_of(*folder, root));
  EXPECT_TRUE(node_is_descendant_of(*nested, root));
  EXPECT_TRUE(node_is_descendant_of(inner->base, root));
  EXPECT_TRUE(node_is_descendant_of(deep->base, root));

  /* Strictly below still holds at the root: it is not its own descendant either. */
  EXPECT_FALSE(node_is_descendant_of(root.base, root));
}

TEST_F(sculpt_layers_tree, move_into_interleaves_kinds_and_sets_parent)
{
  /* The bug this project exists to fix: a folder placed after a layer stays after it. move_into
   * with `after` = a layer puts the folder next in the same children list, and the moved node's
   * parent back-pointer follows it. */
  build_tree_mesh(*mesh);

  ASSERT_NE(root_group(*mesh), nullptr);
  SculptLayerGroup &root = *root_group(*mesh);
  SculptLayerTreeNode *outer = child_by_name(root, "Outer");
  ASSERT_NE(outer, nullptr);
  SculptLayerGroup *folder = node_as_group(child_by_name(root, "Folder"));
  ASSERT_NE(folder, nullptr);
  SculptLayerTreeNode *nested = child_by_name(*folder, "Nested");
  ASSERT_NE(nested, nullptr);

  /* The fixture's nesting move already changed a parent, so this pins the fixup rather than a
   * value that happened to be right from the start. */
  EXPECT_EQ(nested->parent, folder);

  /* A second root layer, so that "after Outer" names a position in the middle of the list: an
   * implementation that appended to the tail instead would still satisfy a two-node list. */
  SculptLayer *tail = add(*mesh, "Tail", SCULPT_LAYER_DOMAIN_VERT, 0);

  {
    /* Precondition: the fixture starts folder-first — the only order the flat lists could express.
     * Asserted so the move below cannot pass by doing nothing. */
    const Vector<SculptLayerTreeNode *> before = children_of(root);
    ASSERT_EQ(before.size(), 3);
    ASSERT_EQ(before[0], &folder->base);
    ASSERT_EQ(before[1], outer);
    ASSERT_EQ(before[2], &tail->base);
  }

  node_move_into(*mesh, folder->base, root, outer);

  /* The folder now sits between the two layers — the interleaving the flat lists had no way to
   * store, and the reason this migration exists. */
  const Vector<SculptLayerTreeNode *> after = children_of(root);
  ASSERT_EQ(after.size(), 3);
  EXPECT_EQ(after[0], outer);
  EXPECT_EQ(after[1], &folder->base);
  EXPECT_EQ(after[2], &tail->base);

  /* Reordering within the same folder must leave the back-pointer naming that folder... */
  EXPECT_EQ(folder->base.parent, &root);
  /* ... and the subtree travels with the folder rather than being detached or reparented. */
  EXPECT_EQ(nested->parent, folder);
  const Vector<SculptLayerTreeNode *> folder_children = children_of(*folder);
  ASSERT_EQ(folder_children.size(), 1);
  EXPECT_EQ(folder_children[0], nested);
}

TEST_F(sculpt_layers_tree, move_into_empty_folder_leaves_both_lists_intact)
{
  /* An empty destination is the one case #BLI_insertlinkafter cannot repair on its own: it takes
   * the node as `first`/`last` and returns without touching the node's own links, which #BLI_remlink
   * deliberately left pointing into the source list. Moving a *non-tail* node is what exposes that —
   * its stale `next` still names the following sibling, so the destination forward-walks straight on
   * into the source and both lists end up claiming the same node. Every other move in this file
   * happens to move a tail, where only the quieter `prev` leaks. */
  SculptLayerGroup *empty = group_add(*mesh, "Empty", 0);
  ASSERT_NE(empty, nullptr);
  SculptLayer *moved = add(*mesh, "Moved", SCULPT_LAYER_DOMAIN_VERT, 0);
  SculptLayer *follower = add(*mesh, "Follower", SCULPT_LAYER_DOMAIN_VERT, 0);

  ASSERT_NE(root_group(*mesh), nullptr);
  SculptLayerGroup &root = *root_group(*mesh);

  {
    /* The two preconditions the corruption needs: the destination holds nothing, and the moved
     * layer has a sibling *after* it to leak. Asserted so the test cannot pass by accident on a
     * fixture that quietly stops meeting them. */
    ASSERT_TRUE(children_of(*empty).is_empty());
    const Vector<SculptLayerTreeNode *> before = children_of(root);
    ASSERT_EQ(before.size(), 3);
    ASSERT_EQ(before[0], &empty->base);
    ASSERT_EQ(before[1], &moved->base);
    ASSERT_EQ(before[2], &follower->base);
  }

  node_move_into(*mesh, moved->base, *empty, nullptr);

  /* The destination holds the moved layer and *stops there*. Against a move that leaves the stale
   * `next` in place, this walk runs on into the source and yields "Follower" as well. */
  const Vector<SculptLayerTreeNode *> dst_children = children_of(*empty);
  ASSERT_EQ(dst_children.size(), 1);
  EXPECT_EQ(dst_children[0], &moved->base);
  EXPECT_EQ(moved->base.parent, empty);

  /* The source lost exactly the moved layer and is still walkable end-to-end. The backward walk is
   * the other half: a stale `prev` surfaces there rather than in the forward order. */
  const Vector<SculptLayerTreeNode *> src_children = children_of(root);
  ASSERT_EQ(src_children.size(), 2);
  EXPECT_EQ(src_children[0], &empty->base);
  EXPECT_EQ(src_children[1], &follower->base);

  Vector<const SculptLayerTreeNode *> src_backward;
  for (const SculptLayerTreeNode &node : root.children.items_reversed()) {
    src_backward.append(&node);
  }
  ASSERT_EQ(src_backward.size(), 2);
  EXPECT_EQ(src_backward[0], &follower->base);
  EXPECT_EQ(src_backward[1], &empty->base);

  /* No node may sit in both lists. A whole-tree walk reaching one twice is the exact shape that
   * doubles a layer's displacement in the composite and makes #tree_free release it twice. */
  Vector<SculptLayerTreeNode *> nodes;
  gather_nodes(root, nodes);
  EXPECT_EQ(nodes.size(), 3);
  Set<SculptLayerTreeNode *> seen;
  for (SculptLayerTreeNode *node : nodes) {
    EXPECT_TRUE(seen.add(node)) << "node '" << node->name << "' is reachable from the root twice";
  }
}

/**
 * The layer span's names in order, joined. Compared as one string rather than as a vector of
 * pointers so that a failure prints the two orders side by side instead of two lists of addresses.
 */
static std::string layer_names(const Span<SculptLayer *> layers)
{
  std::string names;
  for (const SculptLayer *layer : layers) {
    if (!names.empty()) {
      names += ',';
    }
    names += layer->base.name;
  }
  return names;
}

TEST_F(sculpt_layers_tree, cache_rebuilds_after_topology_change)
{
  /* #layers hands out a span into a cache on the group's runtime, rebuilt only once a mutation tags
   * it dirty. Every mutation below is therefore preceded by a *read*, and that read is the whole
   * point: it leaves a warm cache for the mutation to invalidate. Without it the span would simply
   * be built fresh afterwards and a missing dirty-tag would pass unnoticed.
   *
   * What a missing tag costs is not a stale row: #remove frees the layer, so a span that was not
   * rebuilt hands the eval paths a dangling pointer. */
  SculptLayerGroup &root = *root_group(*mesh);

  /* An empty tree is a cache entry like any other — warm it, so even the first #add has something
   * to invalidate. */
  ASSERT_EQ(layer_names(layers(*mesh)), "");

  /* --- add --- */
  SculptLayer *a = add(*mesh, "A", SCULPT_LAYER_DOMAIN_VERT, 0);
  EXPECT_EQ(layer_names(layers(*mesh)), "A");
  SculptLayer *b = add(*mesh, "B", SCULPT_LAYER_DOMAIN_VERT, 0);
  EXPECT_EQ(layer_names(layers(*mesh)), "A,B");

  /* --- reorder among siblings --- */
  /* Moving A after B changes the order but not the membership, which is exactly why this case needs
   * its own assertion: a test comparing the *set* of layers would pass against a #node_move_into
   * that never tagged anything. */
  node_move_into(*mesh, a->base, root, &b->base);
  EXPECT_EQ(layer_names(layers(*mesh)), "B,A");

  /* --- reparent --- */
  SculptLayerGroup *folder = group_add(*mesh, "Folder", 0);
  ASSERT_NE(folder, nullptr);
  /* Warm the folder's own span as well as the root's: the move below has to invalidate both ends,
   * and the folder answering "my layers" for itself is what the per-group cache exists for. */
  ASSERT_EQ(layer_names(layers(*folder)), "");
  ASSERT_EQ(layer_names(layers(*mesh)), "B,A");

  node_move_into(*mesh, b->base, *folder, nullptr);
  EXPECT_EQ(layer_names(layers(*folder)), "B")
      << "the destination folder's own span must pick the moved layer up";
  /* The root's children are now [A, Folder(B)]: B left its slot in front of A and reappears under
   * the folder, so the whole-tree order flips back. This also pins the *upward* propagation — the
   * link happened inside Folder, and the root's span had to be invalidated by it. */
  EXPECT_EQ(layer_names(layers(*mesh)), "A,B");

  /* --- remove --- */
  /* Both spans are warm from the two reads above. Removing a *nested* layer frees it inside Folder,
   * and the root's span — which is still holding it — must not survive that. */
  remove(*mesh, *b);
  EXPECT_EQ(layer_names(layers(*folder)), "");
  EXPECT_EQ(layer_names(layers(*mesh)), "A");

  /* And once more directly under the root, so the removal of a top-level layer is covered too. */
  ASSERT_EQ(layer_names(layers(*mesh)), "A");
  remove(*mesh, *a);
  EXPECT_EQ(layer_names(layers(*mesh)), "");

  /* --- move between sibling folders --- */
  /* Every case above moves a node whose old parent sits on the new parent's own upward walk to the
   * root (the same folder for a reorder, the root itself for the reparent), so tagging only the
   * destination in #node_move_into happens to reach the source too. Two sibling folders break that
   * overlap: FolderB's walk goes straight to the root and never passes through FolderA, so only the
   * explicit source-parent tag (sculpt_layers.cc:352) can invalidate FolderA's own span. Deleting
   * that tag would leave every assertion above this block passing while this one fails. */
  SculptLayerGroup *folder_a = group_add(*mesh, "FolderA", 0);
  ASSERT_NE(folder_a, nullptr);
  SculptLayerGroup *folder_b = group_add(*mesh, "FolderB", 0);
  ASSERT_NE(folder_b, nullptr);
  SculptLayer *c = add(*mesh, "C", SCULPT_LAYER_DOMAIN_VERT, 0);
  node_move_into(*mesh, c->base, *folder_a, nullptr);

  /* Warm both folders' own spans before the move, same discipline as the rest of this test: a
   * cache that was never read is never stale, so a missing tag would pass unnoticed otherwise. */
  ASSERT_EQ(layer_names(layers(*folder_a)), "C");
  ASSERT_EQ(layer_names(layers(*folder_b)), "");

  node_move_into(*mesh, c->base, *folder_b, nullptr);
  EXPECT_EQ(layer_names(layers(*folder_b)), "C")
      << "the destination folder's own span must pick the moved layer up";
  /* This is the case the destination's upward tag cannot cover: FolderA is not an ancestor of
   * FolderB, so only the source-parent tag keeps FolderA's span from handing out a layer that no
   * longer lives there. */
  EXPECT_EQ(layer_names(layers(*folder_a)), "")
      << "the source folder's own span must drop the moved layer";
}

TEST_F(sculpt_layers_tree, cache_rebuilds_after_duplicate)
{
  /* #duplicate links a node in like #add does, but at a position in the middle of a child list
   * rather than at the root's tail — a separate call site, so a separate dirty-tag to miss. */
  SculptLayer *a = add(*mesh, "A", SCULPT_LAYER_DOMAIN_VERT, 0);
  ASSERT_EQ(layer_names(layers(*mesh)), "A");

  duplicate(*mesh, *a);

  const Span<SculptLayer *> after = layers(*mesh);
  ASSERT_EQ(after.size(), 2);
  EXPECT_EQ(after[0], a) << "the duplicate is inserted directly after its source";
}

TEST_F(sculpt_layers_tree, composition_is_order_independent)
{
  /* The invariant the whole design rests on. Composition is a plain sum of per-layer offsets, so
   * where a layer sits among its siblings cannot change the composed result — which is what makes
   * reordering and reparenting free, and what lets #layers hand the eval paths "every layer, order
   * irrelevant". If this ever fails, the model has grown an order-dependency and the tree's free
   * reordering is not free any more.
   *
   * Every value below is dyadic (halves and quarters), so each product and each sum is exact in
   * binary floating point and the two results must match *bit for bit*. That is deliberate: float
   * addition is not associative in general, so composing arbitrary values in two orders would be
   * asserting rounding luck rather than the model — and a tolerance would hide a genuine
   * order-dependency of the same magnitude.
   *
   * #combine_layers_mesh takes its base and output as explicit spans and never consults the mesh, so
   * the fixture's empty mesh is irrelevant here: the layers are sized to the arrays below. */
  SculptLayerGroup &root = *root_group(*mesh);

  SculptLayer *a = add(*mesh, "A", SCULPT_LAYER_DOMAIN_VERT, 3);
  SculptLayer *b = add(*mesh, "B", SCULPT_LAYER_DOMAIN_VERT, 3);

  /* Distinct per element and per layer, so a composition that dropped or double-counted a layer
   * could not coincidentally land on the same answer. */
  const MutableSpan<float3> a_data = data_get(*a);
  ASSERT_EQ(a_data.size(), 3);
  a_data[0] = float3(0.5f, -0.25f, 1.0f);
  a_data[1] = float3(0.25f, 0.5f, -0.5f);
  a_data[2] = float3(-1.0f, 0.25f, 0.75f);

  const MutableSpan<float3> b_data = data_get(*b);
  ASSERT_EQ(b_data.size(), 3);
  b_data[0] = float3(-0.25f, 0.75f, 0.5f);
  b_data[1] = float3(1.0f, -0.5f, 0.25f);
  b_data[2] = float3(0.5f, 0.25f, -0.75f);

  /* Different, and neither is the default 1: #effective then scales the two layers differently, so a
   * composition that ignored influence entirely would still be order-independent and would sail
   * through a version of this test that left both at 1. */
  a->influence = 0.5f;
  b->influence = 1.5f;

  const Array<float3> base = {
      float3(1.0f, 2.0f, 3.0f), float3(-4.0f, 5.0f, 0.5f), float3(0.25f, -1.5f, 2.0f)};

  Array<float3> first(3);
  combine_layers_mesh(base, layers(*mesh), first);

  /* The layers have to actually move the base, or the comparison below is vacuous — two identical
   * no-ops are trivially order-independent. */
  ASSERT_NE(first[0][0], base[0][0]);

  {
    const Span<SculptLayer *> ordered = layers(*mesh);
    ASSERT_EQ(ordered.size(), 2);
    ASSERT_EQ(ordered[0], a);
    ASSERT_EQ(ordered[1], b);
  }

  node_move_into(*mesh, a->base, root, &b->base);

  {
    const Span<SculptLayer *> reordered = layers(*mesh);
    ASSERT_EQ(reordered.size(), 2);
    /* Asserted, not assumed: if the reorder silently did nothing, the equality below would pass
     * while testing nothing at all. */
    ASSERT_EQ(reordered[0], b);
    ASSERT_EQ(reordered[1], a);
  }

  Array<float3> second(3);
  combine_layers_mesh(base, layers(*mesh), second);

  for (int i = 0; i < 3; i++) {
    EXPECT_EQ(first[i][0], second[i][0]) << "element " << i << " x";
    EXPECT_EQ(first[i][1], second[i][1]) << "element " << i << " y";
    EXPECT_EQ(first[i][2], second[i][2]) << "element " << i << " z";
  }
}

TEST_F(sculpt_layers_tree, folder_influence_multiplies_down_the_chain)
{
  /* Nested folders at 0.5 and 0.5 scale a layer at 1.0 to 0.25: #resync_group_state bakes the
   * product of every ancestor folder's influence onto the layer as #group_influence_cached, and
   * #effective multiplies the layer's own influence by that cache — one flat multiply on the hot
   * path. Halves are exact in binary floating point, so the product is exact and the assertions
   * below can be bit-for-bit. */
  SculptLayerGroup *folder_a = group_add(*mesh, "A", 0);
  ASSERT_NE(folder_a, nullptr);
  /* Nested directly inside A (parent_uid = A), so the cascade has to climb two folders, not one. */
  SculptLayerGroup *folder_b = group_add(*mesh, "B", folder_a->base.uid);
  ASSERT_NE(folder_b, nullptr);
  SculptLayer *layer = add(*mesh, "L", SCULPT_LAYER_DOMAIN_VERT, 0);
  ASSERT_NE(layer, nullptr);
  node_move_into(*mesh, layer->base, *folder_b, nullptr);

  /* #add makes the layer enabled with influence 1, so the 0.25 below can only come from the two
   * folders — a cascade that ignored either would land on 0.5 or 1.0 instead. */
  ASSERT_TRUE(layer->base.flag & SCULPT_LAYER_ENABLED);
  ASSERT_EQ(layer->influence, 1.0f);
  folder_a->influence = 0.5f;
  folder_b->influence = 0.5f;

  resync_group_state(*mesh);

  EXPECT_EQ(layer->group_influence_cached, 0.25f);
  EXPECT_EQ(effective(*layer), 0.25f);
}

TEST_F(sculpt_layers_tree, folder_influence_zero_yields_base)
{
  /* A folder dialled to 0 (while staying *enabled* — distinct from the separate
   * #SCULPT_LAYER_GROUP_HIDDEN bit) zeroes every layer below it: its influence cascades into the
   * cache as 0, so #effective returns 0 and #combine_layers_mesh leaves the base untouched. */
  SculptLayerGroup *folder = group_add(*mesh, "F", 0);
  ASSERT_NE(folder, nullptr);
  SculptLayer *layer = add(*mesh, "L", SCULPT_LAYER_DOMAIN_VERT, 3);
  ASSERT_NE(layer, nullptr);
  node_move_into(*mesh, layer->base, *folder, nullptr);

  /* Non-zero deltas, so a composition that ignored the folder's 0 influence would visibly move the
   * base and the equality below would fail. */
  const MutableSpan<float3> data = data_get(*layer);
  ASSERT_EQ(data.size(), 3);
  data[0] = float3(1.0f, 2.0f, 3.0f);
  data[1] = float3(-4.0f, 5.0f, 0.5f);
  data[2] = float3(0.25f, -1.5f, 2.0f);

  folder->influence = 0.0f;
  resync_group_state(*mesh);

  EXPECT_EQ(layer->group_influence_cached, 0.0f);
  EXPECT_EQ(effective(*layer), 0.0f);

  /* The composite is exactly the base: the one enabled layer contributes nothing through the 0
   * folder. */
  const Array<float3> base = {
      float3(0.5f, 1.0f, -2.0f), float3(3.0f, -0.25f, 4.0f), float3(-1.0f, 0.75f, 0.5f)};
  Array<float3> result(3);
  combine_layers_mesh(base, layers(*mesh), result);
  for (int i = 0; i < 3; i++) {
    EXPECT_EQ(result[i][0], base[i][0]) << "element " << i << " x";
    EXPECT_EQ(result[i][1], base[i][1]) << "element " << i << " y";
    EXPECT_EQ(result[i][2], base[i][2]) << "element " << i << " z";
  }
}

}  // namespace blender::bke::sculpt_layers::tests
