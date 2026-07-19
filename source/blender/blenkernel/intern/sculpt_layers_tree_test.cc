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

/* -------------------------------------------------------------------- */
/** \name Grid mask resampling (#resample_grid_masks)
 *
 * The mask counterpart of #resample_grid_layers. No #SubdivCCG is needed: the mask carries its own
 * geometry, so the level change is pure index math over the tree.
 * \{ */

/* Grid size at \a level, mirroring #CCG_grid_size without pulling in the CCG header. */
static int test_grid_size(const int level)
{
  return (1 << (level - 1)) + 1;
}

/** A grid mask over \a grids_num grids at \a level, every element set to \a fill. */
static SculptLayerMask *grid_mask_new(const int grids_num, const int level, const uint8_t fill)
{
  const int gs = test_grid_size(level);
  const int area = gs * gs;
  return mask_new(grids_num * area, area, fill);
}

TEST_F(sculpt_layers_tree, resample_grid_masks_moves_a_layer_mask_to_the_new_level)
{
  /* One grid, level 2 (3x3 = 9 elements) up to level 3 (5x5 = 25). The mask must land on the new
   * level's block size, or the multires paths would stop recognizing it as one block per grid. */
  SculptLayer *layer = add(*mesh, "Layer", SCULPT_LAYER_DOMAIN_GRID, 0);
  ASSERT_NE(layer, nullptr);
  layer->base.mask = grid_mask_new(1, 2, 200);
  ASSERT_NE(layer->base.mask, nullptr);
  ASSERT_EQ(layer->base.mask->totelem, 9);

  resample_grid_masks(*mesh, 1, 3);

  ASSERT_NE(layer->base.mask, nullptr);
  EXPECT_EQ(layer->base.mask->totelem, 25);
  EXPECT_EQ(layer->base.mask->block_size, 25);
  /* A uniform mask stays uniform and keeps its value through the resample: interpolating between
   * equal samples must not perturb it. */
  EXPECT_EQ(mask_value_at(*layer->base.mask, 0), 200);
  EXPECT_EQ(mask_value_at(*layer->base.mask, 24), 200);
}

TEST_F(sculpt_layers_tree, resample_grid_masks_round_trips_through_a_higher_level)
{
  /* Up then back down must be the identity: subsampling reads exactly the points upsampling
   * interpolated *through*, so the coarse samples are preserved. This is the property that keeps a
   * mask from sliding across the surface as the user steps the multires level up and down. */
  SculptLayer *layer = add(*mesh, "Layer", SCULPT_LAYER_DOMAIN_GRID, 0);
  ASSERT_NE(layer, nullptr);
  const int gs = test_grid_size(2);
  Array<float> dense(gs * gs);
  for (const int i : dense.index_range()) {
    dense[i] = float(i % 5) * 0.25f;
  }
  layer->base.mask = mask_compress(dense, gs * gs);
  ASSERT_NE(layer->base.mask, nullptr);
  Array<uint8_t> before(dense.size());
  for (const int i : dense.index_range()) {
    before[i] = mask_value_at(*layer->base.mask, i);
  }

  resample_grid_masks(*mesh, 1, 4);
  ASSERT_EQ(layer->base.mask->totelem, test_grid_size(4) * test_grid_size(4));
  resample_grid_masks(*mesh, 1, 2);

  ASSERT_EQ(layer->base.mask->totelem, gs * gs);
  for (const int i : dense.index_range()) {
    EXPECT_EQ(mask_value_at(*layer->base.mask, i), before[i]) << "element " << i;
  }
}

TEST_F(sculpt_layers_tree, resample_grid_masks_walks_folders_too)
{
  /* A folder's mask attenuates its whole subtree through #chain_mask, so it has to move with the
   * level like a layer's own. Left behind it would be rejected as stale and the subtree would
   * silently spring back to its full contribution — the failure mode is *more* visible than a lost
   * layer mask, not less. */
  SculptLayerGroup *folder = group_add(*mesh, "Folder", 0);
  ASSERT_NE(folder, nullptr);
  folder->base.mask = grid_mask_new(1, 2, 90);
  root_group(*mesh)->base.mask = grid_mask_new(1, 2, 30);

  resample_grid_masks(*mesh, 1, 3);

  ASSERT_NE(folder->base.mask, nullptr);
  EXPECT_EQ(folder->base.mask->totelem, 25);
  EXPECT_EQ(mask_value_at(*folder->base.mask, 0), 90);
  /* The root is the one node a children-only walk would miss. */
  ASSERT_NE(root_group(*mesh)->base.mask, nullptr);
  EXPECT_EQ(root_group(*mesh)->base.mask->totelem, 25);
  EXPECT_EQ(mask_value_at(*root_group(*mesh)->base.mask, 0), 30);
}

