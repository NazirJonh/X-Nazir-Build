/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * Non-destructive sculpt layers (#SculptLayer), held in the tree rooted at
 * #Mesh::sculpt_layer_root.
 *
 * Each layer stores a per-element displacement delta plus an influence factor. The combined
 * sculpted result of a mesh is:
 * \code{.unparsed}
 *   position = base + sum_over_enabled_layers(layer.data[i] * layer.influence)
 * \endcode
 *
 * This module owns the data model (creation, removal, duplication, data buffers), the
 * mesh-domain application math used to keep the combined result in sync, and the blend-file
 * IO / copy / free helpers used by the #Mesh ID type. The sculpt-mode integration (recording
 * strokes, multires application and the operators) lives in the editor module.
 */

#include "DNA_listBase.h"

#include "BLI_array.hh"
#include "BLI_cache_mutex.hh"
#include "BLI_function_ref.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

/**
 * Master switch for every sculpt-layer debug/perf print across the module (stroke timing, base
 * flush traces, CCG attach traces, invariant checks). Each translation unit that has one of these
 * probes ties its local macro (`SLF_PERF`, `SLP_PERF`, `SLP_RNA_PERF`, etc.) to this flag, so
 * flipping it here disables all of them at once for performance measurement instead of hunting
 * down each file's local switch. Set to 0 to compile every probe out as a no-op.
 */
#ifndef SCULPT_LAYERS_DEBUG_LOG
#  define SCULPT_LAYERS_DEBUG_LOG 0
#endif

/**
 * Probe for the cost of #tree_copy, which #mesh_copy_data runs unconditionally on every mesh copy,
 * evaluation copies included (see #copy_profile_note for why that is not simply gated away).
 *
 * Split from #SCULPT_LAYERS_DEBUG_LOG rather than tied to it like the other probes: answering "how
 * many copies does one stroke pay for, and how many bytes" needs a console that is not also
 * carrying the stroke timing and base-flush traces. Set this to 1 on its own to measure, and put
 * it back to 0 — the probe prints from inside a hot path.
 */
#ifndef SCULPT_LAYERS_DEBUG_COPY
#  define SCULPT_LAYERS_DEBUG_COPY 0
#endif

namespace blender {
struct BlendDataReader;
struct BlendWriter;
struct KeyBlock;
struct Main;
struct Mesh;
struct Object;
struct SculptLayer;
struct SculptLayerGroup;
struct SculptLayerMask;
struct SculptLayerTreeNode;
}  // namespace blender

namespace blender::bke::sculpt_layers {

/** Size in bytes of a single #SculptLayer::data element (one `float3`). */
inline constexpr int64_t element_size = sizeof(float3);

/* -------------------------------------------------------------------------------------------------
 * The tree. Every node hangs, directly or through folders, off #Mesh::sculpt_layer_root.
 *
 * The root is a #SculptLayerGroup like any other folder, so a walk, an insert or a reparent has one
 * case rather than a separate "at the top level" case. It is never drawn as a row, always exists,
 * and keeps uid 0.
 */

/**
 * The mesh's root folder. Never null: #root_group_ensure runs on every path that produces a mesh.
 */
SculptLayerGroup *root_group(Mesh &mesh);
const SculptLayerGroup *root_group(const Mesh &mesh);

/**
 * Allocate #Mesh::sculpt_layer_root if it is missing, as an empty, enabled, expanded folder with
 * uid 0 and no parent.
 *
 * Called from #Mesh ID creation and from the blend-file reader, which is also what makes a
 * pre-migration file (whose SDNA has no `sculpt_layer_root` member, so the pointer reads back null)
 * open with an empty tree rather than crash. Idempotent.
 *
 * The enabled/expanded bits matter: the root takes part in the ancestor walks like any other
 * folder, so a root with flag 0 would read as a disabled folder and hide every layer on the mesh.
 */
void root_group_ensure(Mesh &mesh);

/**
 * Checked downcasts of a node to its concrete type — null when \a node is null or of the other
 * kind. The cast itself is legal because #SculptLayerTreeNode is the first member of both concrete
 * types; the kind test is what keeps a folder from being read as a layer (whose #SculptLayer::data
 * would then be read past the end of the smaller allocation).
 */
SculptLayer *node_as_layer(SculptLayerTreeNode *node);
const SculptLayer *node_as_layer(const SculptLayerTreeNode *node);
SculptLayerGroup *node_as_group(SculptLayerTreeNode *node);
const SculptLayerGroup *node_as_group(const SculptLayerTreeNode *node);

/**
 * Find any node of \a mesh by its stable unique id, or null when no node holds it.
 *
 * Replaces the two kind-specific lookups: one uid counter (#node_unique_uid) spans both kinds, so a
 * uid names exactly one node and the caller no longer has to know which list to search. Pair with
 * #node_as_layer / #node_as_group when a specific kind is required.
 *
 * Uid 0 resolves to the root group. NOTE: that is *not* the same convention as
 * #Mesh::sculpt_layers_active_uid, where 0 means "no active layer" — read that field through
 * #active_get, never through this function.
 */
SculptLayerTreeNode *node_find_by_uid(Mesh &mesh, int uid);
const SculptLayerTreeNode *node_find_by_uid(const Mesh &mesh, int uid);

/**
 * Walk \a start and its ancestor folders up to the root, returning the first for which \a predicate
 * is true, or null when none matches.
 *
 * The single cycle-safe ancestor walk of the module: both "is X an ancestor of me"
 * (#node_is_descendant_of) and the tree view's "do all my ancestors carry this flag" row checks are
 * this one walk with a different predicate, so the fragile Floyd bookkeeping lives in exactly one
 * place rather than being re-spelled per call site.
 *
 * Bounded by cycle detection (Floyd) rather than by reaching the root: a cycle in stored data (a
 * corrupt file, a future editing bug) must not hang the UI, which calls this on every redraw. The
 * fast cursor is tested after every single step, so it visits every folder on the chain — including
 * every member of a cycle — before the slow one can catch it; a chain that closes on itself without
 * a match therefore never would, and the walk stops.
 */
const SculptLayerGroup *find_ancestor(const SculptLayerGroup *start,
                                      FunctionRef<bool(const SculptLayerGroup &)> predicate);

/**
 * True when \a node sits anywhere strictly below \a ancestor in the tree.
 *
 * Strict: a node is *not* its own descendant. Callers rejecting a drop of a folder into its own
 * subtree therefore need their own identity check for "into itself" — the two questions are
 * separate and this answers only the second.
 *
 * Bounded by cycle detection rather than by reaching the root (see #find_ancestor, which it walks
 * with): a cycle in stored data (a corrupt file, a future editing bug) must not hang the UI, which
 * calls this on every drop-hover redraw.
 */
bool node_is_descendant_of(const SculptLayerTreeNode &node, const SculptLayerGroup &ancestor);

/**
 * Unlink \a node from wherever it currently sits and link it into \a dst's children, directly after
 * \a after — or at the head when \a after is null.
 *
 * Head rather than tail for a null \a after, because that is the convention the rest of the module
 * already spells ("a null cursor means head", in the move operator and in every undo re-insertion
 * fallback): a drop above the first child names no predecessor, and the node must land where the
 * cursor was.
 *
 * \a after must be a child of \a dst, or null. Moving a folder into its own subtree would detach
 * that subtree from the root, so the call is refused and logged. Callers are still expected to
 * reject it first (see #node_is_descendant_of) — the refusal here is a backstop, not the place to
 * tell the user.
 */
void node_move_into(Mesh &mesh,
                    SculptLayerTreeNode &node,
                    SculptLayerGroup &dst,
                    SculptLayerTreeNode *after);

/**
 * A uid no node of \a mesh holds — the number the next node created on it takes.
 *
 * One counter for every node, of either kind. Layer uids and group uids used to be two counters
 * that both started at 1, so the first layer and the first folder collided on 1 and a uid alone
 * never named a unique row; every caller had to carry the kind alongside it.
 */
int node_unique_uid(const Mesh &mesh);

/**
 * Runtime-only state of a #SculptLayerGroup, reached through #SculptLayerGroup::runtime.
 *
 * Held behind a pointer rather than by value — following #GreasePencilLayerTreeGroup::runtime and
 * #bke::greasepencil::LayerGroupRuntime — because a #CacheMutex and a #Vector are not trivially
 * destructible, and no destructor ever runs on a node (see the allocation notes below). A raw
 * pointer *is* trivially destructible, so the group's static assert still holds and the runtime is
 * instead allocated and freed explicitly, by #group_runtime_ensure / #group_runtime_free.
 */
class SculptLayerGroupRuntime {
 public:
  /** Guards #layer_cache_. */
  CacheMutex layer_cache_mutex_;
  /**
   * Every layer at or below the owning group, depth-first in tree order. Rebuilt by #layers from
   * the child lists; never persisted.
   */
  Vector<SculptLayer *> layer_cache_;

