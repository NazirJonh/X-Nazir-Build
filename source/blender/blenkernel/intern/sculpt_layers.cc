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

/* The sanitized shallow copy in #group_blend_write_recursive relies on this. */
static_assert(std::is_trivially_copyable_v<SculptLayerGroup>,
              "SculptLayerGroup must stay trivially copyable: group_blend_write_recursive writes a "
              "by-value copy with the runtime pointer cleared");

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

void group_runtime_free(SculptLayerGroup &group)
{
  /* #MEM_delete rather than the #MEM_delete_void every other part of a node is freed with: the
   * runtime is the one piece that is always allocated here (never by the blend-file reader) and is
   * not trivially destructible, so its destructor has to run to release the cached vector. */
  MEM_delete(group.runtime);
  group.runtime = nullptr;
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
  /* Bandwidth-bound vertex pass; parallelize since the influence slider runs this on the whole mesh
   * per tick. #parallel_for stays serial below the grain size, so small meshes pay no overhead. */
  threading::parallel_for(positions.index_range(), 8192, [&](const IndexRange range) {
    for (const int64_t i : range) {
      positions[i] += deltas[i] * factor;
    }
  });
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
    threading::parallel_for(r_positions.index_range(), 8192, [&](const IndexRange range) {
      for (const int64_t i : range) {
        r_positions[i] += data[i] * eff;
      }
    });
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
  apply_delta_mesh(layer, factor, mesh.vert_positions_for_write());
  mesh.tag_positions_changed();

  for (KeyBlock &kb : mesh.key->block) {
    if (kb.data == nullptr || kb.totelem != mesh.verts_num) {
      continue;
    }
    MutableSpan<float3> data(static_cast<float3 *>(kb.data), kb.totelem);
    threading::parallel_for(data.index_range(), 8192, [&](const IndexRange range) {
      for (const int64_t i : range) {
        data[i] += deltas[i] * factor;
      }
    });
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
    threading::parallel_for(r_base.index_range(), 8192, [&](const IndexRange range) {
      for (const int64_t i : range) {
        r_base[i] -= data[i] * eff;
      }
    });
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
   * the memfile undo system reads as a change. */
  SculptLayerGroup group_for_write = group;
  group_for_write.runtime = nullptr;
  writer->write_struct_at_address(&group, &group_for_write);
  for (SculptLayerTreeNode &node : group.children) {
    if (SculptLayerGroup *child = node_as_group(&node)) {
      group_blend_write_recursive(writer, *child);
    }
    else if (SculptLayer *layer = node_as_layer(&node)) {
      writer->write_struct(layer);
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
    if (SculptLayerGroup *child = node_as_group(&node)) {
      /* Runtime state is never persisted, and a reader-allocated group has had no constructor run on
       * it. Nulled before the ensure so that anything the file happens to hold in that slot is
       * dropped rather than taken for a live runtime. */
      child->runtime = nullptr;
      group_runtime_ensure(*child);
      group_blend_read_children(reader, *child);
    }
    else if (SculptLayer *layer = node_as_layer(&node)) {
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