TEST_F(sculpt_layers_tree, resample_grid_masks_leaves_vertex_masks_alone)
{
  /* Grid masks are recognized by their own geometry rather than by the domain of the node they hang
   * off — a folder has no domain at all. A vertex mask must not be mistaken for one. */
  SculptLayer *layer = add(*mesh, "Layer", SCULPT_LAYER_DOMAIN_VERT, 0);
  ASSERT_NE(layer, nullptr);
  layer->base.mask = mask_new(4096, SCULPT_LAYER_MASK_VERT_BLOCK, 111);
  const SculptLayerMask *before = layer->base.mask;

  resample_grid_masks(*mesh, 1, 3);

  EXPECT_EQ(layer->base.mask, before);
  EXPECT_EQ(layer->base.mask->totelem, 4096);
  EXPECT_EQ(layer->base.mask->block_size, SCULPT_LAYER_MASK_VERT_BLOCK);
}

TEST_F(sculpt_layers_tree, resample_grid_masks_drops_grid_masks_when_no_levels_remain)
{
  SculptLayer *layer = add(*mesh, "Layer", SCULPT_LAYER_DOMAIN_GRID, 0);
  ASSERT_NE(layer, nullptr);
  layer->base.mask = grid_mask_new(1, 2, 200);

  resample_grid_masks(*mesh, 1, 0);

  /* No grid domain is left for a mask to describe. */
  EXPECT_EQ(layer->base.mask, nullptr);
}

/** \} */

TEST_F(sculpt_layers_tree, tree_copy_deep_copies_masks)
{
  /* #tree_copy shallow-copies each DNA node, so a mask pointer carried over verbatim would be freed
   * twice when both meshes are released — and would let one mesh's mask edits appear on the other.
   * This is the same hazard already handled for #SculptLayerGroup::runtime. */
  SculptLayer *layer = add(*mesh, "Layer", SCULPT_LAYER_DOMAIN_VERT, 0);
  ASSERT_NE(layer, nullptr);
  layer->base.mask = mask_new(4096, SCULPT_LAYER_MASK_VERT_BLOCK, 200);
  ASSERT_NE(layer->base.mask, nullptr);

  /* The root carries a mask too: it is the one node #group_copy_children never visits, so a copy
   * that only walked the children would leave the two roots sharing one mask. */
  root_group(*mesh)->base.mask = mask_new(4096, SCULPT_LAYER_MASK_VERT_BLOCK, 100);

  /* A folder nested inside the root exercises #group_copy_children's group branch, the one mask
   * copy site the top-level layer and the root above do not reach. */
  SculptLayerGroup *folder = group_add(*mesh, "Folder", 0);
  ASSERT_NE(folder, nullptr);
  folder->base.mask = mask_new(4096, SCULPT_LAYER_MASK_VERT_BLOCK, 50);
  const int folder_uid = folder->base.uid;

  Mesh *copy = BKE_mesh_new_nomain(0, 0, 0, 0);
  tree_copy(*copy, *mesh);

  const Span<SculptLayer *> copied = layers(*root_group(*copy));
  ASSERT_EQ(copied.size(), 1);
  ASSERT_NE(copied[0]->base.mask, nullptr);
  EXPECT_NE(copied[0]->base.mask, layer->base.mask);
  EXPECT_EQ(mask_value_at(*copied[0]->base.mask, 0), 200);

  ASSERT_NE(root_group(*copy)->base.mask, nullptr);
  EXPECT_NE(root_group(*copy)->base.mask, root_group(*mesh)->base.mask);
  EXPECT_EQ(mask_value_at(*root_group(*copy)->base.mask, 0), 100);

  SculptLayerGroup *copied_folder = node_as_group(node_find_by_uid(*copy, folder_uid));
  ASSERT_NE(copied_folder, nullptr);
  ASSERT_NE(copied_folder->base.mask, nullptr);
  EXPECT_NE(copied_folder->base.mask, folder->base.mask);
  EXPECT_EQ(mask_value_at(*copied_folder->base.mask, 0), 50);

  BKE_id_free(nullptr, copy);
  /* Must still be readable: the copy's destruction may not have touched these masks. */
  EXPECT_EQ(mask_value_at(*layer->base.mask, 0), 200);
  EXPECT_EQ(mask_value_at(*root_group(*mesh)->base.mask, 0), 100);
  EXPECT_EQ(mask_value_at(*folder->base.mask, 0), 50);
}

