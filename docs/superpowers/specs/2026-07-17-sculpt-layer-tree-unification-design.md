# Sculpt Layer Tree Unification — Design

Date: 2026-07-17
Status: approved, not implemented
Scope: sub-projects 1 + 2 of 3 (see "Decomposition")

## Goal

Let folders and layers be ordered freely among each other — a folder may sit between two layers,
at any level of the tree — and give folders their own influence factor.

## Background: why the current model cannot do this

Folders and layers live in two independent flat lists, `Mesh::sculpt_layers` and
`Mesh::sculpt_layer_groups`, with the tree expressed by parent tags (`SculptLayer::group_uid`,
`SculptLayerGroup::parent_uid`). `build_tree_recursive` therefore draws, at every level, all
folders first and then all layers:

```cpp
for (SculptLayerGroup &group : mesh->sculpt_layer_groups) { if (group.parent_uid == parent_uid) ... }
for (SculptLayer &layer : mesh->sculpt_layers)            { if (layer.group_uid  == parent_uid) ... }
```

No ordering inside either list can interleave the two. A folder dropped onto a layer row is
reparented into that layer's folder but its "Before/After" is meaningless, and when the layer is
already its sibling nothing happens at all — the reported symptom.

Two ways out were considered:

- **A — shared order key.** An order field in both structs; siblings sorted by it at draw time.
  Cheap, and it deletes the cursor-chaining code in `layer_move_to_exec`.
- **B — unified node tree.** One node type, each folder owning an ordered list of children of both
  kinds. Interleaving falls out of the structure; no order key needed.

**B was chosen** because folders are planned to carry their own data (influence now, a per-element
mask later). A folder with a name, influence, flags and per-element data is structurally the same
node as a layer, and keeping the two in separate lists would be modelling the same thing twice.
Migrating once is cheaper than migrating to A and then to B.

## Non-goals

- Clipping / blend modes on folders. These are non-linear and would destroy the commutativity the
  whole evaluation model rests on (see "Evaluation model"). Explicitly out of scope.
- A per-element mask on folders. Sub-project 3, separate spec.
- An "active folder" concept. Folders stay addressed by uid only; the tree view row refuses the
  view's active state (see commit `a4f5ea386b3`).
- Reparenting via Move Up/Down (see "Behaviour decisions").

## Evaluation model — unchanged

The composition stays additive and order-independent:

```
position = base + Σ_layers( data[v] · influence_layer · Π_ancestors( influence_folder ) )
```

Adding a folder influence keeps it a sum of independent terms, so row order never affects the
result. Order is organisational only.

This matters for the hot path. `effective(const SculptLayer &)` takes only the layer and does no
tree walk: the folder cascade is pre-baked into the per-layer `SCULPT_LAYER_GROUP_HIDDEN` bit,
maintained by `resync_group_hidden` on every tree mutation. Folder influence extends that same
mechanism from a boolean to a float:

```
effective(layer) = enabled ? layer.influence * layer.group_influence_cached : 0
```

`group_influence_cached` is the product of the ancestor folders' influence, recomputed by the same
resync pass. The hot path stays one multiply per element and never walks ancestors.

Consequence worth recording: **folder influence does not depend on the migration.** It could land on
the current flat model unchanged. It is included here only because it is cheap once the node exists
and it validates the node design.

## Behaviour decisions

| Question | Decision |
|---|---|
| Move Up/Down when the neighbouring row is a folder | Step over the folder as one indivisible row. The layer keeps its parent. Move Up/Down never reparents — only dragging does. |
| Merge Down when a folder sits between the active layer and the next layer sibling | Skip the folder; merge into the next *layer* sibling. Folders stay invisible to Merge Down, as today. |
| Existing files | Open looking identical: order becomes "folders, then layers", the current draw order. |

## Architecture

### Data model

Modelled on Grease Pencil (`DNA_grease_pencil_types.h`): a common base node embedded by both
concrete types.

```c
struct SculptLayerTreeNode {
  SculptLayerTreeNode *next, *prev;
  char name[64];
  int flag;
  int uid;
  int8_t type;        /* LAYER | GROUP */
  ...
};

struct SculptLayer      { SculptLayerTreeNode base; float influence; int totelem; short domain, level; void *data; };
struct SculptLayerGroup { SculptLayerTreeNode base; float influence; ListBase children; };
```

A folder owns an **ordered list of children of both kinds**. That list *is* the sibling order, so
interleaving needs no extra field.

`Mesh` holds a single root `SculptLayerGroup` (`sculpt_layer_root`, always allocated, never drawn as
a row) rather than a bare children list, following `GreasePencil::root_group_ptr`. This keeps "the
root" and "a folder" the same type, so every tree walk, insertion and reparent has exactly one case
instead of a root special case; uid 0 keeps meaning the root.

