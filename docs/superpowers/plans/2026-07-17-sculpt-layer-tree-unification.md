# Sculpt Layer Tree Unification Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the two flat sculpt layer lists with one nested tree of nodes, so folders and layers can be ordered freely among each other, then give folders their own influence factor.

**Architecture:** A common `SculptLayerTreeNode` embedded by `SculptLayer` and `SculptLayerGroup`, with each folder owning an ordered `ListBase` of children of both kinds. Sibling order becomes list order, so interleaving needs no order field. The evaluation hot path reads a flat layer cache on the root group's runtime, never the tree.

**Tech Stack:** Blender C++20, DNA/RNA, `blender::Vector`/`Span`, GTest, CMake.

**Spec:** `docs/superpowers/specs/2026-07-17-sculpt-layer-tree-unification-design.md` — read it first. It records *why* each decision was made; this plan records *how*.

## Global Constraints

- Never compile without explicit user permission. Never commit without explicit user permission. The user builds manually (`MAKE_FULL` = `.\make full`).
- C++20. 2-space indent, `snake_case`, braces always, comments in `/* */` explaining *why*.
- Prefer `blender::Vector`/`Array`/`Map` over STL. `id_cast<T *>` for ID casts, never `static_cast`.
- `MEM_new`/`MEM_delete` pair only; `MEM_SAFE_FREE` does not exist.
- Sculpt layer nodes must stay **trivially destructible** — enforced by `static_assert(std::is_trivially_destructible_v<SculptLayer>)` in `blenkernel/intern/sculpt_layers.cc:46-55`. The free paths (`MEM_delete_void`) and the blend reader depend on it. See the allocation contract in `BKE_sculpt_layers.hh:337-355`.
- Order must not affect evaluation. `position = base + Σ(data · effective)` stays commutative. Any task that makes order semantic is wrong.
- Layer operators push their own sculpt undo step and must not set `OPTYPE_UNDO` — a memfile step does not compose with the stroke SCULPT steps.

## Central risk: the migration has no small compiling steps

Once `SculptLayer` changes shape, every consumer breaks at once — 495 references to the type across 17 files, 77 of them in `sculpt_undo.cc`. Task boundaries below are drawn where the tree **compiles and behaves identically**, not where the diff is small. Two boundaries exist:

1. After the base node is embedded (still two flat lists).
2. After the tree replaces the lists.

Everything inside a boundary is one commit. Do not try to split it further; a half-migrated DNA struct does not build.

## File structure

| File | Responsibility | Task |
|---|---|---|
| `source/blender/makesdna/DNA_mesh_types.h` | Node structs, deprecated old fields | 1, 3 |
| `source/blender/blenloader/intern/versioning_530.cc` | Old fields → base node; flat lists → tree; uid renumber | 2, 4 |
| `source/blender/blenkernel/BKE_sculpt_layers.hh` | Public API: tree walk, cache, effective | 3, 5, 9 |
| `source/blender/blenkernel/intern/sculpt_layers.cc` | Implementation | 3, 5, 9 |
| `source/blender/blenkernel/intern/sculpt_layers_tree_test.cc` | GTest (new file) | 2, 5, 9 |
| `source/blender/blenkernel/intern/mesh.cc` | copy/free/blend IO of the tree | 4 |
| `source/blender/editors/sculpt_paint/mesh/sculpt_undo.{hh,cc}` | Merged payloads, reparent | 6 |
| `source/blender/editors/sculpt_paint/mesh/sculpt_layers.cc` | Operators | 7 |
| `source/blender/editors/sculpt_paint/mesh/interface_template_sculpt_layer_tree.cc` | Tree view | 8 |
| `source/blender/makesrna/intern/rna_mesh.cc` | Flat collections, influence property | 8, 9 |

---

### Task 1: Embed the base node, keep the two flat lists

Introduces `SculptLayerTreeNode` and moves the shared fields into it. The lists stay flat and behaviour is unchanged — this task exists purely so the DNA move and the tree move are separately reviewable and separately bisectable.

**Files:**
- Modify: `source/blender/makesdna/DNA_mesh_types.h:200-282`
- Modify: every consumer the compiler flags (start with `source/blender/blenkernel/intern/sculpt_layers.cc`)

