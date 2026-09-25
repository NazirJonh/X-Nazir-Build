/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edobj
 *
 * The Mesh Map bake job and its operator.
 *
 * A bake renders one #eMaterialMeshMapType of one object into the atlas Image the material's slot
 * owns. Nothing of the user's scene is touched: the job builds a private #Main with a copy of the
 * evaluated mesh, a copy of the object transform and a throw-away material carrying an emission
 * graph, then drives Cycles on that temporary scene only. The user's own render engine and Cycles
 * settings are never read or written.
 *
 * The job is keyed on the material (the atlas is a material resource, and the status-bar template
 * already enumerates materials), runs Cycles once (#WM_JOB_EXCL_RENDER), and follows the paint-layer
 * bake split the rest of the codebase uses: the main thread prepares the input and commits the
 * result, the worker only renders into private buffers, and the state is moved exclusively through
 * `BKE_mesh_maps_bake_state_*`.
 */

#include <algorithm>
#include <cstring>

#include "BLI_math_matrix_types.hh"
#include "BLI_span.hh"
#include "BLI_string.h"
#include "BLI_time.h"

#include "MEM_guardedalloc.h"

#include "BKE_collection.hh"
#include "BKE_context.hh"
#include "BKE_global.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_maps.hh"
#include "BKE_mesh_maps_bake.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_object.hh"
#include "BKE_paint_layers.hh"
#include "BKE_report.hh"
#include "BKE_scene.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"
#include "DEG_depsgraph_query.hh"

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "RE_bake.h"
#include "RE_engine.h"
#include "RE_pipeline.h"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_prototypes.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "object_intern.hh"

namespace blender::ed::object {
namespace {

struct MeshMapBakeJob {
  /* The user's data, re-found by session UID on commit. */
  Main *bmain = nullptr;
  Scene *scene = nullptr;
  ViewLayer *view_layer = nullptr;
  Object *object = nullptr;
  Material *material = nullptr;
  uint32_t object_session_uid = 0;
  uint32_t material_session_uid = 0;

  int8_t type = MA_MESH_MAP_AO;
  int8_t previous_status = OB_MESH_MAP_STATUS_NONE;
  bool cancelled = false;
  bool failed = false;
  uint32_t start_hash[2] = {0, 0};
  int resolution = 0;
  Image *atlas = nullptr;
  char uv_name[68] = "";

  /* The private render world. */
  Main *temp_main = nullptr;
  Scene *temp_scene = nullptr;
  ViewLayer *temp_view_layer = nullptr;
  Object *temp_object = nullptr;
  Depsgraph *temp_depsgraph = nullptr;
  Render *render = nullptr;

  /* One image (the atlas), the rasterized texels and the float result buffer. */
  BakeImage bake_image = {};
  Image **material_to_image = nullptr;
  BakeTargets targets = {};
  BakePixel *pixels = nullptr;
  float *result = nullptr;
  size_t pixels_num = 0;
};

void mesh_map_bake_progress_cb(void *handle, const float progress)
{
  wmJobWorkerStatus *worker_status = static_cast<wmJobWorkerStatus *>(handle);
  worker_status->progress = progress;
  worker_status->do_update = true;
}

bool mesh_map_bake_test_break_cb(void *handle)
{
  return static_cast<wmJobWorkerStatus *>(handle)->stop;
}

/** The mesh the bake copies: the evaluated one when available, else the object's own data. */
Mesh *mesh_map_bake_object_mesh(Object &ob)
{
  if (Mesh *evaluated = BKE_object_get_evaluated_mesh(&ob)) {
    return evaluated;
  }
  return id_cast<Mesh *>(ob.data);
}

/** Cycles samples/denoise live on the scene's Python-side `cycles` group; set them when present. */
void mesh_map_bake_apply_cycles_settings(Scene &scene, const MaterialMeshMapSettings &settings)
{
  PointerRNA scene_ptr = RNA_id_pointer_create(&scene.id);
  if (RNA_struct_find_property(&scene_ptr, "cycles") == nullptr) {
    return;
  }
  PointerRNA cycles = RNA_pointer_get(&scene_ptr, "cycles");
  if (cycles.data == nullptr) {
    return;
  }
  RNA_int_set(&cycles, "samples", std::max(settings.samples, 1));
  RNA_boolean_set(&cycles, "use_denoising", settings.use_denoise != 0);
}

void mesh_map_bake_free_target(MeshMapBakeJob &job)
{
  if (job.render != nullptr) {
    RE_FreeRender(job.render);
    job.render = nullptr;
  }
  if (job.temp_depsgraph != nullptr) {
    DEG_graph_free(job.temp_depsgraph);
    job.temp_depsgraph = nullptr;
  }
  if (job.temp_main != nullptr) {
    BKE_main_free(job.temp_main);
    job.temp_main = nullptr;
  }
  MEM_delete(job.pixels);
  job.pixels = nullptr;
  MEM_delete(job.result);
  job.result = nullptr;
  MEM_delete(job.material_to_image);
  job.material_to_image = nullptr;
}

/** Build a render depsgraph for \a scene/\a view_layer, updated and ready to evaluate \a ob. */
Depsgraph *mesh_map_bake_depsgraph_new(Main &bmain, Scene &scene, ViewLayer &view_layer)
{
  Depsgraph *depsgraph = DEG_graph_new(&bmain, &scene, &view_layer, DAG_EVAL_RENDER);
  DEG_disable_visibility_optimization(depsgraph);
  DEG_graph_build_from_view_layer(depsgraph);
  BKE_scene_graph_update_tagged(depsgraph, &bmain);
  return depsgraph;
}

/**
 * Prepare everything the worker needs and leave the object state #OB_MESH_MAP_STATUS_BAKING.
 *
 * Every failure past this point marks the state #OB_MESH_MAP_STATUS_ERROR before returning null, so
 * a refused bake never sticks in the in-flight status.
 */
MeshMapBakeJob *mesh_map_bake_job_create(Main &bmain,
                                         Scene &scene,
                                         ViewLayer &view_layer,
                                         Object &ob,
                                         Material &ma,
                                         const int8_t type,
                                         Depsgraph *user_depsgraph,
                                         ReportList *reports)
{
  if (!ELEM(type, MA_MESH_MAP_AO, MA_MESH_MAP_CURVATURE, MA_MESH_MAP_EDGE, MA_MESH_MAP_NORMAL_WORLD,
            MA_MESH_MAP_NORMAL_OBJECT, MA_MESH_MAP_ID_OBJECT, MA_MESH_MAP_ID_MATERIAL))
  {
    BKE_report(reports, RPT_ERROR, "Unsupported mesh map type");
    return nullptr;
  }
  ObjectMeshMapState *state = BKE_mesh_maps_object_state_ensure(ob, ma, type);
  if (state == nullptr) {
    BKE_report(reports, RPT_ERROR, "The object cannot hold a mesh map state");
    return nullptr;
  }
  MeshMapBakeJob *job = MEM_new<MeshMapBakeJob>(__func__);
  job->bmain = &bmain;
  job->scene = &scene;
  job->view_layer = &view_layer;
  job->object = &ob;
  job->material = &ma;
  job->object_session_uid = ob.id.session_uid;
  job->material_session_uid = ma.id.session_uid;
  job->type = type;
  job->resolution = std::max(ma.mesh_map_settings.resolution, 1);
  job->previous_status = BKE_mesh_maps_bake_state_begin(*state);

  /* Evaluate the user's object once, from the viewport depsgraph the staleness check uses: the
   * hash, the mesh copy and the transform all come from it. The caller owns that depsgraph. */
  Object *ob_eval = user_depsgraph != nullptr ? DEG_get_evaluated(user_depsgraph, &ob) : nullptr;
  if (ob_eval == nullptr || ob_eval == &ob) {
    BKE_mesh_maps_bake_state_fail(*state);
    BKE_report(reports, RPT_ERROR, "The object is not in the evaluated scene");
    MEM_delete(job);
    return nullptr;
  }
  const char *uv_name = BKE_mesh_maps_bake_uv_resolve_or_fail(*state, *ob_eval, ma);
  if (uv_name == nullptr) {
    BKE_reportf(reports,
                RPT_ERROR,
                "The object has no UV layer '%s'",
                ma.paint_layers_uv_map[0] != '\0' ? ma.paint_layers_uv_map : "");
    MEM_delete(job);
    return nullptr;
  }
  STRNCPY(job->uv_name, uv_name);
  BKE_mesh_maps_object_hash(*ob_eval, ma, type, job->start_hash);
  const float4x4 object_matrix = ob_eval->object_to_world();

  Mesh *mesh_eval = mesh_map_bake_object_mesh(*ob_eval);
  Mesh *mesh_copy = mesh_eval != nullptr ?
                        id_cast<Mesh *>(BKE_id_copy_ex(nullptr,
                                                       &mesh_eval->id,
                                                       nullptr,
                                                       LIB_ID_CREATE_LOCAL |
                                                           LIB_ID_COPY_LOCALIZE |
                                                           LIB_ID_COPY_NO_ANIMDATA)) :
                        nullptr;
  if (mesh_copy == nullptr) {
    BKE_mesh_maps_bake_state_fail(*state);
    BKE_report(reports, RPT_ERROR, "Could not copy the evaluated mesh");
    MEM_delete(job);
    return nullptr;
  }

  /* The private render world. */
  job->temp_main = BKE_main_new();
  BKE_libblock_management_main_add(job->temp_main, mesh_copy);
  job->temp_scene = BKE_scene_add(job->temp_main, "Mesh Map Bake");
  job->temp_view_layer = static_cast<ViewLayer *>(job->temp_scene->view_layers.first);
  STRNCPY(job->temp_scene->r.engine, RE_engine_id_CYCLES);
  job->temp_scene->r.xsch = job->resolution;
  job->temp_scene->r.ysch = job->resolution;
  job->temp_scene->r.size = 100;
  job->temp_scene->r.cfra = 1;
  mesh_map_bake_apply_cycles_settings(*job->temp_scene, ma.mesh_map_settings);

  job->temp_object = BKE_object_add_for_data(
      job->temp_main, job->temp_scene, job->temp_view_layer, OB_MESH, "Mesh Map Bake", &mesh_copy->id,
      true);
  BKE_object_apply_mat4(job->temp_object, object_matrix.ptr(), false, false);
  BKE_collection_object_add(job->temp_main, job->temp_scene->master_collection, job->temp_object);

  Material *temp_material = BKE_material_add(job->temp_main, "Mesh Map Bake");
  BKE_object_material_slot_add(job->temp_main, job->temp_object);
  BKE_object_material_assign(
      job->temp_main, job->temp_object, temp_material, 1, BKE_MAT_ASSIGN_OBJECT);
  if (BKE_mesh_maps_bake_pass_for_type(type) == SCE_PASS_EMIT) {
    const float ao_distance = BKE_mesh_maps_bake_ao_distance(*mesh_copy, ma.mesh_map_settings);
    BKE_mesh_maps_bake_build_emit_tree(
        *temp_material->nodetree, type, ma.mesh_map_settings, ao_distance);
    BKE_ntree_update_without_main(*temp_material->nodetree);
  }

  job->temp_depsgraph = mesh_map_bake_depsgraph_new(*job->temp_main, *job->temp_scene,
                                                    *job->temp_view_layer);
  job->render = RE_NewSceneRender(job->temp_scene);
  RE_bake_engine_set_engine_parameters(job->render, job->temp_main, job->temp_scene);
  if (!RE_bake_has_engine(job->render)) {
    BKE_mesh_maps_bake_state_fail(*state);
    BKE_report(reports, RPT_ERROR, "The Cycles bake engine is not available");
    mesh_map_bake_free_target(*job);
    MEM_delete(job);
    return nullptr;
  }

  /* The atlas is a resource of the user's material; the pixels stay private until commit. */
  job->atlas = BKE_mesh_maps_bake_atlas_ensure(bmain, ma, type, job->resolution);
  if (job->atlas == nullptr) {
    BKE_mesh_maps_bake_state_fail(*state);
    BKE_report(reports, RPT_ERROR, "Could not create the mesh map atlas image");
    mesh_map_bake_free_target(*job);
    MEM_delete(job);
    return nullptr;
  }

  job->pixels_num = size_t(job->resolution) * size_t(job->resolution);
  job->material_to_image = MEM_new_array_uninitialized<Image *>(1, __func__);
  job->material_to_image[0] = job->atlas;
  job->bake_image.image = job->atlas;
  job->bake_image.tile_number = 0;
  job->bake_image.uv_offset[0] = 0.0f;
  job->bake_image.uv_offset[1] = 0.0f;
  job->bake_image.width = job->resolution;
  job->bake_image.height = job->resolution;
  job->bake_image.offset = 0;
  job->targets.images = &job->bake_image;
  job->targets.images_num = 1;
  job->targets.material_to_image = job->material_to_image;
  job->targets.materials_num = 1;
  job->targets.pixels_num = int(job->pixels_num);
  job->targets.channels_num = 4;
  job->targets.is_noncolor = true;
  job->targets.result = MEM_new_array_zeroed<float>(job->pixels_num * 4, __func__);
  job->pixels = MEM_new_array_uninitialized<BakePixel>(job->pixels_num, __func__);
  job->result = job->targets.result;

  RE_bake_pixels_populate(mesh_copy, job->pixels, job->pixels_num, &job->targets,
                          StringRef(job->uv_name));
  BKE_mesh_maps_bake_restrict_to_material(job->pixels, job->pixels_num, *mesh_copy, ob, ma);
  if (BKE_mesh_maps_bake_type_cpu_id(type)) {
    BKE_mesh_maps_bake_fill_id_pixels(
        job->pixels, job->pixels_num, *mesh_copy, ob, type, 4, job->result);
  }

  BKE_paint_layers_bake_image_pending_add(job->atlas->id.session_uid);
  return job;
}

void mesh_map_bake_start(void *customdata, wmJobWorkerStatus *worker_status)
{
  MeshMapBakeJob *job = static_cast<MeshMapBakeJob *>(customdata);
  if (job->render == nullptr) {
    job->failed = true;
    return;
  }
  RE_progress_cb(job->render, worker_status, mesh_map_bake_progress_cb);
  RE_test_break_cb(job->render, worker_status, mesh_map_bake_test_break_cb);

  if (BKE_mesh_maps_bake_type_renders(job->type)) {
    const eScenePassType pass_type = BKE_mesh_maps_bake_pass_for_type(job->type);
    const bool ok = RE_bake_engine(job->render,
                                   job->temp_depsgraph,
                                   job->temp_object,
                                   0,
                                   job->pixels,
                                   &job->targets,
                                   pass_type,
                                   0,
                                   job->result);
    if (!ok) {
      job->failed = true;
    }
    else if (pass_type == SCE_PASS_NORMAL) {
      /* Cycles always delivers world-space normals; rotate and compress them for the atlas. */
      const eBakeNormalSwizzle swizzle[3] = {R_BAKE_POSX, R_BAKE_POSY, R_BAKE_POSZ};
      eBakeSpace space = R_BAKE_SPACE_WORLD;
      BKE_mesh_maps_bake_normal_space_for_type(job->type, space);
      if (space == R_BAKE_SPACE_OBJECT) {
        RE_bake_normal_world_to_object(
            job->pixels, job->pixels_num, 4, job->result, job->temp_object, swizzle);
      }
      else {
        RE_bake_normal_world_to_world(job->pixels, job->pixels_num, 4, job->result, swizzle);
      }
    }
  }
  if (worker_status->stop) {
    job->cancelled = true;
  }
}

/** Write the rendered pixels into the live atlas and record the state. Main thread only. */
void mesh_map_bake_commit(MeshMapBakeJob &job)
{
  Material *ma = nullptr;
  for (Material &candidate : job.bmain->materials) {
    if (candidate.id.session_uid == job.material_session_uid && &candidate == job.material) {
      ma = &candidate;
      break;
    }
  }
  Object *ob = nullptr;
  for (Object &candidate : job.bmain->objects) {
    if (candidate.id.session_uid == job.object_session_uid && &candidate == job.object) {
      ob = &candidate;
      break;
    }
  }
  /* The scene and view layer are stored by pointer; drop the commit if they went away. */
  bool scene_alive = false;
  for (Scene &candidate : job.bmain->scenes) {
    if (&candidate == job.scene) {
      scene_alive = true;
      break;
    }
  }
  bool view_layer_alive = false;
  if (scene_alive) {
    for (ViewLayer &candidate : job.scene->view_layers) {
      if (&candidate == job.view_layer) {
        view_layer_alive = true;
        break;
      }
    }
  }
  if (ma == nullptr || ob == nullptr || !scene_alive || !view_layer_alive) {
    return;
  }
  ObjectMeshMapState *state = BKE_mesh_maps_object_state_find(*ob, *ma, job.type);
  if (state == nullptr) {
    return;
  }

  if (job.cancelled) {
    BKE_mesh_maps_bake_state_cancel(*state, job.previous_status);
    return;
  }
  if (job.failed) {
    BKE_mesh_maps_bake_state_fail(*state);
    return;
  }

  /* Recompute the content hash from the same viewport evaluation the staleness check uses, so a
   * successful bake stays VALID instead of turning STALE immediately. */
  uint32_t now_hash[2] = {0, 0};
  if (!BKE_mesh_maps_bake_hash_from_viewport(
          *job.bmain, *job.scene, *job.view_layer, *ob, *ma, job.type, now_hash) ||
      !BKE_mesh_maps_bake_commit_is_current(job.start_hash, now_hash))
  {
    BKE_mesh_maps_bake_state_commit(*state, job.start_hash, now_hash, 0);
    return;
  }

  Image *atlas = BKE_mesh_maps_bake_atlas_ensure(*job.bmain, *ma, job.type, job.resolution);
  if (atlas == nullptr) {
    BKE_mesh_maps_bake_state_fail(*state);
    return;
  }
  /* The pixels are float; a byte atlas would swallow them, so a non-float image is a failed bake,
   * not a silent VALID. */
  if (!BKE_mesh_maps_bake_atlas_is_float(*atlas)) {
    BKE_mesh_maps_bake_state_fail(*state);
    WM_main_add_notifier(NC_MATERIAL | ND_SHADING, ma);
    return;
  }
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(atlas, nullptr, &lock);
  if (ibuf != nullptr && ibuf->float_data() != nullptr) {
    BKE_mesh_maps_bake_write_covered(
        job.type,
        job.pixels,
        job.pixels_num,
        4,
        job.result,
        MutableSpan<float>(ibuf->float_data_for_write(), job.pixels_num * 4));
    ibuf->userflags |= IB_DISPLAY_BUFFER_INVALID;
    BKE_image_mark_dirty(atlas, ibuf);
  }
  BKE_image_release_ibuf(atlas, ibuf, lock);
  BKE_image_partial_update_mark_full_update(atlas);
  BKE_image_free_gputextures(atlas);
  BKE_image_memorypack(atlas);

  BKE_mesh_maps_bake_state_commit(
      *state, job.start_hash, now_hash, int(BLI_time_now_seconds_i()));
  WM_main_add_notifier(NC_IMAGE | NA_EDITED, atlas);
  WM_main_add_notifier(NC_MATERIAL | ND_SHADING, ma);
}

void mesh_map_bake_end(void *customdata)
{
  mesh_map_bake_commit(*static_cast<MeshMapBakeJob *>(customdata));
}

void mesh_map_bake_free(void *customdata)
{
  MeshMapBakeJob *job = static_cast<MeshMapBakeJob *>(customdata);
  if (job->atlas != nullptr) {
    BKE_paint_layers_bake_image_pending_remove(job->atlas->id.session_uid);
  }
  mesh_map_bake_free_target(*job);
  G.is_rendering = false;
  G.is_break = false;
  MEM_delete(job);
}

/* -------------------------------------------------------------------- */
/** \name Operator
 * \{ */

const EnumPropertyItem *mesh_map_bake_type_items()
{
  static const EnumPropertyItem items[] = {
      {MA_MESH_MAP_AO, "AO", 0, "Ambient Occlusion", "Per-object ambient occlusion"},
      {MA_MESH_MAP_CURVATURE, "CURVATURE", 0, "Curvature", "Concave/convex curvature"},
      {MA_MESH_MAP_EDGE, "EDGE", 0, "Edge", "Bevel edge mask"},
      {MA_MESH_MAP_NORMAL_WORLD, "NORMAL_WORLD", 0, "World Normal", "World-space normal"},
      {MA_MESH_MAP_NORMAL_OBJECT, "NORMAL_OBJECT", 0, "Object Normal", "Object-space normal"},
      {MA_MESH_MAP_ID_OBJECT, "ID_OBJECT", 0, "Object ID", "Stable object color"},
      {MA_MESH_MAP_ID_MATERIAL, "ID_MATERIAL", 0, "Material ID", "Per-material-slot color"},
      {0, nullptr, 0, nullptr, nullptr},
  };
  return items;
}

bool mesh_map_bake_poll(bContext *C)
{
  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr || ob->type != OB_MESH) {
    return false;
  }
  Material *ma = BKE_object_material_get(ob, ob->actcol);
  if (ma == nullptr || !paint_layers_is_layered(*ma)) {
    return false;
  }
  if (WM_jobs_test(CTX_wm_manager(C), ma, WM_JOB_TYPE_MESH_MAP_BAKE)) {
    return false;
  }
  const RenderEngineType *engine = RE_engines_find(RE_engine_id_CYCLES);
  return engine != nullptr && engine->bake != nullptr;
}

wmOperatorStatus mesh_map_bake_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr || ob->type != OB_MESH) {
    BKE_report(op->reports, RPT_ERROR, "No active mesh object");
    return OPERATOR_CANCELLED;
  }
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const int8_t type = RNA_enum_get(op->ptr, "type");

  int slot = RNA_int_get(op->ptr, "material_index");
  if (slot <= 0) {
    slot = ob->actcol;
  }
  Material *ma = BKE_object_material_get(ob, slot);
  if (ma == nullptr || !paint_layers_is_layered(*ma)) {
    BKE_report(op->reports, RPT_ERROR, "The material slot is not a layered paint material");
    return OPERATOR_CANCELLED;
  }
  if (WM_jobs_test(CTX_wm_manager(C), ma, WM_JOB_TYPE_MESH_MAP_BAKE)) {
    BKE_report(op->reports, RPT_ERROR, "A mesh map bake is already running for this material");
    return OPERATOR_CANCELLED;
  }

  /* The viewport evaluation the staleness check compares against: hash and mesh copy must come
   * from the same depsgraph, not from a fresh render one. */
  Depsgraph *user_depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  MeshMapBakeJob *job = mesh_map_bake_job_create(
      *bmain, *scene, *view_layer, *ob, *ma, type, user_depsgraph, op->reports);
  if (job == nullptr) {
    return OPERATOR_CANCELLED;
  }

  wmJob *wm_job = WM_jobs_get(CTX_wm_manager(C),
                              CTX_wm_window(C),
                              ma,
                              "Baking mesh maps...",
                              WM_JOB_EXCL_RENDER | WM_JOB_PROGRESS,
                              WM_JOB_TYPE_MESH_MAP_BAKE);
  WM_jobs_customdata_set(wm_job, job, mesh_map_bake_free);
  WM_jobs_timer(wm_job, 0.2, NC_MATERIAL, NC_MATERIAL);
  WM_jobs_callbacks(wm_job, mesh_map_bake_start, nullptr, nullptr, mesh_map_bake_end);
  G.is_break = false;
  G.is_rendering = true;
  WM_jobs_start(CTX_wm_manager(C), wm_job);
  return OPERATOR_FINISHED;
}

/** \} */

}  // namespace

void OBJECT_OT_mesh_map_bake(wmOperatorType *ot)
{
  ot->name = "Bake Mesh Map";
  ot->description =
      "Bake a geometry-derived map of the active object into the atlas of a layered material";
  ot->idname = "OBJECT_OT_mesh_map_bake";

  ot->exec = mesh_map_bake_exec;
  ot->poll = mesh_map_bake_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_enum(ot->srna,
               "type",
               mesh_map_bake_type_items(),
               MA_MESH_MAP_AO,
               "Map Type",
               "Mesh map to bake");
  RNA_def_int(ot->srna,
              "material_index",
              0,
              0,
              32767,
              "Material Slot",
              "Object material slot to bake into, 0 for the active one",
              0,
              32767);
}

}  // namespace blender::ed::object
