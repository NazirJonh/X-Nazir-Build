/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spview3d
 */

#include "view3d_sculpt_drop_preview.hh"

#include "BKE_context.hh"
#include "BKE_global.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_mesh.hh"
#include "BKE_object.hh"
#include "BKE_report.hh"
#include "BKE_screen.hh"

#include "BLI_array.hh"
#include "BLI_listbase.h"
#include "BLI_math_matrix.h"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector_types.hh"
#include "BLI_utildefines.h"

#include "BLO_readfile.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "DNA_ID.h"
#include "DNA_collection_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_enums.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_windowmanager_types.h"

#include "ED_screen.hh"
#include "ED_view3d.hh"

#include "GPU_batch.hh"
#include "GPU_index_buffer.hh"
#include "GPU_matrix.hh"
#include "GPU_shader.hh"
#include "GPU_shader_builtin.hh"
#include "GPU_state.hh"
#include "GPU_vertex_buffer.hh"

#include "UI_resources.hh"

#include "WM_api.hh"
#include "WM_types.hh"

namespace blender::ed::view3d::sculpt_drop_preview {

PreviewJobState::~PreviewJobState()
{
  if (temp_ctx != nullptr) {
    BLO_library_temp_free(temp_ctx);
    temp_ctx = nullptr;
  }
  clear_result();
}

void PreviewJobState::clear_result()
{
  for (PreviewItemCPU &item : result.items) {
    if (item.mesh_nomain) {
      BKE_id_free(nullptr, item.mesh_nomain);
      item.mesh_nomain = nullptr;
    }
  }
  result.items.clear();
}

void free_preview_job_customdata(void *p)
{
  /* Job customdata owns one strong ref; drop-side active_job holds the second.
   * Only delete the holder — ~PreviewJobState frees leftover result when the last ref dies. */
  delete static_cast<std::shared_ptr<PreviewJobState> *>(p);
}

static wmWindow *preview_job_window_from_winid(wmWindowManager &wm, const uint winid)
{
  if (winid == 0) {
    return nullptr;
  }
  for (wmWindow &win : wm.windows) {
    if (win.winid == winid) {
      return &win;
    }
  }
  return nullptr;
}

static void preview_initjob(void *customdata)
{
  auto &state = **static_cast<std::shared_ptr<PreviewJobState> *>(customdata);

  if (!state.accepting.load() || state.real_main == nullptr) {
    state.load_error.store(true);
    return;
  }

  ReportList reports;
  BKE_reports_init(&reports, RPT_STORE);

  state.temp_ctx = BLO_library_temp_load_id(
      state.real_main, state.blend_filepath.c_str(), state.idcode, state.idname.c_str(), &reports);

  if (state.temp_ctx == nullptr || state.temp_ctx->temp_id == nullptr) {
    state.load_error.store(true);
    if (state.temp_ctx != nullptr) {
      BLO_library_temp_free(state.temp_ctx);
      state.temp_ctx = nullptr;
    }
  }

  BKE_reports_free(&reports);
}

static bool preview_copy_mesh_item(PreviewItemCPU &item, const Mesh &mesh_src)
{
  item.mesh_nomain = BKE_mesh_copy_for_eval(mesh_src);
  return item.mesh_nomain != nullptr;
}

static void preview_fail_load(PreviewJobState &state)
{
  state.clear_result();
  state.load_error.store(true);
}

static void preview_tag_view3d_redraw(const uint winid, Main *bmain);

static void preview_startjob_object(PreviewJobState &state, Object &ob)
{
  if (ob.type != OB_MESH) {
    preview_fail_load(state);
    return;
  }

  Mesh *mesh = id_cast<Mesh *>(ob.data);
  if (mesh == nullptr || mesh->faces_num == 0) {
    preview_fail_load(state);
    return;
  }

  PreviewItemCPU item;
  item.local_to_drop = float4x4::identity();
  if (!preview_copy_mesh_item(item, *mesh)) {
    preview_fail_load(state);
    return;
  }

  state.result.is_collection = false;
  fill_placement_from_object(ob, state.result.placement);
  state.result.items.append(item);
}

static void preview_startjob_collection(PreviewJobState &state,
                                        Collection &collection,
                                        wmJobWorkerStatus *status)
{
  float3 center(0.0f);
  int mesh_count = 0;

  for (CollectionObject *cob = static_cast<CollectionObject *>(collection.gobject.first); cob;
       cob = cob->next)
  {
    if (cob->ob == nullptr || cob->ob->type != OB_MESH) {
      continue;
    }
    center += float3(cob->ob->object_to_world().location());
    mesh_count++;
  }

  if (mesh_count == 0) {
    preview_fail_load(state);
    return;
  }
  center /= float(mesh_count);

  float4x4 translate_neg_center = float4x4::identity();
  translate_neg_center.location() = -center;

  state.result.is_collection = true;
  state.result.placement = {};
  state.result.placement.use_bottom_offset = false;

  int mesh_processed = 0;
  for (CollectionObject *cob = static_cast<CollectionObject *>(collection.gobject.first); cob;
       cob = cob->next)
  {
    if (cob->ob == nullptr || cob->ob->type != OB_MESH) {
      continue;
    }

    Mesh *mesh = id_cast<Mesh *>(cob->ob->data);
    if (mesh == nullptr || mesh->faces_num == 0) {
      continue;
    }

    if (status->stop || !state.accepting.load()) {
      preview_fail_load(state);
      return;
    }

    PreviewItemCPU item;
    item.local_to_drop = translate_neg_center * cob->ob->object_to_world();
    if (!preview_copy_mesh_item(item, *mesh)) {
      preview_fail_load(state);
      return;
    }
    state.result.items.append(item);

    mesh_processed++;
    status->progress = 0.35f + (0.50f * float(mesh_processed) / float(mesh_count));
    status->do_update = true;
  }

  if (state.result.items.is_empty()) {
    preview_fail_load(state);
  }
}

static void preview_startjob(void *customdata, wmJobWorkerStatus *status)
{
  auto &state = **static_cast<std::shared_ptr<PreviewJobState> *>(customdata);

  const auto should_stop = [&]() { return status->stop || !state.accepting.load(); };

  status->progress = 0.05f;
  status->do_update = true;

  if (should_stop()) {
    preview_fail_load(state);
    return;
  }

  if (state.load_error.load() || state.temp_ctx == nullptr || state.temp_ctx->temp_id == nullptr) {
    preview_fail_load(state);
    return;
  }

  status->progress = 0.35f;
  status->do_update = true;

  if (should_stop()) {
    preview_fail_load(state);
    return;
  }

  ID *temp_id = state.temp_ctx->temp_id;
  if (state.is_collection || GS(temp_id->name) == ID_GR) {
    preview_startjob_collection(state, *id_cast<Collection *>(temp_id), status);
  }
  else {
    preview_startjob_object(state, *id_cast<Object *>(temp_id));
    status->progress = 0.70f;
    status->do_update = true;
  }

  if (state.load_error.load()) {
    return;
  }

  status->progress = 0.85f;
  status->do_update = true;

  if (should_stop()) {
    preview_fail_load(state);
    return;
  }

  if (state.result.items.is_empty()) {
    preview_fail_load(state);
    return;
  }

  status->progress = 1.0f;
  status->do_update = true;
}

static void preview_endjob(void *customdata)
{
  auto &state = **static_cast<std::shared_ptr<PreviewJobState> *>(customdata);

  if (state.temp_ctx != nullptr) {
    BLO_library_temp_free(state.temp_ctx);
    state.temp_ctx = nullptr;
  }

  Main *bmain = state.real_main;
  if (bmain == nullptr) {
    bmain = G_MAIN;
  }

  if (!state.accepting.load()) {
    state.clear_result();
    state.ready.store(false);
    return;
  }
  if (state.load_error.load() || state.result.items.is_empty()) {
    state.ready.store(false);
    state.clear_result();
    /* Wake draw/hover pull so preview_failed is set without requiring mouse move. */
    preview_tag_view3d_redraw(state.winid, bmain);
    return;
  }
  state.ready.store(true);
  /* Wake draw path so preview_try_pull runs without requiring mouse move (design §6.4). */
  preview_tag_view3d_redraw(state.winid, bmain);
}

void preview_job_start(wmWindowManager &wm,
                       wmWindow *win,
                       const std::shared_ptr<PreviewJobState> &state)
{
  /* Owner is the originating #wmDrag: stops only a previous job started by the same drag
   * (e.g. hovering a different dropbox during the same continuous drag), without touching
   * concurrent preview jobs started by other drags (other windows). */
  preview_job_request_stop(wm, state->job_owner);

  wmWindow *job_win = preview_job_window_from_winid(wm, state->winid);
  if (job_win == nullptr) {
    job_win = win;
  }

  wmJob *wm_job = WM_jobs_get(&wm,
                              job_win,
                              state->job_owner,
                              "Loading Sculpt Asset Preview...",
                              WM_JOB_PROGRESS,
                              WM_JOB_TYPE_SCULPT_ASSET_DROP_PREVIEW);

  auto *holder = new std::shared_ptr<PreviewJobState>(state);
  WM_jobs_customdata_set(wm_job, holder, free_preview_job_customdata);
  WM_jobs_timer(wm_job, 0.1, NC_WM | ND_JOB, NC_WM | ND_JOB);
  WM_jobs_callbacks(wm_job, preview_startjob, preview_initjob, nullptr, preview_endjob);
  WM_jobs_start(&wm, wm_job);
}

void preview_job_start(bContext &C, const std::shared_ptr<PreviewJobState> &state)
{
  wmWindowManager *wm = CTX_wm_manager(&C);
  if (!wm) {
    return;
  }
  if (state->real_main == nullptr) {
    state->real_main = CTX_data_main(&C);
  }
  if (state->winid == 0) {
    if (wmWindow *win = CTX_wm_window(&C)) {
      state->winid = win->winid;
    }
  }
  preview_job_start(*wm, CTX_wm_window(&C), state);
}

void preview_job_request_stop(wmWindowManager &wm, const void *owner)
{
  void *customdata = WM_jobs_customdata_from_type(
      &wm, owner, WM_JOB_TYPE_SCULPT_ASSET_DROP_PREVIEW);
  if (customdata) {
    auto &state = **static_cast<std::shared_ptr<PreviewJobState> *>(customdata);
    state.accepting.store(false);
  }
  WM_jobs_stop_type(&wm, owner, WM_JOB_TYPE_SCULPT_ASSET_DROP_PREVIEW);
}

void fill_placement_from_object(const Object &ob, PreviewPlacement &r_place)
{
  r_place.object_scale = float3(1.0f);
  r_place.bottom_offset = float3(0.0f);
  r_place.use_bottom_offset = false;

  float ob_scale[3];
  mat4_to_size(ob_scale, ob.object_to_world().ptr());
  r_place.object_scale = float3(ob_scale);

  if (const std::optional<Bounds<float3>> bb = BKE_object_boundbox_get(&ob)) {
    r_place.bottom_offset = math::midpoint(bb->min, bb->max);
    r_place.bottom_offset[2] = bb->min[2];
    r_place.use_bottom_offset = true;
  }
}

static void apply_user_rotation(const float rotation_angle,
                                const float plane_omat[3][3],
                                float r_mat[4][4])
{
  if (rotation_angle == 0.0f) {
    return;
  }

  float rot3[3][3], cur3[3][3], new3[3][3];
  axis_angle_to_mat3(rot3, plane_omat[2], rotation_angle);
  copy_m3_m4(cur3, r_mat);
  mul_m3_m3m3(new3, rot3, cur3);
  for (int c = 0; c < 3; c++) {
    r_mat[c][0] = new3[c][0];
    r_mat[c][1] = new3[c][1];
    r_mat[c][2] = new3[c][2];
  }
}

void build_single_object_matrix(const float rotation_angle,
                                const float scale_factor,
                                const PreviewPlacement &place,
                                const float plane_omat[3][3],
                                const float snap_loc[3],
                                float r_mat[4][4])
{
  copy_m4_m3(r_mat, plane_omat);
  copy_v3_v3(r_mat[3], snap_loc);

  apply_user_rotation(rotation_angle, plane_omat, r_mat);

  float ob_scale[3];
  copy_v3_v3(ob_scale, place.object_scale);
  mul_v3_fl(ob_scale, scale_factor);
  rescale_m4(r_mat, ob_scale);

  if (place.use_bottom_offset) {
    float3 offset = place.bottom_offset;
    mul_mat3_m4_v3(r_mat, offset);
    sub_v3_v3(r_mat[3], offset);
  }
}

void build_collection_snap_matrix(const float rotation_angle,
                                  const float scale_factor,
                                  const float plane_omat[3][3],
                                  const float snap_loc[3],
                                  float r_mat[4][4])
{
  copy_m4_m3(r_mat, plane_omat);

  apply_user_rotation(rotation_angle, plane_omat, r_mat);

  if (scale_factor != 1.0f) {
    const float s[3] = {scale_factor, scale_factor, scale_factor};
    rescale_m4(r_mat, s);
  }

  copy_v3_v3(r_mat[3], snap_loc);
  r_mat[3][3] = 1.0f;
}

static void preview_item_gpu_free(PreviewItemGPU &item)
{
  if (item.batch_tris) {
    GPU_batch_discard(item.batch_tris);
    item.batch_tris = nullptr;
  }
  if (item.batch_edges) {
    GPU_batch_discard(item.batch_edges);
    item.batch_edges = nullptr;
  }
  if (item.mesh_nomain) {
    BKE_id_free(nullptr, item.mesh_nomain);
    item.mesh_nomain = nullptr;
  }
}

static gpu::Batch *preview_batch_tris_from_mesh(const Mesh &mesh)
{
  const Span<float3> positions = mesh.vert_positions();
  if (positions.is_empty()) {
    return nullptr;
  }

  /* #Mesh::corner_tris() stores face-corner indices, not vertex indices.
   * Convert with #vert_tris_from_corner_tris before filling the GPU index buffer. */
  const Span<int3> corner_tris = mesh.corner_tris();
  const Span<int> corner_verts = mesh.corner_verts();
  if (corner_tris.is_empty() || corner_verts.is_empty()) {
    return nullptr;
  }

  Array<int3> vert_tris(corner_tris.size());
  bke::mesh::vert_tris_from_corner_tris(corner_verts, corner_tris, vert_tris);

  GPUVertFormat format = {};
  const uint pos_id = GPU_vertformat_attr_add(&format, "pos", gpu::VertAttrType::SFLOAT_32_32_32);

  gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
  GPU_vertbuf_data_alloc(*vbo, positions.size());
  GPU_vertbuf_attr_fill(vbo, pos_id, positions.data());

  GPUIndexBufBuilder builder;
  GPU_indexbuf_init(&builder, GPU_PRIM_TRIS, vert_tris.size(), positions.size());
  for (const int i : vert_tris.index_range()) {
    const int3 &tri = vert_tris[i];
    GPU_indexbuf_set_tri_verts(&builder, i, tri[0], tri[1], tri[2]);
  }

  gpu::IndexBuf *ibo = GPU_indexbuf_build(&builder);
  return GPU_batch_create_ex(GPU_PRIM_TRIS, vbo, ibo, GPU_BATCH_OWNS_VBO | GPU_BATCH_OWNS_INDEX);
}

static gpu::Batch *preview_batch_edges_from_mesh(const Mesh &mesh)
{
  const Span<float3> positions = mesh.vert_positions();
  const Span<int2> edges = mesh.edges();
  if (positions.is_empty() || edges.is_empty()) {
    return nullptr;
  }

  GPUVertFormat format = {};
  const uint pos_id = GPU_vertformat_attr_add(&format, "pos", gpu::VertAttrType::SFLOAT_32_32_32);

  gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
  GPU_vertbuf_data_alloc(*vbo, positions.size());
  GPU_vertbuf_attr_fill(vbo, pos_id, positions.data());

  GPUIndexBufBuilder builder;
  GPU_indexbuf_init(&builder, GPU_PRIM_LINES, edges.size(), positions.size());
  for (const int i : edges.index_range()) {
    GPU_indexbuf_set_line_verts(&builder, i, edges[i][0], edges[i][1]);
  }

  gpu::IndexBuf *ibo = GPU_indexbuf_build(&builder);
  return GPU_batch_create_ex(GPU_PRIM_LINES, vbo, ibo, GPU_BATCH_OWNS_VBO | GPU_BATCH_OWNS_INDEX);
}

void preview_runtime_clear(SculptDropPreviewRuntime &rt)
{
  for (PreviewItemGPU &item : rt.items) {
    preview_item_gpu_free(item);
  }
  rt.items.clear();
  rt.placement = {};
  rt.is_collection = false;
  rt.preview_ready = false;
  rt.preview_failed = false;
}

void preview_build_batches(SculptDropPreviewRuntime &rt)
{
  for (PreviewItemGPU &item : rt.items) {
    if (item.batch_tris) {
      GPU_batch_discard(item.batch_tris);
      item.batch_tris = nullptr;
    }
    if (item.batch_edges) {
      GPU_batch_discard(item.batch_edges);
      item.batch_edges = nullptr;
    }
    if (!item.mesh_nomain) {
      continue;
    }
    item.batch_tris = preview_batch_tris_from_mesh(*item.mesh_nomain);
    item.batch_edges = preview_batch_edges_from_mesh(*item.mesh_nomain);
  }
}

static bool preview_copy_mesh_item_gpu(PreviewItemGPU &item, const Mesh &mesh_src)
{
  item.mesh_nomain = BKE_mesh_copy_for_eval(mesh_src);
  return item.mesh_nomain != nullptr;
}

bool preview_build_local_object(SculptDropPreviewRuntime &rt, Depsgraph &depsgraph, Object &ob)
{
  preview_runtime_clear(rt);

  if (ob.type != OB_MESH) {
    rt.preview_failed = true;
    return false;
  }

  Object *ob_eval = DEG_get_evaluated(&depsgraph, &ob);
  const Mesh *mesh_eval = BKE_object_get_evaluated_mesh(ob_eval);
  if (!mesh_eval || mesh_eval->faces_num == 0) {
    rt.preview_failed = true;
    return false;
  }

  PreviewItemGPU item;
  item.local_to_drop = float4x4::identity();
  if (!preview_copy_mesh_item_gpu(item, *mesh_eval)) {
    rt.preview_failed = true;
    return false;
  }

  rt.is_collection = false;
  fill_placement_from_object(*ob_eval, rt.placement);
  rt.items.append(item);
  /* GPU batches are built lazily from the draw path (#preview_draw_paint_cursor), since this
   * function runs during WM drag/drop event handling with no GPU context guaranteed bound. */
  rt.preview_ready = true;
  return true;
}

bool preview_build_local_collection(SculptDropPreviewRuntime &rt,
                                    Depsgraph &depsgraph,
                                    Collection &collection)
{
  preview_runtime_clear(rt);

  float3 center(0.0f);
  int mesh_count = 0;

  for (CollectionObject *cob = static_cast<CollectionObject *>(collection.gobject.first); cob;
       cob = cob->next)
  {
    if (cob->ob == nullptr || cob->ob->type != OB_MESH) {
      continue;
    }
    Object *ob_eval = DEG_get_evaluated(&depsgraph, cob->ob);
    center += float3(ob_eval->object_to_world().location());
    mesh_count++;
  }

  if (mesh_count == 0) {
    rt.preview_failed = true;
    return false;
  }
  center /= float(mesh_count);

  float4x4 translate_neg_center = float4x4::identity();
  translate_neg_center.location() = -center;

  rt.is_collection = true;
  rt.placement = {};
  rt.placement.use_bottom_offset = false;

  for (CollectionObject *cob = static_cast<CollectionObject *>(collection.gobject.first); cob;
       cob = cob->next)
  {
    if (cob->ob == nullptr || cob->ob->type != OB_MESH) {
      continue;
    }

    Object *ob_eval = DEG_get_evaluated(&depsgraph, cob->ob);
    const Mesh *mesh_eval = BKE_object_get_evaluated_mesh(ob_eval);
    if (mesh_eval == nullptr || mesh_eval->faces_num == 0) {
      continue;
    }

    PreviewItemGPU item;
    item.local_to_drop = translate_neg_center * ob_eval->object_to_world();
    if (!preview_copy_mesh_item_gpu(item, *mesh_eval)) {
      preview_runtime_clear(rt);
      rt.preview_failed = true;
      return false;
    }
    rt.items.append(item);
  }

  if (rt.items.is_empty()) {
    rt.preview_failed = true;
    return false;
  }

  /* GPU batches are built lazily from the draw path (#preview_draw_paint_cursor), since this
   * function runs during WM drag/drop event handling with no GPU context guaranteed bound. */
  rt.preview_ready = true;
  return true;
}

/** Re-find View3D window region by winid; tag redraw after async pull (design §6.4). */
static void preview_tag_view3d_redraw(const uint winid, Main *bmain)
{
  if (winid == 0 || bmain == nullptr) {
    return;
  }
  wmWindowManager *wm = static_cast<wmWindowManager *>(bmain->wm.first);
  if (!wm) {
    return;
  }

  for (wmWindow &win : wm->windows) {
    if (win.winid != winid) {
      continue;
    }
    bScreen *screen = WM_window_get_active_screen(&win);
    if (!screen) {
      return;
    }
    for (ScrArea &area : screen->areabase) {
      if (area.spacetype != SPACE_VIEW3D) {
        continue;
      }
      if (ARegion *region = BKE_area_find_region_type(&area, RGN_TYPE_WINDOW)) {
        ED_region_tag_redraw(region);
      }
    }
    return;
  }
}

bool preview_try_pull(SculptDropPreviewRuntime &rt)
{
  if (!rt.active_job) {
    return false;
  }

  if (rt.active_job->load_error.load()) {
    rt.preview_failed = true;
    return false;
  }

  if (!rt.active_job->ready.load() || rt.preview_ready) {
    return false;
  }

  if (rt.active_job->generation != rt.generation) {
    return false;
  }

  PreviewJobState &job = *rt.active_job;
  preview_runtime_clear(rt);

  for (PreviewItemCPU &cpu_item : job.result.items) {
    PreviewItemGPU gpu_item;
    gpu_item.mesh_nomain = cpu_item.mesh_nomain;
    gpu_item.local_to_drop = cpu_item.local_to_drop;
    cpu_item.mesh_nomain = nullptr;
    rt.items.append(gpu_item);
  }

  rt.placement = job.result.placement;
  rt.is_collection = job.result.is_collection;
  job.clear_result();

  preview_build_batches(rt);
  rt.preview_ready = true;

  Main *bmain = rt.active_job->real_main;
  if (bmain == nullptr) {
    bmain = G_MAIN;
  }
  preview_tag_view3d_redraw(rt.redraw_winid, bmain);
  return true;
}

/** Same validity rule as #ed::sculpt_paint::is_symmetry_iteration_valid (kept local to avoid
 * pulling sculpt editor internals into space_view3d). */
static bool drop_preview_symmetry_iteration_valid(const char i, const char symm)
{
  return i == 0 || (symm & i && (symm != 5 || i != 3) && (symm != 6 || !ELEM(i, 3, 5)));
}

static float4x4 drop_preview_symmetry_flip_matrix(const ePaintSymmetryFlags symmpass)
{
  float4x4 flip = float4x4::identity();
  if (symmpass & PAINT_SYMM_X) {
    flip[0][0] = -1.0f;
  }
  if (symmpass & PAINT_SYMM_Y) {
    flip[1][1] = -1.0f;
  }
  if (symmpass & PAINT_SYMM_Z) {
    flip[2][2] = -1.0f;
  }
  return flip;
}

static float4x4 drop_preview_world_matrix_for_symmetry_pass(const Object &active_ob_eval,
                                                           const float4x4 &snap_world,
                                                           const ePaintSymmetryFlags symmpass)
{
  if (symmpass == PAINT_SYMM_NONE) {
    return snap_world;
  }
  const float4x4 local = active_ob_eval.world_to_object() * snap_world;
  const float4x4 local_flipped = drop_preview_symmetry_flip_matrix(symmpass) * local;
  return active_ob_eval.object_to_world() * local_flipped;
}

static void drop_preview_draw_item(const PreviewItemGPU &item,
                                   const float m_drop[4][4],
                                   const float color[4],
                                   const float edge_color[4])
{
  if (!item.batch_tris) {
    return;
  }

  float m_item[4][4];
  mul_m4_m4m4(m_item, m_drop, item.local_to_drop.ptr());

  GPU_matrix_push();
  GPU_matrix_mul(m_item);

  GPU_batch_program_set_builtin(item.batch_tris, GPU_SHADER_3D_UNIFORM_COLOR);
  GPU_batch_uniform_4fv(item.batch_tris, "color", color);
  GPU_batch_draw(item.batch_tris);

  if (item.batch_edges) {
    GPU_batch_program_set_builtin(item.batch_edges, GPU_SHADER_3D_UNIFORM_COLOR);
    GPU_batch_uniform_4fv(item.batch_edges, "color", edge_color);
    GPU_batch_draw(item.batch_edges);
  }

  GPU_matrix_pop();
}

void preview_draw_paint_cursor(bContext *C,
                               const int2 &xy,
                               const float2 & /*tilt*/,
                               void *customdata)
{
  SculptDropData *data = static_cast<SculptDropData *>(customdata);
  if (!data) {
    return;
  }

  SculptDropPreviewRuntime &rt = data->preview;
  preview_try_pull(rt);

  if (!rt.preview_ready || rt.items.is_empty()) {
    return;
  }

  /* The local-object/local-collection preview paths fill CPU-side mesh copies without building
   * GPU batches (that would run without a guaranteed GPU context during drag/drop event
   * handling); build them here instead, on the draw path, the first time they are needed. */
  bool needs_batches = false;
  for (const PreviewItemGPU &item : rt.items) {
    if (!item.batch_tris) {
      needs_batches = true;
      break;
    }
  }
  if (needs_batches) {
    preview_build_batches(rt);
  }

  ScrArea *area = CTX_wm_area(C);
  if (!area || area->spacetype != SPACE_VIEW3D) {
    return;
  }

  ARegion *region = BKE_area_find_region_type(area, RGN_TYPE_WINDOW);
  if (!region) {
    return;
  }
  if (region->alignment == RGN_ALIGN_QSPLIT) {
    region = BKE_area_find_region_xy(area, RGN_TYPE_WINDOW, xy);
    if (!region) {
      return;
    }
  }

  /* Match #v3d_cursor_snap_draw_fn: activate drop snap state and refresh before read. */
  ED_view3d_cursor_snap_state_active_set(data->snap_state);
  const int2 mval(xy.x - region->winrct.xmin, xy.y - region->winrct.ymin);
  ED_view3d_cursor_snap_data_update(data->snap_state, C, region, mval);

  const V3DSnapCursorData *snap_data = ED_view3d_cursor_snap_data_get();
  const bool draw_plane = data->snap_state->draw_plane || data->snap_state->draw_box;
  if (snap_data->type_target == SCE_SNAP_TO_NONE && !draw_plane) {
    return;
  }

  RegionView3D *rv3d = static_cast<RegionView3D *>(region->regiondata);

  wmViewport(&region->winrct);
  GPU_matrix_projection_set(rv3d->winmat);
  GPU_matrix_set(rv3d->viewmat);

  float m_drop[4][4];
  if (rt.is_collection) {
    build_collection_snap_matrix(
        data->rotation_angle, data->scale_factor, snap_data->plane_omat, snap_data->loc, m_drop);
  }
  else {
    build_single_object_matrix(data->rotation_angle,
                               data->scale_factor,
                               rt.placement,
                               snap_data->plane_omat,
                               snap_data->loc,
                               m_drop);
  }

  uchar color_ub[4];
  ui::theme::get_color_4ubv(TH_GIZMO_PRIMARY, color_ub);
  float color[4] = {
      color_ub[0] / 255.0f,
      color_ub[1] / 255.0f,
      color_ub[2] / 255.0f,
      0.45f,
  };
  float edge_color[4] = {
      color_ub[0] / 255.0f,
      color_ub[1] / 255.0f,
      color_ub[2] / 255.0f,
      0.85f,
  };

  const GPUBlend blend = GPU_blend_get();
  const GPUDepthTest depth_test = GPU_depth_test_get();
  const bool depth_mask = GPU_depth_mask_get();
  const GPUFaceCullTest face_cull = GPU_face_culling_get();

  GPU_blend(GPU_BLEND_ALPHA);
  /* Drawn without depth-testing so the preview always reads clearly over the sculpt mesh it will
   * be joined into, regardless of what the surface underneath looks like. */
  GPU_depth_test(GPU_DEPTH_NONE);
  GPU_depth_mask(false);
  GPU_face_culling(GPU_CULL_NONE);

  /* Mirror preview through the active sculpt mesh's symmetry flags (same math as the drop
   * operator). Reuse GPU batches; only the world matrix changes per pass. */
  const Object *active_ob = CTX_data_active_object(C);
  const ePaintSymmetryFlags symm =
      (active_ob && active_ob->type == OB_MESH) ?
          ePaintSymmetryFlags(id_cast<const Mesh *>(active_ob->data)->symmetry) :
          PAINT_SYMM_NONE;
  const Object *active_ob_eval = nullptr;
  if (symm != PAINT_SYMM_NONE) {
    Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
    active_ob_eval = DEG_get_evaluated(depsgraph, active_ob);
  }

  const float4x4 snap_world = float4x4(m_drop);

  for (int symmpass = 0; symmpass <= int(symm); symmpass++) {
    if (!drop_preview_symmetry_iteration_valid(char(symmpass), char(symm))) {
      continue;
    }

    float m_drop_pass[4][4];
    if (symmpass == 0 || active_ob_eval == nullptr) {
      copy_m4_m4(m_drop_pass, m_drop);
    }
    else {
      const float4x4 world_mat = drop_preview_world_matrix_for_symmetry_pass(
          *active_ob_eval, snap_world, ePaintSymmetryFlags(symmpass));
      copy_m4_m4(m_drop_pass, world_mat.ptr());
    }

    for (const PreviewItemGPU &item : rt.items) {
      drop_preview_draw_item(item, m_drop_pass, color, edge_color);
    }
  }

  GPU_shader_unbind();
  GPU_face_culling(face_cull);
  GPU_depth_mask(depth_mask);
  GPU_depth_test(depth_test);
  GPU_blend(blend);
  wmWindowViewport(CTX_wm_window(C));
}

}  // namespace blender::ed::view3d::sculpt_drop_preview
