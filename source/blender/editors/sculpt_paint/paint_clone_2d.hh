/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 * \brief Clone Stamp, 2D Image Editor path: stroke gate and dab.
 *
 * The 3D core (targets, sampling, blending, undo tiles) lives in #paint_clone.cc and works in UV
 * space already; the source lives in #paint_clone_source.hh. This module only supplies what the
 * Image Editor path lacks: a per-stroke gate that splits the Clone Stamp tool from the legacy
 * image clone, and the dab entry the 2D stroke calls.
 *
 * Scope: identity dab transform for the main pass (an Image Editor view has no rotation), no
 * UDIM tiles. Mirror symmetry composes the pass's UV Jacobian into the transform
 * (#symmetry_uv_jacobian), so a mirrored stamp is the mirror image of the main pass's;
 * everything else keeps the UV-locked stamp.
 */

#pragma once

#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector_types.hh"

#include "DNA_image_types.h"
#include "DNA_scene_types.h"

#include "paint_clone_stroke.hh"

/* NOTE: all Blender DNA/data types live in namespace blender. */
namespace blender {
struct Brush;
struct bContext;
struct Image;
struct ImagePaintSettings;
struct Main;
struct Object;
struct Paint;
}  // namespace blender

namespace blender::ed::sculpt_paint::clone {

/* -------------------------------------------------------------------- */
/** \name Tool / stroke gating.
 * \{ */

/**
 * What the dab path needs to build targets, resolved once per stroke by #clone_2d_stroke_gate.
 *
 * The stroke system hands the dab path no #bContext, so the gate parks these here rather than in
 * module state: they are pointers into the file, and a stroke's copy must not outlive the stroke
 * that resolved them.
 *
 * Plain-old-data on purpose: #ImagePaintState embeds this by value and is created with
 * #MEM_new_zeroed, which only accepts trivially constructible types -- so no default member
 * initializers here, and no by-value DNA structs either (#ImageUser carries its own
 * initializers, which already make it non-trivial). Zero comes from #MEM_new_zeroed for stroke
 * states and from the `r_context = Clone2DStrokeContext()` reset at the top of
 * #clone_2d_stroke_gate; every field below is assigned there before #active is set.
 */
struct Clone2DStrokeContext {
  Main *bmain;
  Object *object;
  /** The scene's image-paint settings, so the dab can fix the Relative anchor on the source. */
  ImagePaintSettings *image_paint;
  ePaintCanvasSource canvas_source;
  Image *canvas_image;
  /**
   * Borrowed tile/frame selector for the single-image target build, normally the space's own.
   * A pointer like every other field here: the space outlives the stroke (the stroke state keeps
   * the same #SpaceImage pointer), and a value copy is impossible -- #ImageUser is not trivially
   * constructible, so it cannot sit in this struct by value. Null when picked without a space;
   * the builder then falls back to a default user.
   */
  const ImageUser *canvas_iuser;
  /** The gate said this stroke belongs to the Clone Stamp. Nothing below is read when false. */
  bool active;
};

/** Whether the active tool is the Clone Stamp (`builtin_brush.texture_clone`). */
bool clone_2d_tool_active(const bContext *C);

/**
 * Decide once per stroke, from #paint_2d_new_stroke, whether the whole 2D stroke belongs to the
 * Clone Stamp: the Clone Stamp tool is active and the brush is CLONE. Warns through \a op when
 * the stroke will be a no-op (source not set, or a tiled canvas image) so the reason surfaces
 * once per stroke instead of once per dab.
 *
 * Fills \a r_context on success; leaves it inert (#Clone2DStrokeContext.active false) otherwise.
 */
bool clone_2d_stroke_gate(bContext *C, wmOperator *op, Clone2DStrokeContext &r_context);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Dab path.
 * \{ */

/**
 * Apply one Clone Stamp dab at canvas UV \a dest_uv to every stroke target: all visible material
 * channels on a Material canvas, the single canvas image on an Image canvas. No-op without a
 * valid source (the gate already warned) or writable targets. Undo stays owned by the outer
 * image-paint stroke; the dab pushes its tiles through #clone_capture_undo_tile and publishes
 * dirty regions through #ED_imapaint_dirty_region.
 *
 * \param strength: brush alpha with pressure already folded in by the caller.
 * \param is_main_pass: the un-mirrored pass of this stroke event. Only it records the dab center
 *                      the symmetry duplicate test compares against, and only it may fix the
 *                      Relative anchor -- a mirrored location must not define the offset, the
 *                      same rule the sculpt clone follows.
 * \param symmetry_jacobian: UV Jacobian of this mirror pass (#SymmetryDab.symm_jacobian), which
 *                           turns the stamp into the mirror image of the main pass's. Null for
 *                           the main pass and when the pass has no usable Jacobian -- both stamp
 *                           the un-mirrored, UV-locked way.
 */
void clone_2d_stroke_dab(CloneStrokeRuntime *&owner,
                         const Clone2DStrokeContext &context,
                         const Paint &paint,
                         const Brush &brush,
                         const float2 &dest_uv,
                         float strength,
                         bool is_main_pass,
                         const float2x2 *symmetry_jacobian = nullptr);

/** Whether \a runtime's clone targets write to \a image; used to flag tile redraws. */
bool clone_2d_targets_contain_image(const CloneStrokeRuntime *runtime, const Image *image);

/** \} */

}  // namespace blender::ed::sculpt_paint::clone