TEST_F(sculpt_layers_tree, chain_mask_is_null_without_masks)
{
  /* A tree with no masks at all must hand back null, not a mask full of ones: null is what lets the
   * composite skip the masked paths entirely. */
  SculptLayerGroup *group = group_add(*mesh, "Folder", 0);
  ASSERT_NE(group, nullptr);
  EXPECT_EQ(chain_mask(*group), nullptr);
}

TEST_F(sculpt_layers_tree, chain_mask_multiplies_down_the_tree)
{
  SculptLayerGroup *outer = group_add(*mesh, "Outer", 0);
  ASSERT_NE(outer, nullptr);
  SculptLayerGroup *inner = group_add(*mesh, "Inner", outer->base.uid);
  ASSERT_NE(inner, nullptr);
  outer->base.mask = mask_new(4096, SCULPT_LAYER_MASK_VERT_BLOCK, 255);
  inner->base.mask = mask_new(4096, SCULPT_LAYER_MASK_VERT_BLOCK, 128);

  const SculptLayerMask *chain = chain_mask(*inner);
  ASSERT_NE(chain, nullptr);
  EXPECT_EQ(mask_value_at(*chain, 0), 128);
}

TEST_F(sculpt_layers_tree, chain_mask_invalidates_on_ancestor_edit)
{
  /* Missing this invalidation gives a use-after-free, not a stale UI row — the same hazard already
   * documented for #SculptLayerGroupRuntime::layer_cache_. */
  SculptLayerGroup *outer = group_add(*mesh, "Outer", 0);
  ASSERT_NE(outer, nullptr);
  SculptLayerGroup *inner = group_add(*mesh, "Inner", outer->base.uid);
  ASSERT_NE(inner, nullptr);
  outer->base.mask = mask_new(4096, SCULPT_LAYER_MASK_VERT_BLOCK, 255);
  ASSERT_NE(chain_mask(*inner), nullptr);
  EXPECT_EQ(mask_value_at(*chain_mask(*inner), 0), 255);

  mask_free(outer->base.mask);
  outer->base.mask = mask_new(4096, SCULPT_LAYER_MASK_VERT_BLOCK, 0);
  /* #tag_chain_mask_dirty, not #tag_layers_cache_dirty: a mask edit changes no folder's membership,
   * and the two caches propagate in opposite directions from separate entry points. */
  tag_chain_mask_dirty(*outer);

  ASSERT_NE(chain_mask(*inner), nullptr);
  EXPECT_EQ(mask_value_at(*chain_mask(*inner), 0), 0);
}

TEST_F(sculpt_layers_tree, chain_mask_folds_in_the_root_mask)
{
  /* The root is a folder like any other on this walk, and the one whose mask nothing else in the
   * module ever reaches: #group_copy_children and #group_blend_read_children both only descend into
   * *children*, so a chain that started at the top-level folder instead would look correct
   * everywhere except here — and would drop a mask the user applied to the whole mesh. */
  root_group(*mesh)->base.mask = mask_new(4096, SCULPT_LAYER_MASK_VERT_BLOCK, 128);
  SculptLayerGroup *folder = group_add(*mesh, "Folder", 0);
  ASSERT_NE(folder, nullptr);

  /* The root's own chain is just its own mask: it has no ancestor to fold in. */
  const SculptLayerMask *root_chain = chain_mask(*root_group(*mesh));
  ASSERT_NE(root_chain, nullptr);
  EXPECT_EQ(mask_value_at(*root_chain, 0), 128);
  /* Copied rather than aliased, as every other level is — the runtime owns what it hands out. */
  EXPECT_NE(root_chain, root_group(*mesh)->base.mask);

  /* The folder carries no mask of its own, so its chain is the root's alone rather than null. */
  const SculptLayerMask *folder_chain = chain_mask(*folder);
  ASSERT_NE(folder_chain, nullptr);
  EXPECT_EQ(mask_value_at(*folder_chain, 0), 128);

  /* And with a mask of its own the two multiply: 128 by 128 over 255 rounds to 64. */
  folder->base.mask = mask_new(4096, SCULPT_LAYER_MASK_VERT_BLOCK, 128);
  tag_chain_mask_dirty(*folder);
  ASSERT_NE(chain_mask(*folder), nullptr);
  EXPECT_EQ(mask_value_at(*chain_mask(*folder), 0), 64);
}