  /** Guards #chain_mask_. */
  CacheMutex chain_mask_mutex_;
  /**
   * Product of the masks of every node from the root down to the owning group inclusive, or null
   * when no node on that chain carries one. Built by #chain_mask; never persisted.
   *
   * Built lazily rather than pushed top-down the way #SculptLayer::group_influence_cached is: that
   * one is a single float per layer, so an eager resync on every tree mutation is free, while this
   * is potentially megabytes per folder.
   */
  SculptLayerMask *chain_mask_ = nullptr;

  /** Releases #chain_mask_; defined out of line, where #mask_free is in scope. */
  ~SculptLayerGroupRuntime();
};

/**
 * Allocate \a group's runtime if it is missing. Idempotent.
 *
 * Must be called on every #SculptLayerGroup that comes into existence outside this module's own
 * creation paths — a group allocated for an undo payload, or one handed over by the blend-file
 * reader — because #layers dereferences the runtime rather than allocating it on demand (which
 * would be a data race: #layers is const and runs from evaluation threads).
 *
 * A group copy-constructed from another one needs its #SculptLayerGroup::runtime nulled *first*:
 * the shallow copy brings the source's pointer along, and two groups sharing one runtime would be
 * freed twice and would hand out the wrong tree's layers.
 */
void group_runtime_ensure(SculptLayerGroup &group);

/**
 * Free \a group's runtime and null the pointer. Call before freeing the group itself.
 *
 * The one part of a group that is freed with #MEM_delete rather than #MEM_delete_void: it is never
 * allocated by the blend-file reader, and its destructor has to run to release the cached vector
 * and the cached chain mask.
 */
void group_runtime_free(SculptLayerGroup &group);

/**
 * Invalidate the flat layer span of \a group *and of every folder above it*, so the next #layers
 * call rebuilds it.
 *
 * Upward, because a folder's span holds every layer below it: a change anywhere in a subtree
 * invalidates every ancestor's span too. Descendants are deliberately left alone — their own
 * contents did not change — which is what lets a rebuild higher up reuse their still-valid caches.
 *
 * Every path that links, unlinks or reorders a node must call this. A missed call is not a stale
 * row in the UI: the cached span is what the eval paths read, so it hands out a pointer to freed
 * memory.
 *
 * The chain mask is a separate cache with the opposite dependency direction and its own entry point
 * (#tag_chain_mask_dirty); a structural mutation only touches it when it actually *reparents* a
 * node, which is why the two are not tagged together.
 */
void tag_layers_cache_dirty(SculptLayerGroup &group);

/**
 * Invalidate the chain mask (#chain_mask) of \a group *and of every folder below it*, so the next
 * #chain_mask call rebuilds it.
 *
 * Downward, the mirror image of #tag_layers_cache_dirty: a chain mask folds in every *ancestor's*
 * mask, so it is the descendants that go stale when anything at \a group changes, and the ancestors
 * that do not.
 *
 * Every path that edits, adds or replaces a *folder's* mask must call this, as must every path that
 * moves a *folder* to a different parent (#node_move_into does so for the moved subtree). Kept apart
 * from #tag_layers_cache_dirty deliberately: a reorder among siblings cannot change any chain, and
 * a cached chain mask is potentially megabytes per folder, so tagging it from every structural
 * mutation would throw the whole tree's products away on every layer creation.
 *
 * A *layer's* mask is exempt, and skipping the call for one is an optimization rather than an
 * oversight: #chain_mask folds in #SculptLayerGroup::base's mask alone, so a layer's mask is never
 * part of any product and is read straight off the node by the composite instead.
 *
 * A missed call where it does apply is not a stale row in the UI: #chain_mask hands out a pointer
 * into the cache, and the next rebuild frees the one it replaces.
 */
void tag_chain_mask_dirty(SculptLayerGroup &group);

/**
 * Every layer at or below \a group, depth-first in tree order (a folder's own children follow it).
 *
 * Cached on the group's runtime and rebuilt only after #tag_layers_cache_dirty, so the eval paths
 * (which want "every layer, order irrelevant" and used to get it from the flat #Mesh::sculpt_layers
 * list) no longer walk the tree per call. The cache lives on the *group* rather than on
 * #Mesh::runtime so that any folder can answer "my layers, in order" for itself.
 *
 * The span is a view into the group's cache: it stays valid only until the next structural mutation
 * of that subtree. Callers holding it across an #add / #remove / #node_move_into must re-read it.
 */
Span<SculptLayer *> layers(const SculptLayerGroup &group);
/** #layers over the mesh's whole tree, i.e. from #root_group. */
Span<SculptLayer *> layers(const Mesh &mesh);
/** Every folder strictly below \a group, depth-first. The root itself is never included. */
Vector<SculptLayerGroup *> groups(const SculptLayerGroup &group);
/** #groups over the mesh's whole tree. */
Vector<SculptLayerGroup *> groups(const Mesh &mesh);

/* -------------------------------------------------------------------------------------------------
 * Layer management.
 */

/**
 * Allocate a new layer, give it a unique name and id, append it to the root folder and make it
 * active. The data buffer is zero-initialized to \a totelem elements. \a level is the grid storage
 * level for grid-domain layers (ignored for the vertex domain).
 */
SculptLayer *add(Mesh &mesh, const char *name, short domain, int64_t totelem, short level = 0);

/**
 * Remove \a layer from the mesh and free it (does not touch mesh geometry). When it was the active
 * layer, a neighbour takes over; otherwise the active layer is left alone.
 */
void remove(Mesh &mesh, SculptLayer &layer);

/** Duplicate \a src (including its data) into \a mesh, right after \a src, and make it active. */
SculptLayer *duplicate(Mesh &mesh, const SculptLayer &src);

/**
 * The active layer, or null.
 *
 * Resolved from #Mesh::sculpt_layers_active_uid rather than from a position in the list: a position
 * only identifies a layer for as long as nothing inserts, removes or reorders around it, whereas a
 * uid survives all three.
 */
SculptLayer *active_get(Mesh &mesh);
const SculptLayer *active_get(const Mesh &mesh);
/** Set the active layer; null clears it. \a layer must belong to \a mesh. */
void active_set(Mesh &mesh, const SculptLayer *layer);

/** True when any layer carries the Solo Base marker (see #SCULPT_LAYER_SOLO_HIDDEN). */
bool solo_active(const Mesh &mesh);

/* -------------------------------------------------------------------------------------------------
 * Group (folder) management.
 */

/**
 * Allocate a new group as the last child of the folder named by \a parent_uid (0 = the root), give
 * it a unique id and a name unique across the whole tree (see #node_name_ensure_unique). Unlike #add
 * there is no "active group" to set: groups are always addressed explicitly by uid (see the design
 * doc's non-goals).
 *
 * A \a parent_uid that names a layer, or no node at all, falls back to the root.
 */
SculptLayerGroup *group_add(Mesh &mesh, const char *name, int parent_uid);

/**
 * Unlink \a group from its parent and free it. The group must already be empty: a folder owns its
 * children, so freeing one that still has any would leak the whole subtree. The caller reparents
 * them first (see #SCULPT_OT_layer_group_remove), which is asserted here rather than handled — what
 * an orphaned subtree should become is the caller's decision, not this function's.
 */
void group_remove(Mesh &mesh, SculptLayerGroup &group);

/**
 * Make \a node's name unique across *every* node of the mesh tree — both kinds, at any depth — not
 * merely among its siblings. A no-op for a node with no parent (the root), which is never drawn and
 * needs no name.
 *
 * Whole-tree rather than per-folder because the flat #Mesh::sculpt_layers RNA collection and
 * #rna_SculptLayer_path both address a layer by its name alone: two layers sharing a name — even in
 * different folders — would make either resolve to the wrong one, silently. Follows #GreasePencil,
 * whose #get_node_names likewise spans the whole tree.
 *
 * The single authority for sculpt layer node names, for *both* kinds. It replaces the former
 * `group_name_ensure_unique` and the open-coded #BLI_uniquename call in the layer add path, which
 * were two spellings of the same rule only because layers and folders lived in two lists. A second,
 * subtly different spelling is exactly the kind of drift that makes two paths disagree about what a
 * legal name is.
 *
 * Public rather than internal to #add / #group_add because the RNA name setters need the identical
 * rule.
 */
void node_name_ensure_unique(SculptLayerTreeNode &node);

/* -------------------------------------------------------------------------------------------------
 * Data buffers.
 */

/**
 * True when \a layer holds data that no longer matches its domain's live element count \a elem_num,
 * i.e. the topology changed behind the layer's back (an Edit Mode edit, a modifier, a script or an
 * import that bypassed the layer hooks). The stored deltas are per-element, so there is no
 * meaningful way to apply them to a different element range; every consumer skips such a layer.
 *
 * This is the single authority on staleness: the alternative — comparing #SculptLayer::totelem
 * against an element count at each use — is the same predicate spelled out once per call site, and
 * a new call site that spells it slightly differently would silently apply a mismatched buffer.
 *
 * A layer with no data buffer is *not* stale: it contributes zeros and is allocated on demand by
 * #data_ensure. \a elem_num is the mesh vertex count for #SCULPT_LAYER_DOMAIN_VERT layers and the
 * grid point count for #SCULPT_LAYER_DOMAIN_GRID ones (see #bke::grid_totelem); the editor module's
 * `element_count` resolves it from an object.
 */
bool is_stale(const SculptLayer &layer, int64_t elem_num);

/**
 * Live element count of \a layer's own domain, resolved from mesh data alone, so it is usable where
 * no object (and therefore no #SubdivCCG) is reachable, such as an RNA property getter.
 *
 * Each layer is measured on its own domain, because a mesh can carry both: adding a multires
 * modifier to a mesh that already had vertex layers leaves the two kinds side by side, and the
 * object's current sculpt domain says nothing about the size of the other kind.
 *
 * For a grid layer this is the count implied by the layer's *own* storage level, not the multires
 * top level: a grid layer sitting at a different level is not stale, it is resampled to it
 * (#resample_grid_layers), whereas a count contradicting its own level cannot be mapped at all.
 */
int64_t element_count(const Mesh &mesh, const SculptLayer &layer);

/** #is_stale against #element_count, i.e. staleness judged on the layer's own domain. */
bool is_stale(const Mesh &mesh, const SculptLayer &layer);

/**
 * Ensure \a layer.data holds \a totelem `float3` elements, zero-filling on (re)allocation.
 *
 * \a totelem is 64-bit because a grid-domain count is `corners_num * grid_size(level)^2`, which
 * passes 2^31 at resolutions multires already permits. A negative count is refused (empty span,
 * layer untouched) rather than passed to the allocator as a huge unsigned size.
 */
MutableSpan<float3> data_ensure(SculptLayer &layer, int64_t totelem);
/** View of the layer data, empty when not allocated. */
MutableSpan<float3> data_get(SculptLayer &layer);
Span<float3> data_get(const SculptLayer &layer);
/** Zero the layer data (keeps the allocation). */
void data_clear(SculptLayer &layer);

/* -------------------------------------------------------------------------------------------------
 * Mesh-domain application. Multires (grid domain) is handled in the editor module because it needs
 * the runtime #SubdivCCG, which is not reachable from mesh data alone.
 */

/**
 * Apply `positions[i] += layer.data[i] * factor`. Used to keep the combined result in sync when
 * influence changes (factor = new - old), when toggling visibility (factor = +/- influence) and
 * when removing a layer (factor = -influence). A layer whose element count does not match
 * \a positions (stale after a topology change) is skipped rather than partially applied.
 *
 * Attenuated by the layer's weight mask and by the folder chain above it, exactly as
 * #combine_layers_mesh is. That is not optional: this is how a contribution the composite already
 * laid down gets *incrementally* corrected, so a delta measured without the mask would eat into the
 * masked region a little on every slider drag and would over-subtract when a layer is removed.
 */
void apply_delta_mesh(const SculptLayer &layer, float factor, MutableSpan<float3> positions);

/**
 * Effective influence: the layer's influence when enabled, 0 when disabled. This is the single
 * authority for "how much a layer contributes"; every consumer (mesh combine/derive, the RNA
 * setters, the interactive drag, the multires grid collector and flush, and the undo restore)
 * routes its weight through this function, so a future change to the blend/visibility/mute
 * semantics is a one-line edit here rather than a scattered rewrite. The per-context composition
 * math (object-space vertex add, tangent-space grid add, the GPU drag shader) stays specialized by
 * necessity, but the *weight* they multiply by always comes from here.
 *
 * The weight also accounts for the folder cascade (#SCULPT_LAYER_GROUP_HIDDEN and the ancestor
 * influence product #SculptLayer::group_influence_cached, both maintained by #resync_group_state),
 * so a caller that tests #SCULPT_LAYER_ENABLED and reads #SculptLayer::influence itself instead of
 * routing through here silently ignores folder visibility and folder influence.
 */
float effective(const SculptLayer &layer);

/**
 * Recompute the derived folder-cascade state on every layer from the current tree, in one top-down
 * walk. Two things are written per layer:
 * - #SCULPT_LAYER_GROUP_HIDDEN: set exactly when any group on the #SculptLayerTreeNode::parent chain
 *   is disabled.
 * - #SculptLayer::group_influence_cached: the product of the #SculptLayerGroup::influence of every
 *   ancestor folder, which #effective multiplies the layer's own influence by.
 *
 * Call after every mutation of the tree (create / remove / reparent / toggle a group / change a
 * folder influence), and record the resulting *flag* changes for undo (see
 * #push_sculpt_layer_flags_batch). The float cache is derived, non-undoable state and is deliberately
 * outside that flag diff.
 *
 * A full recompute rather than an incremental toggle, because nesting makes an incremental flag
 * wrong in principle: with a disabled group B inside a disabled group A, one bit on the layer cannot
 * remember "hidden by A" apart from "hidden by B", so re-enabling A alone would wrongly reveal B's
 * layers. A recompute depends only on the current tree, never on the history that produced it.
 *
 * One top-down walk: each folder hands its own effective state to its children, so every node is
 * visited exactly once and no memo table, uid lookup or cycle guard is needed — all three were
 * artifacts of the parent-uid tags, where a child could sit ahead of its parent in the flat list and
 * the chain had to be re-resolved per layer.
 *
 * Only ever writes those two fields on existing layers: it adds no node, removes none, and reorders
 * none. Callers rely on that to diff the flags before and after by walking the tree twice (see the
 * editor's `group_cascade_resync_with_undo`), which is well-defined because #layers returns a
 * deterministic tree order.
 */
void resync_group_state(Mesh &mesh);

/**
 * Float-only counterpart of #resync_group_state: rebuild every layer's
 * #SculptLayer::group_influence_cached from the tree without touching any flag. Used by the undo
 * restore path, where a structural change (reparent / create / delete) staleifies the cache even
 * though the folder influence values themselves are never undone, and the #SCULPT_LAYER_GROUP_HIDDEN
 * / Solo-Base flags were already restored from the undo payload and must not be disturbed. Also used
 * on blend read to derive the cache from the reconstructed tree.
 */
void refresh_group_influence_cache(Mesh &mesh);

/**
 * Recompute combined vertex positions from an un-layered base:
 * `r_positions[i] = base[i] + sum_over_enabled_vert_layers(data[i] * effective(layer))`.
 * Only #SCULPT_LAYER_DOMAIN_VERT layers contribute. A layer whose element count does not match
 * \a base (stale after a topology change) is skipped. \a r_positions and \a base may not alias.
 *
 * A layer carrying a weight mask, or sitting under a folder that does, is attenuated per element by
 * the product of those masks — see #chain_mask. The sum above is then over `data[i] *
 * effective(layer) * mask[i]`. A node with no mask anywhere on its chain costs one pointer test.
 */
void combine_layers_mesh(Span<float3> base,
                         Span<SculptLayer *> layers,
                         MutableSpan<float3> r_positions);

/**
 * Inverse of #combine_layers_mesh: recover the un-layered base from combined positions,
 * `r_base[i] = positions[i] - sum_over_enabled_vert_layers(data[i] * effective(layer))`.
 *
 * Exactly the forward pass with a negated weight, masks included — it runs the same code, so the two
 * cannot drift apart. Anything else would move the base a little on every flush, cumulatively.
 */
void derive_base_mesh(Span<float3> positions,
                      Span<SculptLayer *> layers,
                      MutableSpan<float3> r_base);

/**
 * Compose the enabled vertex-domain layers as a final object-space offset on top of the given
 * positions: `positions[i] += sum_over_enabled_vert_layers(data[i] * effective(layer))`.
 *
 * Unlike #combine_layers_mesh (which rebuilds combined positions from a separate base), this adds
 * onto whatever the positions already hold. Used both by the mesh-eval composition step and by the
 * sculpt-mode display path (composing onto the active shape key's deformed positions). A layer whose
 * element count does not match \a positions is skipped.
 *
 * Masked like #combine_layers_mesh: it routes each layer through #apply_delta_mesh, which is where
 * the weight — folder chain and all — is resolved for every path in this module.
 */
void apply_vert_layers(Span<SculptLayer *> layers, MutableSpan<float3> positions);

/**
 * Convenience wrapper of #apply_vert_layers over the mesh's whole tree and its own positions.
 * This is the mesh-eval composition step: shape keys are applied by the virtual ShapeKey modifier,
 * which overwrites the positions from the key blocks and would otherwise discard the layer
 * contribution, so the layer is re-added here to keep it visible on top of the morphed form. Mirrors
 * the grid-domain composition done at subdivision-surface evaluation time.
 */
void apply_vert_layers_eval(Mesh &mesh);

/**
 * Shape-key transition: move the vertex-layer contribution out of / back into #Mesh::vert_positions.
 *
 * Which carrier holds the layers depends on whether the mesh has shape keys, and the two are
 * mutually exclusive:
 * - No shape key: the layers are baked into #Mesh::vert_positions (the brush writes them there) and
 *   evaluation does not add them again.
 * - Shape key: #Mesh::vert_positions and the key blocks hold the un-layered basis, and the layers
 *   are composed on top at evaluation (see #apply_vert_layers_eval, gated on the shape-key deform).
 *
 * A mesh that gains its first key would otherwise copy the layer-baked positions into the Basis
 * block and then have the layers composed a second time at evaluation (the layer offset shows up
 * doubled); a mesh that loses its last key would keep a basis with no layers in it and the
 * composition step gone (the layers disappear from the surface). #strip_vert_layers_from_positions
 * is therefore called when a mesh gains a #Key (#BKE_key_add) and
 * #bake_vert_layers_into_positions when it loses it (#BKE_object_shapekey_free). Both are a no-op
 * on a mesh without vertex-domain layer data.
 */
void strip_vert_layers_from_positions(Mesh &mesh);
void bake_vert_layers_into_positions(Mesh &mesh);

/**
 * Bake Sculpt Layers on a mesh with *relative* shape keys: append a key block holding the combined
 * vertex-layer contribution, `basis + sum_over_enabled_vert_layers(data[i] * effective(layer))`, at
 * value 1 and relative to the reference key. Returns the new block, or null when the mesh has no
 * relative shape keys or no vertex-layer data to bake.
 *
 * The layers cannot simply be dropped there: with shape keys the positions and the key blocks hold
 * the un-layered basis and the layers are composed on top at evaluation, so removing them would
 * delete the sculpted form. A block relative to the basis contributes exactly its delta to the
 * basis, `(basis + sum) - basis = sum`, independent of every other key's weight — so the surface is
 * unchanged at value 1, and the user keeps the baked result as one shape key they can mute or dial
 * back to 0.
 */
KeyBlock *bake_vert_layers_into_new_shape_key(Mesh &mesh);

/**
 * Fold a vertex layer's contribution into the shape keys: `positions[i] += layer.data[i] * factor`
 * on #Mesh::vert_positions *and on every key block*. The vertex-domain counterpart of
 * #BKE_multires_sculpt_layer_apply_to_mdisps.
 *
 * Only used for *absolute* shape keys, where #bake_vert_layers_into_new_shape_key does not apply (a
 * new block there is another keyframe in time, not a dial-able offset). Shifting every block (and
 * the basis positions that mirror the reference block) by the same per-vertex offset shifts the
 * interpolated result by exactly that offset, so the surface stays put. The bake passes
 * `effective(layer)` and its undo `-effective(layer)`.
 *
 * A no-op on a mesh without shape keys — there the layers are already baked into the positions, so
 * a bake only has to drop the layer list. Key blocks whose element count does not match the mesh
 * (stale) are skipped.
 */
void apply_vert_layer_to_shape_keys(Mesh &mesh, const SculptLayer &layer, float factor);

/* -------------------------------------------------------------------------------------------------
 * Grid (multires) domain maintenance.
 */

/**
 * Resample every grid-domain layer's tangent displacement to \a new_level (bilinear upsample or
 * exact-stride subsample of the coefficients), keeping the invariant that grid layers are always
 * stored at the multires top level. Called whenever the top level changes (Subdivide / Delete
 * Higher). Layers whose stored data cannot be mapped (stale level or topology mismatch) are wiped
 * with a warning; `new_level <= 0` wipes all grid layer data (no subdivision left to displace).
 * \a grids_num is the number of displacement grids (`Mesh::corners_num`).
 */
void resample_grid_layers(Mesh &mesh, int grids_num, int new_level);

/**
 * Resample every grid-domain weight mask in the tree to \a new_level, the mask counterpart of
 * #resample_grid_layers and called alongside it.
 *
 * Masks ride through the same resamplers the layer coefficients do, so a weight lands on exactly
 * the grid point of the coefficient it weights; a mask that slid relative to its layer on a level
 * change would show up as the mask drifting across the surface.
 *
 * Walks folders as well as layers: a folder's mask attenuates its whole subtree through
 * #chain_mask, so leaving it at the old level would silently drop it (#is_stale_mask would reject
 * it and the subtree would spring back to its full contribution). Grid masks are recognized by
 * their own geometry — `block_size` is a CCG grid area and `totelem` is that times \a grids_num —
 * so a vertex-domain mask on the same tree is left untouched. `new_level <= 0` drops grid masks
 * entirely, there being no grid domain left for them to describe.
 */
void resample_grid_masks(Mesh &mesh, int grids_num, int new_level);

/**
 * Resample a dense grid-domain weight buffer from \a old_level to \a new_level, returning a buffer
 * of `grid_totelem(grids_num, new_level)` weights clamped to 0..1.
 *
 * The shared core of #resample_grid_masks and of the weight-mask edit session, which authors on the
 * CCG at the *sculpt* level while the mask is stored at the multires *top* level. Both must map a
 * weight onto the grid point of the coefficient it weights, so both go through this one mapping
 * rather than spelling it twice.
 */
Array<float> resample_grid_mask_values(Span<float> dense,
                                       int grids_num,
                                       int old_level,
                                       int new_level);

/* -------------------------------------------------------------------------------------------------
 * Weight masks (#SculptLayerMask). Attached to a #SculptLayerTreeNode through its `mask` pointer;
 * this section only owns the container's own lifetime, not the tree's use of it.
 */

/** Number of blocks needed to cover \a totelem elements. */
int mask_blocks_num(int64_t totelem, int block_size);

/**
 * True when \a mask cannot be indexed over a domain of \a elem_num elements, so every consumer must
 * treat the node as unmasked rather than apply it.
 *
 * The mask counterpart of #is_stale, and the single authority for the same reason: a call site that
 * spells the condition itself would sooner or later spell it slightly differently and index a block
 * table that does not describe the domain it is walking.
 *
 * Two distinct states answer true here, and both are reachable in normal use:
 * - The element count disagrees, i.e. the topology changed behind the mask's back, or the mask came
 *   from #chain_mask and was folded from an ancestor sized to a different domain.
 * - The mask describes no blocks at all. #mask_blend_read neutralizes a mask from a truncated or
 *   hand-edited file to `totelem == 0, blocks_num == 0` with null array pointers, and #chain_mask
 *   can pass that on as a *non-null* product. On an empty domain the counts would otherwise agree
 *   and a block loop would run zero iterations, which reads as "this node contributes nothing"
 *   rather than "this node is unmasked" — silently dropping its whole contribution.
 */
bool is_stale_mask(const SculptLayerMask &mask, int64_t elem_num);

/**
 * Allocate a mask covering \a totelem elements, every block uniform at \a fill.
 * Returns null for `totelem == 0` — an empty domain carries no mask.
 */
SculptLayerMask *mask_new(int64_t totelem, int block_size, uint8_t fill);

void mask_free(SculptLayerMask *mask);

/** Deep copy. The shallow DNA struct copy done by #tree_copy must never be relied on. */
SculptLayerMask *mask_copy(const SculptLayerMask &src);

/**
 * Heap bytes this mask owns: the struct plus its four arrays.
 *
 * Exists for undo accounting. An undo step that captures masks and does not report them is charged
 * as free against the memory limit in the user preferences, so the stack is never trimmed for them
 * — a long run of mask edits on a dense mesh would then hold far more than the limit allows.
 */
int64_t mask_size_in_bytes(const SculptLayerMask &mask);

/** Value at a single element. Intended for tests and UI, not for hot loops — those go per block. */
uint8_t mask_value_at(const SculptLayerMask &mask, int64_t elem);

/** A single block as the composite loops see it. */
struct MaskBlock {
  bool uniform;
  /** Valid when #uniform. */
  uint8_t value;
  /** Valid when not #uniform: `block_size` bytes (fewer in the tail block). */
  const uint8_t *data;
};

/** \a block is a block index, not an element index — in `[0, mask.blocks_num)`. */
MaskBlock mask_block(const SculptLayerMask &mask, int block);

/** Write \a mask into a dense float buffer of `mask.totelem` elements, values in 0..1. */
void mask_expand(const SculptLayerMask &mask, MutableSpan<float> r_dense);

/**
 * Build a mask from a dense float buffer. Blocks whose bytes all agree collapse to uniform, which
 * is what keeps a mask from degrading to dense storage across repeated edit sessions.
 */
SculptLayerMask *mask_compress(Span<float> dense, int block_size);

/* -------------------------------------------------------------------- */
/** \name Weight mask editing session: suspend and resume
 *
 * While an editing session is open (opened and closed by the editor module, see #mask_edit_begin)
 * the node's weights sit in the *persistent* mask store — the `.sculpt_mask` attribute on the
 * original mesh, or #SubdivCCG::masks, which the multires flush copies into the base mesh's
 * `CD_GRID_PAINT_MASK` layer. That is what lets the whole existing mask toolset author them
 * unchanged, and also what makes them dangerous: anything that serializes the mesh, or that
 * flushes the CCG into the base mesh, would record the layer's weights as the user's own mask.
 *
 * Such an operation is bracketed rather than made to close the session: auto-save runs on a timer
 * and a depsgraph re-evaluation flushes, so closing would discard the user's in-progress mask edit
 * for a reason they never asked for. Inside the bracket the parked mask is back in place and the
 * operation sees exactly the state it would have seen with no session open.
 *
 * The mechanics live here rather than in the editor module because the two writers that must be
 * bracketed are blenkernel's own — #multires_flush_sculpt_updates and
 * #object_update_from_subsurf_ccg, the latter reached from #BKE_object_free_derived_caches on every
 * depsgraph re-evaluation, with no editor call site to wrap. Both bracket themselves, so every
 * caller is covered by construction, including callers that do not exist yet. Only the session's
 * *policy* — which node, when to open and close — stays in the editor module.
 * \{ */

/** Which open sessions a #MaskEditSuspendGuard parks. */
enum class MaskEditDomains {
  /**
   * Both domains. For a writer that serializes the mesh, where either store reaches the file.
   */
  All,
  /**
   * Grid-domain sessions only; a mesh-domain session is left alone entirely, neither parked nor
   * refused.
   *
   * For a writer that only ever touches `CD_GRID_PAINT_MASK`, where a mesh-domain session is not a
   * hazard in the first place. Parking one there would still cost: #mask_edit_suspend_mesh adds and
   * removes a `.sculpt_mask` layer on the mesh in #Main, and both flush primitives run from
   * evaluation — #object_update_from_subsurf_ccg on every depsgraph re-evaluation. Churning
   * attribute storage from there is a far stronger mutation than the in-place displacement writes
   * those functions already admit to, for no protection at all.
   */
  GridsOnly,
};

/**
 * Parks any open sculpt-layer weight-mask editing session for the guard's lifetime, then puts it
 * back.
 *
 * The only way to suspend a session: the suspend and resume primitives are file-private to the
 * implementation, so a bracket cannot be left stranded by an early return, and the nesting of two
 * brackets cannot un-park a session that an outer one still needs parked.
 *
 * The whole-#Main form covers a write of the .blend, where any object may be the one in sculpt
 * mode; the single-object form covers a write derived from that one object. Both are no-ops when no
 * session is open, which is the common case on every save and every re-evaluation.
 */
class MaskEditSuspendGuard {
  Main *bmain_ = nullptr;
  Object *object_ = nullptr;
  /* Held so the resume filters exactly as the suspend did. A guard that declined to park a session
   * must not release a reference an enclosing guard took, which an unfiltered resume would do. */
  MaskEditDomains domains_ = MaskEditDomains::All;
  bool refused_ = false;

