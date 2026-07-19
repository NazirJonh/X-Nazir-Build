/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "BKE_sculpt_layers.hh"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <type_traits>

#include "CLG_log.h"

#include "MEM_guardedalloc.h"

#include "DNA_key_types.h"
#include "DNA_mesh_types.h"

#include "BLI_array.hh"
#include "BLI_listbase.h"
#include "BLI_string_utf8.h"
#include "BLI_string_utils.hh"
#include "BLI_task.hh"

#include "BLT_translation.hh"

#include "BKE_ccg.hh"
#include "BKE_key.hh"
#include "BKE_multires_grid_resample.hh"

#include "BLO_read_write.hh"

static CLG_LogRef LOG = {"bke.sculpt_layers"};

namespace blender::bke::sculpt_layers {

/* The base shared by both node types. Asserted on its own rather than left to the two asserts below
 * to catch it transitively, because it is the one place a non-trivial member would break every free
 * path at once. */
static_assert(std::is_trivially_destructible_v<SculptLayerTreeNode>,
              "SculptLayerTreeNode must remain trivially destructible (see the allocation notes in "
              "BKE_sculpt_layers.hh)");

/* No destructor ever runs on a layer: #tree_free and #remove release the nodes with
 * #MEM_delete_void, because the blend-file reader allocates its own layers C-style and the free
 * side has to accept both origins. See the allocation notes in `BKE_sculpt_layers.hh`. */
static_assert(std::is_trivially_destructible_v<SculptLayer>,
              "SculptLayer must remain trivially destructible (see the allocation notes in "
              "BKE_sculpt_layers.hh)");

/* Groups are freed with #MEM_delete_void as well (see #tree_free), for the same reason: the
 * blend-file reader allocates them C-style. Asserted separately so that adding a non-trivial member
 * to the group breaks the build here rather than corrupting the free path silently. Note this still
 * holds now that a group owns its #SculptLayerGroup::children: that is a #ListBase of raw links, not
 * an owning container, and #tree_free is what walks it. */
static_assert(std::is_trivially_destructible_v<SculptLayerGroup>,
              "SculptLayerGroup must remain trivially destructible (see the allocation notes in "
              "BKE_sculpt_layers.hh)");

/* The counterpart of the assert above, and the reason #SculptLayerGroup::runtime is a pointer: the
 * runtime owns a #CacheMutex and a #Vector, so it is *not* trivially destructible and could never be
 * a by-value member of a group without breaking every #MEM_delete_void free path at once. Held out
 * of line it is a raw pointer to the group (trivially destructible) and a #MEM_new / #MEM_delete
 * pair of its own. Asserted rather than assumed, so that a future runtime trimmed down to trivial
 * members does not quietly make this indirection look unmotivated. */
static_assert(!std::is_trivially_destructible_v<SculptLayerGroupRuntime>,
              "SculptLayerGroupRuntime is held behind a pointer precisely because it needs a "
              "destructor; see group_runtime_free");

/* The sanitized shallow copies in #group_blend_write_recursive rely on these: the group's clears the
 * runtime pointer, the layer's clears #SCULPT_LAYER_REC_EXEMPT. */
static_assert(std::is_trivially_copyable_v<SculptLayerGroup>,
              "SculptLayerGroup must stay trivially copyable: group_blend_write_recursive writes a "
              "by-value copy with the runtime pointer cleared");
static_assert(std::is_trivially_copyable_v<SculptLayer>,
              "SculptLayer must stay trivially copyable: group_blend_write_recursive writes a "
              "by-value copy with the REC exemption bit cleared");

/* -------------------------------------------------------------------- */
/** \name Tree structure
 * \{ */

SculptLayer *node_as_layer(SculptLayerTreeNode *node)
{
  if (node == nullptr || node->type != SCULPT_LAYER_TREE_NODE_TYPE_LAYER) {
    return nullptr;
  }
  return reinterpret_cast<SculptLayer *>(node);
}

const SculptLayer *node_as_layer(const SculptLayerTreeNode *node)
{
  return node_as_layer(const_cast<SculptLayerTreeNode *>(node));
}

SculptLayerGroup *node_as_group(SculptLayerTreeNode *node)
{
  if (node == nullptr || node->type != SCULPT_LAYER_TREE_NODE_TYPE_GROUP) {
    return nullptr;
  }
  return reinterpret_cast<SculptLayerGroup *>(node);
}

const SculptLayerGroup *node_as_group(const SculptLayerTreeNode *node)
{
  return node_as_group(const_cast<SculptLayerTreeNode *>(node));
}

SculptLayerGroup *root_group(Mesh &mesh)
{
  BLI_assert(mesh.sculpt_layer_root != nullptr);
  return mesh.sculpt_layer_root;
}

const SculptLayerGroup *root_group(const Mesh &mesh)
{
  BLI_assert(mesh.sculpt_layer_root != nullptr);
  return mesh.sculpt_layer_root;
}

void root_group_ensure(Mesh &mesh)
{
  if (mesh.sculpt_layer_root != nullptr) {
    return;
  }
  SculptLayerGroup *root = MEM_new<SculptLayerGroup>(__func__);
  root->base.type = SCULPT_LAYER_TREE_NODE_TYPE_GROUP;
  /* Uid 0 and no parent are what identify the root to every walk. */
  root->base.uid = 0;
  root->base.parent = nullptr;
  /* The root takes part in the ancestor walks like any other folder, so a root left at flag 0 would
   * read as a *disabled* folder and hide every layer on the mesh. It has no UI toggle, so nothing
   * would ever clear that. */
  root->base.flag = SCULPT_LAYER_GROUP_ENABLED | SCULPT_LAYER_GROUP_EXPANDED;
  /* A fresh runtime starts dirty, so the empty tree's span is built on the first read rather than
   * assumed. */
  group_runtime_ensure(*root);
  mesh.sculpt_layer_root = root;
}

void group_runtime_ensure(SculptLayerGroup &group)
{
  if (group.runtime == nullptr) {
    group.runtime = MEM_new<SculptLayerGroupRuntime>(__func__);
  }
}

/* Defined out of line rather than in the header, because #mask_free is declared far below
 * #SculptLayerGroupRuntime there. The cached product is owned by the runtime just as the layer
 * span's backing vector is, so it is released the same way — by the destructor — rather than by
 * hand at each of the runtime's free sites. */
SculptLayerGroupRuntime::~SculptLayerGroupRuntime()
{
  mask_free(chain_mask_);
}

void group_runtime_free(SculptLayerGroup &group)
{
  /* #MEM_delete rather than the #MEM_delete_void every other part of a node is freed with: the
   * runtime is the one piece that is always allocated here (never by the blend-file reader) and is
   * not trivially destructible, so its destructor has to run to release the cached vector and the
   * cached chain mask. */
  MEM_delete(group.runtime);
  group.runtime = nullptr;
}

/* Recursive rather than iterative — unlike the ancestor chain in #tag_layers_cache_dirty this is a
 * subtree, so there is something to unwind. Unguarded against cycles like the module's other
 * descendant walks (#groups_gather, #tree_free): the child lists are what a cycle would have to
 * corrupt, and #chain_mask carries the guard for the ancestor direction it actually walks. */
void tag_chain_mask_dirty(SculptLayerGroup &group)
{
  BLI_assert(group.runtime != nullptr);
  group.runtime->chain_mask_mutex_.tag_dirty();
  for (SculptLayerTreeNode &node : group.children) {
    if (SculptLayerGroup *child = node_as_group(&node)) {
      tag_chain_mask_dirty(*child);
    }
  }
}

void tag_layers_cache_dirty(SculptLayerGroup &group)
{
  /* Up to the root, not down: a folder's span holds every layer *below* it, so a change anywhere in
   * a subtree invalidates every ancestor's span as well, while the subtree's own caches still
   * describe their own unchanged contents. Iterative rather than recursive — the chain is a tree
   * path, so there is nothing to unwind. */
  for (SculptLayerGroup *ancestor = &group; ancestor != nullptr;
       ancestor = ancestor->base.parent)
  {
    BLI_assert(ancestor->runtime != nullptr);
    ancestor->runtime->layer_cache_mutex_.tag_dirty();
  }
}

Span<SculptLayer *> layers(const SculptLayerGroup &group)
{
  /* Not allocated on demand: this is const and runs from evaluation threads, so a lazy allocation
   * here would be a data race. Every path that creates a group calls #group_runtime_ensure. */
  BLI_assert(group.runtime != nullptr);
  group.runtime->layer_cache_mutex_.ensure([&]() {
    Vector<SculptLayer *> &cache = group.runtime->layer_cache_;
    /* #clear rather than #clear_and_shrink: the capacity is bounded by the subtree and is exactly
     * what the next rebuild needs. */
    cache.clear();
    for (SculptLayerTreeNode &node : group.children) {
      if (SculptLayer *layer = node_as_layer(&node)) {
        cache.append(layer);
      }
      else if (const SculptLayerGroup *child = node_as_group(&node)) {
        /* Through the child's own cache rather than a raw walk, so a rebuild here warms every
         * folder below it and reuses whatever is already warm. Safe against deadlock because the
         * lock order is always parent to child and the tree has no cycles, and safe against
         * staleness because #tag_layers_cache_dirty propagates the other way: this group can only
         * be dirty when every folder below it that changed is dirty too. */
        cache.extend(layers(*child));
      }
    }
  });
  return group.runtime->layer_cache_.as_span();
}

Span<SculptLayer *> layers(const Mesh &mesh)
{
  return layers(*root_group(mesh));
}

static void groups_gather(const SculptLayerGroup &group, Vector<SculptLayerGroup *> &r_groups)
{
  for (SculptLayerTreeNode &node : group.children) {
    if (SculptLayerGroup *child = node_as_group(&node)) {
      r_groups.append(child);
      groups_gather(*child, r_groups);
    }
  }
}

Vector<SculptLayerGroup *> groups(const SculptLayerGroup &group)
{
  Vector<SculptLayerGroup *> result;
  groups_gather(group, result);
  return result;
}

Vector<SculptLayerGroup *> groups(const Mesh &mesh)
{
  return groups(*root_group(mesh));
}

/* True when \a group's parent chain closes on itself. */
static bool ancestor_chain_has_cycle(const SculptLayerGroup &group)
{
  /* Floyd cycle detection, the same idiom #find_ancestor uses on the same walk, and for the same
   * reason: an arbitrary depth cap would be a second thing to get wrong. */
  const SculptLayerGroup *slow = &group;
  const SculptLayerGroup *fast = &group;
  while (fast != nullptr) {
    fast = fast->base.parent;
    if (fast == nullptr) {
      return false;
    }
    fast = fast->base.parent;
    slow = slow->base.parent;
    if (fast != nullptr && fast == slow) {
      return true;
    }
  }
  return false;
}

const SculptLayerMask *chain_mask(const SculptLayerGroup &group)
{
  /* Not allocated on demand, for the same reason as in #layers: this is const and runs from
   * evaluation threads, so a lazy allocation here would be a data race. */
  BLI_assert(group.runtime != nullptr);
  SculptLayerGroupRuntime &runtime = *group.runtime;

  /* Tested here rather than inside the lambda below, and before any lock is taken, because the
   * recursive rebuild would re-enter *this same group's* #CacheMutex on a chain that closes on
   * itself — and that mutex is not recursive, so the process would hang with no crash dump on
   * evaluation threads. Bailing before the lock is what keeps the recursion below able to assume a
   * strictly rootward, terminating walk. A cycle is only reachable from corrupt data, but this runs
   * on every redraw and must not hang the UI.
   *
   * Null rather than a partial fold: the chain such a tree describes is not a chain at all, so
   * there is no prefix of it that can be trusted, and null is the module's existing "no mask" state
   * that every consumer already handles. It also leaves the cache exactly as it was, so no caller
   * is handed a half-built or freed mask. */
  if (ancestor_chain_has_cycle(group)) {
    return nullptr;
  }

  runtime.chain_mask_mutex_.ensure([&]() {
    /* The previous product is released here rather than by #tag_layers_cache_dirty, which runs on
     * the editing thread and would be freeing a mask an evaluation thread may still be reading.
     * Inside #ensure the cache lock is held and the lambda runs exactly once per dirty cycle, so
     * nothing else is rebuilding it. What the pointer's previous holders see is unchanged either
     * way: the tag is the moment their pointer became stale, which is the same contract #layers
     * documents for its span — that one likewise clears its backing vector inside #ensure. */
    mask_free(runtime.chain_mask_);
    runtime.chain_mask_ = nullptr;

    /* Through the parent's own cache rather than a raw walk up, so a rebuild here warms every
     * folder above it and reuses whatever is already warm. Safe against deadlock because this
     * nested lock is a *different* group's, is always taken rootward — so every chain-mask lock in
     * the process is acquired in one order — and the walk terminates at the root, whose parent is
     * null; the guard above is what rules out the one tree shape that would break both of those.
     * Safe against staleness because #tag_chain_mask_dirty propagates the other way: this group can
     * only be dirty when every folder above it that changed is dirty too. */
    const SculptLayerMask *parent_chain = (group.base.parent != nullptr) ?
                                              chain_mask(*group.base.parent) :
                                              nullptr;
    const SculptLayerMask *own = group.base.mask;

    if (parent_chain == nullptr && own == nullptr) {
      /* Stays null: the whole point is that an unmasked tree costs the composite nothing. */
      return;
    }
    if (own == nullptr) {
      /* Copied rather than aliased: the ancestor's cache is invalidated on its own schedule, and a
       * shared pointer would be freed twice by #group_runtime_free. */
      runtime.chain_mask_ = mask_copy(*parent_chain);
      return;
    }
    if (parent_chain == nullptr) {
      runtime.chain_mask_ = mask_copy(*own);
      return;
    }
    /* Null when the two masks describe different domains (see #mask_multiply), rather than one
     * block table indexed with the other's element count. */
    runtime.chain_mask_ = mask_multiply(*parent_chain, *own);
    if (runtime.chain_mask_ == nullptr) {
      /* The ancestor's chain survives the mismatch on its own instead of being discarded with it.
       * Only one of the two operands is untrustworthy — the ancestor's was already folded from
       * masks that agreed all the way down — and dropping both fails *open*: a subtree parked by
       * masking a folder above it to zero would become fully visible the moment a folder below it
       * went stale. Copying a mask that already describes its own domain cannot index out of
       * bounds, so the fallback is as safe as the null it replaces. */
      runtime.chain_mask_ = mask_copy(*parent_chain);
    }
  });
  return runtime.chain_mask_;
}

static int node_max_uid(const SculptLayerGroup &group)
{
  int uid = group.base.uid;
  for (SculptLayerTreeNode &node : group.children) {
    uid = std::max(uid, node.uid);
    if (const SculptLayerGroup *child = node_as_group(&node)) {
      uid = std::max(uid, node_max_uid(*child));
    }
  }
  return uid;
}

int node_unique_uid(const Mesh &mesh)
{
  /* The root holds uid 0, so this is never 0 — which is what lets 0 keep meaning "the root" for
   * #node_find_by_uid and "no active layer" for #Mesh::sculpt_layers_active_uid. */
  return node_max_uid(*root_group(mesh)) + 1;
}

static SculptLayerTreeNode *node_find_in_group(const SculptLayerGroup &group, const int uid)
{
  for (SculptLayerTreeNode &node : group.children) {
    if (node.uid == uid) {
      return &node;
    }
    if (const SculptLayerGroup *child = node_as_group(&node)) {
      if (SculptLayerTreeNode *found = node_find_in_group(*child, uid)) {
        return found;
      }
    }
  }
  return nullptr;
}

SculptLayerTreeNode *node_find_by_uid(Mesh &mesh, const int uid)
{
  return const_cast<SculptLayerTreeNode *>(node_find_by_uid(const_cast<const Mesh &>(mesh), uid));
}

const SculptLayerTreeNode *node_find_by_uid(const Mesh &mesh, const int uid)
{
  const SculptLayerGroup *root = root_group(mesh);
  if (uid == 0) {
    return &root->base;
  }
  return node_find_in_group(*root, uid);
}

const SculptLayerGroup *find_ancestor(const SculptLayerGroup *start,
                                      FunctionRef<bool(const SculptLayerGroup &)> predicate)
{
  /* Floyd cycle detection rather than a depth cap: an arbitrary cap would be a second thing to get
   * wrong. `fast` is tested after every single step, so it visits every group on the chain —
   * including every member of a cycle, which it covers before `slow` can catch it. A cycle is only
   * reachable from corrupt data, but this runs on every redraw and must not hang the UI. */
  const SculptLayerGroup *slow = start;
  const SculptLayerGroup *fast = start;
  while (fast != nullptr) {
    if (predicate(*fast)) {
      return fast;
    }
    fast = fast->base.parent;
    if (fast == nullptr) {
      break;
    }
    if (predicate(*fast)) {
      return fast;
    }
    fast = fast->base.parent;
    slow = slow->base.parent;
    if (fast != nullptr && fast == slow) {
      /* The chain closed on itself without a match, so going round again cannot find one. */
      return nullptr;
    }
  }
  return nullptr;
}

bool node_is_descendant_of(const SculptLayerTreeNode &node, const SculptLayerGroup &ancestor)
{
  /* Strictly below: the walk starts at the parent, so a node is never its own descendant. Callers
   * rejecting "into itself" need their own identity check. */
  return find_ancestor(node.parent,
                       [&](const SculptLayerGroup &group) { return &group == &ancestor; }) !=
         nullptr;
}

/* True when \a name is held by any node of \a group's subtree, \a exclude aside. */
static bool name_collides_in_subtree(const SculptLayerGroup &group,
                                     const SculptLayerTreeNode &exclude,
                                     const StringRef name)
{
  for (SculptLayerTreeNode &node : group.children) {
    if (&node != &exclude && name == node.name) {
      return true;
    }
    if (const SculptLayerGroup *child = node_as_group(&node)) {
      if (name_collides_in_subtree(*child, exclude, name)) {
        return true;
      }
    }
  }
  return false;
}

void node_name_ensure_unique(SculptLayerTreeNode &node)
{
  if (node.parent == nullptr) {
    /* The root is never drawn and needs no name. */
    return;
  }
  /* Unique across the whole tree, not just the parent's children — see the header for why the flat
   * RNA collection and #rna_SculptLayer_path require it. Climb to the root so every node is in
   * scope whatever depth this one sits at. */
  const SculptLayerGroup *root = node.parent;
  while (root->base.parent != nullptr) {
    root = root->base.parent;
  }
  BLI_uniquename_cb(
      [&](const StringRefNull check_name) {
        return name_collides_in_subtree(*root, node, check_name);
      },
      (node.type == SCULPT_LAYER_TREE_NODE_TYPE_GROUP) ? "Group" : "Layer",
      '.',
      node.name,
      sizeof(node.name));
}

void node_move_into(Mesh &mesh,
                    SculptLayerTreeNode &node,
                    SculptLayerGroup &dst,
                    SculptLayerTreeNode *after)
{
  BLI_assert(&node != &root_group(mesh)->base);
  BLI_assert(after == nullptr || after->parent == &dst);
  BLI_assert(after != &node);
  if (const SculptLayerGroup *moved = node_as_group(&node)) {
    /* Either would detach the moved subtree from the root; the caller rejects it (see
     * #node_is_descendant_of), which is asserted rather than handled. */
    BLI_assert(&dst != moved);
    BLI_assert(!node_is_descendant_of(dst.base, *moved));
  }

  SculptLayerGroup *src_parent = node.parent;
  if (node.parent != nullptr) {
    BLI_remlink(&node.parent->children, &node);
  }
  /* #BLI_remlink leaves \a node's own links pointing into the list it was just removed from, and
   * #BLI_insertlinkafter only overwrites them when \a dst already holds a child: its empty-list
   * branch just becomes `first`/`last` and returns. Moving into an empty folder would therefore
   * splice the source list's tail in behind \a node, and the two lists would share nodes. */
  node.next = node.prev = nullptr;
  node.parent = &dst;
  BLI_insertlinkafter(&dst.children, after, &node);

  /* Both ends. Tagging only \a dst would leave the source folder — and everything above it — handing
   * out a span that still contains the moved node; tagging only the source would leave \a dst's span
   * missing it. A reorder *within* one folder makes these the same group, which is why the order of
   * the two calls does not matter. The moved node's own subtree is deliberately not tagged: its
   * contents did not change, only where it hangs. */
  if (src_parent != nullptr) {
    tag_layers_cache_dirty(*src_parent);
  }
  tag_layers_cache_dirty(dst);

  /* The one structural mutation that also invalidates a chain mask, and the mirror image of the two
   * tags above: a chain mask folds in every *ancestor's* mask, so it is the moved subtree — which
   * now hangs under a different set of ancestors — that went stale, while \a dst, \a src_parent and
   * every other folder keep the chain they had. Moving a layer changes no chain at all, since only
   * folders carry one. Missing this is worse than a stale row: #chain_mask hands out a pointer into
   * the cache, and the next rebuild frees the one it replaces. */
  if (SculptLayerGroup *moved_group = node_as_group(&node)) {
    tag_chain_mask_dirty(*moved_group);
  }

  UNUSED_VARS_NDEBUG(mesh);
}

/** \} */

/**
 * Standalone copy of \a src, detached from the tree and with its own data buffer.
 *
 * Every layer duplication routes through here. A shallow struct copy alone would leave the two
 * layers sharing one #SculptLayer::data allocation, which #tree_free would then release twice, so
 * the deep copy of the buffer must not be separable from the copy of the struct.
 *
 * The copy keeps \a src's uid; a caller inserting it into a mesh assigns a fresh one.
 */
static SculptLayer *layer_copy(const SculptLayer &src)
{
  SculptLayer *layer = MEM_new<SculptLayer>(__func__, src);
  layer->base.next = layer->base.prev = nullptr;
  layer->base.parent = nullptr;
  if (src.data) {
    layer->data = MEM_dupalloc_void(src.data);
  }
  /* The struct copy carried the source's mask pointer over, exactly as it would have carried
   * #SculptLayer::data. Two layers sharing one mask would be freed twice and would show one
   * layer's mask edits on the other. Done here rather than in #group_copy_children so that
   * #duplicate, the other caller, is covered by the same line. */
  if (src.base.mask != nullptr) {
    layer->base.mask = mask_copy(*src.base.mask);
  }
  return layer;
}

/**
 * The layer nearest to \a node among its siblings, searching forward first and then backward, or
 * null when the folder holds no other layer.
 *
 * A plain `next ? next : prev` no longer answers "a neighbour": a sibling can now be a folder, and
 * reading one as a layer would be a type confusion. Only ever used to hand the active marker on.
 */
static SculptLayer *sibling_layer(SculptLayerTreeNode &node)
{
  for (SculptLayerTreeNode *other = node.next; other; other = other->next) {
    if (SculptLayer *layer = node_as_layer(other)) {
      return layer;
    }
  }
  for (SculptLayerTreeNode *other = node.prev; other; other = other->prev) {
    if (SculptLayer *layer = node_as_layer(other)) {
      return layer;
    }
  }
  return nullptr;
}

SculptLayer *add(Mesh &mesh,
                 const char *name,
                 const short domain,
                 const int totelem,
                 const short level)
{
  SculptLayer *layer = MEM_new<SculptLayer>(__func__);
  layer->base.type = SCULPT_LAYER_TREE_NODE_TYPE_LAYER;
  STRNCPY_UTF8(layer->base.name, (name && name[0]) ? name : "Layer");
  layer->influence = 1.0f;
  layer->base.flag = SCULPT_LAYER_ENABLED;
  layer->domain = domain;
  layer->level = (domain == short(SCULPT_LAYER_DOMAIN_GRID)) ? level : 0;
  layer->base.uid = node_unique_uid(mesh);

  SculptLayerGroup &root = *root_group(mesh);
  layer->base.parent = &root;
  BLI_addtail(&root.children, &layer->base);
  tag_layers_cache_dirty(root);
  node_name_ensure_unique(layer->base);

  if (totelem > 0) {
    data_ensure(*layer, totelem);
  }
  active_set(mesh, layer);
  return layer;
}

void remove(Mesh &mesh, SculptLayer &layer)
{
  /* Hand the active marker to a sibling layer, but only when the removed layer held it: unlike the
   * positional index this replaced, a uid keeps identifying the same layer no matter what happens
   * elsewhere in the tree, so removing some *other* layer must leave the active one alone.
   *
   * A neighbour means a *sibling* now, so removing the last layer of a folder clears the active
   * marker rather than reaching outside: the flat list's `next ? next : prev` had one global
   * order to fall back on, whereas a tree has no meaningful "next layer" across folders — any
   * choice would be an arbitrary jump to an unrelated part of the tree. */
  const bool was_active = mesh.sculpt_layers_active_uid == layer.base.uid;
  const SculptLayer *successor = sibling_layer(layer.base);

  if (layer.base.parent != nullptr) {
    BLI_remlink(&layer.base.parent->children, &layer.base);
    /* Before the free, and while the parent back-pointer is still readable: every cached span from
     * this folder up to the root still holds a pointer to this layer, and those spans are what the
     * eval paths read. A missed tag here is a use-after-free, not a stale row. */
    tag_layers_cache_dirty(*layer.base.parent);
  }
  if (layer.data) {
    MEM_delete_void(layer.data);
  }
  /* Owned by the node, exactly as #SculptLayer::data is, so it goes with it. */
  mask_free(layer.base.mask);
  MEM_delete_void(static_cast<void *>(&layer));

  if (was_active) {
    active_set(mesh, successor);
  }
}

SculptLayer *duplicate(Mesh &mesh, const SculptLayer &src)
{
  SculptLayer *layer = layer_copy(src);
  layer->base.uid = node_unique_uid(mesh);

  /* Right after the source, i.e. into the same folder — a duplicate landing somewhere else in the
   * tree would be a surprise. Every node but the root carries a parent, and the root is never a
   * #SculptLayer: falling back to the root group for a parentless source would insert after a node
   * that is not among its children, quietly corrupting the list. */
  BLI_assert(src.base.parent != nullptr);
  SculptLayerGroup &parent = *src.base.parent;
  layer->base.parent = &parent;
  BLI_insertlinkafter(
      &parent.children, const_cast<SculptLayerTreeNode *>(&src.base), &layer->base);
  tag_layers_cache_dirty(parent);
  node_name_ensure_unique(layer->base);
  active_set(mesh, layer);
  return layer;
}

SculptLayer *active_get(Mesh &mesh)
{
  return const_cast<SculptLayer *>(active_get(const_cast<const Mesh &>(mesh)));
}

const SculptLayer *active_get(const Mesh &mesh)
{
  /* Uid 0 means "no active layer" here — it must NOT be handed to #node_find_by_uid, where 0
   * resolves to the root group: every caller would then read a #SculptLayerGroup as a #SculptLayer,
   * putting `influence` on the root's child list and reading `data` past the allocation. */
  if (mesh.sculpt_layers_active_uid == 0) {
    return nullptr;
  }
  /* Kind-checked rather than reinterpreted: one uid counter spans both kinds, so a folder's uid
   * landing in this field (a stale write, a future bug) resolves to a folder, and answering null is
   * the only safe reading of "the active layer". */
  return node_as_layer(node_find_by_uid(mesh, mesh.sculpt_layers_active_uid));
}

void active_set(Mesh &mesh, const SculptLayer *layer)
{
  mesh.sculpt_layers_active_uid = layer ? layer->base.uid : 0;
}

bool solo_active(const Mesh &mesh)
{
  for (const SculptLayer *layer : layers(mesh)) {
    if (layer->base.flag & SCULPT_LAYER_SOLO_HIDDEN) {
      return true;
    }
  }
  return false;
}

SculptLayerGroup *group_add(Mesh &mesh, const char *name, const int parent_uid)
{
  SculptLayerGroup *group = MEM_new<SculptLayerGroup>(__func__);
  group->base.type = SCULPT_LAYER_TREE_NODE_TYPE_GROUP;
  STRNCPY_UTF8(group->base.name, (name && name[0]) ? name : "Group");
  group->base.flag = SCULPT_LAYER_GROUP_ENABLED | SCULPT_LAYER_GROUP_EXPANDED;
  group->base.uid = node_unique_uid(mesh);
  group_runtime_ensure(*group);

  /* Uid 0 resolves to the root, which is what makes "no parent given" and "at the top level" the
   * same call. A uid naming a layer, or nothing at all, falls back to the root rather than leaving
   * the group unreachable. */
  SculptLayerGroup *parent = node_as_group(node_find_by_uid(mesh, parent_uid));
  if (parent == nullptr) {
    parent = root_group(mesh);
  }
  group->base.parent = parent;
  BLI_addtail(&parent->children, &group->base);
  /* The new folder is empty, so the parent's *layer* span does not actually change here. Tagged
   * anyway: "linking a node tags the folder it lands in" is the rule that has to hold without a
   * per-call-site argument about what the node happens to contain — and #node_move_into can put
   * layers inside it a moment later. */
  tag_layers_cache_dirty(*parent);
  node_name_ensure_unique(group->base);
  return group;
}

void group_remove(Mesh &mesh, SculptLayerGroup &group)
{
  /* A folder owns its children, so freeing one that still has any would leak the whole subtree. The
   * caller decides what an orphaned subtree becomes (#SCULPT_OT_layer_group_remove lifts it to the
   * grandparent); this only refuses to guess. */
  BLI_assert(group.children.is_empty());
  BLI_assert(&group != root_group(mesh));

  if (group.base.parent != nullptr) {
    BLI_remlink(&group.base.parent->children, &group.base);
    /* Before the free, while the parent back-pointer is still readable. The folder is asserted empty
     * above, so no ancestor span loses a *layer* here — but see #group_add for why the rule is
     * applied without that argument. */
    tag_layers_cache_dirty(*group.base.parent);
  }
  group_runtime_free(group);
  /* Owned by the node, as the runtime is, so it goes with it. */
  mask_free(group.base.mask);
  MEM_delete_void(static_cast<void *>(&group));

  UNUSED_VARS_NDEBUG(mesh);
}

bool is_stale(const SculptLayer &layer, const int elem_num)
{
  return layer.data != nullptr && layer.totelem != elem_num;
}

int element_count(const Mesh &mesh, const SculptLayer &layer)
{
  if (layer.domain == SCULPT_LAYER_DOMAIN_GRID) {
    return grid_totelem(mesh.corners_num, layer.level);
  }
  return mesh.verts_num;
}

bool is_stale(const Mesh &mesh, const SculptLayer &layer)
{
  return is_stale(layer, element_count(mesh, layer));
}

bool is_stale_mask(const SculptLayerMask &mask, const int64_t elem_num)
{
  /* A mask reaches a composite from two directions and both can be stale: the node's own pointer,
   * and the folder product handed out by #chain_mask, which is built from whatever the ancestors
   * happen to carry and is never resized to the live domain. Both go through here.
   *
   * The block-table tests are not redundant with the element-count one. #mask_blend_read neutralizes
   * a mask from a truncated or hand-edited file to `totelem == 0, blocks_num == 0` with null array
   * pointers, and #chain_mask can hand that on as a non-null product; on an empty domain the element
   * counts would then agree and the block loop would run over zero blocks, which is not "unmasked"
   * but "this layer contributes nothing" — it would drop the layer's whole contribution silently.
   * Answering stale here routes it to the unmasked path instead, which is the safe reading of a mask
   * that describes nothing. */
  return mask.totelem != elem_num || mask.blocks_num <= 0 || mask.block_size <= 0;
}

MutableSpan<float3> data_ensure(SculptLayer &layer, const int totelem)
{
  if (layer.data != nullptr && layer.totelem == totelem) {
    /* Fast path: data already allocated and correctly sized. Kept allocation-free and
     * logging-free — the timing probe below only runs on the (rare) realloc path and only when
     * #SCULPT_LAYERS_DEBUG_LOG is enabled. */
    return {static_cast<float3 *>(layer.data), layer.totelem};
  }
#if SCULPT_LAYERS_DEBUG_LOG
  const auto func_start = std::chrono::high_resolution_clock::now();
  printf("[DEBUG-perf] data_ensure: realloc needed, old_totelem=%d, new_totelem=%d\n",
         layer.totelem, totelem);
#endif
  if (layer.data) {
    MEM_delete_void(layer.data);
  }
  layer.data = MEM_new_array_zeroed<float[3]>(size_t(totelem), __func__);
  layer.totelem = totelem;
#if SCULPT_LAYERS_DEBUG_LOG
  printf("[DEBUG-perf] data_ensure: total=%lld us\n",
         std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::high_resolution_clock::now() - func_start)
             .count());
#endif
  return {static_cast<float3 *>(layer.data), layer.totelem};
}

