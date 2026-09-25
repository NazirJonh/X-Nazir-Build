/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edobj
 *
 * The Mesh Map bake job and its operators.
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
 *
 * A "bake all" run is a sequence of (object, type) pairs. The private render world holds exactly one
 * pair at a time: the main thread builds it, the worker renders it, the main thread commits it and
 * tears it down before the next. This keeps the batch memory flat (an atlas-sized pixel buffer is
 * large), and keeps every user-data access -- the temp scene, the Cycles settings via RNA and the
 * atlas Image -- on the main thread. The pairs are committed one by one, so cancelling a run only
 * rolls back the pairs that were not committed yet.
 */

#include <algorithm>
#include <cmath>
#include <cstring>

#include "BLI_array.hh"
#include "BLI_math_matrix.h"
#include "BLI_math_matrix_types.hh"
#include "BLI_span.hh"
#include "BLI_string.h"
#include "BLI_string_ref.hh"
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
#include "BKE_mesh_maps_bake_handshake.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_object.hh"
#include "BKE_paint_layers.hh"
#include "BKE_report.hh"
#include "BKE_scene.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"
#include "DEG_depsgraph_query.hh"

#include "DNA_ID.h"
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

/** One (object, type) of a run: the private render world plus its bookkeeping. */
struct MeshMapBakeWorld {
  /* The private render world. */
  Main *temp_main = nullptr;
  Scene *temp_scene = nullptr;
  ViewLayer *temp_view_layer = nullptr;
  Object *temp_object = nullptr;
  Depsgraph *temp_depsgraph = nullptr;
  Render *render = nullptr;
  /** The atlas this world bakes into; its pending-bake token is released in the free. */
  Image *atlas = nullptr;

  /* One image (the atlas), the rasterized texels and the float result buffer. */
  BakeImage bake_image = {};
  Image **material_to_image = nullptr;
  BakeTargets targets = {};
  BakePixel *pixels = nullptr;
  float *result = nullptr;
  size_t pixels_num = 0;

  /* The copied mesh the margin is derived from, and its UV layer. */
  Mesh *mesh = nullptr;
  char uv_name[68] = "";
  int8_t type = MA_MESH_MAP_AO;
  int resolution = 0;
  char *margin_mask = nullptr;

  /* The high-poly source copies (objects/meshes owned by `temp_main`) and the cage, plus the
   * raycast data between the low-poly and the sources. */
  Vector<Object *> source_objects;
  Vector<Mesh *> source_meshes;
  Object *cage_object = nullptr;
  Mesh *cage_mesh = nullptr;
  float4x4 low_matrix;
  float4x4 cage_matrix;
  BakeHighPolyData *highpoly = nullptr;
  int highpoly_num = 0;
  BakePixel *pixels_high = nullptr;
};

/** What the pair started from, so a cancel can put the status back. */
struct MeshMapBakePairState {
  Object *object = nullptr;
  uint32_t object_session_uid = 0;
  int8_t type = MA_MESH_MAP_AO;
  int8_t previous_status = OB_MESH_MAP_STATUS_NONE;
  /** Whether #BKE_mesh_maps_bake_state_begin ran for this pair, so a cancel must restore it. */
  bool begun = false;
  uint32_t start_hash[2] = {0, 0};
  bool failed = false;
  char error[192] = "";
};

struct MeshMapBakeJob {
  /* The user's data. */
  Main *bmain = nullptr;
  Scene *scene = nullptr;
  ViewLayer *view_layer = nullptr;
  Material *material = nullptr;
  uint32_t material_session_uid = 0;
  int resolution = 0;
  int margin_pixels = 0;
  ReportList *reports = nullptr;

  Vector<MeshMapBakePairState> pairs;
  /** Index of the pair the world belongs to, or `pairs.size()` when the run is done. */
  int current = 0;
  bool stop_requested = false;
  MeshMapBakeWorld world;
  /** The worker's status, kept so `end` can tell a kill from a normal finish. Set by the worker. */
  wmJobWorkerStatus *worker_status = nullptr;

  /* Main thread <-> worker handshake. The worker waits for a prepared world, then for its result to
   * be committed; the main thread prepares and commits in its update callback. */
  MeshMapBakeHandshake handshake;