TEST_F(sculpt_layers_tree, composite_applies_layer_mask)
{
  /* Masked elements must land at base, unmasked at base + offset. Verified through the public
   * composite so the block dispatch is exercised, not just the container.
   *
   * 8192 elements is two full blocks: the first is dense (one element differs) and the second is
   * uniform, so one call covers both sides of the dispatch. #combine_layers_mesh never consults the
   * mesh, so the fixture's empty mesh is irrelevant — the layer is sized to the arrays below. */
  SculptLayer *layer = add(*mesh, "Layer", SCULPT_LAYER_DOMAIN_VERT, 8192);
  ASSERT_NE(layer, nullptr);
  const MutableSpan<float3> offsets = data_get(*layer);
  ASSERT_EQ(offsets.size(), 8192);
  offsets.fill(float3(0.0f, 0.0f, 1.0f));

  Array<float> dense(8192, 1.0f);
  dense[10] = 0.0f;
  layer->base.mask = mask_compress(dense, SCULPT_LAYER_MASK_VERT_BLOCK);
  ASSERT_NE(layer->base.mask, nullptr);
  ASSERT_EQ(layer->base.mask->block_kind[0], SCULPT_LAYER_MASK_BLOCK_DENSE);
  ASSERT_EQ(layer->base.mask->block_kind[1], SCULPT_LAYER_MASK_BLOCK_UNIFORM);

  const Array<float3> base(8192, float3(0.0f));
  Array<float3> result(8192);
  combine_layers_mesh(base, layers(*mesh), result);

  EXPECT_NEAR(result[10].z, 0.0f, 1e-5f) << "the masked element must stay at the base";
  EXPECT_NEAR(result[11].z, 1.0f, 1e-5f) << "its neighbour in the same dense block must not";
  EXPECT_NEAR(result[5000].z, 1.0f, 1e-5f) << "the uniform block must fold to a plain scalar";

  /* The inverse must undo exactly what the forward pass did, mask and all: #derive_base_mesh is
   * what recovers the un-layered base on every flush, so a weight the two spell differently would
   * drift the base a little each time — silently and cumulatively. */
  Array<float3> derived(8192);
  derive_base_mesh(result, layers(*mesh), derived);
  for (const int i : {10, 11, 5000}) {
    EXPECT_EQ(derived[i].z, base[i].z) << "element " << i;
  }
}

TEST_F(sculpt_layers_tree, composite_applies_folder_mask_to_a_masked_layer)
{
  /* A layer mask and a folder chain mask must both apply, multiplied. This is the one case the
   * per-block dispatch has to carry two masks through at once, and the case where a forward and an
   * inverse spelled separately would disagree. */
  SculptLayerGroup *folder = group_add(*mesh, "Folder", 0);
  ASSERT_NE(folder, nullptr);
  folder->base.mask = mask_new(8192, SCULPT_LAYER_MASK_VERT_BLOCK, 128);
  ASSERT_NE(folder->base.mask, nullptr);
  /* The chain-mask cache happens to be cold here, so the tag is not what makes this test pass today
   * — it is what keeps it testing the mask instead of the cache's initial state. */
  tag_chain_mask_dirty(*folder);

  SculptLayer *layer = add(*mesh, "Layer", SCULPT_LAYER_DOMAIN_VERT, 8192);
  ASSERT_NE(layer, nullptr);
  node_move_into(*mesh, layer->base, *folder, nullptr);
  data_get(*layer).fill(float3(0.0f, 0.0f, 1.0f));

  Array<float> dense(8192, 1.0f);
  dense[10] = 0.0f;
  layer->base.mask = mask_compress(dense, SCULPT_LAYER_MASK_VERT_BLOCK);
  ASSERT_NE(layer->base.mask, nullptr);

  const Array<float3> base(8192, float3(0.0f));
  Array<float3> result(8192);
  combine_layers_mesh(base, layers(*mesh), result);

  EXPECT_NEAR(result[10].z, 0.0f, 1e-5f) << "zero on either side annihilates the element";
  /* 255 by 128 over 255 squared: the folder halves what the layer lets through. */
  EXPECT_NEAR(result[11].z, 128.0f / 255.0f, 1e-5f);
  EXPECT_NEAR(result[5000].z, 128.0f / 255.0f, 1e-5f);

  Array<float3> derived(8192);
  derive_base_mesh(result, layers(*mesh), derived);
  for (const int i : {10, 11, 5000}) {
    EXPECT_NEAR(derived[i].z, base[i].z, 1e-6f) << "element " << i;
  }
}

