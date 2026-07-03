/* SPDX-FileCopyrightText: 2019 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edobj
 */

#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "MEM_guardedalloc.h"

#include "BLI_array.hh"
#include "BLI_bounds.hh"
#include "BLI_disjoint_set.hh"
#include "BLI_kdopbvh.hh"
#include "BLI_math_geom_c.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_matrix_c.hh"
#include "BLI_string.hh"
#include "BLI_string_utf8.hh"
#include "BLI_utildefines.hh"
#include "BLI_vector.hh"

#include "DNA_curves_types.h"
#include "DNA_object_types.h"
#include "DNA_userdef_types.h"

#include "BLT_translation.hh"

#include "BKE_attribute.hh"
#include "BKE_bvhutils.hh"
#include "BKE_context.hh"
#include "BKE_curves.hh"
#include "BKE_global.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_mirror.hh"
#include "BKE_mesh_remesh_voxel.hh"
#include "BKE_modifier.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_report.hh"
#include "BKE_screen.hh"
#include "BKE_shrinkwrap.hh"
#include "BKE_unit.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"

#include "ED_object.hh"
#include "ED_screen.hh"
#include "ED_sculpt.hh"
#include "ED_space_api.hh"
#include "ED_view3d.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "GPU_immediate.hh"
#include "GPU_matrix.hh"
#include "GPU_state.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "UI_interface.hh"

#include "BLF_api.hh"

#include "object_intern.hh" /* own include */

namespace blender::ed::object {

/* TODO(sebpa): unstable, can lead to unrecoverable errors. */
// #define USE_MESH_CURVATURE

/* -------------------------------------------------------------------- */
/** \name Voxel Remesh Operator
 * \{ */

static bool object_remesh_poll(bContext *C)
{
  Object *ob = CTX_data_active_object(C);

  if (ob == nullptr || ob->data == nullptr) {
    return false;
  }

  if (!ID_IS_EDITABLE(ob) || !ID_IS_EDITABLE(ob->data) || ID_IS_OVERRIDE_LIBRARY(ob->data)) {
    CTX_wm_operator_poll_msg_set(C, "The remesher cannot work on linked or override data");
    return false;
  }

  if (BKE_object_is_in_editmode(ob)) {
    CTX_wm_operator_poll_msg_set(C, "The remesher cannot run from edit mode");
    return false;
  }

  if (ob->mode == OB_MODE_SCULPT && ob->runtime->sculpt_session->bm) {
    CTX_wm_operator_poll_msg_set(C, "The remesher cannot run with dyntopo activated");
    return false;
  }

  if (BKE_modifiers_uses_multires(ob)) {
    CTX_wm_operator_poll_msg_set(
        C, "The remesher cannot run with a Multires modifier in the modifier stack");
    return false;
  }

  return ED_operator_object_active_editable_mesh(C);
}

static wmOperatorStatus voxel_remesh_exec(bContext *C, wmOperator *op)
{
  const Scene &scene = *CTX_data_scene(C);
  Object *ob = CTX_data_active_object(C);

  Mesh *mesh = id_cast<Mesh *>(ob->data);

  if (mesh->remesh_voxel_size <= 0.0f) {
    BKE_report(op->reports, RPT_ERROR, "Voxel remesher cannot run with a voxel size of 0.0");
    return OPERATOR_CANCELLED;
  }

  if (mesh->faces_num == 0) {
    return OPERATOR_CANCELLED;
  }

  float isovalue = 0.0f;
  if (mesh->flag & ME_REMESH_REPROJECT_VOLUME) {
    isovalue = mesh->remesh_voxel_size * 0.3f;
  }

  Mesh *new_mesh = BKE_mesh_remesh_voxel(
      mesh, mesh->remesh_voxel_size, mesh->remesh_voxel_adaptivity, isovalue, op->reports);

  if (!new_mesh) {
    BKE_report(op->reports, RPT_ERROR, "Voxel remesher failed to create mesh");
    return OPERATOR_CANCELLED;
  }

  if (ob->mode == OB_MODE_SCULPT) {
    sculpt_paint::undo::geometry_begin(scene, *ob, op);
  }

  if (mesh->flag & ME_REMESH_FIX_POLES && mesh->remesh_voxel_adaptivity <= 0.0f) {
    Mesh *mesh_fixed_poles = BKE_mesh_remesh_voxel_fix_poles(new_mesh);
    BKE_id_free(nullptr, new_mesh);
    new_mesh = mesh_fixed_poles;
  }

  if (mesh->flag & ME_REMESH_REPROJECT_VOLUME) {
    BKE_shrinkwrap_remesh_target_project(new_mesh, mesh, ob);
  }

  if (mesh->flag & ME_REMESH_REPROJECT_ATTRIBUTES) {
    bke::mesh_remesh_reproject_attributes(*mesh, *new_mesh);
  }
  else {
    const VArray<bool> sharp_face = *mesh->attributes().lookup_or_default<bool>(
        "sharp_face", bke::AttrDomain::Face, false);
    bke::mesh_smooth_set(*new_mesh, !sharp_face[0]);
  }

  BKE_mesh_nomain_to_mesh(new_mesh, mesh, ob);
  /* Spatially organize the mesh after remesh. */
  bke::mesh_apply_spatial_organization(*mesh);

  if (ob->mode == OB_MODE_SCULPT) {
    sculpt_paint::undo::geometry_end(*ob);
    BKE_sculptsession_free_pbvh(*ob);
  }
  BKE_mesh_batch_cache_dirty_tag(id_cast<Mesh *>(ob->data), BKE_MESH_BATCH_DIRTY_ALL);
  DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, ob->data);

  return OPERATOR_FINISHED;
}

