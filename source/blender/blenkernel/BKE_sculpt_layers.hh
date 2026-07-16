/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * Non-destructive sculpt layers (#SculptLayer, owned by #Mesh::sculpt_layers).
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

#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"

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

struct BlendWriter;
struct BlendDataReader;

namespace blender {
struct KeyBlock;
struct Mesh;
struct SculptLayer;
struct SculptLayerGroup;
}  // namespace blender

namespace blender::bke::sculpt_layers {

/** Size in bytes of a single #SculptLayer::data element (one `float3`). */
inline constexpr int64_t element_size = sizeof(float3);

/* -------------------------------------------------------------------------------------------------
 * List management. The owner is always #Mesh::sculpt_layers.
 */

/**
 * Allocate a new layer, give it a unique name and id, append it to the mesh and make it active.
 * The data buffer is zero-initialized to \a totelem elements. \a level is the grid storage level
 * for grid-domain layers (ignored for the vertex domain).
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

/** Find a layer by its stable unique id, or null. Uid 0 means "no layer" and always returns null. */
SculptLayer *find_by_uid(Mesh &mesh, int uid);
const SculptLayer *find_by_uid(const Mesh &mesh, int uid);

/** True when any layer carries the Solo Base marker (see #SCULPT_LAYER_SOLO_HIDDEN). */
bool solo_active(const Mesh &mesh);

/* -------------------------------------------------------------------------------------------------
 * Group (folder) list management. The owner is always #Mesh::sculpt_layer_groups.
 */

/**
 * Allocate a new group under \a parent_uid (0 = root), give it a unique id and a name unique among
 * its siblings, and append it to the mesh. Unlike #add there is no "active group" to set: groups are
 * always addressed explicitly by uid (see the design doc's non-goals).
 */
SculptLayerGroup *group_add(Mesh &mesh, const char *name, int parent_uid);

/**
 * Remove \a group from the mesh and free it. Only the group node itself: its direct children are
 * *not* touched, so the caller must reparent them first (see #SCULPT_OT_layer_group_remove), or they
 * would keep naming a uid that no longer resolves.
 */
void group_remove(Mesh &mesh, SculptLayerGroup &group);

/** Find a group by its stable unique id, or null. Uid 0 means "the root" and always returns null. */
SculptLayerGroup *group_find_by_uid(Mesh &mesh, int uid);
const SculptLayerGroup *group_find_by_uid(const Mesh &mesh, int uid);

/**
 * True when \a group is \a ancestor itself or sits anywhere below it in the tree. Used to reject a
 * drop of a group into its own subtree, which would detach that subtree into a cycle unreachable
 * from the root. Walks the #SculptLayerGroup::parent_uid chain, and is bounded by the number of
 * groups even if the stored data somehow already contains a cycle.
 */
bool group_is_descendant_of(const Mesh &mesh, const SculptLayerGroup &group, int ancestor_uid);

/**
 * Make \a group's name unique among its siblings — the groups sharing its #parent_uid — rather than
 * across the whole list. #BLI_uniquename cannot express that: it walks one flat #ListBase, whereas
 * #Mesh::sculpt_layer_groups holds every level of the tree at once, so it would rename a group that
 * merely shares a name with one in a different folder.
 *
 * Public rather than internal to #group_add because the RNA name setter needs the identical rule —
 * a second, subtly different spelling of "unique among siblings" is exactly the kind of drift that
 * makes two paths disagree about what a legal name is.
 */
void group_name_ensure_unique(const Mesh &mesh, SculptLayerGroup &group);

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
 * The weight also accounts for the folder cascade (#SCULPT_LAYER_GROUP_HIDDEN, maintained by
 * #resync_group_hidden), so a caller that tests #SCULPT_LAYER_ENABLED itself instead of routing
 * through here silently ignores folder visibility.
 */
float effective(const SculptLayer &layer);

/**
 * Recompute every layer's #SCULPT_LAYER_GROUP_HIDDEN bit from the current folder tree: a layer is
 * group-hidden exactly when any group on its #SculptLayer::group_uid → #SculptLayerGroup::parent_uid
 * chain is disabled. Call after every mutation of the tree (create / remove / reparent / toggle a
 * group), and record the resulting flag changes for undo (see #push_sculpt_layer_flags_batch).
 *
 * A full recompute rather than an incremental toggle, because nesting makes an incremental flag
 * wrong in principle: with a disabled group B inside a disabled group A, one bit on the layer cannot
 * remember "hidden by A" apart from "hidden by B", so re-enabling A alone would wrongly reveal B's
 * layers. A recompute depends only on the current tree, never on the history that produced it.
 *
 * Each group's effective state is memoized, so the cost does not grow with nesting depth — the tree
 * is walked once, not once per layer. The uid lookups themselves are linear scans, making this
 * quadratic in the number of groups; that is fine here, where groups are few, hand-made, and this
 * runs on a folder mutation rather than on a brush dab.
 *
 * Only ever writes #SCULPT_LAYER_GROUP_HIDDEN on existing layers: it adds nothing to either list,
 * removes nothing, and reorders nothing. Callers rely on that to diff the flags before and after by
 * walking the same list twice (see the editor's `group_cascade_resync_with_undo`).
 */
void resync_group_hidden(Mesh &mesh);

/**
 * Recompute combined vertex positions from an un-layered base:
 * `r_positions[i] = base[i] + sum_over_enabled_vert_layers(data[i] * effective(layer))`.
 * Only #SCULPT_LAYER_DOMAIN_VERT layers contribute. A layer whose element count does not match
 * \a base (stale after a topology change) is skipped. \a r_positions and \a base may not alias.
 */
void combine_layers_mesh(Span<float3> base,
                         const ListBaseT<SculptLayer> &layers,
                         MutableSpan<float3> r_positions);

/**
 * Inverse of #combine_layers_mesh: recover the un-layered base from combined positions,
 * `r_base[i] = positions[i] - sum_over_enabled_vert_layers(data[i] * effective(layer))`.
 */
void derive_base_mesh(Span<float3> positions,
                      const ListBaseT<SculptLayer> &layers,
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
void apply_vert_layers(const ListBaseT<SculptLayer> &layers, MutableSpan<float3> positions);

/**
 * Convenience wrapper of #apply_vert_layers over `mesh.sculpt_layers` and the mesh's own positions.
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
 * - Nodes are allocated with #MEM_new (#add, #duplicate, #copy_list, the undo payload) or by the
 *   blend-file reader (#blend_read), which allocates C-style. Both origins end up in the same list.
 * - Nodes are therefore freed with #MEM_delete_void (#remove, #free_list), the one call that accepts
 *   both. This mirrors #ListBaseT::free_no_destruct, which frees the neighboring
 *   #Mesh::vertex_group_names the same way.
 * - #SculptLayer::data is always allocated with #MEM_new_array_zeroed or #MEM_dupalloc_void and
 *   freed with #MEM_delete_void.
 *
 * No destructor ever runs on a layer, so this holds only while #SculptLayer is trivially
 * destructible; a static assert in `blenkernel/intern/sculpt_layers.cc` enforces that. If a
 * non-trivial member is ever added, the free paths must switch to #MEM_delete and #blend_read must
 * stop handing its nodes to them.
 */

void copy_list(ListBaseT<SculptLayer> *dst, const ListBaseT<SculptLayer> *src);
void free_list(ListBaseT<SculptLayer> *layers);
void blend_write(BlendWriter *writer, ListBaseT<SculptLayer> *layers);
void blend_read(BlendDataReader *reader, ListBaseT<SculptLayer> *layers);

/**
 * The same four ID lifetime helpers for #Mesh::sculpt_layer_groups. Groups own no data buffer, so
 * these are simpler than the layer versions, but they follow the same allocation contract: nodes
 * come from #MEM_new (#group_add, #group_copy_list) or C-style from the blend-file reader
 * (#group_blend_read), and are therefore freed with #MEM_delete_void, which accepts both.
 */
void group_copy_list(ListBaseT<SculptLayerGroup> *dst, const ListBaseT<SculptLayerGroup> *src);
void group_free_list(ListBaseT<SculptLayerGroup> *groups);
void group_blend_write(BlendWriter *writer, ListBaseT<SculptLayerGroup> *groups);
void group_blend_read(BlendDataReader *reader, ListBaseT<SculptLayerGroup> *groups);

}  // namespace blender::bke::sculpt_layers