**Interfaces:**
- Produces: `SculptLayerTreeNode` with `next`, `prev`, `parent`, `name[64]`, `flag`, `uid`, `type`. `SculptLayer::base`, `SculptLayerGroup::base`.
- `SCULPT_LAYER_TREE_NODE_TYPE_LAYER = 0`, `SCULPT_LAYER_TREE_NODE_TYPE_GROUP = 1`.

- [ ] **Step 1: Add the node struct and the type enum**

In `DNA_mesh_types.h`, above `struct SculptLayer`:

```c
enum eSculptLayerTreeNodeType : int8_t {
  SCULPT_LAYER_TREE_NODE_TYPE_LAYER = 0,
  SCULPT_LAYER_TREE_NODE_TYPE_GROUP = 1,
};

struct SculptLayerGroup;

/**
 * Fields shared by every row of the sculpt layer tree. Embedded as #SculptLayer::base and
 * #SculptLayerGroup::base, following #GreasePencilLayerTreeNode.
 */
struct SculptLayerTreeNode {
  SculptLayerTreeNode *next = nullptr, *prev = nullptr;
  /** The folder holding this node. Null only for the root group. */
  SculptLayerGroup *parent = nullptr;
  /** MAX_NAME. Unique among siblings only. */
  char name[64] = {};
  /** #eSculptLayerFlag for a layer, #eSculptLayerGroupFlag for a group. */
  int flag = 0;
  /** Unique across every node of the mesh. Stable across reordering. */
  int uid = 0;
  /** #eSculptLayerTreeNodeType. */
  int8_t type = SCULPT_LAYER_TREE_NODE_TYPE_LAYER;
  char _pad[7] = {};
};
```

- [ ] **Step 2: Move the shared fields into the base**

`SculptLayer` keeps only what a layer alone has. The old fields stay, marked deprecated, so versioning can read them out of existing files:

```c
struct SculptLayer {
  SculptLayerTreeNode base;
  float influence = 1.0f;
  int totelem = 0;
  short domain = 0;
  short level = 0;
  void *data = nullptr;

  /** Deprecated: moved to #SculptLayerTreeNode. Read only by versioning. */
  SculptLayer *next_legacy DNA_DEPRECATED = nullptr;
  SculptLayer *prev_legacy DNA_DEPRECATED = nullptr;
  char name_legacy[64] DNA_DEPRECATED = {};
  int flag_legacy DNA_DEPRECATED = 0;
  int uid_legacy DNA_DEPRECATED = 0;
  int group_uid DNA_DEPRECATED = 0;
  char _pad[4] = {};
};
```

Do the same for `SculptLayerGroup` (`parent_uid` becomes deprecated).

**Read `versioning_530.cc:70-82` first** — it shows the established pattern in this codebase for a deprecated field read by versioning, and the existing `sculpt_layers_active_index` → `sculpt_layers_active_uid` translation is the model to copy.

- [ ] **Step 3: Sweep the consumers**

Mechanical and compiler-guided: `layer->name` → `layer->base.name`, `layer->flag` → `layer->base.flag`, `layer->uid` → `layer->base.uid`, `layer->next` → `layer->base.next`. Do not change any logic in this step. Build, fix, repeat until clean.

Note `ListBaseT<SculptLayer>` iteration relies on `next`/`prev` being the first members. Once they live in `base`, the lists must become `ListBaseT<SculptLayerTreeNode>` or the iteration must go through `base`. Decide once, in `sculpt_layers.cc`, and apply everywhere.

- [ ] **Step 4: Versioning copies the legacy fields into the base**

In `versioning_530.cc`, in a new version block:

```cpp
for (Mesh &mesh : bmain->meshes) {
  for (SculptLayer &layer : mesh.sculpt_layers) {
    STRNCPY(layer.base.name, layer.name_legacy);
    layer.base.flag = layer.flag_legacy;
    layer.base.uid = layer.uid_legacy;
    layer.base.type = SCULPT_LAYER_TREE_NODE_TYPE_LAYER;
  }
  /* Same for the groups, with SCULPT_LAYER_TREE_NODE_TYPE_GROUP. */
}
```

