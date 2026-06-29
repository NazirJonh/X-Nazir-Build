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
 * Apply `positions[i] += layer.data[i] * factor` over the overlapping range. Used to keep the
 * combined result in sync when influence changes (factor = new - old), when toggling visibility
 * (factor = +/- influence) and when removing a layer (factor = -influence).
 */
void apply_delta_mesh(const SculptLayer &layer, float factor, MutableSpan<float3> positions);

/** Effective influence: the layer's influence when enabled, 0 when disabled. */
float effective(const SculptLayer &layer);

/**
 * Recompute combined vertex positions from an un-layered base:
 * `r_positions[i] = base[i] + sum_over_enabled_vert_layers(data[i] * effective(layer))`.
 * Only #SCULPT_LAYER_DOMAIN_VERT layers contribute. Sizes may differ; the overlapping range is
 * used. \a r_positions and \a base may not alias.
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