TEST_F(sculpt_layers_tree, composite_ignores_a_mask_of_the_wrong_size)
{
  /* A mask sized to a stale element count cannot be indexed over the live domain, so the layer must
   * compose unmasked rather than be dropped or read out of bounds. The same guard is what keeps a
   * mask neutralized by #mask_blend_read (`totelem == 0`, `blocks_num == 0`) off the block loop:
   * looping over zero blocks would silently drop the layer's whole contribution. */
  SculptLayer *layer = add(*mesh, "Layer", SCULPT_LAYER_DOMAIN_VERT, 3);
  ASSERT_NE(layer, nullptr);
  data_get(*layer).fill(float3(0.0f, 0.0f, 1.0f));
  layer->base.mask = mask_new(4096, SCULPT_LAYER_MASK_VERT_BLOCK, 0);
  ASSERT_NE(layer->base.mask, nullptr);

  const Array<float3> base(3, float3(0.0f));
  Array<float3> result(3);
  combine_layers_mesh(base, layers(*mesh), result);
  for (const int i : IndexRange(3)) {
    EXPECT_EQ(result[i].z, 1.0f) << "element " << i;
  }
}

TEST_F(sculpt_layers_tree, composite_uniform_zero_and_opaque_blocks_have_correct_values)
{
  /* A uniform-zero block must contribute nothing and a uniform-opaque (255) block must contribute
   * fully, which is the one case the other composite tests never isolate: their zero is a single
   * element inside a dense block, not a whole uniform one. Block 0 is uniformly zero and block 1
   * uniformly opaque, so both values are pinned in one call.
   *
   * This is a value test, not a branch test: #accumulate_layer has a `continue` that skips a
   * uniform-zero block outright as a performance shortcut (a masked layer must stay cheaper than an
   * unmasked one over a parked region). Removing that `continue` would make the loop add `data *
   * 0.0f` instead, which is numerically identical, so every assertion below would still pass. This
   * test therefore cannot detect whether the shortcut is taken; it only pins the values it must not
   * change. */
  SculptLayer *layer = add(*mesh, "Layer", SCULPT_LAYER_DOMAIN_VERT, 8192);
  ASSERT_NE(layer, nullptr);
  data_get(*layer).fill(float3(0.0f, 0.0f, 1.0f));

  Array<float> dense(8192, 1.0f);
  dense.as_mutable_span().slice(0, SCULPT_LAYER_MASK_VERT_BLOCK).fill(0.0f);
  layer->base.mask = mask_compress(dense, SCULPT_LAYER_MASK_VERT_BLOCK);
  ASSERT_NE(layer->base.mask, nullptr);
  ASSERT_EQ(layer->base.mask->block_kind[0], SCULPT_LAYER_MASK_BLOCK_UNIFORM);
  ASSERT_EQ(layer->base.mask->block_kind[1], SCULPT_LAYER_MASK_BLOCK_UNIFORM);

  const Array<float3> base(8192, float3(0.0f));
  Array<float3> result(8192);
  combine_layers_mesh(base, layers(*mesh), result);

  EXPECT_NEAR(result[0].z, 0.0f, 1e-5f) << "the skipped block must be left at the base";
  EXPECT_NEAR(result[4095].z, 0.0f, 1e-5f) << "including its last element";
  EXPECT_NEAR(result[4096].z, 1.0f, 1e-5f) << "the opaque block must still receive the offset";

  /* Skipping a block on the way out has to skip the same block on the way back, or the base would
   * drift by the layer's offset over exactly the region the user parked. */
  Array<float3> derived(8192);
  derive_base_mesh(result, layers(*mesh), derived);
  for (const int i : {0, 4095, 4096}) {
    EXPECT_EQ(derived[i].z, base[i].z) << "element " << i;
  }
}