- [ ] **Step 5: Ask the user to build and verify no behaviour change**

Everything must work exactly as before: add/remove/rename layers and folders, drag, collapse, undo, a stroke with REC armed. Any difference is a migration bug.

- [ ] **Step 6: Ask permission, then commit**

---

### Task 2: Tree construction test (before the tree exists)

Write the test first — it defines what Task 4's versioning must produce.

**Files:**
- Create: `source/blender/blenkernel/intern/sculpt_layers_tree_test.cc`
- Modify: `source/blender/blenkernel/CMakeLists.txt`

**Interfaces:**
- Consumes: `SculptLayerTreeNode` from Task 1.
- Produces: `blender::bke::sculpt_layers::tests` namespace.

- [ ] **Step 1: Register the test file**

In `blenkernel/CMakeLists.txt`, add `intern/sculpt_layers_tree_test.cc` to the `TEST_SRC` list. Copy the surrounding entries' formatting exactly.

- [ ] **Step 2: Write the failing tests**

```cpp
/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "BKE_sculpt_layers.hh"
#include "DNA_mesh_types.h"

namespace blender::bke::sculpt_layers::tests {

/* Builds the flat, pre-migration shape: two root folders, a layer in the first, a nested folder. */
static void build_legacy_mesh(Mesh &mesh);

TEST(sculpt_layers_tree, versioning_preserves_draw_order)
{
  /* Folders drew before layers at every level, so the migrated children list must read
   * folders-then-layers to leave existing files looking identical. */
}

TEST(sculpt_layers_tree, versioning_reparents_dangling_to_root)
{
  /* A layer naming a group_uid no ancestor resolves used to vanish from the tree while still
   * deforming the mesh. It must land at the root instead. */
}

TEST(sculpt_layers_tree, uid_renumber_has_no_collisions)
{
  /* Layer uids and group uids were separate counters that both started at 1. After renumbering,
   * every node uid is distinct and sculpt_layers_active_uid still resolves to the same layer. */
}

}  // namespace blender::bke::sculpt_layers::tests
```

Fill each body against the API Task 3 produces. **Write the assertions before writing Task 3's implementation** — that is the point of doing this task first.

- [ ] **Step 3: Ask the user to build; expect the tests to fail to compile or fail outright**

Expected: `ctest -R sculpt_layers -V` reports failures, not passes.

- [ ] **Step 4: Ask permission, then commit the failing tests**

---

### Task 3: BKE tree API

**Files:**
- Modify: `source/blender/blenkernel/BKE_sculpt_layers.hh`
- Modify: `source/blender/blenkernel/intern/sculpt_layers.cc`
- Modify: `source/blender/makesdna/DNA_mesh_types.h` (root group, children lists)

**Interfaces:**
- Produces:
  - `SculptLayerGroup *root_group(Mesh &mesh)`
  - `SculptLayerTreeNode *node_find_by_uid(Mesh &mesh, int uid)` — replaces the two `find_by_uid` overloads
  - `bool node_is_descendant_of(const SculptLayerTreeNode &node, const SculptLayerGroup &ancestor)`
  - `void node_move_into(Mesh &mesh, SculptLayerTreeNode &node, SculptLayerGroup &dst, SculptLayerTreeNode *after)`
  - `int node_unique_uid(const Mesh &mesh)` — one counter for every node

- [ ] **Step 1: Add the root group to `Mesh`**

`Mesh` gains `SculptLayerGroup *sculpt_layer_root` — always allocated, never drawn as a row, uid 0. Keep `sculpt_layers` and `sculpt_layer_groups` as `DNA_DEPRECATED` for versioning.

Read `DNA_grease_pencil_types.h:461` (`root_group_ptr`) for the precedent.

- [ ] **Step 2: Implement the walk, the lookup and the move**

Keep every existing invariant documented in `BKE_sculpt_layers.hh`: `group_name_ensure_unique` stays the single authority for names; uid 0 keeps meaning "the root"; `node_is_descendant_of` stays cycle-bounded.