MutableSpan<float3> data_get(SculptLayer &layer)
{
  if (layer.data == nullptr) {
    return {};
  }
  return {static_cast<float3 *>(layer.data), layer.totelem};
}

Span<float3> data_get(const SculptLayer &layer)
{
  if (layer.data == nullptr) {
    return {};
  }
  return {static_cast<const float3 *>(layer.data), layer.totelem};
}

void data_clear(SculptLayer &layer)
{
  data_get(layer).fill(float3(0.0f));
}

bool rec_exempt_set(const Mesh &mesh, const SculptLayer *layer)
{
  /* Every layer is visited rather than just the two that change, so that the tree can only ever
   * hold one exempt node no matter what state it was in on entry. That is what makes this the
   * repair for a bit dropped by an undo restore (the reader clears it) as well as the setter. */
  bool changed = false;
  for (SculptLayer *tree_layer : layers(mesh)) {
    const int flag_before = tree_layer->base.flag;
    SET_FLAG_FROM_TEST(tree_layer->base.flag, tree_layer == layer, SCULPT_LAYER_REC_EXEMPT);
    /* Accumulated rather than returned early, because the loop has to finish visiting the tree for
     * the "one exempt node" guarantee above to hold. */
    changed |= tree_layer->base.flag != flag_before;
  }
  return changed;
}

