/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * 3D viewport placement of a layer row's Mirror fold center: a modal session whose temporary
 * gizmo follows the mesh surface under the cursor and writes the row's center live, so the fold's
 * symmetry shows itself in the viewport while the point is still moving. The center is a canvas UV
 * point, so the hover position is converted through the active UV map the same way the sculpt 3D
 * cursor bridge is (#image_paint_symmetry_uv_at_position); the click lands the point, Esc or the
 * right button restores the one the session started from.
 */

#include <cfloat>
#include <cmath>

#include "MEM_guardedalloc.h"

#include "BLI_index_range.hh"
#include "BLI_kdopbvh.hh"
#include "BLI_math_constants.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"
#include "DNA_scene_types.h"
#include "DNA_view3d_types.h"

#include "BKE_context.hh"
#include "BKE_mesh.hh"
#include "BKE_object.hh"
#include "BKE_report.hh"
#include "BKE_screen.hh"

#include "BLT_translation.hh"

#include "ED_screen.hh"
#include "ED_space_api.hh"
#include "ED_sculpt.hh"
#include "ED_undo.hh"
#include "ED_view3d.hh"

#include "GPU_immediate.hh"
#include "GPU_matrix.hh"
#include "GPU_state.hh"
#include "GPU_vertex_format.hh"

#include "UI_resources.hh"

#include "WM_api.hh"
#include "WM_types.hh"
#include "wm_cursors.hh"

#include "paint_layer_mapping_ops.hh"
#include "../mesh/paint_image_uv_symmetry.hh"
#include "uv_mapping_target.hh"
#include "BKE_bvhutils.hh"

namespace blender::ed::sculpt_paint::uv_mapping {

/** Segments of the hover circle; a plain polygon reads fine at this size. */
constexpr int PICK_CIRCLE_SEGMENTS = 32;
/** The circle's and the cross's half-size, in screen pixels at the hover depth. */
constexpr float PICK_RADIUS_PX = 26.0f;
constexpr float PICK_CROSS_PX = 14.0f;

namespace {

/** One Mirror center placement session in the 3D viewport. */
struct MirrorPick3DData {
  PaintLayerRowMappingTarget target;
  bool has_hover = false;
  float3 hover_world = float3(0.0f);
  float3 hover_normal_world = float3(0.0f, 0.0f, 1.0f);
  /** The hover's UV, the value a click would land. */
  float2 hover_uv = float2(0.5f, 0.5f);
  /** World-space size of the gizmo, refreshed from the view distance every hover. */
  float radius_world = 0.1f;
  /** The center the session started from, restored on cancel. */
  float2 start_center = float2(0.5f);
  bool changed = false;
  /** The region draw callback of the temporary gizmo, removed when the session ends. */
  void *draw_handle = nullptr;
};

/* The temporary gizmo: a circle on the surface plus a cross through it, along the surface's own
 * axes. World-space drawing, sized by the view distance so the gizmo stays a constant screen
 * size, the way the sculpt cursor gizmo does. */
void mirror_pick_3d_draw(const bContext * /*C*/, ARegion * /*region*/, void *arg)
{
  auto *data = static_cast<MirrorPick3DData *>(arg);
  if (data == nullptr || !data->has_hover) {
    return;
  }
  const float3 center = data->hover_world;
  float3 normal = data->hover_normal_world;
  const float normal_len = math::length(normal);
  if (normal_len < 1e-8f) {
    return;
  }
  normal /= normal_len;
  const float3 up = (std::fabs(normal.z) < 0.99f) ? float3(0.0f, 0.0f, 1.0f) :
                                                    float3(1.0f, 0.0f, 0.0f);
  float3 tangent = math::cross(up, normal);
  tangent = math::normalize(tangent);
  const float3 bitangent = math::cross(normal, tangent);

  const float radius = data->radius_world;
  const float cross = radius * (PICK_CROSS_PX / PICK_RADIUS_PX);

  const uint pos = GPU_vertformat_attr_add(
      immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
  GPU_line_width(1.5f);
  /* The canvas symmetry line's display color, for the two to read as one family of gizmo. */
  immUniformColor4f(0.2f, 0.7f, 1.0f, 1.0f);
  GPU_blend(GPU_BLEND_ALPHA);

  immBegin(GPU_PRIM_LINE_LOOP, PICK_CIRCLE_SEGMENTS);
  for (const int i : IndexRange(PICK_CIRCLE_SEGMENTS)) {
    const float angle = (float(i) / float(PICK_CIRCLE_SEGMENTS)) * 2.0f * float(M_PI);
    const float3 point = center +
                         (tangent * std::cos(angle) + bitangent * std::sin(angle)) * radius;
    immVertex3fv(pos, point);
  }
  immEnd();

  immBegin(GPU_PRIM_LINES, 8);
  immVertex3fv(pos, center - tangent * cross);
  immVertex3fv(pos, center + tangent * cross);
  immVertex3fv(pos, center - bitangent * cross);
  immVertex3fv(pos, center + bitangent * cross);
  immVertex3fv(pos, center - normal * cross);
  immVertex3fv(pos, center + normal * cross);
  /* A short tail along the normal marks the surface side the fold faces. */
  immVertex3fv(pos, center);
  immVertex3fv(pos, center + normal * radius);
  immEnd();

  GPU_blend(GPU_BLEND_NONE);
  immUnbindProgram();
}

/** Find the mesh surface under \a event's cursor and update \a data's hover from it. */
bool mirror_pick_3d_hover(bContext *C, const wmEvent &event, MirrorPick3DData &data)
{
  const Scene *scene = CTX_data_scene(C);
  const ARegion *region = CTX_wm_region(C);
  const View3D *v3d = CTX_wm_view3d(C);
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  Object *ob = CTX_data_active_object(C);
  RegionView3D *rv3d = (region != nullptr) ? static_cast<RegionView3D *>(region->regiondata) :
                                             nullptr;
  if (scene == nullptr || region == nullptr || v3d == nullptr || depsgraph == nullptr ||
      ob == nullptr || rv3d == nullptr || ob->type != OB_MESH)
  {
    return false;
  }

  const float2 mval_fl(event.mval);
  float ray_start[3];
  float ray_normal[3];
  if (!ED_view3d_win_to_ray_clipped(
          depsgraph, region, v3d, mval_fl, ray_start, ray_normal, true))
  {
    return false;
  }

  /* The UV helper reads the original mesh, so the ray casts against it too: hit and UV then come
   * from the same surface, whatever the modifiers do. */
  const Mesh *mesh = id_cast<const Mesh *>(ob->data);
  if (mesh == nullptr || mesh->faces_num == 0) {
    return false;
  }
  bke::BVHTreeFromMesh tree = mesh->bvh_corner_tris_no_hidden();
  if (tree.tree == nullptr) {
    return false;
  }

  const float4x4 world_to_object = ob->world_to_object();
  const float4x4 object_to_world = ob->object_to_world();
  const float3 start_local = math::transform_point(world_to_object, float3(ray_start));
  float3 direction_local = math::transform_direction(world_to_object, float3(ray_normal));
  if (math::length(direction_local) < 1e-12f) {
    return false;
  }
  direction_local = math::normalize(direction_local);

  BVHTreeRayHit hit{};
  hit.index = -1;
  hit.dist = FLT_MAX;
  BLI_bvhtree_ray_cast(
      tree.tree, start_local, direction_local, 0.0f, &hit, tree.raycast_callback, &tree);
  if (hit.index < 0) {
    return false;
  }

  float uv[2];
  /* The ray already found the triangle, so the UV is read from that one: the hover needs no second
   * search and cannot disagree with the surface under the cursor. */
  if (!image_paint_symmetry_uv_at_position(ob, float3(hit.co), uv, hit.index)) {
    return false;
  }

  data.hover_world = math::transform_point(object_to_world, float3(hit.co));
  const float3 normal_local(hit.no);
  data.hover_normal_world = math::normalize(
      math::transform_direction(object_to_world, normal_local));
  data.radius_world = ED_view3d_pixel_size(rv3d, data.hover_world) * PICK_RADIUS_PX;
  data.hover_uv = float2(uv[0], uv[1]);
  data.has_hover = true;
  return true;
}

/** Close the session: the gizmo goes, the center is restored or the undo step lands. */
void mirror_pick_3d_finish(bContext *C, wmOperator *op, const bool commit)
{
  auto *data = static_cast<MirrorPick3DData *>(op->customdata);
  if (data == nullptr) {
    return;
  }
  ARegion *region = CTX_wm_region(C);
  if (data->draw_handle != nullptr && region != nullptr && region->runtime != nullptr) {
    ED_region_draw_cb_exit(region->runtime->type, data->draw_handle);
    data->draw_handle = nullptr;
  }
  if (wmWindow *window = CTX_wm_window(C)) {
    WM_cursor_modal_restore(window);
  }
  if (ScrArea *area = CTX_wm_area(C)) {
    ED_area_status_text(area, nullptr);
  }

  Material *material = data->target.material();
  if (!commit || !data->changed) {
    /* Restore the center the session started from; a session that never moved it is a no-op. */
    data->target.write_mirror_center(data->start_center);
  }
  else {
    ED_undo_push(C, data->target.mirror_undo_name());
  }
  if (material != nullptr) {
    WM_event_add_notifier(C, NC_MATERIAL | ND_SHADING, &material->id);
  }
  if (region != nullptr) {
    ED_region_tag_redraw(region);
  }
  MEM_delete(data);
  op->customdata = nullptr;
}

void mirror_pick_3d_status(bContext *C, const MirrorPick3DData &data)
{
  if (ScrArea *area = CTX_wm_area(C)) {
    char text[256];
    if (data.has_hover) {
      BLI_snprintf(text,
                   sizeof(text),
                   IFACE_("Click to place the mirror point    UV %.4f  %.4f    Esc restores"),
                   data.hover_uv.x,
                   data.hover_uv.y);
    }
    else {
      /* SNPRINTF needs at least one variadic argument, and this one has none. */
      STRNCPY(text, IFACE_("Hover the object's surface to place the mirror point"));
    }
    ED_area_status_text(area, text);
  }
}

}  // namespace

static bool layer_mapping_mirror_pick_3d_poll(bContext *C)
{
  /* A hit is read in the viewport, so an area without one cannot run this at all. */
  if (CTX_wm_view3d(C) == nullptr) {
    return false;
  }
  return layer_mapping_mirror_target_poll(C, false);
}

static wmOperatorStatus layer_mapping_mirror_pick_3d_invoke(bContext *C,
                                                            wmOperator *op,
                                                            const wmEvent * /*event*/)
{
  PaintLayerRowMappingTarget target;
  const bUUID marker = layer_mapping_mirror_marker_read(op);
  if (!target.resolve_row(*C, marker)) {
    BKE_report(op->reports, RPT_WARNING, "Select a mapped layer row first");
    return OPERATOR_CANCELLED;
  }
  if (CTX_wm_view3d(C) == nullptr) {
    BKE_report(op->reports,
               RPT_WARNING,
               "Run this from the 3D Viewport, where the click has a surface to land on");
    return OPERATOR_CANCELLED;
  }
  float center[2];
  if (!target.read_mirror_center(center)) {
    return OPERATOR_CANCELLED;
  }
  ARegion *region = CTX_wm_region(C);
  if (region == nullptr || region->runtime == nullptr) {
    return OPERATOR_CANCELLED;
  }

  auto *data = MEM_new<MirrorPick3DData>(__func__);
  data->target = target;
  data->start_center = float2(center[0], center[1]);
  data->draw_handle = ED_region_draw_cb_activate(
      region->runtime->type, mirror_pick_3d_draw, data, REGION_DRAW_POST_VIEW);
  op->customdata = data;

  WM_cursor_modal_set(CTX_wm_window(C), WM_CURSOR_CROSS);
  mirror_pick_3d_status(C, *data);
  WM_event_add_modal_handler(C, op);
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus layer_mapping_mirror_pick_3d_modal(bContext *C,
                                                           wmOperator *op,
                                                           const wmEvent *event)
{
  auto *data = static_cast<MirrorPick3DData *>(op->customdata);
  ARegion *region = CTX_wm_region(C);
  if (data == nullptr || region == nullptr || event == nullptr) {
    mirror_pick_3d_finish(C, op, false);
    return OPERATOR_CANCELLED;
  }
  if (ELEM(event->type, EVT_ESCKEY, RIGHTMOUSE) && event->val == KM_PRESS) {
    mirror_pick_3d_finish(C, op, false);
    return OPERATOR_CANCELLED;
  }
  if (event->type == LEFTMOUSE && event->val == KM_PRESS) {
    if (!data->has_hover) {
      mirror_pick_3d_finish(C, op, false);
      return OPERATOR_CANCELLED;
    }
    mirror_pick_3d_finish(C, op, true);
    return OPERATOR_FINISHED;
  }
  if (event->type != MOUSEMOVE && event->type != INBETWEEN_MOUSEMOVE) {
    return OPERATOR_RUNNING_MODAL;
  }

  if (!mirror_pick_3d_hover(C, *event, *data)) {
    /* Off the surface: keep the gizmo hidden, the last hover stays as it was only for drawing. */
    data->has_hover = false;
    ED_region_tag_redraw(region);
    mirror_pick_3d_status(C, *data);
    return OPERATOR_RUNNING_MODAL;
  }

  /* The fold follows the hover live: the symmetry shows itself while the point is still moving. */
  if (!data->target.write_mirror_center(data->hover_uv)) {
    mirror_pick_3d_finish(C, op, false);
    return OPERATOR_CANCELLED;
  }
  data->changed = true;
  if (Material *material = data->target.material()) {
    WM_event_add_notifier(C, NC_MATERIAL | ND_SHADING, &material->id);
  }
  ED_region_tag_redraw(region);
  mirror_pick_3d_status(C, *data);
  return OPERATOR_RUNNING_MODAL;
}

/** The window manager can abort the gesture outside the modal handler: close the session then. */
static void layer_mapping_mirror_pick_3d_cancel(bContext *C, wmOperator *op)
{
  mirror_pick_3d_finish(C, op, false);
}

void PAINT_OT_layer_mapping_mirror_pick_3d(wmOperatorType *ot)
{
  ot->name = "Set Mirror Point on Surface";
  ot->idname = "PAINT_OT_layer_mapping_mirror_pick_3d";
  ot->description =
      "Place the shown layer row's Mirror fold center on the mesh surface: the temporary gizmo "
      "follows the cursor, the symmetry shows live, click to land the point";

  ot->invoke = layer_mapping_mirror_pick_3d_invoke;
  ot->modal = layer_mapping_mirror_pick_3d_modal;
  ot->cancel = layer_mapping_mirror_pick_3d_cancel;
  ot->poll = layer_mapping_mirror_pick_3d_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING;

  layer_mapping_mirror_marker_define(ot);
}

}  // namespace blender::ed::sculpt_paint::uv_mapping