TEST_F(sculpt_layers_tree, composite_ignores_a_mask_describing_no_blocks)
{
  /* The other half of #is_stale_mask: a mask that names the right element count but describes no
   * blocks at all, which is how #mask_blend_read neutralizes a mask from a truncated file. The
   * layer must compose FULLY — a block loop over zero blocks would add nothing at all, and
   * "silently drops the layer's contribution" is precisely the failure the guard exists to stop. */
  SculptLayer *layer = add(*mesh, "Layer", SCULPT_LAYER_DOMAIN_VERT, 8192);
  ASSERT_NE(layer, nullptr);
  data_get(*layer).fill(float3(0.0f, 0.0f, 1.0f));

  layer->base.mask = mask_new(8192, SCULPT_LAYER_MASK_VERT_BLOCK, 0);
  ASSERT_NE(layer->base.mask, nullptr);
  /* Only the block count is neutralized; the arrays stay allocated so #mask_free still owns them. */
  layer->base.mask->blocks_num = 0;
  EXPECT_TRUE(is_stale_mask(*layer->base.mask, 8192));

  const Array<float3> base(8192, float3(0.0f));
  Array<float3> result(8192);
  combine_layers_mesh(base, layers(*mesh), result);
  for (const int i : {0, 4096, 8191}) {
    EXPECT_NEAR(result[i].z, 1.0f, 1e-5f) << "element " << i;
  }
}

TEST_F(sculpt_layers_tree, composite_masks_a_short_tail_block)
{
  /* 5000 elements is one full block and a 904-element tail. The container's own tail handling is
   * pinned by `sculpt_layers_mask.tail_block_is_partial`; what is asserted here is the *composite's*
   * per-block dispatch over that tail, which derives its extent from the position span rather than
   * from the block size and would otherwise run 4096 elements off the end of a 904-element block. */
  SculptLayer *layer = add(*mesh, "Layer", SCULPT_LAYER_DOMAIN_VERT, 5000);
  ASSERT_NE(layer, nullptr);
  data_get(*layer).fill(float3(0.0f, 0.0f, 1.0f));

  Array<float> dense(5000, 1.0f);
  /* One masked element in each block, so the full block and the short tail are both dense. */
  dense[10] = 0.0f;
  dense[4999] = 0.0f;
  layer->base.mask = mask_compress(dense, SCULPT_LAYER_MASK_VERT_BLOCK);
  ASSERT_NE(layer->base.mask, nullptr);
  ASSERT_EQ(layer->base.mask->blocks_num, 2);

  const Array<float3> base(5000, float3(0.0f));
  Array<float3> result(5000);
  combine_layers_mesh(base, layers(*mesh), result);

  EXPECT_NEAR(result[10].z, 0.0f, 1e-5f) << "masked element in the full block";
  EXPECT_NEAR(result[11].z, 1.0f, 1e-5f) << "its neighbour in the full block";
  EXPECT_NEAR(result[4096].z, 1.0f, 1e-5f) << "first element of the short tail";
  EXPECT_NEAR(result[4998].z, 1.0f, 1e-5f) << "next to last element of the short tail";
  EXPECT_NEAR(result[4999].z, 0.0f, 1e-5f) << "masked last element of the short tail";

  Array<float3> derived(5000);
  derive_base_mesh(result, layers(*mesh), derived);
  for (const int i : {10, 11, 4096, 4998, 4999}) {
    EXPECT_EQ(derived[i].z, base[i].z) << "element " << i;
  }
}

/**
 * Assert that every element of \a result carries exactly the weight #mask_block_weight and
 * #mask_elem_weight say it should, given \a layer's resolved masks and its effective influence.
 *
 * The caller arranges for the layer's data to be `(0, 0, 1)` everywhere over a zero base, so a
 * composed `z` *is* the weight the composite applied — which makes this an equality test, not an
 * approximate one. Bit-exact deliberately: the failure this guards against is a fold that differs
 * in the last place, and a tolerance would hide precisely that.
 */