/* #CompositeMask and #node_mask_for_composite are declared in `BKE_sculpt_layers.hh`: the multires
 * grid composite resolves a layer's masks through the same function, so a layer's mask cannot be
 * resolved one way on the vertex domain and another way on the grid domain. */
CompositeMask node_mask_for_composite(const SculptLayer &layer, const int64_t elem_num)
{
  CompositeMask masks;
  /* First, so neither the layer's own mask nor the folder chain above it is even read: the whole
   * product has to disappear, not just the layer's half of it. A folder mask left in force would
   * scale the recorded delta exactly as the layer's own would, and the delta is stored raw (see
   * #rec_exempt_set).
   *
   * One flag test per *layer* per composite — not per element — against a field the caller has
   * already loaded, so the unmasked fast path below is unchanged in cost. */
  if (layer.base.flag & SCULPT_LAYER_REC_EXEMPT) {
    return masks;
  }
  if (layer.base.mask != nullptr && !is_stale_mask(*layer.base.mask, elem_num)) {
    masks.primary = layer.base.mask;
  }
  /* Every node but the root carries a parent, and a layer is never the root; the test is for a node
   * that has been unlinked from the tree but not yet freed. */
  if (layer.base.parent != nullptr) {
    const SculptLayerMask *chain = chain_mask(*layer.base.parent);
    if (chain != nullptr && !is_stale_mask(*chain, elem_num)) {
      masks.secondary = chain;
    }
  }
  /* Two masks that agree on the domain but not on how it is cut into blocks cannot share a block
   * index, so the layer's own is dropped and the folder chain carries the weight alone. That is the
   * same call #chain_mask makes when #mask_multiply rejects a pair, and for the same reason: it
   * fails *closed*, so a subtree parked by masking a folder above it does not spring back into view
   * because a mask below it happened to be cut differently. */
  if (masks.primary != nullptr && masks.secondary != nullptr &&
      masks.primary->block_size != masks.secondary->block_size)
  {
    masks.primary = nullptr;
  }
  /* The chain is the only mask when the layer carries none of its own (or just lost it above). */
  if (masks.primary == nullptr) {
    masks.primary = masks.secondary;
    masks.secondary = nullptr;
  }
  return masks;
}