`uid` is kept: every operator and the whole undo system address nodes by uid, and that is the one
thing the migration does not have to rewrite.

### One uid counter for all nodes

Today layer uids and group uids are separate counters that both start at 1, so "the very first layer
and the very first folder already share the number 1". That forces kind+uid to travel together
everywhere: `ReparentMove::is_group`, the `anchor_is_group` property on `SCULPT_OT_layer_move_to`,
the branches in `group_dest_is_inside_dragged_group`.

With one node type, use **one uid counter for all nodes**. Then `prev_uid` names any sibling
unambiguously and that entire shadow disappears. Versioning renumbers nodes and fixes up every
reference (`group_uid`, `parent_uid`, `Mesh::sculpt_layers_active_uid`).

### Runtime flat layer cache

`combine_layers_mesh`, `derive_base_mesh` and `apply_vert_layers` currently take
`const ListBaseT<SculptLayer> &` and walk a flat list. With a tree they take a flat
`Span<SculptLayer *>` served from a cache in `Mesh::runtime`, invalidated when the tree's topology
changes — mirroring `GreasePencil::layers()` and its `runtime->layer_cache`.

Without this the tree walk lands on the evaluation hot path. This is load-bearing, not an
optimisation.

## Versioning

The old flat lists stay as `DNA_DEPRECATED` fields, read only by versioning:

1. Build the tree from `group_uid` / `parent_uid`.
2. Order each folder's children: subfolders first, then layers, preserving each list's own order —
   this reproduces the current draw order exactly.
3. Assign fresh uids from the single counter; remap `sculpt_layers_active_uid`.
4. A layer or folder whose parent uid does not resolve (a dangling tag — corrupt file, a script
   writing `group_uid` directly) is attached to the root rather than dropped. The current UI silently
   loses such a layer from the tree while it still deforms the mesh; the migration fixes that by
   construction.

## Undo

The addressing model — uid, sibling anchor `prev_uid`, parent uid — is already correct and does not
depend on the lists being flat. Changes:

- `prev_uid` is looked up in the unified tree instead of "whichever list `is_group` selects".
- `ReparentMove::is_group` and the kind tags disappear with the unified uid counter.
- `SculptLayerUndoPayload` and `SculptLayerGroupUndoPayload` converge as the structs converge.
- `push_sculpt_layer_flags_batch` and the resync-diff pattern are unchanged; the resync additionally
  recomputes `group_influence_cached`, which is derived state and needs no undo record of its own —
  it is recomputed from the restored tree, exactly as `SCULPT_LAYER_GROUP_HIDDEN` is today.

## RNA / Python

`mesh.sculpt_layers` is a flat collection used by `properties_data_mesh.py`. It stays a flat
collection, backed by the runtime cache / a tree walk, so existing Python keeps working — the same
way Grease Pencil exposes both `grease_pencil.layers` (flat) and `layer_groups`.

Folder influence is exposed as a normal RNA float property with the same sculpt-aware update
callback the layer influence uses (`flush_interactive_update`), so dragging the slider does not push
a full geometry recalc per tick.

## Testing

GTest, in `blenkernel/intern/` alongside the existing sculpt layer tests:

- Tree construction from flat lists (versioning): nesting, ordering, dangling parent → root.
- Uid renumbering: no collisions, `sculpt_layers_active_uid` still resolves to the same layer.
- Commutativity: composing layers in any sibling order yields identical positions.
- Folder influence: nested folders multiply; influence 0 on a folder contributes exactly base.
- Runtime cache invalidation: add / remove / reparent / reorder rebuild the flat layer span.

## Staging

Each step is built and hand-verified before the next.

1. **Tree migration, no behaviour change.** DNA + versioning + BKE + runtime cache + undo + RNA +
   operators + tree view. Verifiable: everything behaves as before, and free interleaving works.
2. **Folder influence.** Node field + `group_influence_cached` in the resync pass + RNA + UI slider.

## Risks

- The blast radius is 342 references across 29 files, concentrated in the two most fragile areas of
  this project: `sculpt_undo.cc` (40) and the multires path — `multires_reshape.cc` (22),
  `subdiv_displacement_multires.cc`, `draw_pbvh.cc`, `key.cc`.
- Blend read/write becomes recursive; the allocation contract documented in `BKE_sculpt_layers.hh`
  (nodes from `MEM_new` or the C-style reader, freed with `MEM_delete_void`, trivially destructible,
  enforced by a static assert) must be preserved for the node structs.
- Step 1 must not change behaviour. Any behavioural difference found during it is a migration bug,
  not a feature — features go in step 2 or sub-project 3.
