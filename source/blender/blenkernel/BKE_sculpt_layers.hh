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
#  define SCULPT_LAYERS_DEBUG_LOG 1
#endif

struct BlendWriter;
struct BlendDataReader;

namespace blender {
struct KeyBlock;
struct Mesh;
struct SculptLayer;
struct SculptLayerGroup;
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
 * that subtree from the root: the caller must reject that first (see #node_is_descendant_of), which
 * is asserted here rather than handled.
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
 * allocated by the blend-file reader, and its destructor has to run to release the cached vector.
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
 * row in the UI: the cached span is what the eval paths read, so it hands out a pointer to a freed
 * layer.
 */
void tag_layers_cache_dirty(SculptLayerGroup &group);

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
SculptLayer *add(Mesh &mesh, const char *name, short domain, int totelem, short level = 0);

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
bool is_stale(const SculptLayer &layer, int elem_num);

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
int element_count(const Mesh &mesh, const SculptLayer &layer);

/** #is_stale against #element_count, i.e. staleness judged on the layer's own domain. */
bool is_stale(const Mesh &mesh, const SculptLayer &layer);

/** Ensure \a layer.data holds \a totelem `float3` elements, zero-filling on (re)allocation. */
MutableSpan<float3> data_ensure(SculptLayer &layer, int totelem);
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
 */
void combine_layers_mesh(Span<float3> base,
                         Span<SculptLayer *> layers,
                         MutableSpan<float3> r_positions);

/**
 * Inverse of #combine_layers_mesh: recover the un-layered base from combined positions,
 * `r_base[i] = positions[i] - sum_over_enabled_vert_layers(data[i] * effective(layer))`.
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