CompositeMask grid_masks_for_composite(const SculptLayer &layer,
                                       const int64_t elem_num,
                                       const int grid_area)
{
  CompositeMask masks = node_mask_for_composite(layer, elem_num);
  if (masks.primary == nullptr) {
    return masks;
  }
  /* The multires paths index a mask block with a *grid* index, which is only the right block when a
   * GRID mask is cut one block per grid (#SculptLayerMask::block_size documents that contract). A
   * mask cut any other way would silently fold one grid's weights onto another grid's geometry —
   * wrong shape, no crash, no warning — so it is dropped and the layer contributes fully, which is
   * how #is_stale_mask already makes every other unusable mask fail open.
   *
   * Testing `block_size` is only meaningful *after* #node_mask_for_composite has applied
   * #is_stale_mask to both sides: #mask_blend_read neutralizes an unusable mask by resetting its
   * block size to #SCULPT_LAYER_MASK_VERT_BLOCK, so a neutralized GRID mask carries a VERT block
   * size and `block_size` alone is never a safe domain discriminator. Staleness first, block size
   * second — that ordering is load-bearing.
   *
   * NOTE: no GRID mask producer exists yet (the edit session owns it), so this check is currently
   * unexercised by real data. That is why it is written now rather than assumed: the first producer
   * to pick a different block size would otherwise land a silent geometry bug.
   *
   * #node_mask_for_composite guarantees the two sides agree on `block_size`, so one test settles
   * both.
   *
   * NOTE: this fails *open* while the analogous case in #node_mask_for_composite fails *closed*, and
   * the asymmetry is deliberate rather than an oversight. There the two masks disagree with each
   * *other*, so one of them is still indexable and dropping the layer's own leaves the folder chain
   * to carry the weight — a subtree parked by masking a folder stays parked. Here the two agree with
   * each other and both disagree with the *domain*, so there is no usable side left to keep: the
   * choice is between contributing fully and dropping the layer outright, and a #CompositeMask
   * cannot express the latter (it names weights, not participation). Failing open is then the same
   * call #is_stale_mask already makes for every other unusable mask, and is reachable only when a
   * chain's `totelem` coincidentally equals `grids_num * grid_area` while its blocks are cut some
   * other way. */
  if (masks.primary->block_size != grid_area) {
    return {};
  }
  return masks;
}