  int baked_num = 0;
  int stale_num = 0;
  int error_num = 0;
  int cancelled_num = 0;
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

/** A localizable copy of an evaluated mesh, or null. */
Mesh *mesh_map_bake_copy_mesh(Mesh *mesh_eval)
{
  if (mesh_eval == nullptr) {
    return nullptr;
  }
  return id_cast<Mesh *>(BKE_id_copy_ex(nullptr,
                                        &mesh_eval->id,
                                        nullptr,
                                        LIB_ID_CREATE_LOCAL | LIB_ID_COPY_LOCALIZE |
                                            LIB_ID_COPY_NO_ANIMDATA));
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

/** Build a render depsgraph for \a scene/\a view_layer, updated and ready to evaluate \a ob. */
Depsgraph *mesh_map_bake_depsgraph_new(Main &bmain, Scene &scene, ViewLayer &view_layer)
{
  Depsgraph *depsgraph = DEG_graph_new(&bmain, &scene, &view_layer, DAG_EVAL_RENDER);
  DEG_disable_visibility_optimization(depsgraph);
  DEG_graph_build_from_view_layer(depsgraph);
  BKE_scene_graph_update_tagged(depsgraph, &bmain);
  return depsgraph;
}

const char *mesh_map_bake_type_name(const int8_t type)
{
  switch (type) {
    case MA_MESH_MAP_AO:
      return "AO";
    case MA_MESH_MAP_CURVATURE:
      return "Curvature";
    case MA_MESH_MAP_EDGE:
      return "Edge";
    case MA_MESH_MAP_NORMAL_WORLD:
      return "World Normal";
    case MA_MESH_MAP_NORMAL_OBJECT:
      return "Object Normal";
    case MA_MESH_MAP_ID_OBJECT:
      return "Object ID";
    case MA_MESH_MAP_ID_MATERIAL:
      return "Material ID";
    default:
      return "Map";
  }
}

void mesh_map_bake_world_free(MeshMapBakeWorld &world)
{
  if (world.atlas != nullptr) {
    BKE_paint_layers_bake_image_pending_remove(world.atlas->id.session_uid);
    world.atlas = nullptr;
  }
  if (world.render != nullptr) {
    RE_FreeRender(world.render);
    world.render = nullptr;
  }
  if (world.temp_depsgraph != nullptr) {
    DEG_graph_free(world.temp_depsgraph);
    world.temp_depsgraph = nullptr;
  }
  if (world.temp_main != nullptr) {
    BKE_main_free(world.temp_main);
    world.temp_main = nullptr;
  }
  MEM_delete(world.pixels);
  world.pixels = nullptr;
  MEM_delete(world.result);
  world.result = nullptr;
  MEM_delete(world.material_to_image);
  world.material_to_image = nullptr;
  MEM_delete(world.margin_mask);
  world.margin_mask = nullptr;
  MEM_delete(world.highpoly);
  world.highpoly = nullptr;
  MEM_delete(world.pixels_high);
  world.pixels_high = nullptr;
  world = {};
}

Material *mesh_map_bake_find_material(MeshMapBakeJob &job)
{
  for (Material &candidate : job.bmain->materials) {
    if (candidate.id.session_uid == job.material_session_uid && &candidate == job.material) {
      return &candidate;
    }
  }
  return nullptr;
}

Object *mesh_map_bake_find_object(MeshMapBakeJob &job, const MeshMapBakePairState &pair)
{
  for (Object &candidate : job.bmain->objects) {
    if (candidate.id.session_uid == pair.object_session_uid && &candidate == pair.object) {
      return &candidate;
    }
  }
  return nullptr;
}

/**
 * Build the private render world for \a pair, rasterize its texels and leave the object state
 * #OB_MESH_MAP_STATUS_BAKING (already entered). On failure the state is marked
 * #OB_MESH_MAP_STATUS_ERROR, \a pair records the message and false is returned. Main thread only.
 */
bool mesh_map_bake_prepare_pair(MeshMapBakeJob &job, MeshMapBakePairState &pair)
{
  Material *ma = mesh_map_bake_find_material(job);
  Object *ob = mesh_map_bake_find_object(job, pair);
  if (ma == nullptr || ob == nullptr || ob->type != OB_MESH) {
    BLI_strncpy(pair.error, "The object is gone", sizeof(pair.error));
    pair.failed = true;
    return false;
  }
  if (pair.type < 0 || pair.type >= MA_MESH_MAP_TYPE_NUM) {
    BLI_strncpy(pair.error, "Unsupported mesh map type", sizeof(pair.error));
    pair.failed = true;
    return false;
  }

  /* The viewport evaluation the staleness check compares against: hash and mesh copy must come
   * from the same depsgraph, not from a fresh render one. */
  Depsgraph *user_depsgraph = BKE_scene_ensure_depsgraph(job.bmain, job.scene, job.view_layer);
  if (user_depsgraph == nullptr) {
    BLI_strncpy(pair.error, "The scene has no dependency graph", sizeof(pair.error));
    pair.failed = true;
    return false;
  }
  BKE_scene_graph_evaluated_ensure(user_depsgraph, job.bmain);
  Object *ob_eval = DEG_get_evaluated(user_depsgraph, ob);
  if (ob_eval == nullptr || ob_eval == ob) {
    BLI_strncpy(pair.error, "The object is not in the evaluated scene", sizeof(pair.error));
    pair.failed = true;
    return false;
  }

  /* BAKING is entered lazily, when a pair is actually started, so pairs that never run keep their
   * previous status and the panel only shows "Baking..." for work in flight. */
  ObjectMeshMapState *state = BKE_mesh_maps_object_state_ensure(*ob, *ma, pair.type);
  if (state == nullptr) {
    BLI_strncpy(pair.error, "The object cannot hold a mesh map state", sizeof(pair.error));
    pair.failed = true;
    return false;
  }
  pair.previous_status = BKE_mesh_maps_bake_state_begin(*state);
  pair.begun = true;
  const char *uv_name = BKE_mesh_maps_bake_uv_resolve_or_fail(*state, *ob_eval, *ma);
  if (uv_name == nullptr) {
    BLI_snprintf(pair.error,
                 sizeof(pair.error),
                 "The object has no UV layer '%s'",
                 ma->paint_layers_uv_map[0] != '\0' ? ma->paint_layers_uv_map : "");
    pair.failed = true;
    return false;
  }
  STRNCPY(job.world.uv_name, uv_name);
  job.world.type = pair.type;
  BKE_mesh_maps_object_hash(*ob_eval, *ma, pair.type, pair.start_hash);
  const float4x4 object_matrix = ob_eval->object_to_world();

  /* The high-poly source of this material, if any: its objects and cage are copied into the private
   * world so Cycles can trace them. */
  Vector<Object *> source_objects;
  Object *cage = nullptr;
  if (const Object *ob_orig = DEG_get_original(ob)) {
    BKE_mesh_maps_source_resolve(*ob_orig, *ma, source_objects, &cage);
  }
  float cage_extrusion = 0.0f;
  float max_ray_distance = 0.0f;
  if (const Object *ob_orig = DEG_get_original(ob)) {
    if (const ObjectMeshMapSource *source = BKE_mesh_maps_source_find(*ob_orig, *ma)) {
      cage_extrusion = source->cage_extrusion;
      max_ray_distance = source->max_ray_distance;
    }
  }

  Mesh *mesh_copy = mesh_map_bake_copy_mesh(mesh_map_bake_object_mesh(*ob_eval));
  if (mesh_copy == nullptr) {
    BKE_mesh_maps_bake_state_fail(*state);
    BLI_strncpy(pair.error, "Could not copy the evaluated mesh", sizeof(pair.error));
    pair.failed = true;
    return false;
  }
  job.world.mesh = mesh_copy;
  job.world.low_matrix = object_matrix;

  /* The private render world. */
  job.world.temp_main = BKE_main_new();
  BKE_libblock_management_main_add(job.world.temp_main, mesh_copy);
  job.world.temp_scene = BKE_scene_add(job.world.temp_main, "Mesh Map Bake");
  job.world.temp_view_layer = static_cast<ViewLayer *>(job.world.temp_scene->view_layers.first);
  STRNCPY(job.world.temp_scene->r.engine, RE_engine_id_CYCLES);
  job.world.temp_scene->r.xsch = job.resolution;
  job.world.temp_scene->r.ysch = job.resolution;
  job.world.temp_scene->r.size = 100;
  job.world.temp_scene->r.cfra = 1;
  mesh_map_bake_apply_cycles_settings(*job.world.temp_scene, ma->mesh_map_settings);

  job.world.temp_object = BKE_object_add_for_data(job.world.temp_main,
                                                  job.world.temp_scene,
                                                  job.world.temp_view_layer,
                                                  OB_MESH,
                                                  "Mesh Map Bake",
                                                  &mesh_copy->id,
                                                  true);
  BKE_object_apply_mat4(job.world.temp_object, object_matrix.ptr(), false, false);
  BKE_collection_object_add(
      job.world.temp_main, job.world.temp_scene->master_collection, job.world.temp_object);
  if (!source_objects.is_empty()) {
    /* Selected-to-active hides the low-poly so Cycles bakes the high-poly surface it was cast
     * onto, not the low-poly in front of it (mirrors the full bake). */
    job.world.temp_object->visibility_flag |= OB_HIDE_RENDER;
  }

  Material *temp_material = BKE_material_add(job.world.temp_main, "Mesh Map Bake");
  const auto assign_material = [&](Object *temp_ob) {
    BKE_object_material_slot_add(job.world.temp_main, temp_ob);
    BKE_object_material_assign(
        job.world.temp_main, temp_ob, temp_material, 1, BKE_MAT_ASSIGN_OBJECT);
  };
  assign_material(job.world.temp_object);
  if (BKE_mesh_maps_bake_pass_for_type(pair.type) == SCE_PASS_EMIT) {
    const float ao_distance = BKE_mesh_maps_bake_ao_distance(*mesh_copy, ma->mesh_map_settings);
    BKE_mesh_maps_bake_build_emit_tree(
        *temp_material->nodetree, pair.type, ma->mesh_map_settings, ao_distance);
    BKE_ntree_update_without_main(*temp_material->nodetree);
  }

  /* Copy each source object into the private scene. The throw-away emission material goes on them
   * too, so the EMIT passes trace the high-poly through the same value graph. */
  for (Object *source : source_objects) {
    Object *source_eval = DEG_get_evaluated(user_depsgraph, source);
    if (source_eval == nullptr) {
      source_eval = source;
    }
    Mesh *source_copy = mesh_map_bake_copy_mesh(mesh_map_bake_object_mesh(*source_eval));
    if (source_copy == nullptr) {
      BKE_mesh_maps_bake_state_fail(*state);
      BLI_strncpy(pair.error, "Could not copy a high-poly source mesh", sizeof(pair.error));
      pair.failed = true;
      return false;
    }
    BKE_libblock_management_main_add(job.world.temp_main, source_copy);
    Object *temp_source = BKE_object_add_for_data(job.world.temp_main,
                                                  job.world.temp_scene,
                                                  job.world.temp_view_layer,
                                                  OB_MESH,
                                                  "Mesh Map Source",
                                                  &source_copy->id,
                                                  true);
    BKE_object_apply_mat4(temp_source, source_eval->object_to_world().ptr(), false, false);
    BKE_collection_object_add(
        job.world.temp_main, job.world.temp_scene->master_collection, temp_source);
    assign_material(temp_source);
    job.world.source_objects.append(temp_source);
    job.world.source_meshes.append(source_copy);
  }

  /* Cage: a custom cage must mirror the low-poly face count; an empty one means an auto-cage. */
  if (cage != nullptr) {
    Object *cage_eval = DEG_get_evaluated(user_depsgraph, cage);
    if (cage_eval == nullptr) {
      cage_eval = cage;
    }
    Mesh *cage_eval_mesh = mesh_map_bake_object_mesh(*cage_eval);
    if (cage_eval_mesh == nullptr || !BKE_mesh_maps_bake_cage_matches(*mesh_copy, *cage_eval_mesh)) {
      BKE_mesh_maps_bake_state_fail(*state);
      BLI_strncpy(pair.error,
                  "The cage mesh must have the same number of faces as the low-poly object",
                  sizeof(pair.error));
      pair.failed = true;
      return false;
    }
    Mesh *cage_copy = mesh_map_bake_copy_mesh(cage_eval_mesh);
    if (cage_copy == nullptr) {
      BKE_mesh_maps_bake_state_fail(*state);
      BLI_strncpy(pair.error, "Could not copy the cage mesh", sizeof(pair.error));
      pair.failed = true;
      return false;
    }
    BKE_libblock_management_main_add(job.world.temp_main, cage_copy);
    Object *temp_cage = BKE_object_add_for_data(job.world.temp_main,
                                                job.world.temp_scene,
                                                job.world.temp_view_layer,
                                                OB_MESH,
                                                "Mesh Map Cage",
                                                &cage_copy->id,
                                                true);
    BKE_object_apply_mat4(temp_cage, cage_eval->object_to_world().ptr(), false, false);
    BKE_collection_object_add(
        job.world.temp_main, job.world.temp_scene->master_collection, temp_cage);
    /* The cage is only the ray origin; it must not render. */
    temp_cage->visibility_flag |= OB_HIDE_RENDER;
    job.world.cage_object = temp_cage;
    job.world.cage_mesh = cage_copy;
    job.world.cage_matrix = cage_eval->object_to_world();
  }

  job.world.temp_depsgraph = mesh_map_bake_depsgraph_new(
      *job.world.temp_main, *job.world.temp_scene, *job.world.temp_view_layer);
  job.world.render = RE_NewSceneRender(job.world.temp_scene);
  RE_bake_engine_set_engine_parameters(job.world.render, job.world.temp_main, job.world.temp_scene);
  if (!RE_bake_has_engine(job.world.render)) {
    BKE_mesh_maps_bake_state_fail(*state);
    BLI_strncpy(pair.error, "The Cycles bake engine is not available", sizeof(pair.error));
    pair.failed = true;
    return false;
  }

  Image *atlas = BKE_mesh_maps_bake_atlas_ensure(*job.bmain, *ma, pair.type, job.resolution);
  if (atlas == nullptr) {
    BKE_mesh_maps_bake_state_fail(*state);
    BLI_strncpy(pair.error, "Could not create the mesh map atlas image", sizeof(pair.error));
    pair.failed = true;
    return false;
  }
  job.world.atlas = atlas;

  const size_t pixels_num = size_t(job.resolution) * size_t(job.resolution);
  job.world.pixels_num = pixels_num;
  job.world.resolution = job.resolution;
  job.world.material_to_image = MEM_new_array_uninitialized<Image *>(1, __func__);
  job.world.material_to_image[0] = atlas;
  job.world.bake_image.image = atlas;
  job.world.bake_image.tile_number = 0;
  job.world.bake_image.uv_offset[0] = 0.0f;
  job.world.bake_image.uv_offset[1] = 0.0f;
  job.world.bake_image.width = job.resolution;
  job.world.bake_image.height = job.resolution;
  job.world.bake_image.offset = 0;
  job.world.targets.images = &job.world.bake_image;
  job.world.targets.images_num = 1;
  job.world.targets.material_to_image = job.world.material_to_image;
  job.world.targets.materials_num = 1;
  job.world.targets.pixels_num = int(pixels_num);
  job.world.targets.channels_num = 4;
  job.world.targets.is_noncolor = true;
  job.world.targets.result = MEM_new_array_zeroed<float>(pixels_num * 4, __func__);
  job.world.pixels = MEM_new_array_uninitialized<BakePixel>(pixels_num, __func__);
  job.world.result = job.world.targets.result;

  RE_bake_pixels_populate(
      mesh_copy, job.world.pixels, pixels_num, &job.world.targets, StringRef(job.world.uv_name));
  /* Warm the lazy triangulation caches on the main thread: the worker's rasterize there only reads
   * them, and computing them off-thread would race the main thread's own mesh access. */
  (void)mesh_copy->corner_tris();
  (void)mesh_copy->corner_tri_faces();
  BKE_mesh_maps_bake_restrict_to_material(job.world.pixels, pixels_num, *mesh_copy, *ob, *ma);

  job.world.highpoly_num = int(job.world.source_objects.size());
  if (job.world.highpoly_num > 0) {
    job.world.highpoly = MEM_new_array_zeroed<BakeHighPolyData>(job.world.highpoly_num, __func__);
    for (int i = 0; i < job.world.highpoly_num; i++) {
      Object *temp_source = job.world.source_objects[i];
      Object *temp_source_eval = DEG_get_evaluated(job.world.temp_depsgraph, temp_source);
      if (temp_source_eval == nullptr) {
        temp_source_eval = temp_source;
      }
      job.world.highpoly[i].ob = temp_source;
      job.world.highpoly[i].ob_eval = temp_source_eval;
      job.world.highpoly[i].mesh = mesh_map_bake_object_mesh(*temp_source_eval);
      copy_m4_m4(job.world.highpoly[i].obmat, temp_source_eval->object_to_world().ptr());
      invert_m4_m4(job.world.highpoly[i].imat, job.world.highpoly[i].obmat);
      job.world.highpoly[i].is_flip_object = is_negative_m4(
          temp_source_eval->object_to_world().ptr());
    }
    job.world.pixels_high = MEM_new_array_uninitialized<BakePixel>(pixels_num, __func__);
    const bool is_custom_cage = job.world.cage_object != nullptr;
    const float(*mat_low)[4] = job.world.low_matrix.ptr();
    const float(*mat_cage)[4] = is_custom_cage ? job.world.cage_matrix.ptr() : mat_low;
    if (!RE_bake_pixels_populate_from_objects(mesh_copy,
                                              job.world.pixels,
                                              job.world.pixels_high,
                                              job.world.highpoly,
                                              job.world.highpoly_num,
                                              pixels_num,
                                              is_custom_cage,
                                              cage_extrusion,
                                              max_ray_distance,
                                              mat_low,
                                              mat_cage,
                                              job.world.cage_mesh))
    {
      BKE_mesh_maps_bake_state_fail(*state);
      BLI_strncpy(pair.error,
                  "Could not cast the low-poly onto the high-poly source",
                  sizeof(pair.error));
      pair.failed = true;
      return false;
    }
    if (BKE_mesh_maps_bake_type_cpu_id(pair.type)) {
      Vector<const Object *> id_objects;
      Vector<const Mesh *> id_meshes;
      id_objects.reserve(job.world.highpoly_num);
      id_meshes.reserve(job.world.highpoly_num);
      for (int i = 0; i < job.world.highpoly_num; i++) {
        id_objects.append(job.world.highpoly[i].ob);
        id_meshes.append(job.world.highpoly[i].mesh);
      }
      BKE_mesh_maps_bake_fill_id_from_high_poly(job.world.pixels_high,
                                                pixels_num,
                                                id_objects.data(),
                                                size_t(job.world.highpoly_num),
                                                id_meshes.data(),
                                                size_t(job.world.highpoly_num),
                                                pair.type,
                                                4,
                                                job.world.result);
    }
  }
  else if (BKE_mesh_maps_bake_type_cpu_id(pair.type)) {
    BKE_mesh_maps_bake_fill_id_pixels(
        job.world.pixels, pixels_num, *mesh_copy, *ob, pair.type, 4, job.world.result);
  }

  BKE_paint_layers_bake_image_pending_add(atlas->id.session_uid);
  return true;
}

/**
 * Expand this object's result into its margin inside the private buffer, and record the pixel
 * membership in `world.margin_mask`: #FILTER_MASK_USED for a covered texel, #FILTER_MASK_MARGIN for
 * a filled one, zero elsewhere.
 *
 * The adjacent-faces margin of the existing bake is mirrored here, not the isotropic extend: it
 * follows the UV islands and so is the right default for a shared atlas. It does not report the
 * filled texels back through the caller's mask (it copies the mask internally), so a second pass
 * over a one-valued coverage indicator records them; the geometry and coverage are identical, so
 * the membership is exact.
 */
void mesh_map_bake_apply_margin(MeshMapBakeWorld &world, const int margin)
{
  if (margin <= 0 || world.mesh == nullptr || world.result == nullptr ||
      world.margin_mask != nullptr)
  {
    return;
  }
  const size_t pixels_num = world.pixels_num;
  const int resolution = world.resolution;
  const float uv_offset[2] = {0.0f, 0.0f};
  const StringRef uv_name(world.uv_name);

  char *mask = MEM_new_array_zeroed<char>(pixels_num, __func__);
  RE_bake_mask_fill(world.pixels, pixels_num, mask);

  ImBuf *ibuf = IMB_allocImBuf(resolution, resolution, ImBufFlags::FloatData);
  if (ibuf == nullptr) {
    MEM_delete(mask);
    return;
  }
  float *pixels = ibuf->float_data_for_write();
  memcpy(pixels, world.result, sizeof(float) * pixels_num * 4);
  RE_bake_margin(ibuf, mask, margin, R_BAKE_ADJACENT_FACES, world.mesh, uv_name, uv_offset);
  memcpy(world.result, pixels, sizeof(float) * pixels_num * 4);
  IMB_freeImBuf(ibuf);

  ImBuf *indicator = IMB_allocImBuf(resolution, resolution, ImBufFlags::FloatData);
  if (indicator != nullptr) {
    float *values = indicator->float_data_for_write();
    for (size_t i = 0; i < pixels_num; i++) {
      const float value = mask[i] == FILTER_MASK_USED ? 1.0f : 0.0f;
      values[i * 4 + 0] = value;
      values[i * 4 + 1] = value;
      values[i * 4 + 2] = value;
      values[i * 4 + 3] = value;
    }
    char *indicator_mask = MEM_dupalloc(mask);
    RE_bake_margin(indicator,
                   indicator_mask,
                   margin,
                   R_BAKE_ADJACENT_FACES,
                   world.mesh,
                   uv_name,
                   uv_offset);
    for (size_t i = 0; i < pixels_num; i++) {
      if (mask[i] != FILTER_MASK_USED && values[i * 4] > 0.5f) {
        mask[i] = FILTER_MASK_MARGIN;
      }
    }
    MEM_delete(indicator_mask);
    IMB_freeImBuf(indicator);
  }
  world.margin_mask = mask;
}

/** Render and margin-fill the prepared world. Worker thread only. */
void mesh_map_bake_render_world(MeshMapBakeJob &job, MeshMapBakeWorld &world)
{
  if (world.render == nullptr) {
    return;
  }
  if (BKE_mesh_maps_bake_type_renders(world.type)) {
    const eScenePassType pass_type = BKE_mesh_maps_bake_pass_for_type(world.type);
    bool ok = true;
    if (world.highpoly_num > 0) {
      /* Selected-to-active: each source object bakes the texels that were cast onto it, through
       * `pixels_high` whose `object_id` selects the object. */
      for (int i = 0; i < world.highpoly_num && ok; i++) {
        ok = RE_bake_engine(world.render,
                            world.temp_depsgraph,
                            world.highpoly[i].ob_eval,
                            i,
                            world.pixels_high,
                            &world.targets,
                            pass_type,
                            0,
                            world.result);
      }
    }
    else {
      ok = RE_bake_engine(world.render,
                          world.temp_depsgraph,
                          world.temp_object,
                          0,
                          world.pixels,
                          &world.targets,
                          pass_type,
                          0,
                          world.result);
    }
    if (!ok) {
      return;
    }
    if (pass_type == SCE_PASS_NORMAL) {
      /* Cycles always delivers world-space normals; rotate and compress them for the atlas. */
      const eBakeNormalSwizzle swizzle[3] = {R_BAKE_POSX, R_BAKE_POSY, R_BAKE_POSZ};
      eBakeSpace space = R_BAKE_SPACE_WORLD;
      BKE_mesh_maps_bake_normal_space_for_type(world.type, space);
      if (space == R_BAKE_SPACE_OBJECT) {
        RE_bake_normal_world_to_object(
            world.pixels, world.pixels_num, 4, world.result, world.temp_object, swizzle);
      }
      else {
        RE_bake_normal_world_to_world(world.pixels, world.pixels_num, 4, world.result, swizzle);
      }
    }
  }
  mesh_map_bake_apply_margin(world, job.margin_pixels);
}

/** Write the prepared world's pixels into the live atlas and record the state. Main thread only. */
void mesh_map_bake_commit_current(MeshMapBakeJob &job, MeshMapBakePairState &pair)
{
  MeshMapBakeWorld &world = job.world;
  Material *ma = mesh_map_bake_find_material(job);
  Object *ob = mesh_map_bake_find_object(job, pair);
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
  ObjectMeshMapState *state = BKE_mesh_maps_object_state_find(*ob, *ma, pair.type);
  if (state == nullptr) {
    return;
  }

  /* Recompute the content hash from the same viewport evaluation the staleness check uses, so a
   * successful bake stays VALID instead of turning STALE immediately. */
  uint32_t now_hash[2] = {0, 0};
  if (!BKE_mesh_maps_bake_hash_from_viewport(
          *job.bmain, *job.scene, *job.view_layer, *ob, *ma, pair.type, now_hash) ||
      !BKE_mesh_maps_bake_commit_is_current(pair.start_hash, now_hash))
  {
    BKE_mesh_maps_bake_state_commit(*state, pair.start_hash, now_hash, 0);
    job.stale_num++;
    return;
  }

  Image *atlas = BKE_mesh_maps_bake_atlas_ensure(*job.bmain, *ma, pair.type, job.resolution);
  if (atlas == nullptr) {
    BKE_mesh_maps_bake_state_fail(*state);
    job.error_num++;
    return;
  }
  /* The pixels are float; a byte atlas would swallow them, so a non-float image is a failed bake,
   * not a silent VALID. */
  if (!BKE_mesh_maps_bake_atlas_is_float(*atlas)) {
    BKE_mesh_maps_bake_state_fail(*state);
    WM_main_add_notifier(NC_MATERIAL | ND_SHADING, ma);
    job.error_num++;
    return;
  }
  /* The atlas is shared: the object's own pixels always land, but its margin must not paint over
   * an island another object using the material already baked. */
  Array<uint8_t> own_coverage(world.pixels_num, 0);
  BKE_mesh_maps_bake_coverage_from_pixels(world.pixels, world.pixels_num, own_coverage);
  Array<uint8_t> own_margin(world.pixels_num, 0);
  if (world.margin_mask != nullptr) {
    for (size_t i = 0; i < world.pixels_num; i++) {
      own_margin[i] = world.margin_mask[i] == FILTER_MASK_MARGIN ? 1 : 0;
    }
  }
  Array<uint8_t> foreign_coverage(world.pixels_num, 0);
  char overlap_name[MAX_ID_NAME] = "";
  BKE_mesh_maps_bake_foreign_coverage(*job.bmain,
                                      *job.scene,
                                      *job.view_layer,
                                      *ob,
                                      *ma,
                                      job.resolution,
                                      own_coverage,
                                      foreign_coverage,
                                      MutableSpan<char>(overlap_name, sizeof(overlap_name)));
  if (overlap_name[0] != '\0' && job.reports != nullptr) {
    BKE_reportf(job.reports,
                RPT_WARNING,
                "Mesh map islands of '%s' overlap '%s' in UV '%s'",
                ob->id.name + 2,
                overlap_name,
                world.uv_name);
  }

  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(atlas, nullptr, &lock);
  if (ibuf != nullptr && ibuf->float_data() != nullptr) {
    BKE_mesh_maps_bake_write_with_margin(
        pair.type,
        own_coverage,
        own_margin,
        foreign_coverage,
        4,
        world.result,
        MutableSpan<float>(ibuf->float_data_for_write(), world.pixels_num * 4));
    ibuf->userflags |= IB_DISPLAY_BUFFER_INVALID;
    BKE_image_mark_dirty(atlas, ibuf);
  }
  BKE_image_release_ibuf(atlas, ibuf, lock);
  BKE_image_partial_update_mark_full_update(atlas);
  BKE_image_free_gputextures(atlas);
  BKE_image_memorypack(atlas);

  BKE_mesh_maps_bake_state_commit(
      *state, pair.start_hash, now_hash, int(BLI_time_now_seconds_i()));
  job.baked_num++;
  WM_main_add_notifier(NC_IMAGE | NA_EDITED, atlas);
  WM_main_add_notifier(NC_MATERIAL | ND_SHADING, ma);
}

/**
 * Prepare the next renderable world on the main thread, skipping pairs that failed. Returns false
 * when the run is exhausted. Leaves `current` on the prepared pair, or at `pairs.size()`.
 */
bool mesh_map_bake_prepare_next(MeshMapBakeJob &job)
{
  while (job.current < job.pairs.size()) {
    MeshMapBakePairState &pair = job.pairs[job.current];
    if (!pair.failed && mesh_map_bake_prepare_pair(job, pair)) {
      return true;
    }
    job.current++;
  }
  return false;
}

void mesh_map_bake_start(void *customdata, wmJobWorkerStatus *worker_status)
{
  MeshMapBakeJob *job = static_cast<MeshMapBakeJob *>(customdata);
  job->reports = worker_status->reports;
  job->worker_status = worker_status;
  const auto stop_requested = [worker_status] { return worker_status->stop; };
  for (;;) {
    if (!job->handshake.worker_await_world(stop_requested)) {
      break;
    }
    mesh_map_bake_render_world(*job, job->world);
    if (worker_status->stop) {
      job->stop_requested = true;
      break;
    }
    worker_status->progress = float(std::min(job->current + 1, int(job->pairs.size()))) /
                              float(std::max(int(job->pairs.size()), 1));
    worker_status->do_update = true;
    if (!job->handshake.worker_publish_result(stop_requested)) {
      break;
    }
  }
  if (worker_status->stop) {
    job->stop_requested = true;
  }
  worker_status->do_update = true;
}

void mesh_map_bake_update(void *customdata)
{
  MeshMapBakeJob *job = static_cast<MeshMapBakeJob *>(customdata);
  if (!job->handshake.main_try_take_result()) {
    return;
  }
  bool prepared = false;
  if (!job->stop_requested && job->current < job->pairs.size()) {
    mesh_map_bake_commit_current(*job, job->pairs[job->current]);
    mesh_map_bake_world_free(job->world);
    job->current++;
    prepared = mesh_map_bake_prepare_next(*job);
  }
  else {
    mesh_map_bake_world_free(job->world);
  }
  /* Publish the next world first, then release the worker from the commit wait, so it never
   * observes a released commit with no world published yet. */
  job->handshake.main_publish_world(prepared);
  job->handshake.main_finish_commit();
}

/**
 * Roll back the pairs that were begun but never committed, and report the run's outcome. Shared by
 * the worker `end` callback and the synchronous path.
 */
void mesh_map_bake_finalize(MeshMapBakeJob &job, const bool cancelled)
{
  for (int i = job.current; i < job.pairs.size(); i++) {
    MeshMapBakePairState &pair = job.pairs[i];
    if (pair.failed || !pair.begun) {
      continue;
    }
    Material *ma = mesh_map_bake_find_material(job);
    Object *ob = mesh_map_bake_find_object(job, pair);
    if (ma == nullptr || ob == nullptr) {
      continue;
    }
    if (ObjectMeshMapState *state = BKE_mesh_maps_object_state_find(*ob, *ma, pair.type)) {
      BKE_mesh_maps_bake_state_cancel(*state, pair.previous_status);
    }
    job.cancelled_num++;
  }

  job.error_num = 0;
  for (const MeshMapBakePairState &pair : job.pairs) {
    if (pair.failed) {
      job.error_num++;
    }
  }

  if (job.reports == nullptr) {
    return;
  }
  for (const MeshMapBakePairState &pair : job.pairs) {
    if (!pair.failed) {
      continue;
    }
    BKE_reportf(job.reports,
                RPT_ERROR,
                "Mesh map bake of '%s' (%s) failed: %s",
                pair.object != nullptr ? pair.object->id.name + 2 : "?",
                mesh_map_bake_type_name(pair.type),
                pair.error);
  }
  if (cancelled) {
    BKE_reportf(job.reports,
                RPT_WARNING,
                "Mesh map bake cancelled: %d baked, %d rolled back",
                job.baked_num,
                job.cancelled_num);
  }
  else {
    BKE_reportf(job.reports,
                RPT_INFO,
                "Mesh map bake finished: %d baked, %d stale, %d errors, %d cancelled",
                job.baked_num,
                job.stale_num,
                job.error_num,
                job.cancelled_num);
  }
}

void mesh_map_bake_end(void *customdata)
{
  MeshMapBakeJob *job = static_cast<MeshMapBakeJob *>(customdata);
  /* Unblock a worker that is still waiting for a handshake. */
  job->handshake.main_request_exit();
  /* `kill` sets `worker_status.stop` and joins the worker without ever running `update`, so the
   * stop flag, not `job->stop_requested`, is what `end` must trust here. */
  const bool cancelled = job->stop_requested ||
                         (job->worker_status != nullptr && job->worker_status->stop);
  /* A result `update` did not get to; only committed when the run was not cancelled. */
  if (!cancelled && job->handshake.main_try_take_result() && job->current < job->pairs.size()) {
    mesh_map_bake_commit_current(*job, job->pairs[job->current]);
    job->current++;
  }
  mesh_map_bake_world_free(job->world);
  mesh_map_bake_finalize(*job, cancelled);
}

void mesh_map_bake_free(void *customdata)
{
  MeshMapBakeJob *job = static_cast<MeshMapBakeJob *>(customdata);
  mesh_map_bake_world_free(job->world);
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

/** The same maps as #mesh_map_bake_type_items, as an enum-flag bitmask. */
const EnumPropertyItem *mesh_map_bake_type_flag_items()
{
  static const EnumPropertyItem items[] = {
      {1 << MA_MESH_MAP_AO, "AO", 0, "Ambient Occlusion", "Per-object ambient occlusion"},
      {1 << MA_MESH_MAP_CURVATURE, "CURVATURE", 0, "Curvature", "Concave/convex curvature"},
      {1 << MA_MESH_MAP_EDGE, "EDGE", 0, "Edge", "Bevel edge mask"},
      {1 << MA_MESH_MAP_NORMAL_WORLD, "NORMAL_WORLD", 0, "World Normal", "World-space normal"},
      {1 << MA_MESH_MAP_NORMAL_OBJECT, "NORMAL_OBJECT", 0, "Object Normal", "Object-space normal"},
      {1 << MA_MESH_MAP_ID_OBJECT, "ID_OBJECT", 0, "Object ID", "Stable object color"},
      {1 << MA_MESH_MAP_ID_MATERIAL, "ID_MATERIAL", 0, "Material ID", "Per-material-slot color"},
      {0, nullptr, 0, nullptr, nullptr},
  };
  return items;
}

const EnumPropertyItem *mesh_map_bake_object_scope_items()
{
  static const EnumPropertyItem items[] = {
      {MA_MESH_MAP_BAKE_ACTIVE_OBJECT, "ACTIVE", 0, "Active Object", "Bake the active object only"},
      {MA_MESH_MAP_BAKE_ALL_OBJECTS,
       "ALL",
       0,
       "All Objects",
       "Bake every object that uses this material in a slot"},
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

/** Build the plan and pair list. Main thread; returns null and reports on failure. */
MeshMapBakeJob *mesh_map_bake_job_create(Main &bmain,
                                         Scene &scene,
                                         ViewLayer &view_layer,
                                         Object *active_object,
                                         Material &ma,
                                         const int8_t object_scope,
                                         const uint32_t type_mask,
                                         ReportList *reports)
{
  Vector<MeshMapBakePair> plan;
  BKE_mesh_maps_bake_plan_pairs(bmain, active_object, ma, object_scope, type_mask, plan);
  if (plan.is_empty()) {
    BKE_report(reports, RPT_ERROR, "No mesh map bake pairs to process");
    return nullptr;
  }
  MeshMapBakeJob *job = MEM_new<MeshMapBakeJob>(__func__);
  job->bmain = &bmain;
  job->scene = &scene;
  job->view_layer = &view_layer;
  job->material = &ma;
  job->material_session_uid = ma.id.session_uid;
  job->resolution = std::max(ma.mesh_map_settings.resolution, 1);
  job->margin_pixels = std::max(int(std::lround(ma.mesh_map_settings.margin)), 0);
  job->reports = reports;
  job->pairs.reserve(plan.size());

  for (const MeshMapBakePair &p : plan) {
    MeshMapBakePairState pair;
    pair.object = p.object;
    pair.object_session_uid = p.object->id.session_uid;
    pair.type = p.type;
    job->pairs.append(pair);
  }
  return job;
}

/**
 * Run the whole plan on the calling thread: prepare, render and commit each pair in turn. This is
 * the `exec` path, so `bpy.ops.…()` from a script -- and a background run -- bakes synchronously
 * without needing the event loop a wmJob relies on.
 */
wmOperatorStatus mesh_map_bake_run_sync(MeshMapBakeJob *job)
{
  if (job == nullptr) {
    return OPERATOR_CANCELLED;
  }
  const bool was_rendering = G.is_rendering;
  G.is_break = false;
  G.is_rendering = true;
  for (;;) {
    if (!mesh_map_bake_prepare_next(*job)) {
      break;
    }
    mesh_map_bake_render_world(*job, job->world);
    if (G.is_break) {
      job->stop_requested = true;
      mesh_map_bake_world_free(job->world);
      break;
    }
    mesh_map_bake_commit_current(*job, job->pairs[job->current]);
    mesh_map_bake_world_free(job->world);
    job->current++;
  }
  mesh_map_bake_world_free(job->world);
  G.is_rendering = was_rendering;
  G.is_break = false;
  mesh_map_bake_finalize(*job, job->stop_requested);
  MEM_delete(job);
  return OPERATOR_FINISHED;
}

/** Prepare the first world on the main thread and launch the worker. */
wmOperatorStatus mesh_map_bake_job_start(bContext *C, MeshMapBakeJob *job)
{
  if (job == nullptr) {
    return OPERATOR_CANCELLED;
  }
  const bool prepared = mesh_map_bake_prepare_next(*job);
  job->handshake.main_publish_world(prepared);

  Material *ma = mesh_map_bake_find_material(*job);
  if (ma == nullptr) {
    mesh_map_bake_world_free(job->world);
    MEM_delete(job);
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
  WM_jobs_callbacks(wm_job, mesh_map_bake_start, nullptr, mesh_map_bake_update, mesh_map_bake_end);
  G.is_break = false;
  G.is_rendering = true;
  WM_jobs_start(CTX_wm_manager(C), wm_job);
  return OPERATOR_FINISHED;
}

/** Validate the context and build the job for either bake operator. */
MeshMapBakeJob *mesh_map_bake_build_job(bContext *C,
                                        wmOperator *op,
                                        const int8_t object_scope,
                                        uint32_t type_mask)
{
  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr || ob->type != OB_MESH) {
    BKE_report(op->reports, RPT_ERROR, "No active mesh object");
    return nullptr;
  }
  const int slot = [&] {
    const int value = RNA_int_get(op->ptr, "material_index");
    return value <= 0 ? int(ob->actcol) : value;
  }();
  Material *ma = BKE_object_material_get(ob, short(slot));
  if (ma == nullptr || !paint_layers_is_layered(*ma)) {
    BKE_report(op->reports, RPT_ERROR, "The material slot is not a layered paint material");
    return nullptr;
  }
  if (WM_jobs_test(CTX_wm_manager(C), ma, WM_JOB_TYPE_MESH_MAP_BAKE)) {
    BKE_report(op->reports, RPT_ERROR, "A mesh map bake is already running for this material");
    return nullptr;
  }
  if (type_mask == 0) {
    type_mask = MESH_MAP_BAKE_TYPE_MASK_SUPPORTED;
  }
  return mesh_map_bake_job_create(*CTX_data_main(C),
                                  *CTX_data_scene(C),
                                  *CTX_data_view_layer(C),
                                  ob,
                                  *ma,
                                  object_scope,
                                  type_mask,
                                  op->reports);
}

wmOperatorStatus mesh_map_bake_exec(bContext *C, wmOperator *op)
{
  const uint32_t type_mask = 1u << uint(RNA_enum_get(op->ptr, "type"));
  return mesh_map_bake_run_sync(
      mesh_map_bake_build_job(C, op, MA_MESH_MAP_BAKE_ACTIVE_OBJECT, type_mask));
}

wmOperatorStatus mesh_map_bake_invoke(bContext *C, wmOperator *op, const wmEvent * /*event*/)
{
  const uint32_t type_mask = 1u << uint(RNA_enum_get(op->ptr, "type"));
  return mesh_map_bake_job_start(
      C, mesh_map_bake_build_job(C, op, MA_MESH_MAP_BAKE_ACTIVE_OBJECT, type_mask));
}

wmOperatorStatus mesh_map_bake_all_exec(bContext *C, wmOperator *op)
{
  const int8_t scope = RNA_enum_get(op->ptr, "object_scope");
  return mesh_map_bake_run_sync(
      mesh_map_bake_build_job(C, op, scope, uint32_t(RNA_enum_get(op->ptr, "types"))));
}

wmOperatorStatus mesh_map_bake_all_invoke(bContext *C, wmOperator *op, const wmEvent * /*event*/)
{
  const int8_t scope = RNA_enum_get(op->ptr, "object_scope");
  return mesh_map_bake_job_start(
      C, mesh_map_bake_build_job(C, op, scope, uint32_t(RNA_enum_get(op->ptr, "types"))));
}

wmOperatorStatus mesh_map_clear_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr || ob->type != OB_MESH) {
    BKE_report(op->reports, RPT_ERROR, "No active mesh object");
    return OPERATOR_CANCELLED;
  }
  const int slot = [&] {
    const int value = RNA_int_get(op->ptr, "material_index");
    return value <= 0 ? int(ob->actcol) : value;
  }();
  Material *ma = BKE_object_material_get(ob, short(slot));
  if (ma == nullptr || !paint_layers_is_layered(*ma)) {
    BKE_report(op->reports, RPT_ERROR, "The material slot is not a layered paint material");
    return OPERATOR_CANCELLED;
  }
  if (WM_jobs_test(CTX_wm_manager(C), ma, WM_JOB_TYPE_MESH_MAP_BAKE)) {
    BKE_report(op->reports, RPT_ERROR, "A mesh map bake is already running for this material");
    return OPERATOR_CANCELLED;
  }
  uint32_t type_mask = uint32_t(RNA_enum_get(op->ptr, "types"));
  if (type_mask == 0) {
    type_mask = MESH_MAP_BAKE_TYPE_MASK_SUPPORTED;
  }
  Main *bmain = CTX_data_main(C);
  int cleared = 0;
  for (int type = 0; type < MA_MESH_MAP_TYPE_NUM; type++) {
    if ((type_mask & (1u << uint(type))) == 0 ||
        !BKE_mesh_maps_bake_type_is_supported(int8_t(type)))
    {
      continue;
    }
    cleared += BKE_mesh_maps_bake_clear_type(*bmain, *ma, int8_t(type));
  }
  WM_main_add_notifier(NC_MATERIAL | ND_SHADING, ma);
  BKE_reportf(op->reports, RPT_INFO, "Cleared %d mesh map state(s)", cleared);
  return OPERATOR_FINISHED;
}

/** \} */

}  // namespace

void OBJECT_OT_mesh_map_bake(wmOperatorType *ot)
{
  ot->name = "Bake Mesh Map";
  ot->description =
      "Bake a geometry-derived map of the active object into the atlas of a layered material. Run "
      "synchronously from scripts; pass 'INVOKE_DEFAULT' for a background job";
  ot->idname = "OBJECT_OT_mesh_map_bake";

  ot->exec = mesh_map_bake_exec;
  ot->invoke = mesh_map_bake_invoke;
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

void OBJECT_OT_mesh_map_bake_all(wmOperatorType *ot)
{
  ot->name = "Bake All Mesh Maps";
  ot->description =
      "Bake a set of geometry maps into the atlas of a layered material, for one or all objects. "
      "Run synchronously from scripts; pass 'INVOKE_DEFAULT' for a background job";
  ot->idname = "OBJECT_OT_mesh_map_bake_all";

  ot->exec = mesh_map_bake_all_exec;
  ot->invoke = mesh_map_bake_all_invoke;
  ot->poll = mesh_map_bake_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_enum_flag(ot->srna,
                    "types",
                    mesh_map_bake_type_flag_items(),
                    0,
                    "Map Types",
                    "Maps to bake; empty means every supported map");
  RNA_def_enum(ot->srna,
               "object_scope",
               mesh_map_bake_object_scope_items(),
               MA_MESH_MAP_BAKE_ACTIVE_OBJECT,
               "Objects",
               "Which objects to bake");
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

void OBJECT_OT_mesh_map_clear(wmOperatorType *ot)
{
  ot->name = "Clear Mesh Maps";
  ot->description = "Clear the atlas of a set of mesh maps and reset their bake statuses";
  ot->idname = "OBJECT_OT_mesh_map_clear";

  ot->exec = mesh_map_clear_exec;
  ot->poll = mesh_map_bake_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_enum_flag(ot->srna,
                    "types",
                    mesh_map_bake_type_flag_items(),
                    0,
                    "Map Types",
                    "Maps to clear; empty means every supported map");
  RNA_def_int(ot->srna,
              "material_index",
              0,
              0,
              32767,
              "Material Slot",
              "Object material slot to clear, 0 for the active one",
              0,
              32767);
}

}  // namespace blender::ed::object