- [ ] **Step 3: Ask the user to build and run the Task 2 tests**

Run: `ctest -R sculpt_layers -V` from `../build_windows_Full_x64_vc17_Release`.
Expected: the construction and uid tests pass.

- [ ] **Step 4: Ask permission, then commit**

---

### Task 4: Versioning, blend IO, copy/free

**Files:**
- Modify: `source/blender/blenloader/intern/versioning_530.cc`
- Modify: `source/blender/blenkernel/intern/mesh.cc:200-210` (copy), and the free/blend_write/blend_read paths
- Modify: `source/blender/blenkernel/intern/sculpt_layers.cc`

- [ ] **Step 1: Build the tree from the flat lists**

Order: for each parent, its subfolders in their old list order, then its layers in theirs. A node whose parent uid does not resolve attaches to the root. Then renumber every node from the single counter and remap `Mesh::sculpt_layers_active_uid`.

- [ ] **Step 2: Make blend IO and copy/free recursive**

The allocation contract in `BKE_sculpt_layers.hh:337-355` still holds: nodes come from `MEM_new` or the C-style reader, and are freed with `MEM_delete_void`. Keep the `static_assert` for trivial destructibility passing — if a node ever needs a non-trivial member, the whole contract changes and that is a different plan.

- [ ] **Step 3: Ask the user to build, run the tests, and open a pre-migration .blend**

The file must open looking identical: same nesting, same order, same active layer.

- [ ] **Step 4: Ask permission, then commit**

---

### Task 5: Flat layer cache on the root group

**Files:**
- Modify: `source/blender/blenkernel/BKE_sculpt_layers.hh`
- Modify: `source/blender/blenkernel/intern/sculpt_layers.cc`
- Modify: `source/blender/blenkernel/intern/sculpt_layers_tree_test.cc`

**Interfaces:**
- Produces: `Span<SculptLayer *> layers(const SculptLayerGroup &group)` — pre-ordered, cached.

- [ ] **Step 1: Read the precedent**

`BKE_grease_pencil.hh:698-715` (`LayerGroupRuntime`: `nodes_cache_`, `layer_cache_`, `layer_group_cache_` behind one `CacheMutex`) and `grease_pencil.cc` `ensure_nodes_cache()` / `tag_nodes_cache_dirty()`. The cache belongs on the **group**, not on `Mesh::runtime` — so any folder can answer "my layers, in order", which the folder-mask sub-project needs.

- [ ] **Step 2: Write the failing cache-invalidation test**

```cpp
TEST(sculpt_layers_tree, cache_rebuilds_after_topology_change)
{
  /* add / remove / reparent / reorder must each invalidate the flat span. */
}
```

- [ ] **Step 3: Implement the cache and tag it dirty on every structural mutation**

Every function that links, unlinks or reorders a node tags the cache dirty. Missing one is a use-after-free waiting to happen, not a stale-display bug.

- [ ] **Step 4: Point the eval functions at the cache**

`combine_layers_mesh`, `derive_base_mesh`, `apply_vert_layers` change from `const ListBaseT<SculptLayer> &` to `Span<SculptLayer *>`. Their bodies keep skipping stale layers exactly as documented.

- [ ] **Step 5: Point the grid/multires consumers at the cache**

These iterate the flat layer list too and are easy to miss, because they are outside the sculpt layer module and the compiler only flags them once the list is gone:

- `bke::sculpt_layers::resample_grid_layers` (`sculpt_layers.cc`) — walks every grid-domain layer on Subdivide / Delete Higher.
- `source/blender/blenkernel/intern/multires_reshape.cc` — 22 references, the tangent-space path.
- `source/blender/blenkernel/intern/subdiv_displacement_multires.cc`, `intern/multires.cc`, `intern/key.cc`, `source/blender/draw/intern/draw_pbvh.cc`.

None of them care about order — they only need "every layer". Each becomes a loop over the cached span. Do not change any of their math in this task; a behaviour change here is a migration bug.

- [ ] **Step 6: Add the commutativity test**

```cpp
TEST(sculpt_layers_tree, composition_is_order_independent)
{
  /* Compose two layers, reorder them among their siblings, compose again: identical positions.
   * This is the invariant the whole design rests on - if it ever fails, stop. */
}
```