/**
 * `r_positions[i] += data[i] * weight * mask[i]` over the whole domain.
 *
 * The single spelling of a layer's weighted contribution, and deliberately so: #combine_layers_mesh
 * adds it and #derive_base_mesh subtracts it back off to recover the un-layered base, so a forward
 * and an inverse written separately would drift the base a little on every flush — silently and
 * cumulatively. Here the inverse is the *same* code with a negated \a weight, and negation is exact
 * in binary floating point, so the two cancel bit for bit.
 *
 * The arithmetic itself is not spelled here but delegated to #mask_block_weight / #mask_elem_weight,
 * which the multires grid composite folds through as well. That indirection is the point: with the
 * fold written out in both places nothing at run time forced the vertex and grid domains to weigh a
 * mask identically, and a disagreement between them would surface only as a slow, silent drift of
 * the base — never as a crash or a visibly wrong frame.
 */
static void accumulate_layer(const Span<float3> data,
                             const CompositeMask &masks,
                             const float weight,
                             MutableSpan<float3> r_positions)
{
  if (masks.primary == nullptr) {
    /* Unmasked: the original loop, untouched. A layer without a mask pays one pointer test per
     * composite, which is the property that makes this feature affordable to ship.
     *
     * Bandwidth-bound vertex pass; parallelize since the influence slider runs this on the whole
     * mesh per tick. #parallel_for stays serial below the grain size, so small meshes pay no
     * overhead. */
    threading::parallel_for(r_positions.index_range(), 8192, [&](const IndexRange range) {
      for (const int64_t i : range) {
        r_positions[i] += data[i] * weight;
      }
    });
    return;
  }

  const SculptLayerMask &primary = *masks.primary;
  /* Guaranteed by #node_mask_for_composite, and load-bearing: one block index reads both tables. */
  BLI_assert(masks.secondary == nullptr ||
             (masks.secondary->block_size == primary.block_size &&
              masks.secondary->blocks_num == primary.blocks_num));
  BLI_assert(primary.totelem == r_positions.size());

  /* Masked: decide per block, not per element. #mask_block_weight folds every uniform side into the
   * scalar, so a uniform block takes the same flat loop as the unmasked path above, and a block the
   * layer cannot reach at all (a uniform zero on either side) reports #MaskBlockWeight::skip and is
   * dropped outright — which the unmasked path cannot do. Only blocks that stay dense on some side
   * pay for the extra byte per element.
   *
   * The grain is expressed in elements to match the 8192 above rather than fixed in blocks, so both
   * paths hand a task the same amount of work whatever a domain's block size happens to be. */
  const int64_t grain = std::max<int64_t>(1, 8192 / primary.block_size);
  threading::parallel_for(IndexRange(primary.blocks_num), grain, [&](const IndexRange range) {
    for (const int64_t block : range) {
      const int start = int(block) * primary.block_size;
      /* The extent comes from the position span, not from the block size: the tail block is short,
       * and a full-width loop over it would run off the end of both the data and the mask table. */
      const int64_t extent = std::min(int64_t(primary.block_size), r_positions.size() - start);
      BLI_assert(extent > 0);

      const MaskBlockWeight block_weight = mask_block_weight(masks, int(block), weight);
      if (block_weight.skip) {
        continue;
      }
      if (block_weight.fold == MaskFold::Uniform) {
        for (const int64_t i : IndexRange(extent)) {
          r_positions[start + i] += data[start + i] * block_weight.weight;
        }
        continue;
      }
      for (const int64_t i : IndexRange(extent)) {
        r_positions[start + i] += data[start + i] * mask_elem_weight(block_weight, int(i));
      }
    }
  });
}

void apply_delta_mesh(const SculptLayer &layer, const float factor, MutableSpan<float3> positions)
{
  if (layer.data == nullptr || factor == 0.0f) {
    return;
  }
  if (is_stale(layer, positions.size())) {
    /* Skip rather than corrupt a mismatched vertex range. */
    return;
  }
  const Span<float3> deltas(static_cast<const float3 *>(layer.data), layer.totelem);
  /* Masked here rather than only in #combine_layers_mesh, because this is how a layer's contribution
   * is *incrementally* corrected — the influence slider, a visibility toggle, a removal — and those
   * deltas have to be measured with the same weight the composite laid down. An unmasked delta
   * against a masked composite would eat into the masked region a little on every drag. */
  accumulate_layer(deltas, node_mask_for_composite(layer, positions.size()), factor, positions);
}

float effective(const SculptLayer &layer)
{
  /* A layer inside a disabled folder contributes nothing, but keeps its own #SCULPT_LAYER_ENABLED
   * bit — re-enabling the folder must restore exactly what was visible before. One flag test rather
   * than a walk up the group chain, because this runs per vertex / per grid point. */
  if (layer.base.flag & SCULPT_LAYER_GROUP_HIDDEN) {
    return 0.0f;
  }
  /* #group_influence_cached is the product of every ancestor folder's influence, pre-baked by
   * #resync_group_state so this stays one flat multiply per element rather than a walk up the chain. */
  return (layer.base.flag & SCULPT_LAYER_ENABLED) ?
             layer.influence * layer.group_influence_cached :
             0.0f;
}

/**
 * Fold the ancestor state into each child of \a group and write the result onto the layers below it:
 * \a parent_enabled drives the #SCULPT_LAYER_GROUP_HIDDEN flag and \a parent_influence is the running
 * product of ancestor folder influences baked onto each layer's #group_influence_cached.
 *
 * When \a write_flags is false the flag write is skipped and only the float cache is refreshed; the
 * restore path uses that mode so it can rebuild the cache without disturbing the #SCULPT_LAYER_GROUP_HIDDEN
 * / Solo-Base flags that a structural undo restored separately.
 *
 * Top-down, so each folder simply hands its own state to its children and every node is visited
 * exactly once. The memo table, the uid lookups and the cycle guard this replaces were all artifacts
 * of the parent-uid tags, where a child could sit ahead of its parent in the flat list and the
 * ancestor chain had to be re-resolved per layer; a child list cannot express either problem.
 */
