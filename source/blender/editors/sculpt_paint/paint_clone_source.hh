/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 * \brief Clone Stamp source points: session storage, picking and operators.
 */

#pragma once

#include <optional>

#include "paint_clone.hh"

/* NOTE: all Blender DNA/data types live in namespace blender. */
namespace blender {
struct ARegion;
struct bContext;
struct ImagePaintSettings;
struct Mesh;
struct Object;
struct ViewContext;
struct wmEvent;
struct wmOperator;
struct wmOperatorType;
}  // namespace blender

namespace blender::ed::sculpt_paint::clone {

struct CloneSourcePoint {
  /** The picked hit, in the object space the evaluated mesh and the stroke cache both work in. */
  float3 co_object = float3(0.0f);
  float2 uv = float2(0.0f);
  bool is_valid = false;

  /**
   * The surface frame at the moment the source was picked, FROZEN.
   *
   * Both ends of the stamp built from the current view would make the mapping between them
   * view-independent: rotating the viewport rotates both frames by the same angle, which cancels
   * out, and the stamp never turns. Freezing this end is what makes the destination's live frame
   * carry the rotation -- the same relation a 2D clone tool has between the moment the source was
   * set and the moment the stroke is drawn.
   *
   * It also fixes what the source cursor draws: the disc marks the texels actually read, in the
   * orientation they are read in, so it sits on the surface instead of facing the camera.
   */
  CloneSurfaceFrame frame;

  /**
   * Relative mode: where the brush was when the offset was fixed.
   *
   * Absolute keeps reading the same spot forever; Relative reads at a constant offset from the
   * brush, so the source travels with the stroke. That offset cannot be captured when the source
   * is set -- the pointer is on the source at that moment, and the offset would be zero -- so it
   * is taken from the first dab painted afterwards, the same handshake an aligned clone stamp
   * uses. Kept on the source point rather than on the stroke, so the offset survives releasing
   * the mouse and the next stroke continues from where this one left off; setting or resetting
   * the source clears it.
   */
  float2 anchor_uv = float2(0.0f);
  float3 anchor_co_object = float3(0.0f);
  bool anchor_valid = false;

  /**
   * #ID.session_uid of the object the point was picked on.
   *
   * Not the pointer: a freed object's address can be handed back out by the allocator, and a
   * pointer comparison would then accept a stale record as belonging to a live object. Session
   * uids are unique for the session and never reused.
   */
  uint32_t object_session_uid = 0;
  const Mesh *mesh_data_ptr = nullptr;
  std::string uv_layer_name;
  int material_slot = 0;
};

bool clone_source_point_is_valid(const CloneSourcePoint &source, const Object &ob);

/* -------------------------------------------------------------------- */
/** \name Source points (runtime-only, not DNA).
 *
 * The source is gesture state -- a surface frame, a UV map name, evaluated-data pointers -- so it
 * lives for the session rather than in the file. Records are keyed on #ID.session_uid, which is
 * unique for the session and never reused, so a freed object cannot be mistaken for a live one
 * through a recycled address; #clone_source_points_clear_all drops them all on file load.
 * \{ */

void clone_source_point_set(const Object &ob,
                            const float3 &co_object,
                            const float2 &uv,
                            const CloneSurfaceFrame &frame,
                            StringRef uv_layer_name,
                            int material_slot);
const CloneSourcePoint *clone_source_point_get(const Object *ob);
/**
 * Writable counterpart, for the dab path that has to fix #CloneSourcePoint.anchor_uv on the
 * first Relative dab. Same validity rules as the const version.
 */
CloneSourcePoint *clone_source_point_get_for_write(const Object *ob);
void clone_source_point_reset(const Object *ob);
/** Drop every source. Registered on file load; also reachable for a full reset. */
void clone_source_points_clear_all();

/**
 * Register the file-load handler that calls #clone_source_points_clear_all.
 * Idempotent; called once at startup.
 */
void clone_source_points_callbacks_register();

/**
 * The point a dab measures its footprint from, i.e. what #clone_stroke_apply_dab wants as
 * `dab_center_uv`.
 *
 * Absolute answers with \a dest_uv, which makes the source-side offset zero and pins the source.
 * Relative answers with the anchor, so the source is displaced by exactly how far the brush has
 * travelled since the offset was fixed -- and fixes that anchor here on the first dab that asks.
 */
float2 clone_dab_center_uv_get(CloneSourcePoint &source,
                               const float2 &dest_uv,
                               const float3 &dest_co_object,
                               int8_t clone_mode);

/** \} */

/**
 * Canvas UV of one triangle at \a bary_coord, through the same UV-map resolution the dab path
 * uses, so a pick and the stamp that follows it can never disagree on the map.
 */
float2 clone_pick_uv(const Mesh *mesh_eval,
                     ePaintCanvasSource canvas_mode,
                     Object *ob_for_material,
                     int tri_index,
                     const float3 &bary_coord);

/**
 * Ray-cast pick of the mesh under the cursor, for the source operator and the projection dab.
 * False when nothing is hit, so the caller can refuse the pick or skip the dab.
 */
bool clone_pick_face(ViewContext *vc,
                     const int mval[2],
                     int *r_tri_index,
                     int *r_face_index,
                     float3 *r_bary_coord,
                     float r_co[3],
                     float r_no[3],
                     float3 *r_co_object,
                     const Mesh &mesh);

/** Whether dyntopo is active on \a ob, which the Clone Stamp does not support. */
bool clone_dyntopo_active(const Object *ob);
/** Whether the 3D Clone Stamp source operator can run here at all. */
bool clone_poll_basic(bContext *C);

/* -------------------------------------------------------------------- */
/** \name Operators.
 * \{ */

void PAINT_OT_clone_source_set(wmOperatorType *ot);
void PAINT_OT_clone_source_reset(wmOperatorType *ot);

/** \} */

/* -------------------------------------------------------------------- */
/** \name 2D source point (read out of #ImagePaintSettings).
 * \{ */

/**
 * The 2D Clone Stamp source, read out of #ImagePaintSettings.
 *
 * A view over DNA, not storage: the point lives in the scene so it survives a file save, takes
 * part in undo, and is per-scene rather than per-process.
 */
struct Clone2DSource {
  float2 uv = float2(0.0f);
  float2 anchor_uv = float2(0.0f);
  bool anchor_valid = false;
};

/** The source, or nothing when none has been picked. */
std::optional<Clone2DSource> clone_2d_source_get(const ImagePaintSettings &settings);
void clone_2d_source_set(ImagePaintSettings &settings, const float2 &uv);
/**
 * Fix the Relative anchor if it is not fixed yet, and return the dab center the source-side
 * offset is measured from.
 */
float2 clone_2d_dab_center_uv_get(ImagePaintSettings &settings,
                                  const float2 &dest_uv,
                                  int8_t clone_mode);
void clone_2d_source_reset(ImagePaintSettings &settings);

/**
 * Image Editor source pick: convert region mouse coordinates to canvas UV through the region's
 * view2d (display frame, so canvas rotation is undone) and store them as the source.
 *
 * A pick outside the canvas UV bounds is refused with an info report through \a op -- the core
 * sampler wraps out-of-bounds UVs, so a source there would silently read the wrong side of the
 * image.
 */
bool clone_2d_source_set_from_region(ImagePaintSettings &settings,
                                     ARegion &region,
                                     const int mval[2],
                                     wmOperator *op);

/** \} */

}  // namespace blender::ed::sculpt_paint::clone