 public:
  explicit MaskEditSuspendGuard(Main &bmain);
  /* \a domains is deliberately not defaulted. Both current callers want #MaskEditDomains::GridsOnly,
   * and the wider value is actively harmful to them (see the note above): a default would let a new
   * call site park the mesh domain by omission, adding and removing `.sculpt_mask` on every
   * evaluation. Spelling it out is one word at each site and removes the failure mode. */
  explicit MaskEditSuspendGuard(Object &object, MaskEditDomains domains);
  ~MaskEditSuspendGuard();

  MaskEditSuspendGuard(const MaskEditSuspendGuard &) = delete;
  MaskEditSuspendGuard &operator=(const MaskEditSuspendGuard &) = delete;

  /**
   * True when a session is open but could not be parked, so the guarded region would see the
   * layer's weights in the persistent store after all.
   *
   * A suspend refuses rather than destroys: the only way to park a mask whose buffer no longer
   * matches the live domain would be to drop the user's own mask, and a bracket fires on operations
   * the user never asked for.
   *
   * A refusal is *not* a reason to skip the write, and no caller may treat it as one. The condition
   * that causes it — a parked buffer describing a domain the object no longer has — does not clear
   * itself, so a caller that defers would defer forever; for the multires flush that means every
   * depsgraph re-evaluation returning early and the user's base sculpting accumulating in a CCG
   * that is discarded at the next rebuild. The answer is for reporting only: tell the user what was
   * written and why, then write it. Every refusal is also logged once per session by the
   * implementation, so a caller with nowhere to report to is still not silent.
   */
  bool suspend_refused() const
  {
    return refused_;
  }
};

/**
 * Resume an open session regardless of how many brackets are holding it parked, discarding their
 * depth.
 *
 * For the close path alone, which must compress the session's own weights and would otherwise
 * store the *user's* mask onto the node. Every other resume belongs to a #MaskEditSuspendGuard.
 * A no-op when no session is open or none is suspended.
 */
void mask_edit_force_resume(Object &object);

/**
 * Give up every open weight-mask editing session in \a bmain: put each one's parked user mask back
 * into the standard storage and forget the session, without compressing anything onto its node.
 *
 * For a global (memfile) undo decode, and for nothing else. A memfile step is a serialization of the
 * whole #Main taken with every session suspended (#BKE_memfile_undo_encode brackets itself), so what
 * it records is the user's own mask and the node's mask as it stood *before* the session opened. The
 * session's in-flight weights are in neither. Decoding therefore replaces the mask storage under a
 * session state that still claims to own it, and the next close would compress the user's restored
 * mask onto the node and then overwrite that mask with the parked buffer — both halves corrupted,
 * silently.
 *
 * Abandoned rather than closed: closing means compressing the session's dense weights onto its node,
 * and after the decode those weights are gone by definition — the snapshot never held them. What is
 * left to do is exactly what this does, restore the user's mask and drop the session, which is also
 * the state the decoded #Main describes. A #MaskEditSuspendGuard cannot serve here for the same
 * reason: it would put the session back on the far side of the decode, pointing at a buffer that has
 * been replaced.
 *
 * Losing the in-progress mask edit is inherent to a global undo across it and is logged, not silent.
 * Must run *before* the decode replaces the mesh data, while the storage the session borrowed is
 * still the one it borrowed. A no-op when no session is open, which is the common case.
 */
void mask_edit_abandon_all(Main &bmain);

/** \} */

/**
 * The per-element product of \a a and \a b, as a new mask the caller owns.
 *
 * A block that is uniform on both sides stays uniform, and a uniform zero on either side keeps the
 * block uniform as well. That is what lets a folder mask be folded into a whole subtree without
 * pushing every layer under it onto the composite's dense path.
 *
 * Returns null when the two do not describe the same domain (#SculptLayerMask::totelem and
 * `block_size` must both agree), which is also how a mask neutralized by #mask_blend_read reads.
 * Null rather than a product built from one side, and deliberately not an assert: masks go stale as
 * a matter of course when the mesh's element count changes, and once the operands disagree neither
 * one can be trusted to index the domain — a product sized by either block table would read past
 * the end of the other. Null is the module's existing "no mask" state, which every consumer already
 * has to handle.
 */
SculptLayerMask *mask_multiply(const SculptLayerMask &a, const SculptLayerMask &b);

/**
 * The product of the masks of every node from the root down to \a group inclusive, or null when no
 * node on that chain carries one.
 *
 * Cached on the group's runtime (#SculptLayerGroupRuntime::chain_mask_) and rebuilt only after
 * #tag_chain_mask_dirty, so a composite that consults it per element does not refold the chain per
 * call.
 *
 * The mask is owned by that runtime, not by the caller, and is a view into it in exactly the sense
 * the span from #layers is: it stays valid only until the next #tag_chain_mask_dirty on \a group or
 * on any folder above it, which is what a mask edit anywhere on the chain and a move of the subtree
 * both trigger. A caller holding it across one of those reads freed memory. In particular, the
 * rebuild folds in the *parent's* cached product after the parent's own lock has been released
 * again, so the lock held while that pointer is read is only this group's — structurally the same
 * exposure #layers already has when it extends its span from a child's cache, and bounded by the
 * same rule: the tag, not the rebuild, is the moment a held pointer went stale.
 *
 * The result may be *stale*, and callers must gate it with #is_stale_mask before indexing anything
 * with it. It is internally self-consistent — its own `blocks_num`, `block_offset` and `data_num`
 * agree, so it can never be read out of its own bounds — but it is sized to whatever the ancestors
 * happen to carry, which need not be the domain the caller is walking. Neither is a non-null result
 * proof that the chain describes any elements at all: a mask neutralized by #mask_blend_read
 * (`totelem == 0`, `blocks_num == 0`) folds through as a non-null product of zero blocks.
 *
 * Returns null on a tree whose parent chain closes on itself, which is only reachable from corrupt
 * data; see the guard in the implementation for why it is answered rather than asserted.
 */
const SculptLayerMask *chain_mask(const SculptLayerGroup &group);

/**
 * The weight maps that attenuate a layer's contribution, resolved for one composite pass over a
 * domain of a known size. At most two, and never a materialized product of them (see
 * #node_mask_for_composite).
 *
 * Both null means unmasked. Otherwise #primary is set and #secondary may be; when both are set they
 * are guaranteed to agree on `totelem` and `block_size`, so they share one block index.
 */
struct CompositeMask {
  const SculptLayerMask *primary = nullptr;
  const SculptLayerMask *secondary = nullptr;
};

/**
 * The weight maps that apply to \a layer over a domain of \a elem_num elements: its own mask, and
 * the product of the folder masks above it (#chain_mask). Both are gated by #is_stale_mask, the
 * chain no less than the layer's own — #chain_mask is folded from whatever the ancestors carry and
 * is never resized to the live domain, so it can name an element count this composite does not have.
 *
 * The two are handed back side by side rather than multiplied into one mask. A product would have to
 * be materialized (a whole second mask's worth of allocation and writes) and then cached somewhere
 * to be worth it, and neither is justified: the composite already streams 24 bytes per element, so
 * the second mask costs one more byte load and one more multiply, and only in blocks that are dense
 * on both sides. Folding it per block instead needs no allocation, no cache, and no invalidation.
 *
 * Shared by the vertex composite (`sculpt_layers.cc`) and the multires grid composite
 * (`multires_reshape.cc`, `subdiv_displacement_multires.cc`), so a layer's mask resolves exactly
 * once no matter which domain it is composed on.
 *
 * A layer flagged #SCULPT_LAYER_REC_EXEMPT resolves to no masks at all — neither its own nor the
 * folder chain's. See #rec_exempt_set for why.
 */
CompositeMask node_mask_for_composite(const SculptLayer &layer, int64_t elem_num);

/**
 * Whether \a node's own weight mask is in force (#SCULPT_LAYER_MASK_DISABLED unset).
 *
 * Answers for a layer and for a folder alike: the two enums spell the bit differently but share one
 * value, which a #BLI_STATIC_ASSERT beside the definition pins.
 *
 * Consulted on the *composite* path only — #node_mask_for_composite for a layer, the #chain_mask
 * rebuild for a folder. The operators that author a mask deliberately do not consult it: a switched
 * off mask stays fully editable, so its weights can be prepared before it is switched back on.
 */
bool mask_enabled(const SculptLayerTreeNode &node);

/**
 * Switch \a node's own weight mask on or off, leaving the mask itself untouched.
 *
 * Exists so that no caller holding a base-typed node has to choose between the two spellings of the
 * bit: #SCULPT_LAYER_MASK_DISABLED written onto a folder would be right by value and wrong by
 * meaning, and would read as a bug at every later glance. Knowledge of which symbol names the bit
 * stays in this pair.
 *
 * Writes the flag and nothing else. The effective mask of a node moves the composed surface, so
 * every caller must additionally invalidate the folder chain cache (#tag_chain_mask_dirty) and
 * recompose — see #SCULPT_OT_layer_mask_toggle for the full order.
 */
void mask_enabled_set(SculptLayerTreeNode &node, bool enable);

/**
 * Mark \a layer as the one REC is armed on, clearing #SCULPT_LAYER_REC_EXEMPT from every other
 * layer of \a mesh. Passing null clears the exemption throughout, which is what the sculpt-mode
 * exit and entry paths call.
 *
 * While REC is armed on a layer, every composite ignores that layer's mask and the masks of the
 * folders above it. The brush moves the *composed* surface by D and the recorded delta is stored
 * raw, so a masked layer would have to store `D / mask[i]` to reproduce that same D — unbounded
 * where the mask reaches zero, and undefined where it is exactly zero. #layer_toggle_rec_exec
 * already pins #SculptLayer::influence to 1.0 for precisely that reason; this is the same pinning
 * applied to the other factor of the same product.
 *
 * The answer lives on the node rather than in a variable of this module because the composites
 * reach a layer through a #Mesh or through bare spans, and the vertex composite under a shape key
 * runs on an *evaluated* mesh with no #Object and no #SculptSession in scope at all (see
 * #apply_vert_layers_eval). #SculptLayerTreeNode::flag rides the original-to-evaluated copy
 * verbatim, so the exemption arrives wherever the layer does — and is scoped to the one mesh that
 * owns the node, which no process-wide slot could be.
 *
 * The editor module is the single *persistent* writer
 * (#ed::sculpt_paint::layers::rec_exemption_refresh). A caller may still clear the bit temporarily
 * around a question that must be answered as though REC were disarmed, as #layer_toggle_rec_exec
 * does when it asks whether the layer carries a mask at all; what no other site does is leave a
 * value of its own behind. Idempotent, and cheap enough (one flag write per layer) to call from
 * every path that can change either half of the answer.
 *
 * Returns whether any layer's bit actually moved, so that a caller holding a composed surface knows
 * whether that surface is now stale and has to be recomposed. False on the overwhelmingly common
 * "nothing to repair" call, which is what lets the recompose be conditional.
 *
 * Load-bearing constraint for callers: do not flip the bit between a forward compose of a layer and
 * the inverse of that same compose. #combine_layers_mesh and #derive_base_mesh each resolve the
 * layer's masks independently, so a flip in between would have the base absorb the difference
 * between the masked and the unmasked contribution — a permanent dent, not a display artifact. The
 * one caller that must flip it (#layer_toggle_rec_exec) therefore flushes anything holding an
 * un-inverted compose first.
 */
bool rec_exempt_set(const Mesh &mesh, const SculptLayer *layer);

/**
 * Mark \a layer as the one REC is armed on for #SCULPT_LAYER_REC_ARMED, clearing the bit from every
 * other layer of \a mesh. Passing null clears it throughout.
 *
 * The shape of #rec_exempt_set and, in sculpt mode, always called with the same argument — both bits
 * mirror #SculptSession::layers::rec_active onto the active layer, from the single writer
 * #ed::sculpt_paint::layers::rec_exemption_refresh. What separates them is the mode exit: the
 * exemption is cleared there because nothing outside sculpt mode may compose a layer with its weight
 * map dropped, while this bit is deliberately left standing, which is what lets REC survive a trip
 * through object mode. See #SCULPT_LAYER_REC_ARMED.
 *
 * Nothing composes from this bit, so — unlike #rec_exempt_set — moving it never invalidates a
 * composed surface. The return value is therefore informational rather than load-bearing, and no
 * caller has to recompose on it.
 */
bool rec_armed_set(const Mesh &mesh, const SculptLayer *layer);

/**
 * #node_mask_for_composite for a #SCULPT_LAYER_DOMAIN_GRID layer, additionally requiring that the
 * masks are cut one block per grid (`block_size == grid_area`) — the contract the multires paths
 * rely on when they use a grid index as a block index. Masks cut any other way are dropped and the
 * layer contributes fully, consistent with how #is_stale_mask fails open.
 */
CompositeMask grid_masks_for_composite(const SculptLayer &layer, int64_t elem_num, int grid_area);

/** How #mask_elem_weight has to combine the tables in a #MaskBlockWeight. */
enum class MaskFold : int8_t {
  /** No per-element table: #MaskBlockWeight::weight is the whole factor. */
  Uniform = 0,
  /** One dense table. */
  Single = 1,
  /** Two sides, at least one of them dense. */
  Pair = 2,
};

/**
 * One block of a #CompositeMask folded against a scalar weight, so a composite loop decides per
 * block and not per element.
 *
 * This and #mask_elem_weight are the single spelling of a masked layer's weight, and deliberately
 * so: the multires path composes a layer onto the base in four separate places and subtracts it
 * back in three of them, and a forward and an inverse written apart would drift the base a little
 * on every flush — silently and cumulatively. Every direction folds through here, and the inverse
 * is the *same* fold with a negated \a weight, which is exact in binary floating point.
 */
struct MaskBlockWeight {
  MaskFold fold = MaskFold::Uniform;
  /** The layer contributes nothing over this block; the caller skips it outright. */
  bool skip = false;
  /** Scalar factor with every uniform side already folded in. */
  float weight = 1.0f;
  /** Per-element tables, null when that side is uniform (its value is below) or absent. */
  const uint8_t *data_a = nullptr;
  const uint8_t *data_b = nullptr;
  /** Valid for #MaskFold::Pair when the corresponding table is null. */
  uint8_t value_a = 255;
  uint8_t value_b = 255;
};

/**
 * Fold \a block of \a masks against \a weight. \a block is a block index, and the caller must have
 * established that it indexes both masks — which #node_mask_for_composite guarantees.
 */
MaskBlockWeight mask_block_weight(const CompositeMask &masks, int block, float weight);

/**
 * The weight of element \a i within the block \a w was folded for.
 *
 * The two-mask case normalizes the product in one step, `a * b / 255^2`, rather than scaling each
 * side to 0..1 first: 1/255 is not exact in binary floating point, so a fully opaque folder folded
 * in as a separate factor would not leave its subtree's weight quite alone.
 */
inline float mask_elem_weight(const MaskBlockWeight &w, const int i)
{
  switch (w.fold) {
    case MaskFold::Uniform:
      return w.weight;
    case MaskFold::Single:
      return w.weight * float(w.data_a[i]) * (1.0f / 255.0f);
    case MaskFold::Pair: {
      const int value_a = (w.data_a != nullptr) ? int(w.data_a[i]) : int(w.value_a);
      const int value_b = (w.data_b != nullptr) ? int(w.data_b[i]) : int(w.value_b);
      return w.weight * float(value_a * value_b) * (1.0f / (255.0f * 255.0f));
    }
  }
  return w.weight;
}

/**
 * Write \a mask and its arrays. Called from the tree walk in #tree_blend_write, which has already
 * written the node the mask hangs off of — the node's #SculptLayerTreeNode::mask pointer is the
 * address the reader resolves this struct from, so it is stored as-is rather than nulled.
 */
void mask_blend_write(BlendWriter *writer, const SculptLayerMask &mask);

/**
 * Read \a mask's arrays back and validate them against each other. A file whose arrays are missing,
 * short, or inconsistent with the block table leaves the mask in the stale state (`totelem == 0`,
 * `blocks_num == 0`) rather than one the composite loops would read out of bounds.
 */
void mask_blend_read(BlendDataReader *reader, SculptLayerMask *mask);

/* -------------------------------------------------------------------------------------------------
 * ID lifetime helpers, called from the #Mesh ID type callbacks (see `mesh.cc`).
 *
 * NOTE: allocation and freeing of a layer are symmetric, but only in the weaker sense that suits a
 * DNA type, so both sides are documented here:
 *
 * - Nodes are allocated with #MEM_new (#add, #group_add, #duplicate, the undo payload) or by the
 *   blend-file reader (#tree_blend_read), which allocates C-style. Both origins end up in the same
 *   tree.
 * - Nodes are therefore freed with #MEM_delete_void (#remove, #tree_free), the one call that accepts
 *   both. This mirrors #ListBaseT::free_no_destruct, which frees the neighboring
 *   #Mesh::vertex_group_names the same way.
 * - #SculptLayer::data is always allocated with #MEM_new_array_zeroed or #MEM_dupalloc_void and
 *   freed with #MEM_delete_void.
 *
 * No destructor ever runs on a node, so this holds only while #SculptLayer and #SculptLayerGroup are
 * trivially destructible; static asserts in `blenkernel/intern/sculpt_layers.cc` enforce that. Note
 * a group's #SculptLayerGroup::children is a #ListBase of raw links, not an owning container, which
 * is what keeps the group trivially destructible while still owning its subtree — the recursion in
 * #tree_free is what actually releases it.
 *
 * - #SculptLayerGroup::runtime is the one exception, and the reason it is a *pointer*: it owns a
 *   #CacheMutex and a #Vector, so it is emphatically not trivially destructible and is therefore
 *   held out of line, allocated with #MEM_new by #group_runtime_ensure and freed with #MEM_delete
 *   (destructor and all) by #group_runtime_free, which every path calls just before releasing the
 *   group itself. The reader never allocates one — it reads back the null that
 *   #tree_blend_write stores — so the #MEM_new / #MEM_delete pairing is exact here.
 *
 * If a non-trivial member is ever added *by value*, the free paths must switch to #MEM_delete and
 * #tree_blend_read must stop handing its nodes to them.
 */

/**
 * Deep-copy \a src's whole tree into \a dst, including every layer's data buffer, and rebuild the
 * copy's #SculptLayerTreeNode::parent back-pointers. Uids carry over, so the copy's active layer
 * resolves by identity.
 *
 * \a dst must not already own a tree. A shallow copy would leave the two meshes sharing nodes, which
 * #tree_free would then release twice.
 */
void tree_copy(Mesh &dst, const Mesh &src);

#if SCULPT_LAYERS_DEBUG_COPY
/**
 * Report one #tree_copy that just ran, for #SCULPT_LAYERS_DEBUG_COPY.
 *
 * \a to_main tells the two cases apart: a copy that lands in Main is authored data the user asked
 * for, a copy that does not is an evaluation copy the depsgraph made and will throw away. Only the
 * latter is a candidate for sharing the layer buffers instead of duplicating them, and only the
 * count of those per stroke says whether that is worth doing.
 *
 * \a copy_ms is the wall time #tree_copy itself took, which is the number that decides whether the
 * duplication is worth engineering away: the byte count alone cannot tell a stall from bookkeeping
 * the allocator absorbs.
 *
 * Both numbers are cumulative for the session; take the difference across one stroke.
 */
void copy_profile_note(const Mesh &copied, bool to_main, double copy_ms);
#endif

/**
 * Clear #SCULPT_LAYER_REC_EXEMPT and #SCULPT_LAYER_REC_ARMED from every layer of \a mesh.
 *
 * Both bits are scoped to one Blender run and to the object the user armed REC on — they are not
 * authored state. #group_blend_write_recursive strips them for the same reason before writing, and
 * #group_blend_read_children strips them again on the way in.
 *
 * The path that needs this is ID duplication into Main (Shift+D, duplicating the mesh datablock):
 * #tree_copy is a verbatim deep copy, so a duplicate made while REC was armed inherits the
 * exemption, and nothing clears it until the object enters or leaves Sculpt Mode. Until then that
 * layer's own mask *and* its folder chain's are absent from every composite, with nothing in the UI
 * naming the cause.
 *
 * Deliberately *not* applied to evaluation copies: #apply_vert_layers_eval and the multires
 * composite read the flag off the evaluated mesh, so an eval copy that lost it would apply a mask
 * the recording layer must ignore (see #rec_exempt_set for why REC and masks cannot coexist).
 */
void rec_session_flags_clear(Mesh &mesh);

/**
 * Free \a mesh's whole tree, including the root group itself, and null #Mesh::sculpt_layer_root.
 * Recursive: a folder owns its children.
 */
void tree_free(Mesh &mesh);

void tree_blend_write(BlendWriter *writer, Mesh &mesh);
/**
 * Read the tree back and restore what the reader cannot: #SculptLayerTreeNode::parent is a DNA
 * pointer into the same file that no reader fixes up, so it is recomputed from the child lists on
 * the way down rather than stored. Calls #root_group_ensure, so a file whose SDNA predates the tree
 * (or has no sculpt layers at all) yields an empty tree rather than a null root.
 */
void tree_blend_read(BlendDataReader *reader, Mesh &mesh);

}  // namespace blender::bke::sculpt_layers