static void resync_group_state_recursive(const SculptLayerGroup &group,
                                         const bool parent_enabled,
                                         const float parent_influence,
                                         const bool write_flags)
{
  for (SculptLayerTreeNode &node : group.children) {
    if (SculptLayerGroup *child = node_as_group(&node)) {
      resync_group_state_recursive(
          *child,
          parent_enabled && (child->base.flag & SCULPT_LAYER_GROUP_ENABLED) != 0,
          parent_influence * child->influence,
          write_flags);
    }
    else if (SculptLayer *layer = node_as_layer(&node)) {
      if (write_flags) {
        SET_FLAG_FROM_TEST(layer->base.flag, !parent_enabled, SCULPT_LAYER_GROUP_HIDDEN);
      }
      layer->group_influence_cached = parent_influence;
    }
  }
}

void resync_group_state(Mesh &mesh)
{
  const SculptLayerGroup &root = *root_group(mesh);
  /* The root is read like any other folder rather than assumed enabled, so there is one rule for
   * every level. #root_group_ensure is what guarantees its enabled bit is actually set; its own
   * influence (always the default 1 — no UI reaches it) folds in as the seed of the product. */
  resync_group_state_recursive(
      root, (root.base.flag & SCULPT_LAYER_GROUP_ENABLED) != 0, root.influence, true);
}

void refresh_group_influence_cache(Mesh &mesh)
{
  const SculptLayerGroup &root = *root_group(mesh);
  /* Float-only pass: recompute #group_influence_cached without touching a single flag. */
  resync_group_state_recursive(
      root, (root.base.flag & SCULPT_LAYER_GROUP_ENABLED) != 0, root.influence, false);
}

void combine_layers_mesh(const Span<float3> base,
                         const Span<SculptLayer *> layers,
                         MutableSpan<float3> r_positions)
{
  BLI_assert(r_positions.size() == base.size());
  r_positions.copy_from(base);
  for (const SculptLayer *layer_ptr : layers) {
    const SculptLayer &layer = *layer_ptr;
    if (layer.domain != SCULPT_LAYER_DOMAIN_VERT || layer.data == nullptr) {
      continue;
    }
    const float eff = effective(layer);
    if (eff == 0.0f) {
      continue;
    }
    if (is_stale(layer, r_positions.size())) {
      /* Skip rather than compose deltas over a mismatched vertex range. */
      continue;
    }
    const Span<float3> data(static_cast<const float3 *>(layer.data), layer.totelem);
    accumulate_layer(data, node_mask_for_composite(layer, r_positions.size()), eff, r_positions);
  }
}

void apply_vert_layers(const Span<SculptLayer *> layers, MutableSpan<float3> positions)
{
  for (const SculptLayer *layer : layers) {
    if (layer->domain != SCULPT_LAYER_DOMAIN_VERT) {
      continue;
    }
    apply_delta_mesh(*layer, effective(*layer), positions);
  }
}

void apply_vert_layers_eval(Mesh &mesh)
{
  apply_vert_layers(layers(mesh), mesh.vert_positions_for_write());
}

/* Shared body of the two shape-key transition helpers: `positions[i] += sign * sum(...)`. Bails out
 * before touching the positions when the mesh carries no vertex-layer data, so the copy-on-write of
 * #Mesh::vert_positions_for_write is not paid on the (common) mesh without layers. */
static void shift_vert_layers_in_positions(Mesh &mesh, const float sign)
{
  const Span<SculptLayer *> mesh_layers = layers(mesh);
  bool any = false;
  for (const SculptLayer *layer : mesh_layers) {
    if (layer->domain == SCULPT_LAYER_DOMAIN_VERT && layer->data != nullptr &&
        !is_stale(*layer, mesh.verts_num) && effective(*layer) != 0.0f)
    {
      any = true;
      break;
    }
  }
  if (!any) {
    return;
  }
  MutableSpan<float3> positions = mesh.vert_positions_for_write();
  for (const SculptLayer *layer : mesh_layers) {
    if (layer->domain != SCULPT_LAYER_DOMAIN_VERT) {
      continue;
    }
    apply_delta_mesh(*layer, sign * effective(*layer), positions);
  }
  mesh.tag_positions_changed();
}

void strip_vert_layers_from_positions(Mesh &mesh)
{
  shift_vert_layers_in_positions(mesh, -1.0f);
}

void bake_vert_layers_into_positions(Mesh &mesh)
{
  shift_vert_layers_in_positions(mesh, 1.0f);
}

KeyBlock *bake_vert_layers_into_new_shape_key(Mesh &mesh)
{
  Key *key = mesh.key;
  if (key == nullptr || key->type != KEY_RELATIVE) {
    return nullptr;
  }
  const Span<SculptLayer *> mesh_layers = layers(mesh);
  bool any = false;
  for (const SculptLayer *layer : mesh_layers) {
    if (layer->domain == SCULPT_LAYER_DOMAIN_VERT && layer->data != nullptr &&
        !is_stale(*layer, mesh.verts_num))
    {
      any = true;
      break;
    }
  }
  if (!any) {
    return nullptr;
  }

  KeyBlock *kb = BKE_keyblock_add_ctime(key, DATA_("Sculpt Layers"), false);
  /* The positions are the un-layered basis while shape keys are present, so this seeds the block
   * with the basis and the layer sum below turns it into `basis + sum`. */
  BKE_keyblock_convert_from_mesh(&mesh, key, kb);
  if (kb->data == nullptr || kb->totelem != mesh.verts_num) {
    return kb;
  }
  apply_vert_layers(mesh_layers, MutableSpan(static_cast<float3 *>(kb->data), kb->totelem));

  /* Relative to the reference key, so the block's delta is the layer sum alone, and at full value
   * so the surface is unchanged by the bake. */
  kb->relative = short(std::max(BLI_findindex(&key->block, key->refkey), 0));
  kb->curval = 1.0f;
  return kb;
}

void apply_vert_layer_to_shape_keys(Mesh &mesh, const SculptLayer &layer, const float factor)
{
  if (mesh.key == nullptr || layer.domain != SCULPT_LAYER_DOMAIN_VERT || layer.data == nullptr ||
      factor == 0.0f)
  {
    return;
  }
  if (is_stale(layer, mesh.verts_num)) {
    return;
  }
  const Span<float3> deltas(static_cast<const float3 *>(layer.data), layer.totelem);
  /* Resolved once and handed to every carrier below, so the positions and the key blocks cannot end
   * up weighted differently. They are two spellings of the same surface: the bake that reaches here
   * is one-directional, so any disagreement would be permanent — the key blocks would hold
   * `(1 - mask[i]) * delta` of contribution the surface never had. */
  const CompositeMask masks = node_mask_for_composite(layer, mesh.verts_num);
  accumulate_layer(deltas, masks, factor, mesh.vert_positions_for_write());
  mesh.tag_positions_changed();

  for (KeyBlock &kb : mesh.key->block) {
    if (kb.data == nullptr || kb.totelem != mesh.verts_num) {
      continue;
    }
    accumulate_layer(
        deltas, masks, factor, MutableSpan(static_cast<float3 *>(kb.data), kb.totelem));
  }
}

void resample_grid_layers(Mesh &mesh, const int grids_num, const int new_level)
{
  bool warned = false;
  for (SculptLayer *layer_ptr : layers(mesh)) {
    SculptLayer &layer = *layer_ptr;
    if (layer.domain != SCULPT_LAYER_DOMAIN_GRID) {
      continue;
    }
    if (layer.data == nullptr) {
      layer.level = short(std::max(new_level, 0));
      layer.totelem = 0;
      continue;
    }
    if (new_level <= 0) {
      /* No subdivision levels left: there is nothing for a grid layer to displace. */
      MEM_delete_void(layer.data);
      layer.data = nullptr;
      layer.totelem = 0;
      layer.level = 0;
      if (!warned) {
        CLOG_WARN(&LOG, "Grid sculpt layer data cleared: all subdivision levels were removed.");
        warned = true;
      }
      continue;
    }
    if (layer.level == short(new_level) &&
        layer.totelem == grid_totelem(grids_num, new_level)) {
      continue;
    }
    if (layer.level <= 0 || layer.totelem != grid_totelem(grids_num, layer.level)) {
      /* Stale metadata or base topology change: the data cannot be mapped. */
      MEM_delete_void(layer.data);
      layer.data = nullptr;
      layer.totelem = 0;
      layer.level = short(new_level);
      if (!warned) {
        CLOG_WARN(&LOG,
                  "Grid sculpt layer data reset: stored level no longer matches the mesh topology.");
        warned = true;
      }
      continue;
    }
    const Span<float3> src(static_cast<const float3 *>(layer.data), layer.totelem);
    const Array<float3> resampled = (layer.level < new_level) ?
                                        grid_upsample(src, layer.level, new_level, grids_num) :
                                        grid_subsample(src, layer.level, new_level, grids_num);
    MEM_delete_void(layer.data);
    layer.data = nullptr;
    layer.totelem = 0;
    data_ensure(layer, int(resampled.size()));
    data_get(layer).copy_from(resampled);
    layer.level = short(new_level);
  }
}