- [ ] **Step 7: Ask the user to build and run the tests**

- [ ] **Step 8: Ask permission, then commit**

---

### Task 6: Undo

**Files:**
- Modify: `source/blender/editors/sculpt_paint/mesh/sculpt_undo.hh:146-300`
- Modify: `source/blender/editors/sculpt_paint/mesh/sculpt_undo.cc`

- [ ] **Step 1: Merge the two payloads**

`SculptLayerUndoPayload` and `SculptLayerGroupUndoPayload` become one. The group version exists today only because "the uid resolves in a different list" (`sculpt_undo.hh:293-298`) — the single uid counter removes that reason. The merged payload keeps `prev_uid` (sibling anchor, 0 = head) and gains the parent uid; the layer-only fields (`totelem`, `domain`, `level`, data buffer) are only meaningful when `type` is a layer.

- [ ] **Step 2: Drop `ReparentMove::is_group`**

With one uid space, `prev_from`/`prev_to` name any sibling unambiguously. Delete the flag and every branch on it.

- [ ] **Step 3: Verify the ordering contract still holds**

`restore_list` re-applies moves in capture order because an entry's anchor may be another entry in the same batch. That reasoning does not change — but it now spans both kinds, so a folder can be a layer's anchor. Re-read the comment at `sculpt_undo.hh:252-262` and update it to say so.

- [ ] **Step 4: Ask the user to build and hand-test undo**

Undo/redo of: reorder, reparent, folder create, folder disband, rename, a stroke interleaved with a folder operation. This is the highest-risk area in the project — the memory notes record a prior bug where layer operators pushed memfile steps that did not compose with stroke steps.

- [ ] **Step 5: Ask permission, then commit**

---

### Task 7: Operators

**Files:**
- Modify: `source/blender/editors/sculpt_paint/mesh/sculpt_layers.cc`

- [ ] **Step 1: Rewrite `layer_move_to_exec` against the tree**

Read `sculpt_layers.cc:1672-1845` first. The whole `layer_cursor` / `group_cursor` chaining, the self-splice guards, and the `anchor_is_group` property exist only because there were two lists. With one children list they collapse into: resolve the anchor node, resolve the destination folder, unlink each moved node, relink after a running cursor.

Delete the `anchor_is_group` RNA property. Its doc-comment explains it exists because "layer uids and group uids are independent counters" — that is now false.

- [ ] **Step 2: Fix Move Up/Down**

Per the spec: the neighbouring **row** is the next sibling of either kind. A folder is one indivisible row — step over it, never into it. Move Up/Down never reparents.

Read `sculpt_layers.cc:1604-1632`. `BLI_listbase_link_move` on the flat layer list becomes a move within the parent's children list.

- [ ] **Step 3: Fix Merge Down**

Per the spec: `layer_below` skips folders and finds the next **layer** sibling. Read `sculpt_layers.cc:1923-1935` — the existing comment already explains why "below" means "next sibling within the same folder"; the reason survives, only the walk changes.

- [ ] **Step 4: Ask the user to build and hand-test**

Move a folder between two layers. Move a layer above a folder. Merge down across a folder. Move Up/Down over a folder.

- [ ] **Step 5: Ask permission, then commit**

---

### Task 8: Tree view and RNA collections

**Files:**
- Modify: `source/blender/editors/sculpt_paint/mesh/interface_template_sculpt_layer_tree.cc`
- Modify: `source/blender/makesrna/intern/rna_mesh.cc`

- [ ] **Step 1: Rewrite `build_tree_recursive` as a children walk**

Read `interface_template_sculpt_layer_tree.cc:566-583`. The two loops (folders, then layers) become one loop over the parent's children in list order. The comment citing "design doc §2, folders first" must go — that rule is what this project deletes.

- [ ] **Step 2: Simplify the drop target**

`group_dest_is_inside_dragged_group` and `SculptLayerDropTarget` lose their kind branching. `can_drop` keeps rejecting a folder dropped into its own subtree — that stays a real constraint.