void OBJECT_OT_voxel_remesh(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Voxel Remesh";
  ot->description =
      "Calculates a new manifold mesh based on the volume of the current mesh. All data layers "
      "will be lost";
  ot->idname = "OBJECT_OT_voxel_remesh";

  /* API callbacks. */
  ot->poll = object_remesh_poll;
  ot->exec = voxel_remesh_exec;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Voxel Size Operator
 * \{ */

#define VOXEL_SIZE_EDIT_MAX_GRIDS_LINES 500
#define VOXEL_SIZE_EDIT_MAX_STR_LEN 20

struct VoxelSizeEditCustomData {
  void *draw_handle;
  Object *active_object;

  float init_mval[2];
  float slow_mval[2];

  bool slow_mode;

  float init_voxel_size;
  float slow_voxel_size;
  float voxel_size;
  float voxel_size_min;
  float voxel_size_max;

  float preview_plane[4][3];

  float text_mat[4][4];
};

static void voxel_size_parallel_lines_draw(uint pos3d,
                                           const float initial_co[3],
                                           const float end_co[3],
                                           const float length_co[3],
                                           const float spacing)
{
  const float total_len = len_v3v3(initial_co, end_co);
  const int tot_lines = int(total_len / spacing);
  const int tot_lines_half = (tot_lines / 2) + 1;
  float spacing_dir[3], lines_start[3];
  float line_dir[3];
  sub_v3_v3v3(spacing_dir, end_co, initial_co);
  normalize_v3(spacing_dir);

  sub_v3_v3v3(line_dir, length_co, initial_co);

  if (tot_lines > VOXEL_SIZE_EDIT_MAX_GRIDS_LINES || tot_lines <= 1) {
    return;
  }

  mid_v3_v3v3(lines_start, initial_co, end_co);

  immBegin(GPU_PRIM_LINES, uint(tot_lines_half) * 2);
  for (int i = 0; i < tot_lines_half; i++) {
    float line_start[3];
    float line_end[3];
    madd_v3_v3v3fl(line_start, lines_start, spacing_dir, spacing * i);
    add_v3_v3v3(line_end, line_start, line_dir);
    immVertex3fv(pos3d, line_start);
    immVertex3fv(pos3d, line_end);
  }
  immEnd();

  mul_v3_fl(spacing_dir, -1.0f);

  immBegin(GPU_PRIM_LINES, uint(tot_lines_half - 1) * 2);
  for (int i = 1; i < tot_lines_half; i++) {
    float line_start[3];
    float line_end[3];
    madd_v3_v3v3fl(line_start, lines_start, spacing_dir, spacing * i);
    add_v3_v3v3(line_end, line_start, line_dir);
    immVertex3fv(pos3d, line_start);
    immVertex3fv(pos3d, line_end);
  }
  immEnd();
}

static void voxel_size_edit_draw(const bContext *C, ARegion * /*region*/, void *arg)
{
  VoxelSizeEditCustomData *cd = static_cast<VoxelSizeEditCustomData *>(arg);

  GPU_blend(GPU_BLEND_ALPHA);
  GPU_line_smooth(true);

  uint pos3d = GPU_vertformat_attr_add(
      immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
  GPU_matrix_push();
  GPU_matrix_mul(cd->active_object->object_to_world().ptr());

  /* Draw Rect */
  immUniformColor4f(0.9f, 0.9f, 0.9f, 0.8f);
  GPU_line_width(3.0f);

  immBegin(GPU_PRIM_LINES, 8);
  immVertex3fv(pos3d, cd->preview_plane[0]);
  immVertex3fv(pos3d, cd->preview_plane[1]);

  immVertex3fv(pos3d, cd->preview_plane[1]);
  immVertex3fv(pos3d, cd->preview_plane[2]);

  immVertex3fv(pos3d, cd->preview_plane[2]);
  immVertex3fv(pos3d, cd->preview_plane[3]);

  immVertex3fv(pos3d, cd->preview_plane[3]);
  immVertex3fv(pos3d, cd->preview_plane[0]);
  immEnd();

  /* Draw Grid */
  GPU_line_width(1.0f);

  const float total_len = len_v3v3(cd->preview_plane[0], cd->preview_plane[1]);
  const int tot_lines = int(total_len / cd->voxel_size);

  /* Smooth-step to reduce the alpha of the grid as the line number increases. */
  const float a = VOXEL_SIZE_EDIT_MAX_GRIDS_LINES * 0.1f;
  const float b = VOXEL_SIZE_EDIT_MAX_GRIDS_LINES;
  const float x = clamp_f((tot_lines - a) / (b - a), 0.0f, 1.0);
  const float alpha_factor = 1.0f - (x * x * (3.0f - 2.0f * x));

  immUniformColor4f(0.9f, 0.9f, 0.9f, 0.75f * alpha_factor);
  voxel_size_parallel_lines_draw(
      pos3d, cd->preview_plane[0], cd->preview_plane[1], cd->preview_plane[3], cd->voxel_size);
  voxel_size_parallel_lines_draw(
      pos3d, cd->preview_plane[1], cd->preview_plane[2], cd->preview_plane[0], cd->voxel_size);

  /* Draw text */
  const uiStyle *style = ui::style_get();
  const uiFontStyle *fstyle = &style->widget;
  const int fontid = fstyle->uifont_id;
  float strwidth, strheight;
  short fstyle_points = fstyle->points;
  char str[VOXEL_SIZE_EDIT_MAX_STR_LEN];
  short strdrawlen = 0;
  Scene *scene = CTX_data_scene(C);
  const UnitSettings &unit = scene->unit;

  BKE_unit_value_as_string_scaled(
      str, sizeof(str), cd->voxel_size, -3, B_UNIT_LENGTH, unit, true, true);
  strdrawlen = BLI_strlen_utf8(str);

  immUnbindProgram();

  GPU_matrix_push();
  GPU_matrix_mul(cd->text_mat);
  /* (Constant viewport) scale is already accounted for in 'text_mat'. */
  BLF_size(fontid, 10.0f * fstyle_points);
  BLF_color3f(fontid, 1.0f, 1.0f, 1.0f);
  BLF_width_and_height(fontid, str, strdrawlen, &strwidth, &strheight);
  BLF_position(fontid, -0.5f * strwidth, -0.5f * strheight, 0.0f);
  BLF_draw(fontid, str, strdrawlen);
  GPU_matrix_pop();

  GPU_matrix_pop();

  GPU_blend(GPU_BLEND_NONE);
  GPU_line_smooth(false);
}

static void voxel_size_edit_cancel(bContext *C, wmOperator *op)
{
  ARegion *region = CTX_wm_region(C);
  VoxelSizeEditCustomData *cd = static_cast<VoxelSizeEditCustomData *>(op->customdata);

  ED_region_draw_cb_exit(region->runtime->type, cd->draw_handle);

  MEM_delete(cd);

  ED_workspace_status_text(C, nullptr);
}

static void voxel_size_edit_update_header(wmOperator *op, bContext *C)
{
  VoxelSizeEditCustomData *cd = static_cast<VoxelSizeEditCustomData *>(op->customdata);
  WorkspaceStatus status(C);
  status.item(IFACE_("Confirm"), ICON_EVENT_RETURN, ICON_MOUSE_LMB);
  status.item(IFACE_("Cancel"), ICON_EVENT_ESC, ICON_MOUSE_RMB);
  status.item(IFACE_("Change Size"), ICON_MOUSE_MOVE);
  status.item_bool(IFACE_("Precision Mode"), cd->slow_mode, ICON_EVENT_SHIFT);
}

static wmOperatorStatus voxel_size_edit_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  ARegion *region = CTX_wm_region(C);
  VoxelSizeEditCustomData *cd = static_cast<VoxelSizeEditCustomData *>(op->customdata);
  Object *active_object = cd->active_object;
  Mesh *mesh = id_cast<Mesh *>(active_object->data);

  /* Cancel modal operator */
  if ((event->type == EVT_ESCKEY && event->val == KM_PRESS) ||
      (event->type == RIGHTMOUSE && event->val == KM_PRESS))
  {
    voxel_size_edit_cancel(C, op);
    ED_region_tag_redraw(region);
    return OPERATOR_FINISHED;
  }

  /* Finish modal operator */
  if ((event->type == LEFTMOUSE && event->val == KM_RELEASE) ||
      (event->type == EVT_RETKEY && event->val == KM_PRESS) ||
      (event->type == EVT_PADENTER && event->val == KM_PRESS))
  {
    ED_region_draw_cb_exit(region->runtime->type, cd->draw_handle);
    mesh->remesh_voxel_size = cd->voxel_size;
    MEM_delete(cd);
    ED_region_tag_redraw(region);
    ED_workspace_status_text(C, nullptr);
    WM_event_add_notifier(C, NC_GEOM | ND_DATA, nullptr);
    return OPERATOR_FINISHED;
  }

  const float mval[2] = {float(event->mval[0]), float(event->mval[1])};

  float d = cd->init_mval[0] - mval[0];

  if (cd->slow_mode) {
    d = cd->slow_mval[0] - mval[0];
  }

  d *= cd->voxel_size_min * 0.25f;

  if (cd->slow_mode) {
    cd->voxel_size = cd->slow_voxel_size + d * 0.05f;
  }
  else {
    cd->voxel_size = cd->init_voxel_size + d;
  }

  if (event->type == EVT_LEFTSHIFTKEY && event->val == KM_PRESS) {
    cd->slow_mode = true;
    copy_v2_v2(cd->slow_mval, mval);
    cd->slow_voxel_size = cd->voxel_size;
  }
  if (event->type == EVT_LEFTSHIFTKEY && event->val == KM_RELEASE) {
    cd->slow_mode = false;
    cd->slow_voxel_size = 0.0f;
  }

  cd->voxel_size = clamp_f(
      cd->voxel_size, max_ff(cd->voxel_size_min, 0.0001f), cd->voxel_size_max);

  ED_region_tag_redraw(region);

  voxel_size_edit_update_header(op, C);
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus voxel_size_edit_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  ARegion *region = CTX_wm_region(C);
  Object *active_object = CTX_data_active_object(C);
  Mesh *mesh = id_cast<Mesh *>(active_object->data);

  VoxelSizeEditCustomData *cd = MEM_new_zeroed<VoxelSizeEditCustomData>(
      "Voxel Size Edit OP Custom Data");

  /* Initial operator Custom Data setup. */
  cd->draw_handle = ED_region_draw_cb_activate(
      region->runtime->type, voxel_size_edit_draw, cd, REGION_DRAW_POST_VIEW);
  cd->active_object = active_object;
  cd->init_mval[0] = event->mval[0];
  cd->init_mval[1] = event->mval[1];
  cd->slow_mode = false;
  op->customdata = cd;

  /* Select the front facing face of the mesh bounding box. */
  const Bounds<float3> bounds = *mesh->bounds_min_max();
  const std::array<float3, 8> bounds_box = bounds::corners(bounds);

  /* Indices of the Bounding Box faces. */
  const int BB_faces[6][4] = {
      {3, 0, 4, 7},
      {1, 2, 6, 5},
      {3, 2, 1, 0},
      {4, 5, 6, 7},
      {0, 1, 5, 4},
      {2, 3, 7, 6},
  };

  copy_v3_v3(cd->preview_plane[0], bounds_box[BB_faces[0][0]]);
  copy_v3_v3(cd->preview_plane[1], bounds_box[BB_faces[0][1]]);
  copy_v3_v3(cd->preview_plane[2], bounds_box[BB_faces[0][2]]);
  copy_v3_v3(cd->preview_plane[3], bounds_box[BB_faces[0][3]]);

  RegionView3D *rv3d = CTX_wm_region_view3d(C);

  float mat[3][3];
  float current_normal[3];
  float view_normal[3] = {0.0f, 0.0f, 1.0f};

  /* Calculate the view normal. */
  invert_m4_m4(active_object->runtime->world_to_object.ptr(),
               active_object->object_to_world().ptr());
  copy_m3_m4(mat, rv3d->viewinv);
  mul_m3_v3(mat, view_normal);
  copy_m3_m4(mat, active_object->world_to_object().ptr());
  mul_m3_v3(mat, view_normal);
  normalize_v3(view_normal);

  normal_tri_v3(current_normal, cd->preview_plane[0], cd->preview_plane[1], cd->preview_plane[2]);

  float min_dot = dot_v3v3(current_normal, view_normal);
  float current_dot = 1;

  /* Check if there is a face that is more aligned towards the view. */
  for (int i = 0; i < 6; i++) {
    normal_tri_v3(current_normal,
                  bounds_box[BB_faces[i][0]],
                  bounds_box[BB_faces[i][1]],
                  bounds_box[BB_faces[i][2]]);
    current_dot = dot_v3v3(current_normal, view_normal);

    if (current_dot < min_dot) {
      min_dot = current_dot;
      copy_v3_v3(cd->preview_plane[0], bounds_box[BB_faces[i][0]]);
      copy_v3_v3(cd->preview_plane[1], bounds_box[BB_faces[i][1]]);
      copy_v3_v3(cd->preview_plane[2], bounds_box[BB_faces[i][2]]);
      copy_v3_v3(cd->preview_plane[3], bounds_box[BB_faces[i][3]]);
    }
  }

  /* Cap the max/min voxel size based on the point where we cant visually display any more info
   * with grid lines. */
  cd->voxel_size_max = max_ff(len_v3v3(cd->preview_plane[1], cd->preview_plane[0]),
                              len_v3v3(cd->preview_plane[3], cd->preview_plane[0])) *
                       0.5f;
  cd->voxel_size_min = cd->voxel_size_max / VOXEL_SIZE_EDIT_MAX_GRIDS_LINES;
  cd->init_voxel_size = clamp_f(
      mesh->remesh_voxel_size, max_ff(cd->voxel_size_min, 0.0001f), cd->voxel_size_max);
  cd->voxel_size = cd->init_voxel_size;

  /* Matrix calculation to position the text in 3D space. */
  float text_pos[3];
  float scale_mat[4][4];

  float d_a[3], d_b[3];
  float d_a_proj[2], d_b_proj[2];
  float preview_plane_proj[4][2];
  const float y_axis_proj[2] = {0.0f, 1.0f};

  mid_v3_v3v3(text_pos, cd->preview_plane[0], cd->preview_plane[2]);

  /* Project the selected face in the previous step of the Bounding Box. */
  for (int i = 0; i < 4; i++) {
    float preview_plane_world_space[3];
    mul_v3_m4v3(
        preview_plane_world_space, active_object->object_to_world().ptr(), cd->preview_plane[i]);
    ED_view3d_project_v2(region, preview_plane_world_space, preview_plane_proj[i]);
  }

  /* Get the initial X and Y axis of the basis from the edges of the Bounding Box face. */
  sub_v3_v3v3(d_a, cd->preview_plane[1], cd->preview_plane[0]);
  sub_v3_v3v3(d_b, cd->preview_plane[3], cd->preview_plane[0]);
  normalize_v3(d_a);
  normalize_v3(d_b);

  /* Project the X and Y axis. */
  sub_v2_v2v2(d_a_proj, preview_plane_proj[1], preview_plane_proj[0]);
  sub_v2_v2v2(d_b_proj, preview_plane_proj[3], preview_plane_proj[0]);
  normalize_v2(d_a_proj);
  normalize_v2(d_b_proj);

  unit_m4(cd->text_mat);

  /* Select the axis that is aligned with the view Y axis to use it as the basis Y. */
  if (fabsf(dot_v2v2(d_a_proj, y_axis_proj)) > fabsf(dot_v2v2(d_b_proj, y_axis_proj))) {
    copy_v3_v3(cd->text_mat[0], d_b);
    copy_v3_v3(cd->text_mat[1], d_a);

    /* Flip the X and Y basis vectors to make sure they always point upwards and to the right. */
    if (d_b_proj[0] < 0.0f) {
      mul_v3_fl(cd->text_mat[0], -1.0f);
    }
    if (d_a_proj[1] < 0.0f) {
      mul_v3_fl(cd->text_mat[1], -1.0f);
    }
  }
  else {
    copy_v3_v3(cd->text_mat[0], d_a);
    copy_v3_v3(cd->text_mat[1], d_b);
    if (d_a_proj[0] < 0.0f) {
      mul_v3_fl(cd->text_mat[0], -1.0f);
    }
    if (d_b_proj[1] < 0.0f) {
      mul_v3_fl(cd->text_mat[1], -1.0f);
    }
  }

  /* Use the Bounding Box face normal as the basis Z. */
  normal_tri_v3(cd->text_mat[2], cd->preview_plane[0], cd->preview_plane[1], cd->preview_plane[2]);

  /* Invert object scale. */
  float scale[3];
  mat4_to_size(scale, active_object->object_to_world().ptr());
  invert_v3(scale);
  size_to_mat4(scale_mat, scale);

  mul_m4_m4_pre(cd->text_mat, scale_mat);

  /* Write the text position into the matrix. */
  copy_v3_v3(cd->text_mat[3], text_pos);

  /* Scale the text to constant viewport size. */
  float text_pos_word_space[3];
  mul_v3_m4v3(text_pos_word_space, active_object->object_to_world().ptr(), text_pos);
  const float pixelsize = ED_view3d_pixel_size_no_ui_scale(rv3d, text_pos_word_space);
  scale_m4_fl(scale_mat, pixelsize * 0.5f);
  mul_m4_m4_post(cd->text_mat, scale_mat);

  WM_event_add_modal_handler(C, op);

  ED_region_tag_redraw(region);

  voxel_size_edit_update_header(op, C);

  return OPERATOR_RUNNING_MODAL;
}

static bool voxel_size_edit_poll(bContext *C)
{
  return CTX_wm_region_view3d(C) && object_remesh_poll(C);
}

void OBJECT_OT_voxel_size_edit(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Edit Voxel Size";
  ot->description = "Modify the mesh voxel size interactively used in the voxel remesher";
  ot->idname = "OBJECT_OT_voxel_size_edit";

  /* API callbacks. */
  ot->poll = voxel_size_edit_poll;
  ot->invoke = voxel_size_edit_invoke;
  ot->modal = voxel_size_edit_modal;
  ot->cancel = voxel_size_edit_cancel;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Quadriflow Remesh Operator
 * \{ */

#define QUADRIFLOW_MIRROR_BISECT_TOLERANCE 0.005f

enum {
  QUADRIFLOW_REMESH_RATIO = 1,
  QUADRIFLOW_REMESH_EDGE_LENGTH,
  QUADRIFLOW_REMESH_FACES,
};

enum eQuadreFlowStatus {
  QUADRIFLOW_STATUS_SUCCESS = 1,
  QUADRIFLOW_STATUS_FAIL = 0,
  QUADRIFLOW_STATUS_CANCELED = -1,
  QUADRIFLOW_STATUS_NONMANIFOLD = -2,
};

enum eSymmetryAxes {
  SYMMETRY_AXES_X = (1 << 0),
  SYMMETRY_AXES_Y = (1 << 1),
  SYMMETRY_AXES_Z = (1 << 2),
};

struct QuadriFlowJob {
  /* from wmJob */
  Object *owner;
  wmJobWorkerStatus *worker_status;

  const wmOperator *op;
  Scene *scene;
  int target_faces;
  int seed;
  bool use_mesh_symmetry;
  eSymmetryAxes symmetry_axes;

  bool use_preserve_sharp;
  bool use_preserve_boundary;
  bool use_preserve_face_sets;
  bool use_mesh_curvature;
  bool use_merge_parts;
  float curvature_strength;
  float adaptive_density;
  int relax_iterations;

  /* Optional guide curves (soft orientation constraints), resolved on the main
   * thread into the target object's local space before the worker runs. */
  float guide_radius;
  float guide_strength;
  Array<float3> guide_stroke_points;
  Array<int> guide_stroke_offsets;

  bool preserve_attributes;
  bool smooth_normals;

  eQuadreFlowStatus status;
  bool is_nonblocking_job;
};

static bool mesh_is_manifold_consistent(Mesh *mesh)
{
  /* In this check we count boundary edges as manifold. Additionally, we also
   * check that the direction of the faces are consistent and doesn't suddenly
   * flip
   */
  const Span<float3> positions = mesh->vert_positions();
  const Span<int2> edges = mesh->edges();
  const Span<int> corner_verts = mesh->corner_verts();
  const Span<int> corner_edges = mesh->corner_edges();

  bool is_manifold_consistent = true;
  char *edge_faces = MEM_new_array_zeroed<char>(mesh->edges_num, "remesh_manifold_check");
  int *edge_vert = MEM_new_array_uninitialized<int>(mesh->edges_num, "remesh_consistent_check");

  for (uint i = 0; i < mesh->edges_num; i++) {
    edge_vert[i] = -1;
  }

  for (const int corner_i : corner_verts.index_range()) {
    const int vert = corner_verts[corner_i];
    const int edge = corner_edges[corner_i];
    edge_faces[edge] += 1;
    if (edge_faces[edge] > 2) {
      is_manifold_consistent = false;
      break;
    }

    if (edge_vert[edge] == -1) {
      edge_vert[edge] = vert;
    }
    else if (edge_vert[edge] == vert) {
      /* Mesh has flips in the surface so it is non consistent */
      is_manifold_consistent = false;
      break;
    }
  }

  if (is_manifold_consistent) {
    for (const int i : edges.index_range()) {
      /* Check for wire edges. */
      if (edge_faces[i] == 0) {
        is_manifold_consistent = false;
        break;
      }
      /* Check for zero length edges */
      if (compare_v3v3(positions[edges[i][0]], positions[edges[i][1]], 1e-4f)) {
        is_manifold_consistent = false;
        break;
      }
    }
  }

  MEM_delete(edge_faces);
  MEM_delete(edge_vert);

  return is_manifold_consistent;
}

static void quadriflow_free_job(void *customdata)
{
  QuadriFlowJob *qj = static_cast<QuadriFlowJob *>(customdata);
  MEM_delete(qj);
}

/* called by quadriflowjob, only to check job 'stop' value */
static int quadriflow_break_job(void *customdata)
{
  QuadriFlowJob *qj = static_cast<QuadriFlowJob *>(customdata);
  // return *(qj->stop);

  /* this is not nice yet, need to make the jobs list template better
   * for identifying/acting upon various different jobs canceled */
  /* but for now we'll reuse the render break... */
  bool should_break = false;

  if (qj->is_nonblocking_job) {
    bool should_break = (G.is_break);
    if (should_break) {
      qj->status = QUADRIFLOW_STATUS_CANCELED;
    }
  }

  return should_break;
}

/** Called by ocean-bake, #wmJob sends notifier. */
static void quadriflow_update_job(void *customdata, float progress, int *cancel)
{
  QuadriFlowJob *qj = static_cast<QuadriFlowJob *>(customdata);

  if (quadriflow_break_job(qj)) {
    *cancel = 1;
  }
  else {
    *cancel = 0;
  }

  qj->worker_status->do_update = true;
  qj->worker_status->progress = progress;
}

static Mesh *remesh_symmetry_bisect(Mesh *mesh, eSymmetryAxes symmetry_axes)
{
  MirrorModifierData mmd = {};
  mmd.tolerance = QUADRIFLOW_MIRROR_BISECT_TOLERANCE;

  Mesh *mesh_bisect, *mesh_bisect_temp;
  mesh_bisect = BKE_mesh_copy_for_eval(*mesh);

  int axis;
  float plane_co[3], plane_no[3];
  zero_v3(plane_co);

  for (char i = 0; i < 3; i++) {
    eSymmetryAxes symm_it = eSymmetryAxes(1 << i);
    if (symmetry_axes & symm_it) {
      axis = i;
      mmd.flag = {};
      mmd.flag &= MirrorModifierFlag(int(MOD_MIR_BISECT_AXIS_X) << i);
      zero_v3(plane_no);
      plane_no[axis] = -1.0f;
      mesh_bisect_temp = mesh_bisect;
      mesh_bisect = BKE_mesh_mirror_bisect_on_mirror_plane_for_modifier(
          &mmd, mesh_bisect, axis, plane_co, plane_no);
      if (mesh_bisect_temp != mesh_bisect) {
        BKE_id_free(nullptr, mesh_bisect_temp);
      }
    }
  }

  BKE_id_free(nullptr, mesh);

  return mesh_bisect;
}

static Mesh *remesh_symmetry_mirror(Object *ob, Mesh *mesh, eSymmetryAxes symmetry_axes)
{
  MirrorModifierData mmd = {};
  mmd.tolerance = QUADRIFLOW_MIRROR_BISECT_TOLERANCE;
  Mesh *mesh_mirror, *mesh_mirror_temp;

  mesh_mirror = mesh;

  int axis;

  for (char i = 0; i < 3; i++) {
    eSymmetryAxes symm_it = eSymmetryAxes(1 << i);
    if (symmetry_axes & symm_it) {
      axis = i;
      mmd.flag = {};
      mmd.flag &= MirrorModifierFlag(int(MOD_MIR_AXIS_X) << i);
      mesh_mirror_temp = mesh_mirror;
      mesh_mirror = BKE_mesh_mirror_apply_mirror_on_axis_for_modifier(
          &mmd, ob, mesh_mirror, axis, true, nullptr, nullptr);
      if (mesh_mirror_temp != mesh_mirror) {
        BKE_id_free(nullptr, mesh_mirror_temp);
      }
    }
  }

  return mesh_mirror;
}

/* Number of edge-connected islands among vertices used by faces. Loose
 * vertices and wire edges are ignored: the voxel union that consumes this
 * count only sees the face surface anyway. */
static int remesh_face_islands_num(const Mesh &mesh)
{
  const Span<int2> edges = mesh.edges();
  const Span<int> corner_verts = mesh.corner_verts();

  DisjointSet<int> islands(mesh.verts_num);
  for (const int2 &edge : edges) {
    islands.join(edge[0], edge[1]);
  }

  Array<bool> used(mesh.verts_num, false);
  for (const int v : corner_verts) {
    used[v] = true;
  }
  /* Count distinct roots among used vertices; the root itself may be an
   * unused wire-edge vertex of the same island. */
  Array<bool> root_seen(mesh.verts_num, false);
  int islands_num = 0;
  for (const int v : IndexRange(mesh.verts_num)) {
    if (!used[v]) {
      continue;
    }
    const int root = islands.find_root(v);
    if (!root_seen[root]) {
      root_seen[root] = true;
      islands_num++;
    }
  }
  return islands_num;
}

static void quadriflow_start_job(void *customdata, wmJobWorkerStatus *worker_status)
{
  QuadriFlowJob *qj = static_cast<QuadriFlowJob *>(customdata);

  qj->worker_status = worker_status;
  qj->status = QUADRIFLOW_STATUS_SUCCESS;

  if (qj->is_nonblocking_job) {
    G.is_break = false; /* XXX shared with render - replace with job 'stop' switch */
  }

  Object *ob = qj->owner;
  Mesh *mesh = id_cast<Mesh *>(ob->data);
  Scene &scene = *qj->scene;
  Mesh *new_mesh;
  Mesh *bisect_mesh;

  /* Check if the mesh is manifold. Quadriflow requires manifold meshes */
  if (!mesh_is_manifold_consistent(mesh)) {
    qj->status = QUADRIFLOW_STATUS_NONMANIFOLD;
    return;
  }

  /* Run Quadriflow bisect operations on a copy of the mesh to keep the code readable without
   * freeing the original ID */
  bisect_mesh = BKE_mesh_copy_for_eval(*mesh);

  /* Merge separate parts into one watertight volume (voxel union) before quad
   * remeshing, so intersecting pieces become a single surface instead of being
   * remeshed independently. Only runs when the mesh actually has multiple
   * face islands; a single-piece mesh keeps its original surface untouched.
   * The voxel size is derived from the target quad edge length so the union
   * keeps more detail than the final quads can express. */
  if (qj->use_merge_parts) {
    const int islands_num = remesh_face_islands_num(*bisect_mesh);
    if (islands_num > 1) {
      const float area = BKE_mesh_calc_area(bisect_mesh);
      if (area > 0.0f) {
        const float target_edge_len = std::sqrt(area / float(math::max(qj->target_faces, 1)));
        const float voxel_size = math::max(0.5f * target_edge_len, 1e-5f);
        if (Mesh *merged = BKE_mesh_remesh_voxel(bisect_mesh, voxel_size, 0.0f, 0.0f, nullptr)) {
          BKE_id_free(nullptr, bisect_mesh);
          bisect_mesh = merged;
          BKE_reportf(worker_status->reports,
                      RPT_INFO,
                      "QuadriFlow: merged %d parts into one volume (voxel size %f)",
                      islands_num,
                      double(voxel_size));
        }
        else {
          BKE_report(worker_status->reports,
                     RPT_WARNING,
                     "QuadriFlow: could not merge mesh parts (voxel remesh failed), remeshing "
                     "the parts separately");
        }
      }
    }
  }

  /* Bisect the input mesh using the paint symmetry settings */
  bisect_mesh = remesh_symmetry_bisect(bisect_mesh, qj->symmetry_axes);

  /* Build a per-vertex orientation guide from surface curvature and/or guide
   * curves, then feed it to the cross-field solver. Computed on the bisected
   * mesh so the guide indices match the solver input. The two sources combine
   * into the same arrays; the stronger weight wins per vertex. */
  Array<float3> guide_dirs;
  Array<float> guide_weights;
  Array<float> guide_pin_weights;
  const float *guide_dirs_ptr = nullptr;
  const float *guide_weights_ptr = nullptr;
  const float *guide_pin_weights_ptr = nullptr;
  const bool use_strokes = qj->guide_stroke_offsets.size() >= 2;
  if (qj->use_mesh_curvature || use_strokes || qj->use_preserve_face_sets) {
    const int verts_num = bisect_mesh->verts_num;
    guide_dirs.reinitialize(verts_num);
    guide_weights.reinitialize(verts_num);
    guide_dirs.fill(float3(0.0f));
    guide_weights.fill(0.0f);

    if (qj->use_mesh_curvature) {
      bke::mesh_curvature_guide_field(
          *bisect_mesh, qj->curvature_strength, guide_dirs, guide_weights);
    }
    if (use_strokes) {
      const OffsetIndices<int> offsets(qj->guide_stroke_offsets.as_span());
      bke::mesh_guide_strokes_field(*bisect_mesh,
                                    qj->guide_stroke_points,
                                    offsets,
                                    qj->guide_radius,
                                    qj->guide_strength,
                                    guide_dirs,
                                    guide_weights);
    }
    if (qj->use_preserve_face_sets) {
      /* Only face set boundaries pin positions; curvature and stroke guides
       * steer orientation without restricting where the quads land. */
      guide_pin_weights.reinitialize(verts_num);
      guide_pin_weights.fill(0.0f);
      bke::mesh_face_set_boundaries_field(
          *bisect_mesh, guide_dirs, guide_weights, guide_pin_weights);
      guide_pin_weights_ptr = guide_pin_weights.data();
    }
    guide_dirs_ptr = reinterpret_cast<const float *>(guide_dirs.data());
    guide_weights_ptr = guide_weights.data();
  }

  /* Adaptive quad density from the absolute curvature: smaller quads on
   * detailed regions, larger on flat ones. Independent from the orientation
   * guides above. */
  Array<float> guide_scales;
  const float *guide_scales_ptr = nullptr;
  if (qj->adaptive_density > 0.0f) {
    guide_scales.reinitialize(bisect_mesh->verts_num);
    bke::mesh_curvature_density_field(*bisect_mesh, qj->adaptive_density, guide_scales);
    guide_scales_ptr = guide_scales.data();
  }

  /* Report the guide statistics (Info editor and console). This both verifies
   * that the guided path actually ran in this build and gives the numbers
   * needed to tune strength/adaptivity when the effect looks too weak. */
  {
    int guided_num = 0;
    float weight_max = 0.0f;
    for (const float w : guide_weights.as_span()) {
      if (w > 0.01f) {
        guided_num++;
        weight_max = math::max(weight_max, w);
      }
    }
    float scale_min = 1.0f;
    float scale_max = 1.0f;
    for (const float s : guide_scales.as_span()) {
      scale_min = math::min(scale_min, s);
      scale_max = math::max(scale_max, s);
    }
    BKE_reportf(worker_status->reports,
                RPT_INFO,
                "QuadriFlow guides: %d of %d vertices oriented (max weight %.2f), "
                "density scale %.2f-%.2f, relax %d",
                guided_num,
                bisect_mesh->verts_num,
                double(weight_max),
                double(scale_min),
                double(scale_max),
                qj->relax_iterations);
    /* Job INFO reports are not printed by default; mirror the line to stdout
     * (visible in the system console) so the guided path is verifiable. */
    printf("QuadriFlow guides: %d of %d vertices oriented (max weight %.2f), "
           "density scale %.2f-%.2f, relax %d\n",
           guided_num,
           bisect_mesh->verts_num,
           double(weight_max),
           double(scale_min),
           double(scale_max),
           qj->relax_iterations);
    fflush(stdout);
  }

  new_mesh = BKE_mesh_remesh_quadriflow(bisect_mesh,
                                        qj->target_faces,
                                        qj->seed,
                                        qj->use_preserve_sharp,
                                        (qj->use_preserve_boundary || qj->use_mesh_symmetry),
                                        /*adaptive_scale*/ false,
                                        quadriflow_update_job,
                                        static_cast<void *>(qj),
                                        guide_dirs_ptr,
                                        guide_weights_ptr,
                                        guide_pin_weights_ptr,
                                        guide_scales_ptr);

  /* Post-process: relax the quads on the original surface for more uniform
   * shapes, before the bisected source mesh is freed. Edges sharper than 30
   * degrees are kept fixed so creases and hard corners are not rounded. */
  if (new_mesh != nullptr && qj->relax_iterations > 0) {
    bke::mesh_relax_reproject(
        *new_mesh, *bisect_mesh, qj->relax_iterations, 0.5f, /*sharp_angle=30deg*/ 0.523599f);
  }

  BKE_id_free(nullptr, bisect_mesh);

  if (new_mesh == nullptr) {
    worker_status->do_update = true;
    worker_status->stop = false;
    if (qj->status == QUADRIFLOW_STATUS_SUCCESS) {
      /* This is not a user cancellation event. */
      qj->status = QUADRIFLOW_STATUS_FAIL;
    }
    return;
  }

  /* Mirror the Quadriflow result to build the final mesh */
  new_mesh = remesh_symmetry_mirror(qj->owner, new_mesh, qj->symmetry_axes);

  if (ob->mode == OB_MODE_SCULPT) {
    sculpt_paint::undo::geometry_begin(scene, *ob, qj->op);
  }

  if (qj->preserve_attributes) {
    bke::mesh_remesh_reproject_attributes(*mesh, *new_mesh);
  }

  BKE_mesh_nomain_to_mesh(new_mesh, mesh, ob);

  bke::mesh_smooth_set(*id_cast<Mesh *>(ob->data), qj->smooth_normals);

  if (ob->mode == OB_MODE_SCULPT) {
    sculpt_paint::undo::geometry_end(*ob);
    BKE_sculptsession_free_pbvh(*ob);
  }
  BKE_mesh_batch_cache_dirty_tag(id_cast<Mesh *>(ob->data), BKE_MESH_BATCH_DIRTY_ALL);

  worker_status->do_update = true;
  worker_status->stop = false;
}

static void quadriflow_end_job(void *customdata)
{
  QuadriFlowJob *qj = static_cast<QuadriFlowJob *>(customdata);

  Object *ob = qj->owner;

  if (qj->is_nonblocking_job) {
    WM_locked_interface_set(static_cast<wmWindowManager *>(G_MAIN->wm.first), false);
  }

  ReportList *reports = qj->worker_status->reports;
  switch (qj->status) {
    case QUADRIFLOW_STATUS_SUCCESS:
      /* Spatially organize the mesh after remesh. */
      bke::mesh_apply_spatial_organization(*id_cast<Mesh *>(ob->data));
      DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
      BKE_reportf(reports, RPT_INFO, "QuadriFlow: Remeshing completed");
      break;
    case QUADRIFLOW_STATUS_FAIL:
      BKE_reportf(reports, RPT_ERROR, "QuadriFlow: Remeshing failed");
      break;
    case QUADRIFLOW_STATUS_CANCELED:
      BKE_report(reports, RPT_WARNING, "QuadriFlow: Remeshing canceled");
      break;
    case QUADRIFLOW_STATUS_NONMANIFOLD:
      BKE_report(reports,
                 RPT_WARNING,
                 "QuadriFlow: The mesh needs to be manifold and have face normals that point in a "
                 "consistent direction");
      break;
  }
}

static wmOperatorStatus quadriflow_remesh_exec(bContext *C, wmOperator *op)
{
  QuadriFlowJob *job = MEM_new<QuadriFlowJob>("QuadriFlowJob");

  job->op = op;
  job->owner = CTX_data_active_object(C);
  job->scene = CTX_data_scene(C);

  job->target_faces = RNA_int_get(op->ptr, "target_faces");
  job->seed = RNA_int_get(op->ptr, "seed");

  job->use_mesh_symmetry = RNA_boolean_get(op->ptr, "use_mesh_symmetry");

  job->use_preserve_sharp = RNA_boolean_get(op->ptr, "use_preserve_sharp");
  job->use_preserve_boundary = RNA_boolean_get(op->ptr, "use_preserve_boundary");
  job->use_preserve_face_sets = RNA_boolean_get(op->ptr, "use_preserve_face_sets");

  job->use_mesh_curvature = RNA_boolean_get(op->ptr, "use_mesh_curvature");
  job->use_merge_parts = RNA_boolean_get(op->ptr, "use_merge_parts");
  job->curvature_strength = RNA_float_get(op->ptr, "curvature_strength");
  job->adaptive_density = RNA_float_get(op->ptr, "adaptive_density");
  job->relax_iterations = RNA_int_get(op->ptr, "relax_iterations");

  /* Resolve optional guide curves here (main thread) into the target object's
   * local space, so the worker thread only needs the plain point data. */
  job->guide_strength = RNA_float_get(op->ptr, "guide_strength");
  job->guide_radius = RNA_float_get(op->ptr, "guide_radius");
  char guide_name[MAX_ID_NAME - 2];
  RNA_string_get(op->ptr, "guide_curves_object", guide_name);
  if (guide_name[0] == '\0' && job->owner) {
    /* Default to the companion object created by the guide-draw tool, so drawn
     * guides are picked up without the user typing the name. */
    SNPRINTF(guide_name, "%s_QuadGuides", job->owner->id.name + 2);
  }
  if (guide_name[0] != '\0' && job->owner) {
    Main *bmain = CTX_data_main(C);
    Object *guide_ob = reinterpret_cast<Object *>(
        BKE_libblock_find_name(bmain, ID_OB, guide_name));
    if (guide_ob && guide_ob->type == OB_CURVES) {
      const Curves &curves_id = *id_cast<const Curves *>(guide_ob->data);
      const bke::CurvesGeometry &curves = curves_id.geometry.wrap();
      const Span<float3> src = curves.positions();
      const OffsetIndices<int> by_curve = curves.points_by_curve();
      if (!src.is_empty() && by_curve.size() > 0) {
        const float4x4 to_target = job->owner->world_to_object() *
                                   guide_ob->object_to_world();
        job->guide_stroke_points.reinitialize(src.size());
        math::transform_points(src, to_target, job->guide_stroke_points.as_mutable_span());
        job->guide_stroke_offsets.reinitialize(by_curve.data().size());
        job->guide_stroke_offsets.as_mutable_span().copy_from(by_curve.data());
      }
    }
  }

  job->preserve_attributes = RNA_boolean_get(op->ptr, "preserve_attributes");
  job->smooth_normals = RNA_boolean_get(op->ptr, "smooth_normals");

  /* Update the target face count if symmetry is enabled */
  Object *ob = CTX_data_active_object(C);
  if (ob && job->use_mesh_symmetry) {
    Mesh *mesh = BKE_mesh_from_object(ob);
    job->symmetry_axes = eSymmetryAxes(mesh->symmetry);
    for (char i = 0; i < 3; i++) {
      eSymmetryAxes symm_it = eSymmetryAxes(1 << i);
      if (job->symmetry_axes & symm_it) {
        job->target_faces = job->target_faces / 2;
      }
    }
  }
  else {
    job->use_mesh_symmetry = false;
    job->symmetry_axes = eSymmetryAxes{};
  }

  eQuadreFlowStatus status = QUADRIFLOW_STATUS_SUCCESS;
  if ((op->flag & OP_IS_INVOKE) == 0) {
    /* This is called directly from the exec operator, this operation is now blocking */
    job->is_nonblocking_job = false;
    wmJobWorkerStatus worker_status = {};
    worker_status.reports = op->reports;
    quadriflow_start_job(job, &worker_status);

    status = job->status;
    quadriflow_end_job(job);
    quadriflow_free_job(job);
  }
  else {
    /* Non blocking call. For when the operator has been called from the GUI. */
    job->is_nonblocking_job = true;

    wmJob *wm_job = WM_jobs_get(CTX_wm_manager(C),
                                CTX_wm_window(C),
                                CTX_data_scene(C),
                                "Remeshing with QuadriFlow...",
                                WM_JOB_PROGRESS,
                                WM_JOB_TYPE_QUADRIFLOW_REMESH);

    WM_jobs_customdata_set(wm_job, job, quadriflow_free_job);
    WM_jobs_timer(wm_job, 0.1, NC_GEOM | ND_DATA, NC_GEOM | ND_DATA);
    WM_jobs_callbacks(wm_job, quadriflow_start_job, nullptr, nullptr, quadriflow_end_job);

    WM_locked_interface_set(CTX_wm_manager(C), true);

    WM_jobs_start(CTX_wm_manager(C), wm_job);
  }

  if (status == QUADRIFLOW_STATUS_SUCCESS) {
    return OPERATOR_FINISHED;
  }
  /* Only ever runs with immediate execution. */
  return OPERATOR_CANCELLED;
}

static bool quadriflow_check(bContext *C, wmOperator *op)
{
  int mode = RNA_enum_get(op->ptr, "mode");

  if (mode == QUADRIFLOW_REMESH_EDGE_LENGTH) {
    float area = RNA_float_get(op->ptr, "mesh_area");
    if (area < 0.0f) {
      Object *ob = CTX_data_active_object(C);
      area = BKE_mesh_calc_area(id_cast<const Mesh *>(ob->data));
      RNA_float_set(op->ptr, "mesh_area", area);
    }
    int faces_num;
    float edge_len = RNA_float_get(op->ptr, "target_edge_length");

    faces_num = area / (edge_len * edge_len);
    RNA_int_set(op->ptr, "target_faces", faces_num);
  }
  else if (mode == QUADRIFLOW_REMESH_RATIO) {
    Object *ob = CTX_data_active_object(C);
    Mesh *mesh = id_cast<Mesh *>(ob->data);

    int faces_num;
    float ratio = RNA_float_get(op->ptr, "target_ratio");

    faces_num = mesh->faces_num * ratio;

    RNA_int_set(op->ptr, "target_faces", faces_num);
  }

  return true;
}

/* Hide the target variables if they are not active */
static bool quadriflow_poll_property(const bContext *C, wmOperator *op, const PropertyRNA *prop)
{
  const char *prop_id = RNA_property_identifier(prop);

  if (STRPREFIX(prop_id, "target")) {
    int mode = RNA_enum_get(op->ptr, "mode");

    if (STREQ(prop_id, "target_edge_length") && mode != QUADRIFLOW_REMESH_EDGE_LENGTH) {
      return false;
    }
    if (STREQ(prop_id, "target_faces")) {
      if (mode != QUADRIFLOW_REMESH_FACES) {
        /* Make sure we can edit the target_faces value even if it doesn't start as EDITABLE */
        float area = RNA_float_get(op->ptr, "mesh_area");
        if (area < -0.8f) {
          area += 0.2f;
          /* Make sure we have up to date values from the start */
          RNA_def_property_flag(const_cast<PropertyRNA *>(prop), PROP_EDITABLE);
          quadriflow_check(const_cast<bContext *>(C), op);
        }

        /* Only disable input */
        RNA_def_property_clear_flag(const_cast<PropertyRNA *>(prop), PROP_EDITABLE);
      }
      else {
        RNA_def_property_flag(const_cast<PropertyRNA *>(prop), PROP_EDITABLE);
      }
    }
    else if (STREQ(prop_id, "target_ratio") && mode != QUADRIFLOW_REMESH_RATIO) {
      return false;
    }
  }

  if (STREQ(prop_id, "curvature_strength") && !RNA_boolean_get(op->ptr, "use_mesh_curvature")) {
    return false;
  }

  if (STREQ(prop_id, "guide_strength") || STREQ(prop_id, "guide_radius")) {
    char guide_name[MAX_ID_NAME - 2];
    RNA_string_get(op->ptr, "guide_curves_object", guide_name);
    if (guide_name[0] == '\0') {
      return false;
    }
  }

  return true;
}

static const EnumPropertyItem mode_type_items[] = {
    {QUADRIFLOW_REMESH_RATIO,
     "RATIO",
     0,
     "Ratio",
     "Specify target number of faces relative to the current mesh"},
    {QUADRIFLOW_REMESH_EDGE_LENGTH,
     "EDGE",
     0,
     "Edge Length",
     "Input target edge length in the new mesh"},
    {QUADRIFLOW_REMESH_FACES, "FACES", 0, "Faces", "Input target number of faces in the new mesh"},
    {0, nullptr, 0, nullptr, nullptr},
};

static wmOperatorStatus quadriflow_remesh_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  return WM_operator_props_popup_confirm_ex(
      C, op, event, IFACE_("QuadriFlow Remesh the Selected Mesh"), IFACE_("Remesh"));
}

void OBJECT_OT_quadriflow_remesh(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "QuadriFlow Remesh";
  ot->description =
      "Create a new quad based mesh using the surface data of the current mesh. All data "
      "layers will be lost";
  ot->idname = "OBJECT_OT_quadriflow_remesh";

  /* API callbacks. */
  ot->poll = object_remesh_poll;
  ot->poll_property = quadriflow_poll_property;
  ot->check = quadriflow_check;
  ot->invoke = quadriflow_remesh_invoke;
  ot->exec = quadriflow_remesh_exec;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  PropertyRNA *prop;

  /* properties */
  RNA_def_boolean(ot->srna,
                  "use_mesh_symmetry",
                  true,
                  "Use Mesh Symmetry",
                  "Generates a symmetrical mesh using the mesh symmetry configuration");

  RNA_def_boolean(ot->srna,
                  "use_preserve_sharp",
                  true,
                  "Preserve Sharp",
                  "Try to preserve sharp features on the mesh");

  RNA_def_boolean(ot->srna,
                  "use_merge_parts",
                  true,
                  "Merge Parts",
                  "Union separate or intersecting parts of the mesh into a single volume before "
                  "remeshing (uses a voxel pass; only applies when the mesh has multiple parts)");

  RNA_def_boolean(ot->srna,
                  "use_preserve_boundary",
                  false,
                  "Preserve Mesh Boundary",
                  "Try to preserve mesh boundary on the mesh");
  RNA_def_boolean(ot->srna,
                  "use_preserve_face_sets",
                  true,
                  "Preserve Face Sets",
                  "Align the new mesh to the Face Set boundaries");
  RNA_def_boolean(ot->srna,
                  "use_mesh_curvature",
                  true,
                  "Use Mesh Curvature",
                  "Align the new edge flow to the surface curvature of the original mesh");

  prop = RNA_def_float_factor(ot->srna,
                              "curvature_strength",
                              0.7f,
                              0.0f,
                              1.0f,
                              "Curvature Strength",
                              "How strongly the curvature guides the edge flow",
                              0.0f,
                              1.0f);

  prop = RNA_def_float_factor(ot->srna,
                              "adaptive_density",
                              0.5f,
                              0.0f,
                              1.0f,
                              "Adaptive Density",
                              "Concentrate smaller quads in curved, detailed regions and larger "
                              "quads in flat ones (0 gives uniform quad size)",
                              0.0f,
                              1.0f);

  RNA_def_int(ot->srna,
              "relax_iterations",
              2,
              0,
              20,
              "Relax Iterations",
              "Number of smoothing steps applied to the result while keeping it on the original "
              "surface, for more uniform quad shapes",
              0,
              10);

  RNA_def_string(ot->srna,
                 "guide_curves_object",
                 nullptr,
                 MAX_ID_NAME - 2,
                 "Guide Curves",
                 "Name of a Curves object whose strokes guide the edge flow");

  prop = RNA_def_float_factor(ot->srna,
                              "guide_strength",
                              0.7f,
                              0.0f,
                              1.0f,
                              "Guide Strength",
                              "How strongly the guide curves constrain the edge flow",
                              0.0f,
                              1.0f);

  prop = RNA_def_float(ot->srna,
                       "guide_radius",
                       0.1f,
                       0.0f,
                       FLT_MAX,
                       "Guide Radius",
                       "Radius of influence around each guide curve segment",
                       0.0001f,
                       1.0f);

  RNA_def_boolean(ot->srna,
                  "preserve_attributes",
                  false,
                  "Preserve Attributes",
                  "Reproject attributes onto the new mesh");

  RNA_def_boolean(ot->srna,
                  "smooth_normals",
                  false,
                  "Smooth Normals",
                  "Set the output mesh normals to smooth");

  RNA_def_enum(ot->srna,
               "mode",
               mode_type_items,
               QUADRIFLOW_REMESH_FACES,
               "Mode",
               "How to specify the amount of detail for the new mesh");

  prop = RNA_def_float(ot->srna,
                       "target_ratio",
                       1,
                       0,
                       FLT_MAX,
                       "Ratio",
                       "Relative number of faces compared to the current mesh",
                       0.0f,
                       1.0f);

  prop = RNA_def_float(ot->srna,
                       "target_edge_length",
                       0.1f,
                       0.0000001f,
                       FLT_MAX,
                       "Edge Length",
                       "Target edge length in the new mesh",
                       0.00001f,
                       1.0f);

  prop = RNA_def_int(ot->srna,
                     "target_faces",
                     4000,
                     1,
                     INT_MAX,
                     "Number of Faces",
                     "Approximate number of faces (quads) in the new mesh",
                     1,
                     INT_MAX);

  prop = RNA_def_float(
      ot->srna,
      "mesh_area",
      -1.0f,
      -FLT_MAX,
      FLT_MAX,
      "Old Object Face Area",
      "This property is only used to cache the object area for later calculations",
      0.0f,
      FLT_MAX);
  RNA_def_property_flag(prop, (PROP_HIDDEN | PROP_SKIP_SAVE));

  RNA_def_int(ot->srna,
              "seed",
              0,
              0,
              INT_MAX,
              "Seed",
              "Random seed to use with the solver. Different seeds will cause the remesher to "
              "come up with different quad layouts on the mesh",
              0,
              255);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Quadriflow Guide Draw Operator
 *
 * Draws guide curves directly on a mesh surface to steer the QuadriFlow edge
 * flow. Each stroke is ray-cast onto the surface and appended to a companion
 * Curves object named "<mesh>_QuadGuides", which #OBJECT_OT_quadriflow_remesh
 * reads through its "Guide Curves" property. Stroke points are stored in world
 * space and the guide object is kept at the identity transform, so the
 * remesher's guide-to-target space conversion is a no-op.
 * \{ */

struct QuadGuideDrawData {
  Object *target = nullptr;
  /** Ray-cast acceleration structure for the evaluated target surface. */
  bke::BVHTreeFromMesh surface_bvh;
  /** World-space points of the in-progress stroke. */
  Vector<float3> stroke;
};

static bool quadriflow_guide_raycast(QuadGuideDrawData &cd,
                                     const ARegion *region,
                                     const int mval[2],
                                     float3 &r_local_co)
{
  const float mval_f[2] = {float(mval[0]), float(mval[1])};
  float3 ray_start_world, ray_dir_world;
  ED_view3d_win_to_ray(region, mval_f, ray_start_world, ray_dir_world);

  /* The surface mesh and its BVH are in the target object's local space. */
  const float4x4 &world_to_object = cd.target->world_to_object();
  const float3 ray_start = math::transform_point(world_to_object, ray_start_world);
  const float3 ray_dir = math::normalize(math::transform_direction(world_to_object, ray_dir_world));

  BVHTreeRayHit hit;
  hit.dist = FLT_MAX;
  hit.index = -1;
  BLI_bvhtree_ray_cast(cd.surface_bvh.tree,
                       ray_start,
                       ray_dir,
                       0.0f,
                       &hit,
                       cd.surface_bvh.raycast_callback,
                       &cd.surface_bvh);
  if (hit.index == -1) {
    return false;
  }
  /* The hit is in the target object's local space, matching the mesh fed to the
   * remesher. */
  r_local_co = float3(hit.co);
  return true;
}

static Object *quadriflow_guide_object_ensure(bContext *C, Object *target)
{
  Main *bmain = CTX_data_main(C);
  char name[MAX_ID_NAME - 2];
  SNPRINTF(name, "%s_QuadGuides", target->id.name + 2);

  Object *guide = reinterpret_cast<Object *>(BKE_libblock_find_name(bmain, ID_OB, name));
  if (guide != nullptr && guide->type == OB_CURVES) {
    return guide;
  }

  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  /* Created at the origin (identity transform) so world-space strokes map directly. */
  guide = BKE_object_add(bmain, scene, view_layer, OB_CURVES, name);
  DEG_relations_tag_update(bmain);

  /* Adding an object makes it active; keep the mesh active so drawing continues. */
  BKE_view_layer_synced_ensure(*bmain, scene, view_layer);
  if (Base *target_base = BKE_view_layer_base_find(view_layer, target)) {
    base_activate(C, target_base);
  }
  return guide;
}

static void quadriflow_guide_commit(bContext *C, QuadGuideDrawData *cd)
{
  if (cd->stroke.size() < 2) {
    return;
  }
  Object *guide = quadriflow_guide_object_ensure(C, cd->target);
  Curves &curves_id = *id_cast<Curves *>(guide->data);
  bke::CurvesGeometry &curves = curves_id.geometry.wrap();

  const int old_points_num = curves.points_num();
  const int old_curves_num = curves.curves_num();
  const int stroke_point_num = cd->stroke.size();

  curves.resize(old_points_num + stroke_point_num, old_curves_num + 1);
  curves.offsets_for_write()[old_curves_num + 1] = old_points_num + stroke_point_num;

  /* Strokes are captured in the target's local space; store them in the guide
   * object's local space, the convention for Curves point data. */
  const float4x4 to_guide = guide->world_to_object() * cd->target->object_to_world();
  math::transform_points(cd->stroke.as_span(),
                         to_guide,
                         curves.positions_for_write().slice(old_points_num, stroke_point_num));

  curves.fill_curve_types(CURVE_TYPE_POLY);
  curves.update_curve_types();
  curves.tag_topology_changed();
  curves.tag_positions_changed();

  DEG_id_tag_update(&curves_id.id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, &curves_id.id);
  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, guide);
}

static void quadriflow_guide_draw_exit(wmOperator *op)
{
  if (QuadGuideDrawData *cd = static_cast<QuadGuideDrawData *>(op->customdata)) {
    MEM_delete(cd);
    op->customdata = nullptr;
  }
}

static wmOperatorStatus quadriflow_guide_draw_invoke(bContext *C,
                                                     wmOperator *op,
                                                     const wmEvent *event)
{
  ARegion *region = CTX_wm_region(C);
  Object *ob = CTX_data_active_object(C);

  /* Ray-cast against the original mesh (the same surface the remesher consumes),
   * not the evaluated copy: it stays valid for the whole modal session and keeps
   * the guides aligned to the mesh that is actually remeshed. */
  const Mesh *mesh = id_cast<const Mesh *>(ob->data);
  if (mesh->faces_num == 0) {
    BKE_report(op->reports, RPT_ERROR, "The active mesh has no faces to draw on");
    return OPERATOR_CANCELLED;
  }

  QuadGuideDrawData *cd = MEM_new<QuadGuideDrawData>(__func__);
  cd->target = ob;
  cd->surface_bvh = mesh->bvh_corner_tris();
  op->customdata = cd;

  float3 co;
  if (quadriflow_guide_raycast(*cd, region, event->mval, co)) {
    cd->stroke.append(co);
  }

  WM_event_add_modal_handler(C, op);
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus quadriflow_guide_draw_modal(bContext *C,
                                                    wmOperator *op,
                                                    const wmEvent *event)
{
  QuadGuideDrawData *cd = static_cast<QuadGuideDrawData *>(op->customdata);
  ARegion *region = CTX_wm_region(C);

  switch (event->type) {
    case MOUSEMOVE: {
      float3 co;
      if (quadriflow_guide_raycast(*cd, region, event->mval, co)) {
        cd->stroke.append(co);
      }
      break;
    }
    case LEFTMOUSE:
      if (event->val == KM_RELEASE) {
        quadriflow_guide_commit(C, cd);
        quadriflow_guide_draw_exit(op);
        return OPERATOR_FINISHED;
      }
      break;
    case EVT_ESCKEY:
    case RIGHTMOUSE:
      quadriflow_guide_draw_exit(op);
      return OPERATOR_CANCELLED;
    default:
      break;
  }
  return OPERATOR_RUNNING_MODAL;
}

static bool quadriflow_guide_draw_poll(bContext *C)
{
  const Object *ob = CTX_data_active_object(C);
  return ob != nullptr && ob->type == OB_MESH && CTX_wm_region_view3d(C) != nullptr;
}

void OBJECT_OT_quadriflow_guide_draw(wmOperatorType *ot)
{
  ot->name = "Draw Quad Guide";
  ot->description =
      "Draw a guide curve on the mesh surface to steer the QuadriFlow edge flow. The stroke is "
      "stored in a companion Curves object referenced by the remesh operator";
  ot->idname = "OBJECT_OT_quadriflow_guide_draw";

  ot->invoke = quadriflow_guide_draw_invoke;
  ot->modal = quadriflow_guide_draw_modal;
  ot->poll = quadriflow_guide_draw_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

}  // namespace blender::ed::object