/* Upper bound for the grid-level search below. Far above any usable multires level; it exists only
 * so the search is bounded, and is kept low enough that `1 << (level - 1)` cannot overflow. */
static constexpr int SCULPT_LAYER_MAX_GRID_LEVEL = 24;

/**
 * The subdivision level a GRID-domain mask describes, or -1 if it does not describe one.
 *
 * Derived from the mask itself rather than from the node it hangs off: a folder carries a mask but
 * no level field, and a folder mask is exactly as much a part of the grid composite as a layer's
 * own (see #chain_mask). A GRID mask is cut one block per grid, so `block_size` is a CCG grid area
 * and `totelem` is that times the grid count — which pins the level with no convention to agree on.
 */
static int grid_mask_level(const SculptLayerMask &mask, const int grids_num)
{
  if (mask.block_size <= 0 || grids_num <= 0) {
    return -1;
  }
  if (int64_t(mask.totelem) != int64_t(grids_num) * mask.block_size) {
    return -1;
  }
  /* `block_size` must be exactly `CCG_grid_size(level)^2` for some level. Searched rather than
   * derived by a log: the search is obviously right, and the level range is tiny. */
  for (int level = 1; level <= SCULPT_LAYER_MAX_GRID_LEVEL; level++) {
    const int gs = CCG_grid_size(level);
    const int64_t area = int64_t(gs) * gs;
    if (area == mask.block_size) {
      return level;
    }
    if (area > mask.block_size) {
      break;
    }
  }
  return -1;
}

/* Resample one node's mask from \a old_level to \a new_level, in place. */
static void resample_one_grid_mask(SculptLayerTreeNode &node,
                                   const int grids_num,
                                   const int old_level,
                                   const int new_level)
{
  SculptLayerMask &mask = *node.mask;
  /* Ride the mask through the *same* resamplers the layer data uses, one weight per `x` channel.
   * The point is not to save code but to make it impossible for a mask to land on a different grid
   * point than the coefficient it weights: the two would drift apart the moment the two mappings
   * were spelled separately, and the symptom — a mask sliding across the surface on a level change
   * — would be blamed on anything but the resampler. */
  Array<float> dense(mask.totelem);
  mask_expand(mask, dense);
  Array<float3> packed(mask.totelem);
  threading::parallel_for(IndexRange(mask.totelem), 8192, [&](const IndexRange range) {
    for (const int64_t i : range) {
      packed[i] = float3(dense[i], 0.0f, 0.0f);
    }
  });

  const Array<float3> resampled = (old_level < new_level) ?
                                      grid_upsample(packed, old_level, new_level, grids_num) :
                                      grid_subsample(packed, old_level, new_level, grids_num);

  Array<float> out(resampled.size());
  threading::parallel_for(resampled.index_range(), 8192, [&](const IndexRange range) {
    for (const int64_t i : range) {
      /* Bilinear upsampling can overshoot a hair outside 0..1 at the interpolated points; clamp so
       * a weight never re-enters the composite as a negative or amplifying factor. */
      out[i] = std::clamp(resampled[i].x, 0.0f, 1.0f);
    }
  });

  const int new_gs = CCG_grid_size(new_level);
  SculptLayerMask *replacement = mask_compress(out, new_gs * new_gs);
  if (replacement == nullptr) {
    /* Losing the mask is not a no-op: the node it hangs off then contributes at full strength
     * everywhere the user had masked it down, so say so rather than let the surface change on a
     * level switch with nothing in the log. */
    CLOG_WARN(&LOG, "Grid sculpt layer mask dropped: it could not be resampled to the new level.");
  }
  mask_free(node.mask);
  node.mask = replacement;
}

/* Resample the mask \a node carries, if it is a grid-domain one. Does not recurse. */
static void resample_node_grid_mask(SculptLayerTreeNode &node,
                                    const int grids_num,
                                    const int new_level)
{
  if (node.mask == nullptr) {
    return;
  }
  const int old_level = grid_mask_level(*node.mask, grids_num);
  if (old_level < 1 || old_level == new_level) {
    /* Either not a grid mask (a vertex-domain mask on a folder is left alone) or already at the
     * target level. */
    return;
  }
  if (new_level <= 0) {
    /* No subdivision levels left: there is no grid domain for a mask to describe. */
    mask_free(node.mask);
    node.mask = nullptr;
    return;
  }
  resample_one_grid_mask(node, grids_num, old_level, new_level);
}

/* Recursive worker for #resample_grid_masks. Visits \a node itself and everything below it. */
static void resample_grid_masks_recursive(SculptLayerTreeNode &node,
                                          const int grids_num,
                                          const int new_level)
{
  resample_node_grid_mask(node, grids_num, new_level);
  if (SculptLayerGroup *group = node_as_group(&node)) {
    for (SculptLayerTreeNode &child : group->children) {
      resample_grid_masks_recursive(child, grids_num, new_level);
    }
  }
}

void resample_grid_masks(Mesh &mesh, const int grids_num, const int new_level)
{
  SculptLayerGroup *root = root_group(mesh);
  if (root == nullptr) {
    return;
  }
  /* The root's own mask is handled directly rather than by recursing on `root->base`: that node
   * *is* the root, so recursing on it would walk the whole tree a second time and resample every
   * mask below twice. */
  resample_node_grid_mask(root->base, grids_num, new_level);
  for (SculptLayerTreeNode &child : root->children) {
    resample_grid_masks_recursive(child, grids_num, new_level);
  }
  /* Folder masks feed #chain_mask, whose cached products were folded at the old level. */
  tag_chain_mask_dirty(*root);
}

void derive_base_mesh(const Span<float3> positions,
                      const Span<SculptLayer *> layers,
                      MutableSpan<float3> r_base)
{
  BLI_assert(r_base.size() == positions.size());
  r_base.copy_from(positions);
  for (const SculptLayer *layer_ptr : layers) {
    const SculptLayer &layer = *layer_ptr;
    if (layer.domain != SCULPT_LAYER_DOMAIN_VERT || layer.data == nullptr) {
      continue;
    }
    const float eff = effective(layer);
    if (eff == 0.0f) {
      continue;
    }
    if (is_stale(layer, r_base.size())) {
      /* Skip rather than derive the base over a mismatched vertex range. */
      continue;
    }
    const Span<float3> data(static_cast<const float3 *>(layer.data), layer.totelem);
    /* The forward pass with a negated weight, not a subtraction written out a second time: `r - x`
     * and `r + (-x)` agree bit for bit, and sharing the code is what makes it impossible for the
     * mask dispatch to be spelled two subtly different ways. See #accumulate_layer. */
    accumulate_layer(data, node_mask_for_composite(layer, r_base.size()), -eff, r_base);
  }
}

/* -------------------------------------------------------------------- */
/** \name ID lifetime and blend-file IO
 * \{ */

/* Copy \a src's children into \a dst, recursively, rebuilding the parent back-pointers as it goes.
 * \a dst is already a copy of \a src's own fields; only the subtree below it is filled in here. */
static void group_copy_children(SculptLayerGroup &dst, const SculptLayerGroup &src)
{
  /* The shallow copy of \a src brought its child links along, which point into the *source* tree. */
  dst.children.clear_no_delete();
  for (SculptLayerTreeNode &node : src.children) {
    SculptLayerTreeNode *copy = nullptr;
    if (const SculptLayerGroup *src_group = node_as_group(&node)) {
      SculptLayerGroup *group = MEM_new<SculptLayerGroup>(__func__, *src_group);
      /* The struct copy brought the *source's* runtime pointer along. Two groups sharing one runtime
       * would be freed twice, and the copy would hand out the source tree's layers. Nulling first is
       * what makes the ensure below allocate rather than keep it. */
      group->runtime = nullptr;
      /* Same hazard, for the folder's own mask: the struct copy brought the source's pointer along.
       * The layer branch below is covered inside #layer_copy. */
      if (src_group->base.mask != nullptr) {
        group->base.mask = mask_copy(*src_group->base.mask);
      }
      group_runtime_ensure(*group);
      group_copy_children(*group, *src_group);
      copy = &group->base;
    }
    else {
      copy = &layer_copy(*node_as_layer(&node))->base;
    }
    copy->next = copy->prev = nullptr;
    copy->parent = &dst;
    BLI_addtail(&dst.children, copy);
  }
}

void tree_copy(Mesh &dst, const Mesh &src)
{
  BLI_assert(dst.sculpt_layer_root == nullptr);
  const SculptLayerGroup &src_root = *root_group(src);
  SculptLayerGroup *root = MEM_new<SculptLayerGroup>(__func__, src_root);
  root->base.next = root->base.prev = nullptr;
  root->base.parent = nullptr;
  /* As in #group_copy_children: the struct copy carried the source root's runtime pointer over, and
   * the copy needs its own. A fresh runtime starts dirty, which is what makes the whole copied tree
   * build its spans from its own nodes on first read rather than inherit anything from \a src. */
  root->runtime = nullptr;
  /* Same hazard as in #group_copy_children, for the root's own mask — the one node that function
   * never visits, since it only ever walks children. */
  root->base.mask = (src_root.base.mask != nullptr) ? mask_copy(*src_root.base.mask) : nullptr;
  group_runtime_ensure(*root);
  dst.sculpt_layer_root = root;
  group_copy_children(*root, src_root);
}