- [ ] **Step 3: Keep the flat RNA collections working**

`mesh.sculpt_layers` stays a flat collection, backed by the Task 5 cache, so `scripts/startup/bl_ui/properties_data_mesh.py` (`DATA_PT_sculpt_layers`, lines 272/284/293/393) keeps working unchanged. Grease Pencil exposes both a flat `layers` and `layer_groups` the same way.

- [ ] **Step 4: Ask the user to build and hand-test the panel and Python**

- [ ] **Step 5: Ask permission, then commit**

---

### Task 9: Folder influence

Step 2 of the spec. The tree must be landed and verified before starting this.

**Files:**
- Modify: `source/blender/makesdna/DNA_mesh_types.h`
- Modify: `source/blender/blenkernel/intern/sculpt_layers.cc:368-432`
- Modify: `source/blender/blenkernel/intern/sculpt_layers_tree_test.cc`

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(sculpt_layers_tree, folder_influence_multiplies_down_the_chain)
{
  /* Nested folders at 0.5 and 0.5 scale a layer at 1.0 to 0.25. */
}

TEST(sculpt_layers_tree, folder_influence_zero_yields_base)
{
  /* A folder at influence 0 contributes exactly the base positions. */
}
```

- [ ] **Step 2: Add `SculptLayerGroup::influence` and the per-layer cache**

`SculptLayer` gains `float group_influence_cached` — the product of its ancestor folders' influence, recomputed by the same pass that maintains `SCULPT_LAYER_GROUP_HIDDEN`. Rename `resync_group_hidden` to `resync_group_state` and update its doc-comment, which currently promises it "only ever writes `SCULPT_LAYER_GROUP_HIDDEN`" — callers diff flags before and after on that promise (`group_cascade_resync_with_undo`), so the float must be handled there too or explicitly excluded from the diff.

- [ ] **Step 3: Extend `effective()`**

```cpp
float effective(const SculptLayer &layer)
{
  if (layer.base.flag & SCULPT_LAYER_GROUP_HIDDEN) {
    return 0.0f;
  }
  return (layer.base.flag & SCULPT_LAYER_ENABLED) ?
             layer.influence * layer.group_influence_cached :
             0.0f;
}
```

`SCULPT_LAYER_GROUP_HIDDEN` stays. It is not "influence 0": the tree view greys a layer's controls by the bit and REC refuses to arm on a layer carrying it, and a disabled folder is a different statement from a folder dialled to 0.

- [ ] **Step 4: Expose it in RNA and the UI**

Use the same sculpt-aware update callback the layer influence uses (`flush_interactive_update`, `ED_sculpt.hh:134`), so dragging the slider does not push a geometry recalc per tick. Note its documented limit: the fast path only applies to vertex-domain mesh PBVH without deform modifiers or shape keys.

- [ ] **Step 5: Ask the user to build, run the tests, and hand-test**

- [ ] **Step 6: Ask permission, then commit**

---

## Not in this plan

Tracked so they are not lost:

- **Folder mask** (sub-project 3) — per-element data on a folder. Needs its own spec: it drags in the whole `domain`/`level`/`totelem` apparatus, `is_stale`, undo payload data buffers, and multires resampling on Subdivide / Delete Higher. It is also the reason the flat cache lives on the group.
- **Search expands folders permanently** — `AbstractTreeViewItem::on_filter` calls `set_collapsed(false)` on the parents of a match, and this tree's `set_collapsed` persists that to DNA. Typing in the tree's search box therefore rewrites the user's collapse state. Pre-existing, out of scope, unfixed.
- **F2 rename and arrow-key navigation on folders** — both resolve the *active* item, and folder rows deliberately refuse the active state (commit `a4f5ea386b3`). Double-click rename, `X` delete and the context menu work, because they go through the selection. Fix only if it bothers the user.
- **Right-click on a folder adds to the selection** instead of replacing it, because `on_activate` selects without the select operator's preceding deselect-all. Layers do not have this asymmetry.
- **`f77092e8b2f` is unverified** — the collapse-across-rebuilds fix is committed but has never been built. Verify it before starting Task 1, so a migration bug is never confused with it.