static void expect_composite_weights_match_authority(const SculptLayer &layer,
                                                     const Span<float3> result,
                                                     const float weight)
{
  const CompositeMask masks = node_mask_for_composite(layer, result.size());
  ASSERT_NE(masks.primary, nullptr);
  const int block_size = masks.primary->block_size;
  for (const int64_t i : result.index_range()) {
    const MaskBlockWeight block_weight = mask_block_weight(masks, int(i / block_size), weight);
    /* A skipped block contributes nothing, so over a zero base its elements stay at zero. */
    const float expected = block_weight.skip ? 0.0f :
                                               mask_elem_weight(block_weight, int(i % block_size));
    EXPECT_EQ(result[i].z, expected) << "element " << i;
  }
}

TEST_F(sculpt_layers_tree, composite_weights_match_the_shared_mask_authority)
{
  /* The cross-path oracle for the mask fold. #mask_block_weight / #mask_elem_weight are the single
   * spelling of a masked layer's weight, shared by the vertex composite here and by the multires
   * grid composite; a second spelling anywhere would drift the base silently on every flush. The
   * companion test in `sculpt_layers_mask_test.cc` pins those two functions against literals, which
   * says nothing about whether the vertex composite actually calls them — so this drives the *real*
   * public composite (#combine_layers_mesh) and compares every element against the authority
   * computed independently.
   *
   * It fails if #accumulate_layer ever reintroduces its own arithmetic, reassociates the two-mask
   * product, scales the sides separately, mishandles the short tail block, or skips a block the
   * authority does not (and the reverse). It does *not* police mask resolution — both sides ask
   * #node_mask_for_composite — only the fold applied on top of it.
   *
   * The domain is 5000 elements over a 4096-element block: one full block, one short tail. */
  const int totelem = 5000;
  SculptLayerGroup *folder = group_add(*mesh, "Folder", 0);
  ASSERT_NE(folder, nullptr);

  SculptLayer *layer = add(*mesh, "Layer", SCULPT_LAYER_DOMAIN_VERT, totelem);
  ASSERT_NE(layer, nullptr);
  node_move_into(*mesh, layer->base, *folder, nullptr);
  /* A unit offset along z, so a composed z reads back as the weight that was applied to it. */
  data_get(*layer).fill(float3(0.0f, 0.0f, 1.0f));

  /* Block 0 varies per element (including both extremes) so it stays dense; the tail block is
   * constant so #mask_compress collapses it to uniform. One call therefore covers both block kinds
   * on each side of the single/pair dispatch. */
  Array<float> dense(totelem);
  for (const int i : IndexRange(SCULPT_LAYER_MASK_VERT_BLOCK)) {
    dense[i] = float(i % 256) / 255.0f;
  }
  dense.as_mutable_span().drop_front(SCULPT_LAYER_MASK_VERT_BLOCK).fill(0.5f);
  layer->base.mask = mask_compress(dense, SCULPT_LAYER_MASK_VERT_BLOCK);
  ASSERT_NE(layer->base.mask, nullptr);
  ASSERT_EQ(layer->base.mask->blocks_num, 2);
  ASSERT_EQ(layer->base.mask->block_kind[0], SCULPT_LAYER_MASK_BLOCK_DENSE);
  ASSERT_EQ(layer->base.mask->block_kind[1], SCULPT_LAYER_MASK_BLOCK_UNIFORM);

  folder->base.mask = mask_new(totelem, SCULPT_LAYER_MASK_VERT_BLOCK, 128);
  ASSERT_NE(folder->base.mask, nullptr);
  tag_chain_mask_dirty(*folder);

  const float eff = effective(*layer);
  ASSERT_GT(eff, 0.0f) << "a layer the composite skips outright would pass this test vacuously";

  const Array<float3> base(totelem, float3(0.0f));
  Array<float3> result(totelem);

  /* Two masks: the dense block takes the mixed pair branch, the uniform tail the uniform pair. */
  combine_layers_mesh(base, layers(*mesh), result);
  expect_composite_weights_match_authority(*layer, result, eff);

  /* The inverse is the same fold with a negated weight and must cancel bit for bit, which is what
   * keeps the base from creeping on every flush. */
  Array<float3> derived(totelem);
  derive_base_mesh(result, layers(*mesh), derived);
  for (const int64_t i : derived.index_range()) {
    EXPECT_EQ(derived[i].z, base[i].z) << "element " << i;
  }

  /* One mask: dropping the folder's mask leaves the layer's own, which exercises the single-mask
   * branches over the very same blocks. */
  mask_free(folder->base.mask);
  folder->base.mask = nullptr;
  tag_chain_mask_dirty(*folder);
  combine_layers_mesh(base, layers(*mesh), result);
  expect_composite_weights_match_authority(*layer, result, eff);
}