/* Free every child of \a group, recursively; \a group itself is left to the caller. */
static void group_free_children(SculptLayerGroup &group)
{
  for (SculptLayerTreeNode &node : group.children.items_mutable()) {
    if (SculptLayerGroup *child = node_as_group(&node)) {
      group_free_children(*child);
      group_runtime_free(*child);
    }
    else if (SculptLayer *layer = node_as_layer(&node)) {
      if (layer->data) {
        MEM_delete_void(layer->data);
      }
    }
    /* Tolerates null, so folders and layers without a mask need no branch here. */
    mask_free(node.mask);
    /* #MEM_delete_void rather than #MEM_delete: the blend-file reader allocates its nodes C-style
     * and this one call accepts both origins. See the allocation notes in `BKE_sculpt_layers.hh`. */
    MEM_delete_void(static_cast<void *>(&node));
  }
  group.children.clear_no_delete();
}

void tree_free(Mesh &mesh)
{
  if (mesh.sculpt_layer_root == nullptr) {
    return;
  }
  group_free_children(*mesh.sculpt_layer_root);
  /* The root is the one group #group_free_children never reaches — it only walks children — so its
   * runtime is released here. No span is left stale by the loop above: every group it frees has its
   * runtime freed with it, and the root's goes now. */
  group_runtime_free(*mesh.sculpt_layer_root);
  /* The root is the one node #group_free_children never reaches, as with its runtime above. */
  mask_free(mesh.sculpt_layer_root->base.mask);
  MEM_delete_void(static_cast<void *>(mesh.sculpt_layer_root));
  mesh.sculpt_layer_root = nullptr;
}

/* Write \a group and everything below it, depth-first — the order #group_blend_read_children reads
 * them back in. Mirrors `write_layer_tree_group` in `grease_pencil.cc`. */
static void group_blend_write_recursive(BlendWriter *writer, SculptLayerGroup &group)
{
  /* The runtime is rebuilt on read, so the file gets a null rather than this session's heap address.
   * Written from a sanitized shallow copy — legal because #SculptLayerGroup is a trivially copyable
   * DNA struct — but recorded *at the real group's address*, which is what keeps the child nodes'
   * #SculptLayerTreeNode::parent pointers and the parent's child links resolving to it on read.
   * Writing the live pointer would also make two otherwise identical trees compare unequal, which
   * the memfile undo system reads as a change.
   *
   * #SculptLayerTreeNode::mask is deliberately *not* nulled the way the runtime is: it is the
   * address the mask is written at on the next line, and the address the reader resolves it from.
   * Unlike the runtime it is also stable across undo steps, since it only changes when the mask
   * itself is added, dropped or reallocated — which is a real change. */
  SculptLayerGroup group_for_write = group;
  group_for_write.runtime = nullptr;
  writer->write_struct_at_address(&group, &group_for_write);
  if (group.base.mask != nullptr) {
    mask_blend_write(writer, *group.base.mask);
  }
  for (SculptLayerTreeNode &node : group.children) {
    if (SculptLayerGroup *child = node_as_group(&node)) {
      group_blend_write_recursive(writer, *child);
    }
    else if (SculptLayer *layer = node_as_layer(&node)) {
      /* #SCULPT_LAYER_REC_EXEMPT is session state that happens to live in a DNA field, so it is
       * stripped here the same way the group's runtime pointer is above: from a sanitized shallow
       * copy recorded *at the real layer's address*, which is what keeps the sibling links and the
       * children's #SculptLayerTreeNode::parent pointers resolving on read.
       *
       * A `.blend` carrying this bit would open with that layer's weight map silently absent from
       * the composite, permanently, with nothing in the UI naming the cause. Clearing it here also
       * keeps two otherwise identical trees comparing equal in the memfile undo encoder whether or
       * not REC happened to be armed. The cost is that a memfile undo restore drops the bit —
       * repaired by #ed::sculpt_paint::layers::rec_exemption_refresh, which every commit and every
       * stroke start already calls. Erring that way round is deliberate: a dropped bit is a
       * composite that looks masked until the next commit, a leaked bit is a wrong surface with no
       * way back. */
      SculptLayer layer_for_write = *layer;
      layer_for_write.base.flag &= ~SCULPT_LAYER_REC_EXEMPT;
      writer->write_struct_at_address(layer, &layer_for_write);
      if (layer->base.mask != nullptr) {
        mask_blend_write(writer, *layer->base.mask);
      }
      if (layer->data) {
        writer->write_float3_array(layer->totelem, static_cast<const float *>(layer->data));
      }
    }
  }
}

void tree_blend_write(BlendWriter *writer, Mesh &mesh)
{
  if (mesh.sculpt_layer_root == nullptr) {
    return;
  }
  group_blend_write_recursive(writer, *mesh.sculpt_layer_root);
}

static void group_blend_read_children(BlendDataReader *reader, SculptLayerGroup &group)
{
  BLO_read_struct_list(reader, SculptLayerTreeNode, &group.children);
  for (SculptLayerTreeNode &node : group.children) {
    /* Not stored: #SculptLayerTreeNode::parent is a DNA pointer that no reader fixes up, so it is
     * recomputed from the child list on the way down. Anything read from the file is a stale
     * address. */
    node.parent = &group;
    /* Read for both node kinds before the split below, since the mask hangs off the shared base. */
    BLO_read_struct(reader, SculptLayerMask, &node.mask);
    if (node.mask != nullptr) {
      mask_blend_read(reader, node.mask);
    }
    if (SculptLayerGroup *child = node_as_group(&node)) {
      /* Runtime state is never persisted, and a reader-allocated group has had no constructor run on
       * it. Nulled before the ensure so that anything the file happens to hold in that slot is
       * dropped rather than taken for a live runtime. */
      child->runtime = nullptr;
      group_runtime_ensure(*child);
      group_blend_read_children(reader, *child);
    }
    else if (SculptLayer *layer = node_as_layer(&node)) {
      /* Defense in depth, not a versioning step. #group_blend_write_recursive already strips this
       * bit, so no file this build writes can carry it, and official builds predating the bit left
       * it unset. What this covers is everything else that can reach the reader: a file written by
       * an intermediate build of this branch, a hand-edited or truncated one, or a future build
       * that repurposes the bit. The failure it prevents is the worst kind this subsystem has —
       * silent and sticky, a layer whose weight map is gone from the composite with no operator
       * that restores it — and the cost is one AND on a path that already visits every node. */
      layer->base.flag &= ~SCULPT_LAYER_REC_EXEMPT;
      float3 *data = static_cast<float3 *>(layer->data);
      BLO_read_array_and_validate_size(reader, &data, &layer->totelem);
      layer->data = data;
    }
  }
}

void tree_blend_read(BlendDataReader *reader, Mesh &mesh)
{
  BLO_read_struct(reader, SculptLayerGroup, &mesh.sculpt_layer_root);
  if (mesh.sculpt_layer_root != nullptr) {
    /* The root read from the file is the one group nothing else gives a runtime to:
     * #root_group_ensure below returns early precisely because the pointer is non-null, and
     * #group_blend_read_children only ever descends into *children*. Left unhandled, the first
     * #layers call on an opened file would dereference whatever sat in that slot. */
    mesh.sculpt_layer_root->runtime = nullptr;
    /* The root's own mask, for the same reason: #group_blend_read_children only ever descends into
     * children, so nothing else resolves the file address sitting in this slot. Only on this
     * branch — a root built by #root_group_ensure below was never read from a file and already
     * holds a null here. */
    BLO_read_struct(reader, SculptLayerMask, &mesh.sculpt_layer_root->base.mask);
    if (mesh.sculpt_layer_root->base.mask != nullptr) {
      mask_blend_read(reader, mesh.sculpt_layer_root->base.mask);
    }
  }
  /* A file older than the tree has no `sculpt_layer_root` member at all, so the pointer reads back
   * null and this gives the mesh an empty tree — which is exactly the agreed outcome for a
   * pre-migration file: it opens without its sculpt layers rather than crashing. */
  root_group_ensure(mesh);
  /* Idempotent, and a no-op on the branch where #root_group_ensure just built the root. */
  group_runtime_ensure(*mesh.sculpt_layer_root);
  mesh.sculpt_layer_root->base.parent = nullptr;
  group_blend_read_children(reader, *mesh.sculpt_layer_root);
  /* #group_influence_cached is derived state: the value read from the file is stale (or 0 from a file
   * predating the field), so rebuild it from the reconstructed tree. Float-only, since the flags read
   * from the file are authoritative and must not be touched here. */
  refresh_group_influence_cache(mesh);
}

/** \} */

}  // namespace blender::bke::sculpt_layers
