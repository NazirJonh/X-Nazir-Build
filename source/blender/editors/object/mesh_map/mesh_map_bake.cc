/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edobj
 *
 * The Mesh Map bake job and its operators.
 *
 * A bake turns one #eMaterialMeshMapType of one object into the atlas Image the material's slot
 * owns. Nothing of the user's scene is touched: the job works on local copies of the evaluated
 * meshes, fills the CPU ID maps into a private float buffer, and -- for the ray-traced maps --
 * builds a throw-away #Main with a temporary scene, an emission-graph material and copies of the
 * high-poly sources, drives Cycles on that private world, then commits the covered texels into the
 * atlas on the main thread. The user's own render engine and Cycles settings are never read or
 * written; only the private scene's are pointed at.
 *
 * The job is keyed on the material (the atlas is a material resource, and the status-bar template
 * already enumerates materials), runs Cycles once (#WM_JOB_EXCL_RENDER), and follows the paint-layer
 * bake split the rest of the codebase uses: the main thread prepares the input and commits the
 * result, the worker only renders into private buffers, and the state is moved exclusively through
 * `BKE_mesh_maps_bake_state_*`.
 *
 * A "bake all" run is a sequence of (object, type) pairs. The private world holds exactly one pair
 * at a time: the main thread builds it, the worker runs its part, the main thread commits it and
 * tears it down before the next. This keeps the batch memory flat (an atlas-sized pixel buffer is
 * large), and keeps every user-data access -- the depsgraph, the mesh copies and the atlas Image --
 * on the main thread. The pairs are committed one by one, so cancelling a run only rolls back the
 * pairs that were not committed yet.
 */

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#include "BLI_array.hh"
#include "BLI_index_range.hh"
#include "BLI_listbase_iterator.hh"
#include "BLI_math_matrix.h"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_span.hh"
#include "BLI_string.h"
#include "BLI_string_ref.hh"
#include "BLI_time.h"
#include "BLI_utildefines.h"

#include "MEM_guardedalloc.h"

#include "BKE_context.hh"
#include "BKE_collection.hh"
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
#include "BKE_image_tile_buffer.hh"
#include "BKE_object.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_tile.hh"
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
#include "RNA_enum_types.hh"
#include "RNA_prototypes.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "../object_intern.hh"

namespace blender::ed::object {
namespace {

/**
 * The texel pass of #BKE_mesh_maps_bake_foreign_coverage: pure geometry, so the bake image is null.
 * #RE_bake_pixels_populate always indexes `material_to_image`, even with no materials; a single
 * null entry that matches the null bake image keeps the pass about geometry only. The pass is one
 * UDIM tile's: \a tile_number is the UDIM tile number it shifts the UVs into.
 */
void mesh_map_bake_populate_geometry_pixels(Mesh &mesh,
                                            MutableSpan<BakePixel> pixels,
                                            const int resolution,
                                            const StringRef uv_name,
                                            const int tile_number)
{
  Image *material_to_image[1] = {nullptr};
  BakeImage bake_image = {};
  bake_image.image = nullptr;
  bake_image.tile_number = tile_number;
  bake_image.width = resolution;
  bake_image.height = resolution;
  bake_image.offset = 0;
  const blender::float2 tile_origin = BKE_image_get_tile_uv_origin(tile_number);
  bake_image.uv_offset[0] = tile_origin.x;
  bake_image.uv_offset[1] = tile_origin.y;
  BakeTargets targets = {};
  targets.images = &bake_image;
  targets.images_num = 1;
  targets.material_to_image = material_to_image;
  targets.materials_num = 1;
  targets.pixels_num = int(pixels.size());
  targets.channels_num = 4;
  RE_bake_pixels_populate(&mesh, pixels.data(), size_t(pixels.size()), &targets, uv_name);
}

/**
 * One snapshot of the raster-side parameters of a pair, taken on the main thread from the
 * material's settings before anything runs: the worker reads nothing but the world, so every
 * control the worker, the margin pass and the commit need lives here rather than in the user's
 * data. Plain data, no DNA/RNA.
 *
 * The atlas and the raster are named apart so the two texel sizes cannot be confused: the atlas
 * is the resolution the map is stored at, the raster is what the engine renders or the CPU fills
 * write into before the commit resolves it -- for a render pair the atlas times the
 * supersampling, for a CPU pair the atlas itself.
 */
struct MeshMapBakeRaster {
  /** The atlas side: the resolution the map's image is stored at. */
  int atlas_resolution = 0;
  /** The raster side: `atlas_resolution * supersampling` for a render pair, the atlas itself for
   * a CPU pair. */
  int raster_resolution = 0;
  /** The margin in RASTER texels: the atlas margin times the supersampling, because the margin
   * pass runs on the raster and the commit's box-downsample must land the atlas margin exactly. */
  int margin_raster_pixels = 0;
  /** The margin in ATLAS texels, the setting itself: the variant-A clear radius the commit grows
   * is measured in atlas texels and has to match what the merge and the extend are about to
   * rewrite. */
  int margin_atlas_pixels = 0;
  /** #eMaterialMeshMapMarginType; its values match the #eBakeMarginType the margin pass takes. */
  int8_t margin_type = MA_MESH_MAP_MARGIN_ADJACENT_FACES;
  /** The pair's #eMaterialMeshMapType. */
  int8_t type = MA_MESH_MAP_AO;
  /** Whether the tangent normal of this pair flips its green channel (the DirectX convention). */
  bool directx_normal = false;
  /** #eMaterialMeshMapPositionNormalization for a Position pair. */
  int8_t position_normalization = MA_MESH_MAP_POSITION_BOX;
  /** The raster is the atlas resolution times this; the commit boxes the result down. */
  int supersampling = 1;
  /** The UV layer the raster samples, copied out of the mesh (see
   * #BKE_mesh_maps_bake_uv_resolve_or_fail). */
  char uv_name[68] = "";
};

/** One (object, type) of a run: the private world plus its bookkeeping. */
struct MeshMapBakeWorld {
  /* The private render world. Null for a CPU ID pair, which needs no engine: those bake from a
   * local mesh copy, with no temporary Main to build. */
  Main *temp_main = nullptr;
  Scene *temp_scene = nullptr;
  ViewLayer *temp_view_layer = nullptr;
  Object *temp_object = nullptr;
  Depsgraph *temp_depsgraph = nullptr;
  Render *render = nullptr;

  /** The atlas this world bakes into. */
  Image *atlas = nullptr;

  /* The atlas is a tiled image, so a bake is one raster per tile the object's UVs cover: one
   * #BakeImage per tile, each shifted by its tile origin (`uv_offset`), its texels one block of
   * the shared buffers below (`offset`). The raster prunes the tiles it left empty, so the vector
   * can shrink after the prepare. */
  Vector<BakeImage> images;
  int tiles_num = 0;
  Image **material_to_image = nullptr;
  BakeTargets targets = {};
  BakePixel *pixels = nullptr;
  float *result = nullptr;
  /** One tile's raster is #pixels_num texels; the buffers cover every tile in #images. */
  size_t pixels_num = 0;

  /* The copied low-poly mesh the margin is derived from. */
  Mesh *mesh = nullptr;
  /**
   * The raster-side snapshot of this pair (see #MeshMapBakeRaster): the worker, the margin pass
   * and the commit read nothing but this for the pair's type, texel sizes and UV layer, so none
   * of them can drift back to the user's data.
   */
  MeshMapBakeRaster raster;
  char *margin_mask = nullptr;

  /* The high-poly source: for a CPU ID pair, the user's original objects (their names decide the
   * ID colours, and the ray code only reads a name for its error path) and local copies of their
   * evaluated meshes, owned here. For a render pair, temp copies owned by `temp_main` -- Cycles
   * must see them inside `temp_depsgraph`, not the user's objects from another depsgraph. The cage
   * likewise: original object for the matrix (CPU) or temp copy (render), owned copy for the ray
   * data. */
  Vector<Object *> source_objects;
  Vector<Mesh *> source_meshes;
  Object *cage_object = nullptr;
  Mesh *cage_mesh = nullptr;
  float4x4 low_matrix;
  float4x4 cage_matrix;
  BakeHighPolyData *highpoly = nullptr;
  int highpoly_num = 0;
  BakePixel *pixels_high = nullptr;
  /** Set by the worker when the pair's private pass failed (the engine bake, or the margin
   * phase's buffer allocation), so the commit marks the pair ERROR instead of writing a broken
   * result. */
  bool worker_failed = false;
};

/** What the pair started from, so a cancel can put the status back. */
struct MeshMapBakePairState {
  Object *object = nullptr;
  uint32_t object_session_uid = 0;
  int8_t type = MA_MESH_MAP_AO;
  int8_t previous_status = OB_MESH_MAP_STATUS_NONE;
  /** The atlas side and the merge margin this pair bakes at, resolved from its slot. */
  int resolution = 0;
  int margin_pixels = 0;
  /** The atlas copy the pair bakes into (#MaterialMeshMapSlot::uid); zero is the primary. */
  int32_t slot_uid = 0;
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

/**
 * Progress and break hooks the render layer would call from inside Cycles. This build does not
 * wire them through #RE_bake_engine (the private Render inherits its own break check via
 * #G.is_break, which the synchronous exec path and #WM_jobs_setting_kill set for us). They stay
 * here so a future engine-side hook can drop in without touching the surrounding code.
 */
[[maybe_unused]] void mesh_map_bake_progress_cb(void *handle, const float progress)
{
  wmJobWorkerStatus *worker_status = static_cast<wmJobWorkerStatus *>(handle);
  worker_status->progress = progress;
  worker_status->do_update = true;
}

[[maybe_unused]] bool mesh_map_bake_test_break_cb(void *handle)
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
void mesh_map_bake_apply_cycles_settings(Scene &scene, const bool denoise, const int samples)
{
  PointerRNA scene_ptr = RNA_id_pointer_create(&scene.id);
  if (RNA_struct_find_property(&scene_ptr, "cycles") == nullptr) {
    return;
  }
  PointerRNA cycles = RNA_pointer_get(&scene_ptr, "cycles");
  if (cycles.data == nullptr) {
    return;
  }
  RNA_int_set(&cycles, "samples", std::max(samples, 1));
  RNA_boolean_set(&cycles, "use_denoising", denoise);
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
  const MeshMapTypeInfo *info = BKE_mesh_maps_type_info_get(type);
  /* Keeps the label of a type this build does not list (a file saved by a newer Blender) readable.
   * The names come from the descriptor table, so the reports and the RNA enum agree. */
  return info != nullptr ? info->ui_name : "Map";
}

void mesh_map_bake_world_free(MeshMapBakeWorld &world)
{
  /* Order mirrors M:: the engine handle first (it references the depsgraph's evaluated objects),
   * then the depsgraph, then the temp-Main that owns every temp ID including the mesh copies that
   * the render branch stores in `world.mesh`/`world.source_meshes`/`world.cage_mesh`. The CPU
   * branch keeps those as null-Main locals, so it frees them explicitly before the Main-free
   * short-circuits on a null `temp_main`. */
  if (world.render != nullptr) {
    RE_FreeRender(world.render);
    world.render = nullptr;
  }
  if (world.temp_depsgraph != nullptr) {
    DEG_graph_free(world.temp_depsgraph);
    world.temp_depsgraph = nullptr;
  }
  if (world.temp_main != nullptr) {
    /* Every mesh/object copy created for the render world lives inside `temp_main`; dropping it
     * also drops `world.mesh`, `world.source_meshes`, `world.cage_mesh` and their objects. The
     * pointers themselves are cleared by the assignment at the bottom. */
    BKE_main_free(world.temp_main);
    world.temp_main = nullptr;
  }
  else {
    /* CPU branch: the mesh copies are local IDs outside any Main; that is exactly how they were
     * created, so a null-Main free releases them and their custom data. */
    if (world.mesh != nullptr) {
      BKE_id_free(nullptr, &world.mesh->id);
      world.mesh = nullptr;
    }
    for (Mesh *source_mesh : world.source_meshes) {
      if (source_mesh != nullptr) {
        BKE_id_free(nullptr, &source_mesh->id);
      }
    }
    if (world.cage_mesh != nullptr) {
      BKE_id_free(nullptr, &world.cage_mesh->id);
      world.cage_mesh = nullptr;
    }
  }
  world.source_meshes.clear();
  world.source_objects.clear();
  world.cage_object = nullptr;
  world.temp_scene = nullptr;
  world.temp_view_layer = nullptr;
  world.temp_object = nullptr;
  world.atlas = nullptr;
  world.images = {};
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
 * Copy the resolved high-poly source objects into the world. Shared by both branches (the copy
 * logic and its failure path are identical; only the placement differs): a render pair gets temp
 * objects inside the private scene, sharing \a temp_material so the EMIT passes trace the
 * high-poly through the same value graph, while a CPU pair keeps the user's original objects
 * (their names decide the ID colours) next to local mesh copies owned by the world. \a
 * temp_material is null for the CPU branch.
 *
 * Returns false with the pair failed when a source mesh could not be copied.
 */
bool mesh_map_bake_prepare_sources(MeshMapBakeWorld &world,
                                   const Vector<Object *> &source_objects,
                                   const bool is_render,
                                   Material *temp_material,
                                   Depsgraph &user_depsgraph,
                                   MeshMapBakePairState &pair,
                                   ObjectMeshMapState &state)
{
  if (!is_render) {
    /* CPU branch: the user's original objects go into `world.source_objects` so the
     * ID colour reads their names, and their evaluated meshes go into `world.source_meshes` as
     * local copies owned by the world (freed by a null-Main `BKE_id_free` in the world-free). */
    world.source_objects.reserve(source_objects.size());
    world.source_meshes.reserve(source_objects.size());
  }
  for (Object *source : source_objects) {
    Object *source_eval = DEG_get_evaluated(&user_depsgraph, source);
    if (source_eval == nullptr) {
      source_eval = source;
    }
    Mesh *source_copy = mesh_map_bake_copy_mesh(mesh_map_bake_object_mesh(*source_eval));
    if (source_copy == nullptr) {
      BKE_mesh_maps_bake_state_fail(state);
      BLI_strncpy(pair.error, "Could not copy a high-poly source mesh", sizeof(pair.error));
      pair.failed = true;
      return false;
    }
    if (is_render) {
      BKE_libblock_management_main_add(world.temp_main, source_copy);
      Object *temp_source = BKE_object_add_for_data(world.temp_main,
                                                    world.temp_scene,
                                                    world.temp_view_layer,
                                                    OB_MESH,
                                                    "Mesh Map Source",
                                                    &source_copy->id,
                                                    true);
      BKE_object_apply_mat4(temp_source, source_eval->object_to_world().ptr(), false, false);
      BKE_collection_object_add(
          world.temp_main, world.temp_scene->master_collection, temp_source);
      BKE_object_material_slot_add(world.temp_main, temp_source);
      BKE_object_material_assign(
          world.temp_main, temp_source, temp_material, 1, BKE_MAT_ASSIGN_OBJECT);
      world.source_objects.append(temp_source);
    }
    else {
      world.source_objects.append(source);
    }
    world.source_meshes.append(source_copy);
  }
  return true;
}

/**
 * Copy the pair's cage into the world; an empty \a cage means an auto-cage and nothing to do. The
 * match check and the copy are shared by both branches; only the placement differs: a render pair
 * adds a temp object to the private scene (hidden from render, because the cage is only the ray
 * origin), a CPU pair keeps the user's original object for the matrix. Either way the world keeps
 * the cage mesh copy and the cage's evaluated matrix.
 *
 * Returns false with the pair failed when the cage does not match the low-poly or cannot be
 * copied.
 */
bool mesh_map_bake_prepare_cage(MeshMapBakeWorld &world,
                                Object *cage,
                                const bool is_render,
                                Depsgraph &user_depsgraph,
                                MeshMapBakePairState &pair,
                                ObjectMeshMapState &state)
{
  if (cage == nullptr) {
    return true;
  }
  Object *cage_eval = DEG_get_evaluated(&user_depsgraph, cage);
  if (cage_eval == nullptr) {
    cage_eval = cage;
  }
  Mesh *cage_eval_mesh = mesh_map_bake_object_mesh(*cage_eval);
  if (cage_eval_mesh == nullptr ||
      !BKE_mesh_maps_bake_cage_matches(*world.mesh, *cage_eval_mesh))
  {
    BKE_mesh_maps_bake_state_fail(state);
    BLI_strncpy(pair.error,
                "The cage mesh must have the same number of faces as the low-poly object",
                sizeof(pair.error));
    pair.failed = true;
    return false;
  }
  Mesh *cage_copy = mesh_map_bake_copy_mesh(cage_eval_mesh);
  if (cage_copy == nullptr) {
    BKE_mesh_maps_bake_state_fail(state);
    BLI_strncpy(pair.error, "Could not copy the cage mesh", sizeof(pair.error));
    pair.failed = true;
    return false;
  }
  if (is_render) {
    BKE_libblock_management_main_add(world.temp_main, cage_copy);
    Object *temp_cage = BKE_object_add_for_data(world.temp_main,
                                                world.temp_scene,
                                                world.temp_view_layer,
                                                OB_MESH,
                                                "Mesh Map Cage",
                                                &cage_copy->id,
                                                true);
    BKE_object_apply_mat4(temp_cage, cage_eval->object_to_world().ptr(), false, false);
    BKE_collection_object_add(world.temp_main, world.temp_scene->master_collection, temp_cage);
    /* The cage is only the ray origin; it must not render. */
    temp_cage->visibility_flag |= OB_HIDE_RENDER;
    world.cage_object = temp_cage;
  }
  else {
    world.cage_object = cage;
  }
  world.cage_mesh = cage_copy;
  world.cage_matrix = cage_eval->object_to_world();
  return true;
}

/**
 * Fill a Curvature pair through the Normal Filter: read the object's baked Tangent Normal atlas
 * tile by tile and write its divergence as the curvature value. The filter is atlas-to-atlas: the
 * Tangent Normal atlas must exist at the same resolution, with a float buffer like every atlas.
 *
 * The Tangent Normal atlas is resolved through #BKE_mesh_maps_slot_find.
 *
 * Returns false with the pair failed when the object's Tangent Normal state is not VALID or no
 * longer matches the object's content, when the Tangent Normal atlas is missing, is not float,
 * sits at another resolution, or does not carry one of the tiles the curvature raster covers. A
 * missing tile means the tangent bake covered another UV layout, so filtering would fill the
 * covered texels with zeros and then call the pair a successful bake.
 */
bool mesh_map_bake_fill_curvature_filter(MeshMapBakeWorld &world,
                                         Material &ma,
                                         MeshMapBakePairState &pair,
                                         ObjectMeshMapState &state,
                                         Object &ob,
                                         const Object &ob_eval)
{
  /* The atlas alone is not proof of a usable bake: a failed render leaves its tiles allocated but
   * black, and a stale bake was made from the old geometry. Either would filter to neutral 0.5 in
   * every covered texel and still be committed as a successful bake, so the state of the same
   * object's Tangent Normal pair gates the filter. `is_current` catches VALID-but-old as well:
   * the tangent hash is compared against the viewport evaluation the bake itself uses. */
  ObjectMeshMapState *normal_state = BKE_mesh_maps_object_state_find_uid(
      ob, ma, MA_MESH_MAP_NORMAL_TANGENT, 0);
  if (normal_state == nullptr || normal_state->status != OB_MESH_MAP_STATUS_VALID) {
    BKE_mesh_maps_bake_state_fail(state);
    BLI_strncpy(pair.error,
                "The Normal Filter needs the Tangent Normal map baked first (its state is not "
                "valid)",
                sizeof(pair.error));
    pair.failed = true;
    return false;
  }
  if (!BKE_mesh_maps_object_state_is_current(*normal_state, ob_eval, ma)) {
    BKE_mesh_maps_bake_state_fail(state);
    BLI_strncpy(pair.error,
                "The Tangent Normal map is stale; bake it again before the Normal Filter",
                sizeof(pair.error));
    pair.failed = true;
    return false;
  }
  const MaterialMeshMapSlot *normal_slot = BKE_mesh_maps_slot_find(ma, MA_MESH_MAP_NORMAL_TANGENT);
  Image *normal_atlas = normal_slot != nullptr ? normal_slot->image : nullptr;
  if (normal_atlas == nullptr) {
    BKE_mesh_maps_bake_state_fail(state);
    BLI_strncpy(pair.error, "The Normal Filter needs the Tangent Normal map baked first",
                sizeof(pair.error));
    pair.failed = true;
    return false;
  }
  if (!BKE_mesh_maps_bake_atlas_is_float(*normal_atlas)) {
    BKE_mesh_maps_bake_state_fail(state);
    BLI_strncpy(pair.error, "The Tangent Normal atlas is not a float buffer", sizeof(pair.error));
    pair.failed = true;
    return false;
  }
  int normal_width = 0;
  int normal_height = 0;
  BKE_image_get_size(normal_atlas, nullptr, &normal_width, &normal_height);
  if (normal_width != world.raster.atlas_resolution ||
      normal_height != world.raster.atlas_resolution)
  {
    BKE_mesh_maps_bake_state_fail(state);
    BLI_strncpy(pair.error, "The Tangent Normal atlas must be at the same resolution",
                sizeof(pair.error));
    pair.failed = true;
    return false;
  }
  const int radius = std::clamp(int(ma.mesh_map_settings.curvature_radius),
                                MESH_MAP_BAKE_CURVATURE_RADIUS_MIN,
                                MESH_MAP_BAKE_CURVATURE_RADIUS_MAX);
  const float scale = ma.mesh_map_settings.curvature_scale;
  const bool directx = ma.mesh_map_settings.use_directx_normal != 0;
  for (const int tile : IndexRange(world.tiles_num)) {
    const BakeImage &image = world.images[tile];
    if (BKE_image_get_tile(normal_atlas, image.tile_number) == nullptr) {
      /* The curvature covers a tile the tangent bake never wrote (its UV layout differed), so the
       * filter would leave that tile's covered texels at zero and still call the pair a bake. */
      BKE_mesh_maps_bake_state_fail(state);
      BLI_snprintf(pair.error,
                   sizeof(pair.error),
                   "The Tangent Normal atlas has no UDIM tile %d; bake Tangent Normal at this "
                   "resolution first",
                   image.tile_number);
      pair.failed = true;
      return false;
    }
    ImageUser normal_iuser;
    BKE_imageuser_default(&normal_iuser);
    normal_iuser.tile = image.tile_number;
    ImageTileBufferScope normal_buffer(*normal_atlas, &normal_iuser);
    if (normal_buffer == nullptr || normal_buffer->float_data() == nullptr) {
      /* The tile exists but holds no readable float pixels: filtering nothing would silently
       * produce a black curvature tile. */
      BKE_mesh_maps_bake_state_fail(state);
      BLI_snprintf(pair.error,
                   sizeof(pair.error),
                   "The Tangent Normal atlas tile %d has no float pixels",
                   image.tile_number);
      pair.failed = true;
      return false;
    }
    /* The image's own size is checked once above, but a tiled image may hold a tile at another
     * size; the filter writes a `resolution` block per tile, so a smaller tile must fail rather
     * than be overrun. */
    if (normal_buffer->x != world.raster.atlas_resolution ||
        normal_buffer->y != world.raster.atlas_resolution)
    {
      BKE_mesh_maps_bake_state_fail(state);
      BLI_snprintf(pair.error,
                   sizeof(pair.error),
                   "The Tangent Normal atlas tile %d is %dx%d, expected %dx%d",
                   image.tile_number,
                   normal_buffer->x,
                   normal_buffer->y,
                   world.raster.atlas_resolution,
                   world.raster.atlas_resolution);
      pair.failed = true;
      return false;
    }
    BKE_mesh_maps_bake_fill_curvature_from_normal(world.pixels + image.offset,
                                                  world.pixels_num,
                                                  world.raster.atlas_resolution,
                                                  Span<float>(normal_buffer->float_data(),
                                                              size_t(world.pixels_num) * 4),
                                                  radius,
                                                  scale,
                                                  directx,
                                                  world.result + size_t(image.offset) * 4);
  }
  return true;
}

/**
 * Fill a Thickness or a Bent Normal pair: the two CPU ray fills share their exact shape and only
 * the per-direction ray semantics differ, so one call serves both and \a type picks the fill. An
 * empty \a sources means the pair has no high-poly source: the low-poly is its own interior.
 */
void mesh_map_bake_fill_thickness_or_bent(MeshMapBakeWorld &world,
                                          const size_t pixels_total,
                                          Material &ma,
                                          const Span<MeshMapBakeThicknessSource> sources,
                                          const int8_t type)
{
  /* The BKE fills clamp the ray count against the shared `MESH_MAP_BAKE_CPU_RAYS` limits
   * themselves, so the editor passes the raw setting and the bounds exist in one place only. */
  if (type == MA_MESH_MAP_THICKNESS) {
    BKE_mesh_maps_bake_fill_thickness(world.pixels,
                                      pixels_total,
                                      *world.mesh,
                                      world.low_matrix,
                                      sources,
                                      int(ma.mesh_map_settings.cpu_rays),
                                      world.result);
  }
  else {
    BKE_mesh_maps_bake_fill_bent_normals(world.pixels,
                                         pixels_total,
                                         *world.mesh,
                                         world.low_matrix,
                                         sources,
                                         int(ma.mesh_map_settings.cpu_rays),
                                         world.result);
  }
}

/**
 * Fill the pair's private result on the CPU, dispatching on the pair's type: Curvature (Normal
 * Filter) reads the primary Tangent Normal atlas, Thickness and
 * Bent Normal ray-march the high-poly sources, and the ID maps read the object or material
 * colours. A render pair reaches only the high-poly cast: its result is filled by the worker
 * instead.
 *
 * The per-type fill entry points keep their own signatures (documented in
 * #BKE_mesh_maps_bake.hh), so the dispatch is the local switch below rather than a forced
 * shared shape. Returns false with the pair failed; true means the result -- or the render world
 * the worker fills -- is ready.
 */
bool mesh_map_bake_fill_cpu_map(MeshMapBakeWorld &world,
                                MeshMapBakePairState &pair,
                                ObjectMeshMapState &state,
                                Material &ma,
                                Object &ob,
                                const Object &ob_eval,
                                const MeshMapTypeInfo & /*type_info*/,
                                const bool is_render,
                                const float cage_extrusion,
                                const BakeRayLimits &ray_limits,
                                Depsgraph &user_depsgraph)
{
  const size_t pixels_total = world.pixels_num * size_t(world.tiles_num);
  if (!is_render && pair.type == MA_MESH_MAP_CURVATURE) {
    return mesh_map_bake_fill_curvature_filter(world, ma, pair, state, ob, ob_eval);
  }

  world.highpoly_num = int(world.source_objects.size());
  if (world.highpoly_num > 0) {
    world.highpoly = MEM_new_array_zeroed<BakeHighPolyData>(world.highpoly_num, __func__);
    for (int i = 0; i < world.highpoly_num; i++) {
      Object *source_obj = world.source_objects[i];
      Depsgraph *source_depsgraph = is_render ? world.temp_depsgraph : &user_depsgraph;
      Object *source_eval = DEG_get_evaluated(source_depsgraph, source_obj);
      if (source_eval == nullptr) {
        source_eval = source_obj;
      }
      /* A render world raycasts against the evaluated temp mesh; a CPU world against its own
       * null-Main copy so the ray pass reads nothing but private data. */
      Mesh *source_mesh = is_render ? mesh_map_bake_object_mesh(*source_eval) :
                                      world.source_meshes[i];
      world.highpoly[i].ob = source_obj;
      world.highpoly[i].ob_eval = source_eval;
      world.highpoly[i].mesh = source_mesh;
      copy_m4_m4(world.highpoly[i].obmat, source_eval->object_to_world().ptr());
      invert_m4_m4(world.highpoly[i].imat, world.highpoly[i].obmat);
      world.highpoly[i].is_flip_object = is_negative_m4(source_eval->object_to_world().ptr());
    }

    /* Only the engine and the ID maps read the cast result: the render pairs bake the high-poly
     * surface the low-poly was cast onto, and the CPU ID maps read the hit object/triangle. The
     * ray-marched CPU fills (Thickness, Bent Normal) build their own BVH from `world.source_meshes`
     * and never read `pixels_high`; `RE_bake_pixels_populate_from_objects` would only cost a full
     * texel-by-source cast and, on a miss, rewrite `world.pixels` `primitive_id`s, punching holes
     * in the raster the CPU fill relies on. */
    const bool needs_cast = is_render ||
                            ELEM(pair.type, MA_MESH_MAP_ID_OBJECT, MA_MESH_MAP_ID_MATERIAL);
    if (needs_cast) {
      world.pixels_high = MEM_new_array_uninitialized<BakePixel>(pixels_total, __func__);
      const bool is_custom_cage = world.cage_mesh != nullptr;
      const float(*mat_low)[4] = world.low_matrix.ptr();
      const float(*mat_cage)[4] = is_custom_cage ? world.cage_matrix.ptr() : mat_low;
      if (!RE_bake_pixels_populate_from_objects(world.mesh,
                                                world.pixels,
                                                world.pixels_high,
                                                world.highpoly,
                                                world.highpoly_num,
                                                pixels_total,
                                                is_custom_cage,
                                                cage_extrusion,
                                                ray_limits,
                                                mat_low,
                                                mat_cage,
                                                world.cage_mesh))
      {
        BKE_mesh_maps_bake_state_fail(state);
        BLI_strncpy(pair.error,
                    "Could not cast the low-poly onto the high-poly source",
                    sizeof(pair.error));
        pair.failed = true;
        return false;
      }
    }
  }

  if (is_render) {
    /* A render pair reaches only the high-poly cast above: its result is filled by the worker
     * instead. */
    return true;
  }

  switch (pair.type) {
    case MA_MESH_MAP_CURVATURE:
      /* Handled above, before the high-poly setup: the Normal Filter reads the primary Tangent
       * Normal atlas instead of the sources. */
      break;
    case MA_MESH_MAP_THICKNESS:
    case MA_MESH_MAP_BENT_NORMAL: {
      /* The interior the rays march through (or the hemisphere they see) is the high-poly set;
       * with no sources the low-poly itself is the interior. The origin stays on the low-poly's
       * surface, so the low-poly never joins its own source list. */
      Vector<MeshMapBakeThicknessSource> thickness_sources;
      thickness_sources.reserve(world.source_meshes.size());
      for (const int i : IndexRange(world.highpoly_num)) {
        /* The copied mesh is the evaluated one, so its placement must come from the evaluated
         * object too: a constraint or driver that moves the source between the original and the
         * evaluated state would otherwise shift the interior relative to the sampled surface. */
        const Object *source_eval = world.highpoly[i].ob_eval;
        thickness_sources.append({world.source_meshes[i], source_eval->object_to_world()});
      }
      mesh_map_bake_fill_thickness_or_bent(world, pixels_total, ma, thickness_sources, pair.type);
      break;
    }
    default: {
      /* The ID maps read the object or material colours: from the high-poly sources when given,
       * from the low-poly's own faces otherwise. */
      if (world.highpoly_num > 0) {
        Vector<const Object *> id_objects;
        Vector<const Mesh *> id_meshes;
        id_objects.reserve(world.highpoly_num);
        id_meshes.reserve(world.highpoly_num);
        for (int i = 0; i < world.highpoly_num; i++) {
          id_objects.append(world.source_objects[i]);
          id_meshes.append(world.source_meshes[i]);
        }
        BKE_mesh_maps_bake_fill_id_from_high_poly(world.pixels_high,
                                                  pixels_total,
                                                  id_objects.data(),
                                                  size_t(world.highpoly_num),
                                                  id_meshes.data(),
                                                  size_t(world.highpoly_num),
                                                  pair.type,
                                                  4,
                                                  world.result);
      }
      else {
        /* No source: the low-poly's own faces decide every covered texel. */
        BKE_mesh_maps_bake_fill_id_pixels(
            world.pixels, pixels_total, *world.mesh, ob, pair.type, 4, world.result);
      }
      break;
    }
  }
  return true;
}


/**
 * Resolve the atlas the pair bakes into: the slot named by the pair's uid, and its float atlas at
 * the pair's resolution, created on first use. Returns null -- with the pair and its state failed
 * -- when the copy is gone or the atlas cannot be created.
 */
Image *mesh_map_bake_resolve_pair_atlas(Main &bmain,
                                        Material &ma,
                                        MeshMapBakePairState &pair,
                                        ObjectMeshMapState &state)
{
  MaterialMeshMapSlot *pair_slot = BKE_mesh_maps_slot_find_uid(ma, pair.type, pair.slot_uid);
  if (pair_slot == nullptr) {
    if (pair.slot_uid == 0) {
      /* The plan's primary pair for a type with no slot yet: the wrapper ensures the primary and
       * creates its atlas, exactly as a fresh material's first bake always did. */
      Image *atlas = BKE_mesh_maps_bake_atlas_ensure(
          bmain, ma, pair.type, pair.resolution);
      if (atlas == nullptr) {
        BKE_mesh_maps_bake_state_fail(state);
        BLI_strncpy(pair.error, "Could not create the mesh map atlas image", sizeof(pair.error));
        pair.failed = true;
        return nullptr;
      }
      return atlas;
    }
    BKE_mesh_maps_bake_state_fail(state);
    BLI_strncpy(pair.error, "The atlas copy of this bake is gone", sizeof(pair.error));
    pair.failed = true;
    return nullptr;
  }
  Image *atlas = BKE_mesh_maps_bake_atlas_ensure_slot(bmain, *pair_slot, pair.resolution);
  if (atlas == nullptr) {
    BKE_mesh_maps_bake_state_fail(state);
    BLI_strncpy(pair.error, "Could not create the mesh map atlas image", sizeof(pair.error));
    pair.failed = true;
    return nullptr;
  }
  return atlas;
}

/**
 * Build the private world for \a pair, rasterize its texels, fill the CPU ID result (or the render
 * world for a Cycles type) and leave the object state #OB_MESH_MAP_STATUS_BAKING (already entered).
 * On failure the state is marked #OB_MESH_MAP_STATUS_ERROR, \a pair records the message and false is
 * returned. Main thread only.
 *
 * The phases live in the file-local helpers above: the source and cage copies shared by both
 * branches in #mesh_map_bake_prepare_sources / #mesh_map_bake_prepare_cage, and the per-type CPU
 * fills in #mesh_map_bake_fill_cpu_map.
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
  const MeshMapTypeInfo *type_info = BKE_mesh_maps_type_info_get(pair.type);
  if (type_info == nullptr) {
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
  ObjectMeshMapState *state = BKE_mesh_maps_object_state_ensure_uid(
      *ob, *ma, pair.type, pair.slot_uid);
  if (state == nullptr) {
    BLI_strncpy(pair.error, "The object cannot hold a mesh map state", sizeof(pair.error));
    pair.failed = true;
    return false;
  }
  pair.previous_status = BKE_mesh_maps_bake_state_begin(*state);
  pair.begun = true;
  const char *uv_name = BKE_mesh_maps_bake_uv_resolve_or_fail(*state, *ob_eval);
  if (uv_name == nullptr) {
    BLI_strncpy(pair.error, "The object has no UV layer", sizeof(pair.error));
    pair.failed = true;
    return false;
  }
  STRNCPY(job.world.raster.uv_name, uv_name);
  job.world.raster.type = pair.type;
  BKE_mesh_maps_object_hash(*ob_eval, *ma, pair.type, pair.start_hash);
  const float4x4 object_matrix = ob_eval->object_to_world();

  const bool is_render = BKE_mesh_maps_pair_renders(*ma, pair.type);
  /* A Curvature pair on the Normal Filter runs on the CPU over the already-baked Tangent
   * Normal atlas: it needs neither a high-poly source nor a cage, and copying/validating them
   * would only add a failure path (a broken source or a mismatched cage) to a pair that reads
   * neither. */
  const bool is_curvature_filter = !is_render && pair.type == MA_MESH_MAP_CURVATURE;

  /* The UDIM tiles this pair can cover, from the UV bounds of the faces that use the material,
   * before anything is allocated: the memory budget below needs the candidate count, and a tile
   * only another material's faces touch must not be paid for. Read off the evaluated mesh (the
   * copy below is its localize twin, so the UVs agree), so the pass allocates nothing but the
   * tile list itself. */
  Vector<int> tile_numbers;
  bool uv_outside_udim = false;
  if (Mesh *mesh_eval = mesh_map_bake_object_mesh(*ob_eval)) {
    const blender::bke::AttributeAccessor attributes = mesh_eval->attributes();
    const std::string name(job.world.raster.uv_name);
    const blender::bke::AttributeReader<float2> uv_reader = attributes.lookup<float2>(
        name, blender::bke::AttrDomain::Corner);
    if (uv_reader) {
      const VArraySpan<float2> uv_map(*uv_reader);
      const VArraySpan<int> material_indices = *attributes.lookup<int>(
          "material_index", blender::bke::AttrDomain::Face);
      if (!uv_map.is_empty()) {
        const OffsetIndices<int> faces = mesh_eval->faces();
        for (const int face : faces.index_range()) {
          const IndexRange corners = faces[face];
          if (corners.is_empty()) {
            continue;
          }
          /* The same membership rule #BKE_mesh_maps_bake_restrict_to_material applies to the
           * rasterized texels: a face counts when its slot resolves to this material. */
          const int slot = material_indices.is_empty() ? 0 : material_indices[face];
          if (BKE_object_material_get(ob, short(slot + 1)) != ma) {
            continue;
          }
          float2 uv_min = uv_map[corners.first()];
          float2 uv_max = uv_min;
          for (const int corner : corners.drop_front(1)) {
            uv_min = math::min(uv_min, uv_map[corner]);
            uv_max = math::max(uv_max, uv_map[corner]);
          }
          BKE_paint_layers_tiles_from_uv_bounds(uv_min, uv_max, uv_outside_udim, tile_numbers);
        }
      }
    }
  }
  if (tile_numbers.is_empty()) {
    /* No UV layer to read, none of it names a tile, or no face uses the material: the base square
     * is the only candidate, as when the bake was single-tile. */
    tile_numbers.append(PAINT_TILE_NUMBER_BASE);
  }
  std::sort(tile_numbers.begin(), tile_numbers.end());
  if (uv_outside_udim && job.reports != nullptr) {
    BKE_reportf(job.reports,
                RPT_WARNING,
                "UV map '%s' of '%s' has corners outside the UDIM range (U and V 0 to 10), "
                "they are not baked",
                job.world.raster.uv_name,
                ob->id.name + 2);
  }

  /* The engine rasterizes supersampled and the commit boxes down into the atlas; the CPU paths
   * work texel-to-texel with their atlases, so they stay at the atlas resolution. */
  const int requested_supersampling = is_render ?
                                          std::clamp(int(ma->mesh_map_settings.supersampling),
                                                     MESH_MAP_BAKE_SUPERSAMPLING_MIN,
                                                     MESH_MAP_BAKE_SUPERSAMPLING_MAX) :
                                          1;
  /* The memory budget of the pair (see #BKE_mesh_maps_bake_estimate_bytes), checked before the
   * first allocation and for render and CPU pairs alike. An over-budget pair first trades
   * supersampling down -- the raster is the largest single cost -- and is refused only when even
   * the unsupersampled pair cannot fit. The raster limit rides along as the coarse raster-side
   * guard, clamping the same way. */
  const size_t candidate_tiles = size_t(tile_numbers.size());
  const size_t budget_bytes = size_t(MESH_MAP_BAKE_MEMORY_LIMIT_MB) * 1024 * 1024;
  int supersampling = requested_supersampling;
  while (supersampling > 1) {
    const bool fits = pair.resolution * supersampling <=
                          MESH_MAP_BAKE_RENDER_RESOLUTION_LIMIT &&
                      BKE_mesh_maps_bake_estimate_bytes(
                          pair.resolution, supersampling, candidate_tiles) <= budget_bytes;
    if (fits) {
      break;
    }
    supersampling--;
  }
  const int render_resolution = pair.resolution * supersampling;
  const size_t estimate_bytes = BKE_mesh_maps_bake_estimate_bytes(
      pair.resolution, supersampling, candidate_tiles);
  if (render_resolution > MESH_MAP_BAKE_RENDER_RESOLUTION_LIMIT ||
      estimate_bytes > budget_bytes)
  {
    /* Even the unsupersampled pair is over: refusing is the only honest answer left. */
    BKE_mesh_maps_bake_state_fail(*state);
    BLI_snprintf(pair.error,
                 sizeof(pair.error),
                 "The bake needs about %d MB (%d UDIM tile(s) at %dx%d), over the %d MB memory "
                 "budget; lower the atlas resolution or the tiles the UVs cover",
                 int(estimate_bytes / (1024 * 1024)),
                 int(candidate_tiles),
                 render_resolution,
                 render_resolution,
                 MESH_MAP_BAKE_MEMORY_LIMIT_MB);
    pair.failed = true;
    return false;
  }
  if (supersampling < requested_supersampling && job.reports != nullptr) {
    /* Both bounds clamp the factor down: name the one that actually refused the requested value. */
    const bool over_budget = BKE_mesh_maps_bake_estimate_bytes(
                                 pair.resolution, requested_supersampling, candidate_tiles) >
                             budget_bytes;
    BKE_reportf(job.reports,
                RPT_INFO,
                "Supersampling reduced from %d to %d (%s)",
                requested_supersampling,
                supersampling,
                over_budget ? "memory budget" : "raster limit");
  }

  /* The high-poly source of this material, if any. For a CPU pair its meshes are copied locally so
   * the ray pass can build its BVH trees off the user's data; for a render pair they are copied
   * into the private Main so Cycles can see them inside the render depsgraph. */
  Vector<Object *> source_objects;
  Object *cage = nullptr;
  float cage_extrusion = 0.0f;
  /* The ray-cast limits the high-poly cast runs with: zero defaults when the object holds no
   * source, so the cast then reads the same limits an empty bake would. */
  BakeRayLimits ray_limits{};
  if (!is_curvature_filter) {
    BKE_mesh_maps_source_resolve(*ob, *ma, source_objects, &cage);
    if (const ObjectMeshMapSource *source = BKE_mesh_maps_source_find(*ob, *ma)) {
      cage_extrusion = source->cage_extrusion;
      ray_limits.max_ray_distance = source->max_ray_distance;
      ray_limits.max_rear_ray_distance = source->max_rear_ray_distance;
      ray_limits.ignore_backfacing = source->use_ignore_backfacing != 0;
      ray_limits.skew = std::clamp(source->skew, 0.0f, 1.0f);
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
  /* The per-map controls this pair bakes with: the shared encodings and the two texel sizes, so
   * the worker and the commit read nothing but the world. The raster margin scales with the
   * supersampling so the commit's box-downsample lands the atlas margin exactly, while the atlas
   * margin stays itself: the variant-A clear radius grows in atlas texels and has to match what
   * the merge and the extend are about to rewrite. */
  MeshMapBakeRaster &raster = job.world.raster;
  raster.atlas_resolution = pair.resolution;
  raster.supersampling = supersampling;
  raster.raster_resolution = render_resolution;
  raster.margin_atlas_pixels = pair.margin_pixels;
  raster.margin_raster_pixels = pair.margin_pixels * supersampling;
  raster.margin_type = ma->mesh_map_settings.margin_type;
  raster.directx_normal = ma->mesh_map_settings.use_directx_normal != 0;
  raster.position_normalization = ma->mesh_map_settings.position_normalization;

  if (is_render) {
    /* The private render world: a fresh Main holding the mesh copy, a throw-away scene with the
     * Cycles engine, and the low-poly object positioned at the user's matrix. Everything below
     * lives inside `temp_main` so a single #BKE_main_free releases it all. */
    job.world.temp_main = BKE_main_new();
    BKE_libblock_management_main_add(job.world.temp_main, mesh_copy);
    job.world.temp_scene = BKE_scene_add(job.world.temp_main, "Mesh Map Bake");
    job.world.temp_view_layer = static_cast<ViewLayer *>(job.world.temp_scene->view_layers.first);
    STRNCPY(job.world.temp_scene->r.engine, RE_engine_id_CYCLES);
    /* The bake raster itself comes from #BakeImage (at the raster resolution); the scene size only
     * keeps the temp render consistent with it, and the dummy percent scale and frame give the
     * throw-away scene valid nonzero `r.size`/`r.cfra` so the engine setup never sees zeros. */
    job.world.temp_scene->r.xsch = render_resolution;
    job.world.temp_scene->r.ysch = render_resolution;
    job.world.temp_scene->r.size = 100;
    job.world.temp_scene->r.cfra = 1;
    mesh_map_bake_apply_cycles_settings(*job.world.temp_scene,
                                        BKE_mesh_maps_bake_slot_denoise(*ma, pair.type),
                                        BKE_mesh_maps_bake_slot_samples(*ma, pair.type));

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
      const int samples = BKE_mesh_maps_bake_slot_samples(*ma, pair.type);
      BKE_mesh_maps_bake_build_emit_tree(*temp_material->nodetree,
                                         pair.type,
                                         ma->mesh_map_settings,
                                         ao_distance,
                                         samples);
      BKE_ntree_update_without_main(*temp_material->nodetree);
    }

    /* Copy each source object into the private scene, sharing the throw-away material so the EMIT
     * passes trace the high-poly through the same value graph. */
    if (!mesh_map_bake_prepare_sources(
            job.world, source_objects, true, temp_material, *user_depsgraph, pair, *state))
    {
      return false;
    }

    /* Cage: a custom cage must mirror the low-poly face count; an empty one means an auto-cage. */
    if (!mesh_map_bake_prepare_cage(job.world, cage, true, *user_depsgraph, pair, *state)) {
      return false;
    }

    job.world.temp_depsgraph = mesh_map_bake_depsgraph_new(
        *job.world.temp_main, *job.world.temp_scene, *job.world.temp_view_layer);
    job.world.render = RE_NewSceneRender(job.world.temp_scene);
    RE_bake_engine_set_engine_parameters(
        job.world.render, job.world.temp_main, job.world.temp_scene);
    if (!RE_bake_has_engine(job.world.render)) {
      BKE_mesh_maps_bake_state_fail(*state);
      BLI_strncpy(pair.error, "The Cycles bake engine is not available", sizeof(pair.error));
      pair.failed = true;
      return false;
    }
  }
  else {
    if (!is_curvature_filter) {
      if (!mesh_map_bake_prepare_sources(
              job.world, source_objects, false, nullptr, *user_depsgraph, pair, *state))
      {
        return false;
      }
      if (!mesh_map_bake_prepare_cage(job.world, cage, false, *user_depsgraph, pair, *state)) {
        return false;
      }
    }
  }

  Image *atlas = mesh_map_bake_resolve_pair_atlas(*job.bmain, *ma, pair, *state);
  if (atlas == nullptr) {
    return false;
  }
  job.world.atlas = atlas;

  const size_t pixels_num = size_t(render_resolution) * size_t(render_resolution);
  job.world.pixels_num = pixels_num;

  /* The atlas is a tiled image: the bake is one raster per tile the object's UVs cover. The
   * candidates came from the material's faces' UV bounds before the budget check and the
   * allocations (half-open at the top, so the standard 0..1 layout covers the base tile alone),
   * and the raster still prunes them once it is populated -- a face bounding box can name a tile
   * no texel of the material lands in, and the base tile the UVs never touch earns nothing
   * either. The atlas tiles of the survivors are added after the prune, so nothing is allocated
   * for a tile the bake never writes; a foreign object's tiles are not this bake's, and its
   * coverage is weighed per tile in the commit. */
  size_t pixels_total = pixels_num * size_t(tile_numbers.size());
  job.world.images.reserve(tile_numbers.size());
  for (const int i : tile_numbers.index_range()) {
    BakeImage image = {};
    image.image = atlas;
    image.tile_number = tile_numbers[i];
    /* The bake raster is the raster side, on purpose: the engine pass and the margin fill run at
     * this size, and the commit resolves it down into the atlas. */
    image.width = render_resolution;
    image.height = render_resolution;
    image.offset = size_t(i) * pixels_num;
    const float2 tile_origin = BKE_image_get_tile_uv_origin(tile_numbers[i]);
    image.uv_offset[0] = tile_origin.x;
    image.uv_offset[1] = tile_origin.y;
    job.world.images.append(image);
  }
  job.world.material_to_image = MEM_new_array_uninitialized<Image *>(1, __func__);
  job.world.material_to_image[0] = atlas;
  job.world.targets.images = job.world.images.data();
  job.world.targets.images_num = int(job.world.images.size());
  job.world.targets.material_to_image = job.world.material_to_image;
  job.world.targets.materials_num = 1;
  job.world.targets.pixels_num = int(pixels_total);
  job.world.targets.channels_num = 4;
  job.world.targets.is_noncolor = true;
  job.world.targets.result = MEM_new_array_zeroed<float>(pixels_total * 4, __func__);
  job.world.pixels = MEM_new_array_uninitialized<BakePixel>(pixels_total, __func__);
  job.world.result = job.world.targets.result;

  RE_bake_pixels_populate(mesh_copy,
                          job.world.pixels,
                          pixels_total,
                          &job.world.targets,
                          StringRef(job.world.raster.uv_name));
  /* Warm the lazy triangulation caches on the main thread: the worker's rasterize there only reads
   * them, and computing them off-thread would race the main thread's own mesh access. */
  (void)mesh_copy->corner_tris();
  (void)mesh_copy->corner_tri_faces();
  BKE_mesh_maps_bake_restrict_to_material(job.world.pixels, pixels_total, *mesh_copy, *ob, *ma);

  /* The raster decides which of the candidates the bake covers: a tile no used texel landed in is
   * dropped here, its block of the shared buffers with it, so the render and the commit walk only
   * the tiles that hold something. */
  Vector<int> kept_indices = BKE_mesh_maps_bake_tiles_with_pixels(
      Span<BakePixel>(job.world.pixels, pixels_total), int64_t(pixels_num), tile_numbers.as_span());
  if (kept_indices.is_empty()) {
    /* Nothing was rasterized anywhere (a degenerate UV set): keep the first candidate so the pair
     * still renders one consistent, empty tile. */
    kept_indices.append(0);
  }
  if (kept_indices.size() < tile_numbers.size()) {
    Vector<int> kept_tiles;
    kept_tiles.reserve(kept_indices.size());
    Vector<BakeImage> kept_images;
    kept_images.reserve(kept_indices.size());
    size_t write_block = 0;
    for (const int kept : kept_indices) {
      if (size_t(kept) != write_block) {
        memcpy(job.world.pixels + write_block * pixels_num,
               job.world.pixels + int64_t(kept) * pixels_num,
               pixels_num * sizeof(BakePixel));
      }
      BakeImage image = job.world.images[kept];
      image.offset = write_block * pixels_num;
      kept_images.append(image);
      kept_tiles.append(tile_numbers[kept]);
      write_block++;
    }
    tile_numbers = std::move(kept_tiles);
    job.world.images = std::move(kept_images);
    pixels_total = write_block * pixels_num;
    job.world.targets.pixels_num = int(pixels_total);
    job.world.targets.images = job.world.images.data();
    job.world.targets.images_num = int(job.world.images.size());
  }
  for (const int tile_number : tile_numbers) {
    if (!BKE_paint_layers_map_coverage_tile_ensure(
            *atlas, tile_number, pair.resolution, pair.resolution))
    {
      BKE_mesh_maps_bake_state_fail(*state);
      BLI_snprintf(pair.error,
                   sizeof(pair.error),
                   "Could not add UDIM tile %d to the mesh map atlas",
                   tile_number);
      pair.failed = true;
      return false;
    }
  }
  job.world.tiles_num = int(job.world.images.size());
  BLI_assert(job.world.tiles_num >= 1);

  return mesh_map_bake_fill_cpu_map(job.world,
                                    pair,
                                    *state,
                                    *ma,
                                    *ob,
                                    *ob_eval,
                                    *type_info,
                                    is_render,
                                    cage_extrusion,
                                    ray_limits,
                                    *user_depsgraph);
}

/**
 * Expand this object's result into its margin inside the private buffer, and record the pixel
 * membership in `world.margin_mask`: #FILTER_MASK_USED for a covered texel, #FILTER_MASK_MARGIN
 * for a filled one, zero elsewhere.
 *
 * The pass runs at the raster resolution, so the margin it grows is the raster-side one
 * (#MeshMapBakeRaster.margin_raster_pixels, the atlas margin times the supersampling); the
 * commit's box-downsample then lands the atlas margin exactly.
 *
 * The margin kind is the pair's (#eMaterialMeshMapMarginType): the adjacent-faces fill follows the
 * UV islands and so is the right default for a shared atlas, while the extend fill reaches outward
 * into the background like the full bake's isotropic margin. Both fills report their membership
 * through the mask themselves -- #IMB_filter_extend marks every texel it fills, and the
 * adjacent-faces fill keeps the covered texels #FILTER_MASK_USED -- so the commit reads which
 * texels the margin wrote straight from the mask, with no second pass over the private buffer.
 *
 * Returns true when the pass ran or legitimately had nothing to do (no margin set, no prepared
 * world, or the pass already ran); returns false when the private float buffer cannot be
 * allocated, which the caller reports as a failed pair.
 */
bool mesh_map_bake_apply_margin(MeshMapBakeWorld &world)
{
  const int margin = world.raster.margin_raster_pixels;
  if (margin <= 0 || world.mesh == nullptr || world.result == nullptr ||
      world.margin_mask != nullptr)
  {
    return true;
  }
  const size_t pixels_num = world.pixels_num;
  const int resolution = world.raster.raster_resolution;
  const StringRef uv_name(world.raster.uv_name);
  const int tiles_num = world.tiles_num;
  const eBakeMarginType margin_type = eBakeMarginType(world.raster.margin_type);

  ImBuf *ibuf = IMB_allocImBuf(resolution, resolution, ImBufFlags::FloatData);
  if (ibuf == nullptr || ibuf->float_data_for_write() == nullptr) {
    /* Out of memory: a result without its margin band would read as seams, so the caller refuses
     * the pair instead of committing it. The mask stays null, so no half-filled state survives. */
    if (ibuf != nullptr) {
      IMB_freeImBuf(ibuf);
    }
    return false;
  }
  float *pixels = ibuf->float_data_for_write();
  world.margin_mask = MEM_new_array_zeroed<char>(pixels_num * size_t(tiles_num), __func__);
  for (const int tile : IndexRange(tiles_num)) {
    const BakeImage &image = world.images[tile];
    char *mask = world.margin_mask + size_t(tile) * pixels_num;
    float *result_tile = world.result + size_t(tile) * pixels_num * 4;
    RE_bake_mask_fill(world.pixels + image.offset, pixels_num, mask);

    memcpy(pixels, result_tile, sizeof(float) * pixels_num * 4);
    RE_bake_margin(ibuf, mask, margin, margin_type, world.mesh, uv_name, image.uv_offset);
    memcpy(result_tile, pixels, sizeof(float) * pixels_num * 4);
  }
  IMB_freeImBuf(ibuf);
  return true;
}

/**
 * Run the prepared world's worker half. Worker thread only.
 *
 * A render world calls #RE_bake_engine on the private buffers, then converts the world-space
 * normals Cycles delivers into the space the map claims. A CPU world already has its result from
 * the prepare. Either way the margin expansion that follows touches nothing but the private
 * buffers.
 */
void mesh_map_bake_worker_run(MeshMapBakeWorld &world)
{
  if (BKE_mesh_maps_bake_type_renders(world.raster.type)) {
    if (world.render == nullptr) {
      /* A rendered type normally holds a render world from the prepare. The one legitimate way to
       * reach here without one is the Normal Filter curvature pair: its prepare succeeded and
       * already filled the result on the CPU, so the margin below still applies to it, exactly
       * as to the ID fills, before the return keeps the result for the commit. The guard only
       * catches a truly inconsistent world beyond that. */
      if (!mesh_map_bake_apply_margin(world)) {
        world.worker_failed = true;
        return;
      }
      return;
    }
    const eScenePassType pass_type = BKE_mesh_maps_bake_pass_for_type(world.raster.type);
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
      /* The commit sees the flag and marks the pair ERROR instead of writing the empty result. */
      world.worker_failed = true;
      return;
    }
    if (pass_type == SCE_PASS_NORMAL) {
      /* Cycles always delivers world-space normals; rotate and compress them for the atlas. The
       * whole result is walked: every tile's pixels are one block of the same buffers. */
      const eBakeNormalSwizzle swizzle[3] = {R_BAKE_POSX, R_BAKE_POSY, R_BAKE_POSZ};
      eBakeSpace space = R_BAKE_SPACE_WORLD;
      BKE_mesh_maps_bake_normal_space_for_type(world.raster.type, space);
      const size_t pixels_total = world.pixels_num * size_t(world.tiles_num);
      if (space == R_BAKE_SPACE_OBJECT) {
        RE_bake_normal_world_to_object(
            world.pixels, pixels_total, 4, world.result, world.temp_object, swizzle);
      }
      else if (space == R_BAKE_SPACE_TANGENT) {
        /* The tangent frame comes from the low-poly mesh's active UV; the conversion turns the
         * world normals into it through the low-poly matrix, so a moved object keeps the same
         * map. */
        RE_bake_normal_world_to_tangent(world.pixels,
                                        pixels_total,
                                        4,
                                        world.result,
                                        world.mesh,
                                        swizzle,
                                        world.low_matrix.ptr());
        if (world.raster.directx_normal) {
          /* DirectX flips the green channel of an encoded tangent map. The uncovered texels stay
           * zero and the commit never writes them, so flipping everything is safe. */
          const size_t floats_total = pixels_total * 4;
          for (size_t i = 0; i < floats_total; i += 4) {
            world.result[i + 1] = 1.0f - world.result[i + 1];
          }
        }
      }
      else {
        RE_bake_normal_world_to_world(world.pixels, pixels_total, 4, world.result, swizzle);
      }
    }
    else if (world.raster.type == MA_MESH_MAP_POSITION &&
             world.raster.position_normalization != MA_MESH_MAP_POSITION_RAW)
    {
      /* The emission pass delivered the raw world-space position; remap it into the object's
       * bounds so the map stays a readable gradient. Only the covered and margin texels survive
       * the commit, so the background's remapped zeros do not matter. */
      const Span<float3> positions = world.mesh->vert_positions();
      if (!positions.is_empty()) {
        /* The bounds are in world space, the same space the emission graph writes: seed them with
         * the first *transformed* vertex, not with its object-space position, or a non-identity
         * object matrix would fold the local origin into the world bounds. */
        float3 first_world;
        mul_v3_m4v3(first_world, world.low_matrix.ptr(), positions[0]);
        float3 bb_min = first_world;
        float3 bb_max = first_world;
        for (const float3 &position : positions) {
          float3 world_position;
          mul_v3_m4v3(world_position, world.low_matrix.ptr(), position);
          bb_min = math::min(bb_min, world_position);
          bb_max = math::max(bb_max, world_position);
        }
        const size_t pixels_total = world.pixels_num * size_t(world.tiles_num);
        BKE_mesh_maps_bake_normalize_position(world.result,
                                              pixels_total,
                                              bb_min,
                                              bb_max,
                                              world.raster.position_normalization);
      }
    }
  }
  if (!mesh_map_bake_apply_margin(world)) {
    world.worker_failed = true;
  }
}

/**
 * The atlas-side view of a finished pair: the resolved result and the two masks the merge reads,
 * all at the atlas resolution, one `atlas_total` block each (the result times 4). The storage
 * members keep the buffers alive when the spans are not aliases.
 */
struct MeshMapBakeCommitView {
  /** The resolved buffers; empty when the spans alias the world's raster or masks instead. */
  Array<float> result_storage;
  Array<uint8_t> own_storage;
  Array<uint8_t> margin_storage;
  /** The atlas-side buffers the merge reads. */
  Span<float> result;
  Span<uint8_t> own;
  Span<uint8_t> margin;
};

/**
 * Build the atlas-side view of the finished raster. A supersampled raster is resolved through
 * #BKE_mesh_maps_bake_resolve_raster into freshly allocated buffers the view owns; a factor of 1
 * means the raster is the atlas, so the view aliases the world's raster blocks and the own and
 * margin masks are extracted from the pixels and the margin mask without a downsample.
 */
MeshMapBakeCommitView mesh_map_bake_commit_resolve(MeshMapBakeWorld &world, const int8_t type)
{
  const int factor = world.raster.supersampling;
  const int atlas_resolution = world.raster.atlas_resolution;
  const int tiles_num = world.tiles_num;
  const int64_t raster_texels_per_tile = int64_t(world.pixels_num);
  const int64_t atlas_texels_per_tile = int64_t(atlas_resolution) * int64_t(atlas_resolution);
  const int64_t raster_total = raster_texels_per_tile * tiles_num;
  const int64_t atlas_total = atlas_texels_per_tile * tiles_num;

  /* The merge reads the raster's own coverage and its margin fill as 0/1 masks: the pixels give
   * the own side, the margin mask the #FILTER_MASK_MARGIN texels the margin pass wrote. */
  Array<uint8_t> raster_own(raster_total, 0);
  BKE_mesh_maps_bake_coverage_from_pixels(world.pixels, size_t(raster_total), raster_own);
  Array<uint8_t> raster_margin(raster_total, 0);
  if (world.margin_mask != nullptr) {
    for (int64_t i = 0; i < raster_total; i++) {
      raster_margin[i] = world.margin_mask[i] == FILTER_MASK_MARGIN ? 1 : 0;
    }
  }

  MeshMapBakeCommitView view;
  if (factor > 1) {
    view.result_storage = Array<float>(atlas_total * 4);
    view.own_storage = Array<uint8_t>(atlas_total, 0);
    view.margin_storage = Array<uint8_t>(atlas_total, 0);
    BKE_mesh_maps_bake_resolve_raster(Span<float>(world.result, raster_total * 4),
                                      raster_own.as_span(),
                                      raster_margin.as_span(),
                                      type,
                                      world.raster.raster_resolution,
                                      atlas_resolution,
                                      tiles_num,
                                      MutableSpan<float>(view.result_storage.data(),
                                                         atlas_total * 4),
                                      MutableSpan<uint8_t>(view.own_storage.data(), atlas_total),
                                      MutableSpan<uint8_t>(view.margin_storage.data(),
                                                           atlas_total));
    view.result = view.result_storage.as_span();
    view.own = view.own_storage.as_span();
    view.margin = view.margin_storage.as_span();
    return view;
  }
  /* The raster is the atlas: the masks and the result merge as they are, with no downsample and
   * no normal renormalization, since nothing was averaged that could shrink a normal. */
  view.result = Span<float>(world.result, atlas_total * 4);
  view.own = raster_own.as_span();
  view.margin = raster_margin.as_span();
  /* The masks alias the local arrays, so their storage moves into the view to outlive it. */
  view.own_storage = std::move(raster_own);
  view.margin_storage = std::move(raster_margin);
  return view;
}

/**
 * Merge the resolved pair into \a atlas tile by tile: each tile runs the variant-A clear of this
 * object's stale margin ring, the guarded merge and the extend passes through
 * #BKE_mesh_maps_bake_extend_passes, then is marked dirty for the image system. A tile without a
 * float buffer holds nothing to merge into and is skipped.
 */
void mesh_map_bake_commit_tiles(Image &atlas,
                                const MeshMapBakeWorld &world,
                                const int8_t type,
                                const int atlas_resolution,
                                const int margin_atlas_pixels,
                                const int dilate_iterations,
                                const Span<uint8_t> own_coverage,
                                const Span<uint8_t> own_margin,
                                const Span<uint8_t> foreign_coverage,
                                const Span<float> result_atlas)
{
  const int64_t atlas_texels_per_tile = int64_t(atlas_resolution) * int64_t(atlas_resolution);
  for (const int tile : IndexRange(world.tiles_num)) {
    ImageUser iuser;
    BKE_imageuser_default(&iuser);
    iuser.tile = world.images[tile].tile_number;
    ImageTileBufferScope buffer(atlas, &iuser);
    if (buffer == nullptr || buffer->float_data() == nullptr) {
      continue;
    }
    const int64_t tile_start = int64_t(tile) * atlas_texels_per_tile;
    const Span<uint8_t> own_tile = own_coverage.slice(tile_start, atlas_texels_per_tile);
    const Span<uint8_t> own_margin_tile = own_margin.slice(tile_start, atlas_texels_per_tile);
    const Span<uint8_t> foreign_tile = foreign_coverage.slice(tile_start, atlas_texels_per_tile);
    /* One BKE call per tile: the variant-A clear of this object's stale margin ring, the merge
     * that never lands on another object's islands, and the extend passes that push the map past
     * its margin -- none of them over a foreign texel. The margin here is the atlas-side one
     * (#MeshMapBakeRaster.margin_atlas_pixels): #MeshMapBakeRaster.margin_raster_pixels is what
     * the raster-resolution margin fill ran with, while the clear region grows in atlas texels
     * and has to match what the merge and the extend are about to rewrite. */
    BKE_mesh_maps_bake_extend_passes(type,
                                     own_tile,
                                     own_margin_tile,
                                     foreign_tile,
                                     4,
                                     result_atlas.data() + tile_start * 4,
                                     margin_atlas_pixels,
                                     dilate_iterations,
                                     *buffer);
    buffer->userflags |= IB_DISPLAY_BUFFER_INVALID;
    BKE_image_mark_dirty(&atlas, buffer.get());
  }
}

/** Publish the finished atlas to the image system: full-update invalidation, a GPU refresh and a
 * pack, so the viewport and the file see the merged tiles. */
void mesh_map_bake_commit_mark_updated(Image &atlas)
{
  BKE_image_partial_update_mark_full_update(&atlas);
  BKE_image_free_gputextures(&atlas);
  BKE_image_memorypack(&atlas);
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
  ObjectMeshMapState *state = BKE_mesh_maps_object_state_find_uid(
      *ob, *ma, pair.type, pair.slot_uid);
  if (state == nullptr) {
    return;
  }
  if (world.worker_failed) {
    BKE_mesh_maps_bake_state_fail(*state);
    BLI_strncpy(pair.error,
                "The bake pass failed; the atlas was left untouched",
                sizeof(pair.error));
    pair.failed = true;
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

  MaterialMeshMapSlot *pair_slot = BKE_mesh_maps_slot_find_uid(*ma, pair.type, pair.slot_uid);
  if (pair_slot == nullptr) {
    BKE_mesh_maps_bake_state_fail(*state);
    job.error_num++;
    return;
  }
  Image *atlas = BKE_mesh_maps_bake_atlas_ensure_slot(
      *job.bmain, *pair_slot, pair.resolution);
  if (atlas == nullptr) {
    BKE_mesh_maps_bake_state_fail(*state);
    BLI_strncpy(pair.error,
                "Could not create the mesh map atlas image at commit time",
                sizeof(pair.error));
    pair.failed = true;
    return;
  }
  /* The pixels are float; a byte atlas would swallow them, so a non-float image is a failed bake,
   * not a silent VALID. */
  if (!BKE_mesh_maps_bake_atlas_is_float(*atlas)) {
    BKE_mesh_maps_bake_state_fail(*state);
    BLI_strncpy(pair.error,
                "The mesh map atlas is not a float buffer; the baked pixels cannot be stored",
                sizeof(pair.error));
    pair.failed = true;
    WM_main_add_notifier(NC_MATERIAL | ND_SHADING, ma);
    return;
  }
  /* A supersampled raster boxes down into the atlas resolution first, so the merge, the foreign
   * coverage and the dilation all work at one texel size; a factor of 1 resolves to nothing and
   * the view aliases the raster blocks. */
  const MeshMapBakeCommitView view = mesh_map_bake_commit_resolve(world, pair.type);
  const int tiles_num = world.tiles_num;
  const int64_t atlas_total = int64_t(world.raster.atlas_resolution) *
                              int64_t(world.raster.atlas_resolution) * tiles_num;

  /* The atlas is shared: the object's own pixels always land, but its margin must not paint over
   * an island another object using the material already baked. Everything below is per tile: the
   * raster covers every tile in the world, and each tile weighs its own islands. */
  Array<uint8_t> foreign_coverage(atlas_total, 0);
  char overlap_name[MAX_ID_NAME] = "";
  Vector<int> tile_numbers;
  tile_numbers.reserve(size_t(tiles_num));
  for (const int i : IndexRange(tiles_num)) {
    tile_numbers.append(world.images[i].tile_number);
  }
  BKE_mesh_maps_bake_foreign_coverage(*job.bmain,
                                      *job.scene,
                                      *job.view_layer,
                                      *ob,
                                      *ma,
                                      world.raster.atlas_resolution,
                                      tile_numbers.as_span(),
                                      view.own,
                                      mesh_map_bake_populate_geometry_pixels,
                                      foreign_coverage,
                                      MutableSpan<char>(overlap_name, sizeof(overlap_name)));
  if (overlap_name[0] != '\0' && job.reports != nullptr) {
    BKE_reportf(job.reports,
                RPT_WARNING,
                "Mesh map islands of '%s' overlap '%s' in UV '%s'",
                ob->id.name + 2,
                overlap_name,
                world.raster.uv_name);
  }

  const int dilate_iterations = std::clamp(int(ma->mesh_map_settings.dilate_iterations),
                                           MESH_MAP_BAKE_DILATE_MIN,
                                           MESH_MAP_BAKE_DILATE_MAX);
  mesh_map_bake_commit_tiles(*atlas,
                             world,
                             pair.type,
                             world.raster.atlas_resolution,
                             world.raster.margin_atlas_pixels,
                             dilate_iterations,
                             view.own,
                             view.margin,
                             foreign_coverage.as_span(),
                             view.result);
  mesh_map_bake_commit_mark_updated(*atlas);

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
    /* A failed prepare can leave a partial world behind (a mesh copy already stored); without a
     * temporary Main nobody else owns it, so drop it before the next pair reuses the world. */
    mesh_map_bake_world_free(job.world);
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
    mesh_map_bake_worker_run(job->world);
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
    if (ObjectMeshMapState *state = BKE_mesh_maps_object_state_find_uid(
            *ob, *ma, pair.type, pair.slot_uid))
    {
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

/** One enum-flag item of the descriptor table: bit `type` set, texts from the table. */
constexpr EnumPropertyItem mesh_map_bake_type_flag_item(const int8_t type)
{
  const blender::MeshMapTypeInfo *info = blender::BKE_mesh_maps_type_info_get(type);
  return {1 << info->id, info->idname, 0, info->ui_name, info->ui_description};
}

/**
 * The same maps as #rna_enum_material_mesh_map_type_items, as an enum-flag bitmask (bit `type`
 * set). Built from #MESH_MAP_BAKE_TYPE_INFOS, so the identifiers, names and descriptions live only
 * in the table; the order is pinned by this list to the operators' historical listing (render maps
 * first, then the CPU ones).
 */
constexpr EnumPropertyItem mesh_map_bake_type_flag_items_array[] = {
    mesh_map_bake_type_flag_item(MA_MESH_MAP_AO),
    mesh_map_bake_type_flag_item(MA_MESH_MAP_CURVATURE),
    mesh_map_bake_type_flag_item(MA_MESH_MAP_EDGE),
    mesh_map_bake_type_flag_item(MA_MESH_MAP_POSITION),
    mesh_map_bake_type_flag_item(MA_MESH_MAP_NORMAL_WORLD),
    mesh_map_bake_type_flag_item(MA_MESH_MAP_NORMAL_OBJECT),
    mesh_map_bake_type_flag_item(MA_MESH_MAP_NORMAL_TANGENT),
    mesh_map_bake_type_flag_item(MA_MESH_MAP_ID_OBJECT),
    mesh_map_bake_type_flag_item(MA_MESH_MAP_ID_MATERIAL),
    mesh_map_bake_type_flag_item(MA_MESH_MAP_THICKNESS),
    mesh_map_bake_type_flag_item(MA_MESH_MAP_BENT_NORMAL),
    {0, nullptr, 0, nullptr, nullptr},
};

const EnumPropertyItem *mesh_map_bake_type_flag_items()
{
  return mesh_map_bake_type_flag_items_array;
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
  if (ma == nullptr || !BKE_paint_layers_is_layered(*ma)) {
    return false;
  }
  return !WM_jobs_test(CTX_wm_manager(C), ma, WM_JOB_TYPE_MESH_MAP_BAKE);
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
  job->reports = reports;
  job->pairs.reserve(plan.size());

  for (const MeshMapBakePair &p : plan) {
    MeshMapBakePairState pair;
    pair.object = p.object;
    pair.object_session_uid = p.object->id.session_uid;
    pair.type = p.type;
    pair.slot_uid = p.slot_uid;
    /* Each pair bakes at its own slot's resolution and merge margin: a slot override changes
     * them for this map only, so the snapshot travels with the pair instead of the job. RNA
     * clamps the overrides, but hand-written DNA can carry anything and the budget math
     * multiplies them: hold them to the constant bounds instead of trusting the DNA. */
    pair.resolution = std::clamp(BKE_mesh_maps_bake_slot_resolution(ma, p.type),
                                 MESH_MAP_BAKE_RESOLUTION_MIN,
                                 MESH_MAP_BAKE_RESOLUTION_MAX);
    pair.margin_pixels = std::clamp(
        int(std::lround(BKE_mesh_maps_bake_slot_margin(ma, p.type))),
        0,
        int(MESH_MAP_BAKE_MARGIN_MAX));
    job->pairs.append(pair);
  }
  return job;
}

/**
 * Run the whole plan on the calling thread: prepare, run and commit each pair in turn. This is the
 * `exec` path, so `bpy.ops.…()` from a script -- and a background run -- bakes synchronously
 * without needing the event loop a wmJob relies on.
 */
wmOperatorStatus mesh_map_bake_run_sync(MeshMapBakeJob *job)
{
  if (job == nullptr) {
    return OPERATOR_CANCELLED;
  }
  /* Cycles reads #G.is_break to end a sample early when the user asks to stop, and honours
   * #G.is_rendering as the flag that gives it that permission. The synchronous path runs on the
   * main thread without a wmJob, so nothing else would set them. */
  const bool was_rendering = G.is_rendering;
  G.is_break = false;
  G.is_rendering = true;
  for (;;) {
    if (!mesh_map_bake_prepare_next(*job)) {
      break;
    }
    mesh_map_bake_worker_run(job->world);
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
  /* Match the full-bake job: mark the app as rendering so Cycles sees #G.is_break as the kill
   * signal, and let #mesh_map_bake_free reset both when the job ends. */
  G.is_break = false;
  G.is_rendering = true;
  WM_jobs_start(CTX_wm_manager(C), wm_job);
  return OPERATOR_FINISHED;
}

/**
 * An empty \a requested means the operator's default: every map the bake can produce.
 */
uint32_t mesh_map_bake_resolve_type_mask(const uint32_t requested)
{
  return requested == 0 ? MESH_MAP_BAKE_TYPE_MASK_ALL : requested;
}

/** Validate the context and build the job for either bake operator. */
MeshMapBakeJob *mesh_map_bake_build_job(bContext *C,
                                        wmOperator *op,
                                        const int8_t object_scope,
                                        const uint32_t type_mask)
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
  if (ma == nullptr || !BKE_paint_layers_is_layered(*ma)) {
    BKE_report(op->reports, RPT_ERROR, "The material slot is not a layered paint material");
    return nullptr;
  }
  if (WM_jobs_test(CTX_wm_manager(C), ma, WM_JOB_TYPE_MESH_MAP_BAKE)) {
    BKE_report(op->reports, RPT_ERROR, "A mesh map bake is already running for this material");
    return nullptr;
  }
  MeshMapBakeJob *job = mesh_map_bake_job_create(*CTX_data_main(C),
                                                 *CTX_data_scene(C),
                                                 *CTX_data_view_layer(C),
                                                 ob,
                                                 *ma,
                                                 object_scope,
                                                 type_mask,
                                                 op->reports);
  if (job != nullptr) {
    /* A negative UID bakes every copy of the included types; zero or more bakes exactly that
     * copy, which the panel's per-slot quick bake uses. */
    const int slot_uid = RNA_int_get(op->ptr, "slot_uid");
    if (slot_uid >= 0) {
      job->pairs.remove_if([&](const MeshMapBakePairState &pair) {
        return pair.slot_uid != int32_t(slot_uid);
      });
      if (job->pairs.is_empty()) {
        BKE_report(op->reports, RPT_ERROR, "No atlas copy matches this bake");
        MEM_delete(job);
        return nullptr;
      }
    }
  }
  return job;
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
  if (CTX_wm_window(C) == nullptr) {
    /* A background run has no event loop to drive a wmJob; the synchronous path does the same
     * work on this thread. */
    return mesh_map_bake_run_sync(
        mesh_map_bake_build_job(C, op, MA_MESH_MAP_BAKE_ACTIVE_OBJECT, type_mask));
  }
  return mesh_map_bake_job_start(
      C, mesh_map_bake_build_job(C, op, MA_MESH_MAP_BAKE_ACTIVE_OBJECT, type_mask));
}

wmOperatorStatus mesh_map_bake_all_exec(bContext *C, wmOperator *op)
{
  const int8_t scope = RNA_enum_get(op->ptr, "object_scope");
  const uint32_t type_mask = mesh_map_bake_resolve_type_mask(
      uint32_t(RNA_enum_get(op->ptr, "types")));
  return mesh_map_bake_run_sync(mesh_map_bake_build_job(C, op, scope, type_mask));
}

wmOperatorStatus mesh_map_bake_all_invoke(bContext *C, wmOperator *op, const wmEvent * /*event*/)
{
  const int8_t scope = RNA_enum_get(op->ptr, "object_scope");
  const uint32_t type_mask = mesh_map_bake_resolve_type_mask(
      uint32_t(RNA_enum_get(op->ptr, "types")));
  if (CTX_wm_window(C) == nullptr) {
    return mesh_map_bake_run_sync(mesh_map_bake_build_job(C, op, scope, type_mask));
  }
  return mesh_map_bake_job_start(C, mesh_map_bake_build_job(C, op, scope, type_mask));
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
  if (ma == nullptr || !BKE_paint_layers_is_layered(*ma)) {
    BKE_report(op->reports, RPT_ERROR, "The material slot is not a layered paint material");
    return OPERATOR_CANCELLED;
  }
  if (WM_jobs_test(CTX_wm_manager(C), ma, WM_JOB_TYPE_MESH_MAP_BAKE)) {
    BKE_report(op->reports, RPT_ERROR, "A mesh map bake is already running for this material");
    return OPERATOR_CANCELLED;
  }
  const uint32_t type_mask = mesh_map_bake_resolve_type_mask(
      uint32_t(RNA_enum_get(op->ptr, "types")));
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
               rna_enum_material_mesh_map_type_items,
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
  RNA_def_int(ot->srna,
              "slot_uid",
              -1,
              -1,
              INT32_MAX,
              "Atlas Copy",
              "Bake only this atlas copy's UID; negative bakes every copy of the type",
              -1,
              INT32_MAX);
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
  RNA_def_int(ot->srna,
              "slot_uid",
              -1,
              -1,
              INT32_MAX,
              "Atlas Copy",
              "Bake only this atlas copy's UID; negative bakes every copy of the type",
              -1,
              INT32_MAX);
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