/* -------------------------------------------------------------------- */
/** \name Corrupt tree guards
 * \{ */

/**
 * Point \a group's child list at \a node without touching \a node's own links.
 *
 * This is the one way to reproduce what a damaged `.blend` produces: \a node stays a member of the
 * list it really belongs to — its #SculptLayerTreeNode::next and #SculptLayerTreeNode::prev are left
 * alone — while a second #ListBase claims it as well. A walk from the root then reaches \a node
 * twice, which is the only shape a cycle can take in an intrusive list.
 *
 * #node_move_into cannot express this: it maintains both ends, so a folder moved below itself leaves
 * the root's reach entirely and no descendant walk ever sees it. That is a different failure (lost
 * subtree, covered by #move_into_own_subtree_is_refused) and would not exercise these guards.
 */
static void graft_child_raw(SculptLayerGroup &group, SculptLayerTreeNode &node)
{
  group.children.first = &node;
  group.children.last = &node;
}

/** Builds `root -> [Outer -> [Inner], Tail]`, then makes `Inner`'s list claim `Outer` as well. */
static void build_interleaved_mesh(Mesh &mesh)
{
  SculptLayerGroup *outer = group_add(mesh, "Outer", 0);
  SculptLayerGroup *inner = group_add(mesh, "Inner", 0);
  add(mesh, "Tail", SCULPT_LAYER_DOMAIN_VERT, 0);
  node_move_into(mesh, inner->base, *outer, nullptr);
  graft_child_raw(*inner, outer->base);
}

TEST_F(sculpt_layers_tree, free_terminates_on_interleaved_child_lists)
{
  /* Without the visited set #group_gather_owned descends Outer -> Inner -> Outer until the stack
   * runs out; with it, every node is collected — and so released — exactly once. Reaching the end of
   * this test at all is the assertion; a second release of the same node, or a link read after the
   * node holding it was freed, is what the address sanitizer catches. */
  build_interleaved_mesh(*mesh);

  tree_free(*mesh);

  EXPECT_EQ(mesh->sculpt_layer_root, nullptr);
}

TEST_F(sculpt_layers_tree, copy_terminates_on_interleaved_child_lists)
{
  /* The same corruption through the copy path: the node claimed by two lists is copied once, so the
   * copy is a finite tree rather than an unbounded expansion of the repeated branch. */
  build_interleaved_mesh(*mesh);

  Mesh *copy = BKE_mesh_new_nomain(0, 0, 0, 0);
  tree_copy(*copy, *mesh);

  ASSERT_NE(root_group(*copy), nullptr);
  Vector<SculptLayerTreeNode *> nodes;
  gather_nodes(*root_group(*copy), nodes);
  EXPECT_LE(nodes.size(), 3) << "the repeated branch was copied more than once";

  BKE_id_free(nullptr, copy);
}

TEST_F(sculpt_layers_tree, move_into_own_subtree_is_refused)
{
  /* Moving a folder below itself takes the whole subtree out of the root's reach: the node leaves
   * the list the root can walk and joins one only the subtree can. Every descendant walk starts at
   * the root, so nothing would notice — the layers would simply be gone, and leak in #tree_free.
   * Release builds have to refuse this, not merely assert it. */
  SculptLayerGroup *outer = group_add(*mesh, "Outer", 0);
  SculptLayerGroup *inner = group_add(*mesh, "Inner", 0);
  node_move_into(*mesh, inner->base, *outer, nullptr);

  ASSERT_NE(root_group(*mesh), nullptr);
  SculptLayerGroup &root = *root_group(*mesh);

  node_move_into(*mesh, outer->base, *inner, nullptr);

  EXPECT_EQ(outer->base.parent, &root) << "the folder was moved into its own subtree";
  EXPECT_EQ(children_of(root).size(), 1);
  EXPECT_EQ(children_of(*outer).size(), 1);
}

/** \} */

}  // namespace blender::bke::sculpt_layers::tests
