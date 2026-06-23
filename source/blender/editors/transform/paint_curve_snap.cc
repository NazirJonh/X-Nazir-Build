/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edtransform
 *
 * View3D geometry snapping for paint-curve editing (vertex / edge / face targets).
 * Implemented here because #bf_editor_transform depends on #bf_editor_sculpt_paint,
 * not the other way around.
 */

#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_view3d_types.h"

#include "BLI_math_matrix.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector_types.hh"
#include "BLI_offset_indices.hh"

#include "BKE_context.hh"
#include "BKE_global.hh"
#include "BKE_mesh.hh"
#include "BKE_object.hh"
#include "BKE_screen.hh"

#include "DEG_depsgraph_query.hh"

#include "ED_paint.hh"
#include "ED_transform_snap_object_context.hh"

#include "MEM_guardedalloc.h"

#include "transform_snap.hh"

namespace blender {

struct PaintCurveSnapContext {
  ed::transform::SnapObjectContext *object_context = nullptr;
};

/**
 * Resolve the effective snap-element mask from #ToolSettings.
 *
 * Paint-curve header snap (#draw_paint_curve_snap_template) drives #ToolSettings.snap_mode
 * via `snap_elements`, not #ToolSettings.snap_mode_tools (that belongs to tool-placement).
 * When the header has no geometry targets, fall back to #snap_mode_tools.
 */
eSnapMode ED_paintcurve_snap_mode_sanitize(const eSnapMode snap_mode)
{
  if ((snap_mode & SCE_SNAP_TO_GEOM) != SCE_SNAP_TO_NONE) {
    return eSnapMode(snap_mode & ~(SCE_SNAP_TO_INCREMENT | SCE_SNAP_TO_GRID));
  }
  return snap_mode;
}

eSnapMode ED_paintcurve_snap_elements(const ToolSettings *ts)
{
  eSnapMode snap_mode = eSnapMode(ts->snap_mode);
  if ((snap_mode & SCE_SNAP_TO_INCREMENT) && (ts->snap_flag & SCE_SNAP_ABS_GRID)) {
    snap_mode |= SCE_SNAP_TO_GRID;
  }
  if ((snap_mode & SCE_SNAP_TO_GEOM) == SCE_SNAP_TO_NONE &&
      ts->snap_mode_tools != SCE_SNAP_TO_NONE)
  {
    snap_mode = eSnapMode(ts->snap_mode_tools);
  }
  return ED_paintcurve_snap_mode_sanitize(snap_mode);
}

PaintCurveSnapContext *ED_paintcurve_snap_context_create()
{
  PaintCurveSnapContext *ctx = MEM_new<PaintCurveSnapContext>(__func__);
  ctx->object_context = ed::transform::snap_object_context_create();
  return ctx;
}

void ED_paintcurve_snap_context_destroy(PaintCurveSnapContext *snap_ctx)
{
  if (snap_ctx == nullptr) {
    return;
  }
  ed::transform::snap_object_context_destroy(snap_ctx->object_context);
  MEM_delete(snap_ctx);
}

/**
 * Snap to the geometric center of the face under \a mval (world space).
 *
 * The standard #SCE_SNAP_TO_FACE_MIDPOINT path is unreliable for paint-curve placement: the
 * raycast-polygon refinement (#snap_polygon_mesh) only produces edges/vertices, never a face
 * midpoint, so face-center snapping falls back to a projected-nearest search that requires the
 * cursor to be within a few pixels of the (possibly distant) center and is additionally clipped by
 * the occlusion plane on the coplanar center. Raycasting the face and computing its center is
 * robust and matches the user intent of "snap to the face I am pointing at".
 */
static bool paintcurve_snap_face_center(ed::transform::SnapObjectContext *sctx,
                                        Depsgraph *depsgraph,
                                        const View3D *v3d,
                                        const ARegion *region,
                                        const ed::transform::SnapObjectParams *params,
                                        const float mval[2],
                                        float r_world[3])
{
  float loc[3];
  float no[3];
  float obmat[4][4];
  int index = -1;
  const Object *ob = nullptr;
  float dist_px = float(SNAP_MIN_DISTANCE) * U.pixelsize;

  const eSnapMode hit = ed::transform::snap_object_project_view3d_ex(sctx,
                                                                     depsgraph,
                                                                     region,
                                                                     v3d,
                                                                     SCE_SNAP_TO_FACE,
                                                                     params,
                                                                     nullptr,
                                                                     mval,
                                                                     nullptr,
                                                                     &dist_px,
                                                                     loc,
                                                                     no,
                                                                     &index,
                                                                     &ob,
                                                                     obmat,
                                                                     nullptr);
  if (hit != SCE_SNAP_TO_FACE || ob == nullptr || index < 0) {
    return false;
  }

  const Mesh *mesh = BKE_object_get_evaluated_mesh(ob);
  if (mesh == nullptr || index >= mesh->faces_num) {
    return false;
  }

  const OffsetIndices<int> faces = mesh->faces();
  const Span<int> corner_verts = mesh->corner_verts();
  const Span<float3> positions = mesh->vert_positions();
  const float3 center_local = bke::mesh::face_center_calc(positions,
                                                          corner_verts.slice(faces[index]));

  mul_v3_m4v3(r_world, obmat, center_local);
  return true;
}

bool ED_paintcurve_snap_point(bContext *C,
                              PaintCurveSnapContext *snap_ctx,
                              Depsgraph *depsgraph,
                              const View3D *v3d,
                              ARegion *region,
                              Object *obact,
                              const float mval[2],
                              const float prev_co_world[3],
                              float r_hit_ob[3])
{
  if (snap_ctx == nullptr || snap_ctx->object_context == nullptr || depsgraph == nullptr ||
      v3d == nullptr || region == nullptr || obact == nullptr)
  {
    return false;
  }

  ToolSettings *ts = CTX_data_tool_settings(C);
  if (ts == nullptr) {
    return false;
  }

  const eSnapMode snap_to = eSnapMode(ED_paintcurve_snap_elements(ts) & SCE_SNAP_TO_GEOM);
  if (snap_to == SCE_SNAP_TO_NONE) {
    return false;
  }

  ed::transform::SnapObjectParams params{};
  /* Match every other caller of #snap_object_project_view3d (cursor, gizmo,
   * ruler, edit-curve ...): paint-curve editing snaps to *all* visible scene
   * geometry, including the active sculpt object. Starting from
   * #SCE_SNAP_TARGET_NOT_SELECTED filters out the selected active object in
   * Object Mode, which made Vertex/Edge/Face snapping silently no-op. */
  params.snap_target_select = SCE_SNAP_TARGET_ALL;
  params.edit_mode_type = ed::transform::SNAP_GEOM_FINAL;
  params.occlusion_test = ed::transform::SNAP_OCCLUSION_AS_SEEM;
  params.use_backface_culling = (ts->snap_flag & SCE_SNAP_BACKFACE_CULLING) != 0;

  float dist_px = float(SNAP_MIN_DISTANCE) * U.pixelsize;
  float hit_world[3];

  const eSnapMode hit = ed::transform::snap_object_project_view3d(snap_ctx->object_context,
                                                                  depsgraph,
                                                                  region,
                                                                  v3d,
                                                                  snap_to,
                                                                  &params,
                                                                  nullptr,
                                                                  mval,
                                                                  prev_co_world,
                                                                  &dist_px,
                                                                  hit_world,
                                                                  nullptr);

  if (hit == SCE_SNAP_TO_NONE) {
    /* When "Face Center" is requested, the projected-nearest midpoint search above is unreliable
     * (see #paintcurve_snap_face_center). Raycast the face under the cursor and snap to its center
     * instead. */
    if (!(snap_to & SCE_SNAP_TO_FACE_MIDPOINT) ||
        !paintcurve_snap_face_center(
            snap_ctx->object_context, depsgraph, v3d, region, &params, mval, hit_world))
    {
      return false;
    }
  }

  Object *ob_eval = DEG_get_evaluated(depsgraph, obact);
  if (ob_eval == nullptr) {
    ob_eval = obact;
  }
  const float (*world_to_ob)[4] = ob_eval->world_to_object().ptr();
  mul_v3_m4v3(r_hit_ob, world_to_ob, hit_world);
  return true;
}

}  // namespace blender
