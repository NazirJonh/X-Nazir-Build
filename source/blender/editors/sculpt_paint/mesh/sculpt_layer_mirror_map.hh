/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Core of #SCULPT_OT_layer_mirror_copy (see `sculpt_layer_mirror.cc`): the mapping from a source
 * side onto its mirrored targets and the write of that mapping into a layer's data. Nothing below
 * this interface touches a #bContext or a #wmOperator, so the modal preview and the exec path
 * cannot drift apart, and the core is testable without a UI.
 */

#pragma once

#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "DNA_scene_enums.h"

#include "BKE_sculpt_layers.hh"

struct Mesh;
struct Object;

namespace blender::ed::sculpt_paint::layers::mirror_copy {

/** How the mirrored displacement lands on the target side. */
enum class Mode {
  Replace = 0,
  Add = 1,
};

/** Which part of the source side is mirrored. */
enum class Limit {
  All = 0,
  FaceSet = 1,
};

/**
 * Everything the mapping and the write need, resolved once.
 */
struct MirrorParams {
  Mode mode = Mode::Replace;
  Limit limit = Limit::All;
  bool use_mask = false;
  /** True for the grid (multires) domain, false for the vertex domain. */
  bool grids = false;
  /** Enabled symmetry axes. */
  ePaintSymmetryFlags symm = PAINT_SYMM_NONE;
  /** Object-space position of the source point; its per-axis sign picks the source side. */
  float3 source_point = float3(0.0f);
  /** How far a mirrored position may be from its target and still count as one. */
  float tolerance = 0.0f;
  /** Distance within which an element counts as lying on a symmetry plane. */
  float plane_eps = 0.0f;
  /** Face set the source point sits in; -1 leaves the face-set limit unrestricted. */
  int face_set_id = -1;
};

/** One mirrored displacement: from source element \a src onto target element \a dst. */
struct MirrorPair {
  /** Layer element index, in the layer's own layout (the top level for grids). */
  int64_t src;
  int64_t dst;
  /**
   * The mirrored target delta, unmasked: Replace writes it times the weight, Add sums it. Kept
   * unmasked so the mask limit can be toggled without rebuilding the mapping.
   */
  float3 dst_delta;
  /** Composite weight-mask value of the source element (1.0 when the layer carries no mask). */
  float src_weight;
  /** Whether the source lies in the face set the source point resolved to. */
  bool src_in_face_set;
};

/**
 * The distance between neighbouring elements of the domain the layer stores in, in object space:
 * the coarse edge length for the vertex domain, and that length divided by the layer's own
 * top-level subdivision for the grid domain.
 *
 * The tolerance has to be relative to it. Measured against the coarse edge alone it is many grid
 * cells wide at a sculpt level above the first, and then the nearest search accepts a point that is
 * not the mirror of anything; tightened to the cell it is what the plane test and the target
 * distance check are comparing against.
 */
float mirror_element_spacing(const Mesh &mesh, int top_grid_size);

/**
 * Map the whole source side of \a layer, in the domain \a params.grids names, into \a r_pairs.
 *
 * The source side is decided by \a params.source_point, or — when \a click_elem is not -1 — by the
 * base position of that element, which is then also recorded into \a params.source_point. A face
 * set under the source point is resolved the same way when \a params.limit asks for one and
 * \a params.face_set_id does not hold it yet.
 *
 * Builds the session state the vertex domain pairs on. Returns false when nothing can be mapped
 * (the domain does not match the layer, no sources on the side, no targets within tolerance), with
 * \a r_pairs cleared.
 */
bool mirror_mapping_build(Object &object,
                          const SculptLayer &layer,
                          MirrorParams &params,
                          int64_t click_elem,
                          Vector<MirrorPair> &r_pairs);

/**
 * Write the cached mapping into \a data and return how many targets it reached.
 *
 * The mask is applied here rather than baked into the pairs, and the face-set limit filters the
 * pairs rather than rebuilding them, so a key press re-writes the same mapping instead of
 * re-searching the mesh. A target with no source at all keeps what it had: the copy is of one side,
 * not a clear of the other.
 */
int mirror_mapping_apply(const MirrorParams &params,
                         Span<MirrorPair> pairs,
                         MutableSpan<float3> data);

}  // namespace blender::ed::sculpt_paint::layers::mirror_copy
