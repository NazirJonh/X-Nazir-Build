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
#  define SCULPT_LAYERS_DEBUG_LOG 1
#endif

struct BlendWriter;
struct BlendDataReader;

namespace blender {
struct Mesh;
struct SculptLayer;
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

/** Remove \a layer from the mesh and free it (does not touch mesh geometry). */
void remove(Mesh &mesh, SculptLayer &layer);

/** Duplicate \a src (including its data) into \a mesh, right after \a src, and make it active. */
SculptLayer *duplicate(Mesh &mesh, const SculptLayer &src);

/** Active layer or null. */
SculptLayer *active_get(Mesh &mesh);
const SculptLayer *active_get(const Mesh &mesh);
/** Set the active layer (null clears the active index). */
void active_set(Mesh &mesh, const SculptLayer *layer);

/** Index of \a layer in the list, or -1. */
int index_of(const Mesh &mesh, const SculptLayer &layer);
/** Find a layer by its stable unique id, or null. */
SculptLayer *find_by_uid(Mesh &mesh, int uid);

/** True when any layer carries the Solo Base marker (see #SCULPT_LAYER_SOLO_HIDDEN). */
bool solo_active(const Mesh &mesh);

/* -------------------------------------------------------------------------------------------------
 * Data buffers.
 */

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
 */
float effective(const SculptLayer &layer);

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
 * NOTE: layers are duplicated with C-style allocators (#BLI_duplicatelist / #MEM_dupalloc_void) but
 * freed with #MEM_delete. This relies on #SculptLayer staying trivially destructible (enforced by a
 * static assert in `sculpt_layers.cc`). If a non-trivial member is ever added, switch the
 * duplication/free paths to matching C++-style allocators.
 */

void copy_list(ListBaseT<SculptLayer> *dst, const ListBaseT<SculptLayer> *src);
void free_list(ListBaseT<SculptLayer> *layers);
void blend_write(BlendWriter *writer, ListBaseT<SculptLayer> *layers);
void blend_read(BlendDataReader *reader, ListBaseT<SculptLayer> *layers);

}  // namespace blender::bke::sculpt_layers
