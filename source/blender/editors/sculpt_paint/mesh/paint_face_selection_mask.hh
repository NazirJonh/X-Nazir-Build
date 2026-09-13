/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Face selection masking for painting tools: #Mesh.editflag & #ME_EDIT_PAINT_FACE_SEL with the
 * `.select_poly` face attribute restrict painting (color attributes, image canvases, curve
 * patches) to the selected faces. The per-stroke state and the helpers live here.
 */

#pragma once

#include <cstdint>
#include <optional>

#include "BLI_array.hh"
#include "BLI_span.hh"
#include "BLI_virtual_array.hh"

namespace blender {

struct Mesh;
struct Object;

namespace ed::sculpt_paint {

/**
 * The mesh-level face selection masking state, derived from #Mesh.editflag and `.select_poly`:
 * - #Disabled: `ME_EDIT_PAINT_FACE_SEL` is clear.
 * - #Empty: the flag is set but no face is selected (or the attribute is missing).
 * - #Active: the flag is set with a non-empty selection.
 *
 * Only #Active restricts painting; #Empty is a no-op restriction. The distinction is needed by
 * operators that must know whether the masking flag is on, such as Paint Mask Island.
 */
enum class FaceSelectionState : uint8_t {
  Disabled,
  Empty,
  Active,
};

/** \return The face selection masking state of \a mesh. */
FaceSelectionState face_selection_state(const Mesh &mesh);

/**
 * Face selection masking state for painting tools, built once per stroke (or filter cache
 * lifetime) instead of per dab -- the selection cannot change while a stroke runs. See
 * #face_selection_mask_ensure and #face_selection_mask_build.
 */
struct FaceSelectionMask {
  /** No value until the mask is built. */
  std::optional<FaceSelectionState> state;
  /** Face selection for corner-domain filtering; empty in every state but #Active. The span owns
   * the underlying #VArray, so no separate handle is needed to keep it alive. */
  VArraySpan<bool> select_poly;
  /** Per-vertex "at least one owning face is selected", indexed like the mesh verts; only filled
   * in #Active. Point-domain values cannot be masked more strictly than this. */
  Array<bool> vert_paintable;
};

/**
 * Disable brush influence on vertices that are not part of the face selection mask: a vertex
 * keeps its factor only when at least one of its owning faces is selected. Point-domain color
 * attributes cannot be masked more strictly (a shared vertex has a single value); corner-domain
 * attributes are additionally filtered per face in #color::color_vert_get / #color::color_vert_set.
 * A \a face_selection_mask not in the #FaceSelectionState::Active state leaves the factors
 * untouched. O(verts): reads the per-stroke #FaceSelectionMask::vert_paintable table.
 */
void filter_factors_with_face_selection(const FaceSelectionMask &face_selection_mask,
                                        const Span<int> verts,
                                        MutableSpan<float> r_factors);

/**
 * Build the face selection masking state for \a mesh from scratch into \a mask. For one-shot
 * operators that have no stroke or filter cache to hold the per-stroke state (see
 * #face_selection_mask_ensure).
 */
void face_selection_mask_build(const Mesh &mesh, FaceSelectionMask &mask);

/**
 * The face selection masking state for \a ob, built lazily on the first call and cached in the
 * object's stroke cache (or filter cache) for the rest of the stroke: per-dab paths must not
 * re-scan `.select_poly` or re-derive the per-vertex table on every dab, the selection cannot
 * change while a stroke runs. Asserts that a stroke or filter cache exists.
 */
const FaceSelectionMask &face_selection_mask_ensure(Object &ob);

}  // namespace ed::sculpt_paint
}  // namespace blender
