/* SPDX-FileCopyrightText: 2006 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * Manages materials, lights and textures.
 */

#include <cstdio>
#include <cstring>

#include "BKE_lib_id.hh"
#include "MEM_guardedalloc.h"

#include "DNA_material_types.h"
#include "DNA_scene_types.h"
#include "DNA_world_types.h"

#include "BLI_listbase.h"
#include "BLI_map.hh"
#include "BLI_math_vector.h"
#include "BLI_string.h"
#include "BLI_time.h"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_colorband.hh"
#include "BKE_node.hh"
#include "BKE_node_runtime.hh"
#include "BKE_paint_layers_gradient.hh"

#include "DNA_node_types.h"

#include "NOD_shader.h"
#include "NOD_shader_nodes_inline.hh"

#include "GPU_material.hh"
#include "GPU_pass.hh"
#include "GPU_shader.hh"
#include "GPU_texture.hh"
#include "GPU_uniform_buffer.hh"

#include "DRW_engine.hh"

#include "gpu_node_graph.hh"

#include "atomic_ops.h"

namespace blender {

static void gpu_material_ramp_texture_build(GPUMaterial *mat);
static void gpu_material_sky_texture_build(GPUMaterial *mat);

/* Structs */
#define MAX_COLOR_BAND 128
#define MAX_GPU_SKIES 8

struct GPUColorBandBuilder {
  float pixels[MAX_COLOR_BAND][CM_TABLE + 1][4];
  int current_layer;
};

struct GPUSkyBuilder {
  float pixels[MAX_GPU_SKIES][GPU_SKY_WIDTH * GPU_SKY_HEIGHT][4];
  int current_layer;
};

/* Where a uniform of the node tree buffer can be re-read from when only its value changed. */
enum eGPUMaterialValueScope {
  /* A node of the material's own node tree (the response nodes live there). */
  GPU_MAT_VALUE_MATERIAL_TREE = 0,
  /* A node of the generated paint-layer tree: the rows' group instances hold the edited values. */
  GPU_MAT_VALUE_PAINT_LAYERS_TREE = 1,
  /* Not a tree a value edit writes: such an entry is never recorded. */
  GPU_MAT_VALUE_UNTRACKED = 2,
};

/** One uniform of the node-tree UBO whose value came from a socket. */
struct GPUMaterialValueEntry {
  /* Byte offset of the entry inside the UBO. */
  size_t offset;
  /* Number of floats the entry stores, i.e. the GPU type of the socket (never a matrix here). */
  GPUType type;
  eGPUMaterialValueScope scope;
  int node_identifier;
  int socket_index;
  /* The consuming node only reads this value: a change can be a buffer update. When false, a change
   * can also change the generated code (a material flag, a different function), so it needs a
   * rebuild. */
  bool safe;
};

/** A generated ColorRamp node of the build, found again in the evaluated tree by its key. */
struct GPUMaterialRampEntry {
  std::string key;
  /* Row of #GPUMaterial::coba_tex holding the band, or -1 when the node reads the stops as uniforms
   * it computed itself (one or two stops), rewritten by #GPUMaterial::ramp_uniforms. */
  int row;
  /* Hashes of the band the build was made from. */
  uint64_t hash;
  uint64_t shape_hash;
};

/** One uniform a ramp's stops became, rewritten by role when the ramp is edited in place. */
struct GPUMaterialRampUniform {
  /* Byte offset of the float(s) this record writes inside the UBO. */
  size_t offset;
  /* The ramp the edit will come from: the key of the generated ColorRamp node. */
  std::string key;
  GPURampUniformRole role;
  /* Number of floats this record writes. */
  int components;
};

struct GPUMaterial {
  /* Contains #gpu::Shader and source code for deferred compilation.
   * Can be shared between materials sharing same node-tree topology. */
  GPUPass *pass = nullptr;
  /* Optimized GPUPass, situationally compiled after initial pass for optimal realtime performance.
   * This shader variant bakes dynamic uniform data as constant. This variant will not use
   * the ubo, and instead bake constants directly into the shader source. */
  GPUPass *optimized_pass = nullptr;

  /* UBOs for this material parameters. */
  gpu::UniformBuf *ubo = nullptr;
  /* Some flags about the nodetree & the needed resources. */
  eGPUMaterialFlag flag = GPU_MATFLAG_UPDATED;
  /* The engine type this material is compiled for. */
  eGPUMaterialEngine engine;
  /* Identify shader variations (shadow, probe, world background...) */
  uint64_t uuid = 0;
  /* Number of generated function. */
  int generated_function_len = 0;

  /* Source material, might be null. */
  Material *source_material = nullptr;
  /* 1D Texture array containing all color bands. */
  gpu::Texture *coba_tex = nullptr;
  /* Builder for coba_tex. */
  GPUColorBandBuilder *coba_builder = nullptr;
  /* 2D Texture array containing all sky textures. */
  gpu::Texture *sky_tex = nullptr;
  /* Builder for sky_tex. */
  GPUSkyBuilder *sky_builder = nullptr;
  /* Low level node graph(s). Also contains resources needed by the material. */
  GPUNodeGraph graph = {};

  bool has_surface_output = false;
  bool has_volume_output = false;
  bool has_displacement_output = false;

  /**
   * Where the value of each uniform of #ubo came from, when it came from a socket. A layered
   * material's edit re-reads those values instead of rebuilding this material; see
   * #GPU_material_values_update. Empty for a material that cannot take the fast path.
   */
  Vector<GPUMaterialValueEntry> value_entries;
  /** The generated ramps of this build; see #GPU_material_ramp_track. */
  Vector<GPUMaterialRampEntry> ramp_entries;
  /**
   * The uniforms the recorded ramps' stops became, when they became uniforms of the nodes' own
   * making (one or two stops); a ramp edit rewrites them by role. Empty for a ramp whose band lives
   * in #coba_tex.
   */
  Vector<GPUMaterialRampUniform> ramp_uniforms;
  /** CPU copy of #ubo's contents, so an update can tell what changed and upload only then. */
  Vector<uchar> ubo_values;
  /** The generated tree's topology hash this build was made for. */
  uint64_t value_generation = 0;
  /** True when the material is layered, i.e. a value edit is expected to reach this build. */
  bool values_required = false;
  /** True when this build recorded where its uniform values come from and can apply an edit. */
  bool values_tracked = false;
  /** While building: where the values written into the inlined tree came from. */
  const Map<const bNodeSocket *, const bNodeSocket *> *value_origin = nullptr;

  /**
   * A previous build kept only so the viewport keeps drawing it while this one compiles. It is
   * never returned by #GPU_material_from_nodetree; #GPU_material_fallback_find hands it out for a
   * queued replacement and #GPU_material_rebuild_begin frees it on the next rebuild.
   */
  bool is_fallback = false;

  std::string name;

  GPUMaterial(eGPUMaterialEngine engine) : engine(engine) {};

  ~GPUMaterial()
  {
    gpu_node_graph_free(&graph);

    if (optimized_pass != nullptr) {
      GPU_pass_release(optimized_pass);
    }
    if (pass != nullptr) {
      GPU_pass_release(pass);
    }
    if (ubo != nullptr) {
      GPU_uniformbuf_free(ubo);
    }
    if (coba_builder != nullptr) {
      MEM_delete(coba_builder);
    }
    if (coba_tex != nullptr) {
      GPU_texture_free(coba_tex);
    }
    if (sky_tex != nullptr) {
      GPU_texture_free(sky_tex);
    }
  }
};

/* Public API */

GPUMaterialFromNodeTreeResult GPU_material_from_nodetree(
    Material *ma,
    bNodeTree *ntree,
    ListBaseT<LinkData> *gpumaterials,
    const char *name,
    eGPUMaterialEngine engine,
    uint64_t shader_uuid,
    bool deferred_compilation,
    GPUCodegenCallbackFn callback,
    void *thunk,
    GPUMaterialPassReplacementCallbackFn pass_replacement_cb)
{
  /* Search if this material is not already compiled. */
  LinkData *cached_link = nullptr;
  for (LinkData &link : *gpumaterials) {
    GPUMaterial *mat = static_cast<GPUMaterial *>(link.data);
    /* A fallback is a previous build the viewport may still draw: never take it for the current
     * one. */
    if (mat->is_fallback) {
      continue;
    }
    if (mat->uuid == shader_uuid && mat->engine == engine) {
      cached_link = &link;
      break;
    }
  }
  if (cached_link != nullptr) {
    GPUMaterial *mat = static_cast<GPUMaterial *>(cached_link->data);
    /* This build may have been carried over from the evaluated copy the update replaced, which a
     * build names. Point it at the copy it is drawn from. */
    mat->source_material = ma;
    /* A layered material keeps this build across an edit of its values: the node tree is unchanged,
     * so only the entries of its uniform buffer move (#GPU_material_values_update). */
    if (GPU_material_values_update(mat, ma)) {
      if (!deferred_compilation) {
        GPU_pass_ensure_its_ready(mat->pass);
      }
      return {mat};
    }
    /* The edit cannot be applied to this build, and drawing it would show the old values. Drop it
     * and build again, the way the material was handled before the value path existed. */
    BLI_remlink(gpumaterials, cached_link);
    MEM_delete(cached_link);
    GPU_material_free_single(mat);
  }

  GPUMaterialFromNodeTreeResult result;

  GPUMaterial *mat = MEM_new<GPUMaterial>(__func__, engine);
  mat->source_material = ma;
  mat->uuid = shader_uuid;
  mat->name = name;
  result.material = mat;

  /* A layered material can take a value edit without a rebuild. Its generated tree names the row
   * values, so a socket written into the inlined tree has to be traced back to it; a build for any
   * other material records nothing and insists on being rebuilt, as before. */
  Map<const bNodeSocket *, const bNodeSocket *> value_origin;
  const uint64_t value_generation = (ma != nullptr) ?
                                        BKE_material_paint_layers_topology_generation(*ma) :
                                        0;
  const bool is_layered = value_generation != 0;
  mat->values_required = is_layered;
  mat->value_generation = value_generation;

  /* Localize tree to create links for reroute and mute. */
  bNodeTree *localtree = bke::node_tree_add_tree(
      nullptr, (StringRef(ntree->id.name) + " Inlined").c_str(), ntree->idname);
  localtree->flag |= NTREE_IS_GPU_SHADER_INTERNAL;
  nodes::InlineShaderNodeTreeParams inline_params;
  inline_params.allow_preserving_repeat_zones = true;
  inline_params.target_engine_ = engine == GPU_MAT_EEVEE ? SHD_OUTPUT_EEVEE : SHD_OUTPUT_ALL;
  if (is_layered) {
    inline_params.value_origin = &value_origin;
    inline_params.unfold_origin_trees.append(ma->nodetree);
    inline_params.unfold_origin_trees.append(ma->paint_layers_tree);
  }
  nodes::inline_shader_node_tree(*ntree, *localtree, inline_params);

  /* A folded value is computed from other values instead of copied from a socket, so the map cannot
   * name a source for it and a re-read would leave it stale. Such a tree asks for a rebuild. */
  /* Only a fold that read a socket a value edit writes is a problem: the same fold over a helper
   * group's own defaults stays right after any edit. */
  bool folded_editable = false;
  if (is_layered) {
    for (const bNodeSocket *origin : inline_params.r_folded_origins) {
      const bNodeTree *origin_tree = &origin->owner_tree();
      if (origin_tree == ma->paint_layers_tree || origin_tree == ma->nodetree) {
        folded_editable = true;
        break;
      }
    }
  }
  mat->values_tracked = is_layered && !folded_editable;

  for (nodes::InlineShaderNodeTreeParams::ErrorMessage &error : inline_params.r_error_messages) {
    result.errors.append({error.node, std::move(error.message)});
  }

  mat->value_origin = mat->values_tracked ? &value_origin : nullptr;
  ntreeGPUMaterialNodes(localtree, mat);
  mat->value_origin = nullptr;

  gpu_material_ramp_texture_build(mat);
  gpu_material_sky_texture_build(mat);

  /* Use default material pass when possible. */
  if (GPUPass *default_pass = pass_replacement_cb ? pass_replacement_cb(thunk, mat) : nullptr) {
    mat->pass = default_pass;
    GPU_pass_acquire(mat->pass);
    /** WORKAROUND:
     * The node tree code is never executed in default replaced passes,
     * but the GPU validation will still complain if the node tree UBO is not bound.
     * So we create a dummy UBO with (at least) the size of the default material one (192 bytes).
     * We allocate 256 bytes to leave some room for future changes. */
    mat->ubo = GPU_uniformbuf_create_ex(256, nullptr, "Dummy UBO");
    /* This variant runs no node tree, so it holds no value an edit could refresh: it must not ask
     * to be rebuilt for one either. */
    mat->values_required = false;
    mat->values_tracked = false;
  }
  else {
    /* Create source code and search pass cache for an already compiled version. */
    mat->pass = GPU_generate_pass(
        mat, &mat->graph, mat->name.c_str(), engine, deferred_compilation, callback, thunk, false);
  }

  /* Determine whether we should generate an optimized variant of the graph.
   * Heuristic is based on complexity of default material pass and shader node graph.
   *
   * Never for a layered material: that variant bakes the uniforms into the shader source, which is
   * exactly what a value edit has to be able to avoid recompiling. */
  if (!mat->values_required && GPU_pass_should_optimize(mat->pass)) {
    mat->optimized_pass = GPU_generate_pass(
        mat, &mat->graph, mat->name.c_str(), engine, true, callback, thunk, true);
  }

  gpu_node_graph_free_nodes(&mat->graph);
  /* Only free after GPU_pass_shader_get where gpu::UniformBuf read data from the local
   * tree. */
  BKE_id_free(nullptr, &localtree->id);

  /* Note that even if building the shader fails in some way, we want to keep
   * it to avoid trying to compile again and again, and simply do not use
   * the actual shader on drawing. */
  LinkData *link = MEM_new_zeroed<LinkData>("GPUMaterialLink");
  link->data = mat;
  BLI_addtail(gpumaterials, link);

  return result;
}

GPUMaterial *GPU_material_from_callbacks(eGPUMaterialEngine engine,
                                         ConstructGPUMaterialFn construct_function_cb,
                                         GPUCodegenCallbackFn generate_code_function_cb,
                                         void *thunk)
{
  /* Allocate a new material and its material graph. */
  GPUMaterial *material = MEM_new<GPUMaterial>(__func__, engine);

  /* Construct the material graph by adding and linking the necessary GPU material nodes. */
  construct_function_cb(thunk, material);

  /* Create and initialize the texture storing color bands used by Ramp and Curve nodes. */
  gpu_material_ramp_texture_build(material);

  /* Lookup an existing pass in the cache or generate a new one. */
  material->pass = GPU_generate_pass(material,
                                     &material->graph,
                                     __func__,
                                     engine,
                                     false,
                                     generate_code_function_cb,
                                     thunk,
                                     false);

  /* Determine whether we should generate an optimized variant of the graph.
   * Heuristic is based on complexity of default material pass and shader node graph. */
  if (GPU_pass_should_optimize(material->pass)) {
    material->optimized_pass = GPU_generate_pass(material,
                                                 &material->graph,
                                                 __func__,
                                                 engine,
                                                 true,
                                                 generate_code_function_cb,
                                                 thunk,
                                                 true);
  }

  gpu_node_graph_free_nodes(&material->graph);

  return material;
}

void GPU_material_free_single(GPUMaterial *material)
{
  MEM_delete(material);
}

void GPU_material_free(ListBaseT<LinkData> *gpumaterial)
{
  for (LinkData &link : *gpumaterial) {
    GPUMaterial *material = static_cast<GPUMaterial *>(link.data);
    GPU_material_free_single(material);
  }
  gpumaterial->free_no_destruct();
}

void GPU_material_rebuild_begin(ListBaseT<LinkData> *gpumaterials)
{
  LinkData *link = static_cast<LinkData *>(gpumaterials->first);
  while (link != nullptr) {
    LinkData *next = link->next;
    GPUMaterial *material = static_cast<GPUMaterial *>(link->data);
    if (material->is_fallback) {
      /* A whole generation behind: the replacement built after it had its chance to compile. */
      BLI_remlink(gpumaterials, link);
      MEM_delete(link);
      GPU_material_free_single(material);
    }
    else {
      /* The build about to be replaced is what the viewport keeps showing until the new one is
       * ready. */
      material->is_fallback = true;
    }
    link = next;
  }
}

const bNodeSocket *GPU_material_uniform_value_origin(const GPUMaterial *mat,
                                                     const bNodeSocket *socket)
{
  if (mat == nullptr || mat->value_origin == nullptr) {
    return nullptr;
  }
  return mat->value_origin->lookup_default(socket, nullptr);
}

/** Read one float of a socket's stored value, without converting between socket types. */
static bool socket_value_read(const bNodeSocket &socket, float r_value[4])
{
  if (socket.default_value == nullptr) {
    return false;
  }
  switch (socket.type) {
    case SOCK_FLOAT:
      r_value[0] = socket.default_value_typed<bNodeSocketValueFloat>()->value;
      return true;
    case SOCK_INT:
      r_value[0] = float(socket.default_value_typed<bNodeSocketValueInt>()->value);
      return true;
    case SOCK_BOOLEAN:
      r_value[0] = socket.default_value_typed<bNodeSocketValueBoolean>()->value ? 1.0f : 0.0f;
      return true;
    case SOCK_VECTOR:
      copy_v3_v3(r_value, socket.default_value_typed<bNodeSocketValueVector>()->value);
      return true;
    case SOCK_RGBA:
      copy_v4_v4(r_value, socket.default_value_typed<bNodeSocketValueRGBA>()->value);
      return true;
    default:
      return false;
  }
}

/** Number of floats a socket of \a type contributes to the buffer, or 0 when it is not a value. */
static int socket_value_components(const eNodeSocketDatatype type)
{
  switch (type) {
    case SOCK_FLOAT:
    case SOCK_INT:
    case SOCK_BOOLEAN:
      return 1;
    case SOCK_VECTOR:
      return 3;
    case SOCK_RGBA:
      return 4;
    default:
      return 0;
  }
}

/** Write the float(s) one ramp uniform record stands for, from \a band's stops. False when the
 * band cannot provide them, which keeps this build on the rebuild path. */
static bool gpu_material_ramp_uniform_write(const GPUMaterialRampUniform &uniform,
                                            const ColorBand &band,
                                            Vector<uchar> &ubo_values,
                                            bool &r_changed)
{
  float value[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  switch (uniform.role) {
    case GPU_RAMP_UNIFORM_MUL_BIAS_0:
    case GPU_RAMP_UNIFORM_MUL_BIAS_1: {
      if (band.tot < 2) {
        return false;
      }
      float mul_bias[2];
      BKE_colorband_valtorgb_mul_bias(band, mul_bias);
      value[0] = (uniform.role == GPU_RAMP_UNIFORM_MUL_BIAS_0) ? mul_bias[0] : mul_bias[1];
      break;
    }
    case GPU_RAMP_UNIFORM_CONST_POS:
      if (band.tot < 2) {
        return false;
      }
      value[0] = BKE_colorband_valtorgb_const_pos(band);
      break;
    case GPU_RAMP_UNIFORM_COLOR_0:
    case GPU_RAMP_UNIFORM_COLOR_SINGLE:
      if (band.tot < 1) {
        return false;
      }
      copy_v4_v4(value, &band.data[0].r);
      break;
    case GPU_RAMP_UNIFORM_COLOR_1:
      if (band.tot < 2) {
        return false;
      }
      copy_v4_v4(value, &band.data[1].r);
      break;
    case GPU_RAMP_UNIFORM_NONE:
      return false;
  }

  const size_t write_size = size_t(uniform.components) * sizeof(float);
  if (memcmp(ubo_values.data() + uniform.offset, value, write_size) != 0) {
    memcpy(ubo_values.data() + uniform.offset, value, write_size);
    r_changed = true;
  }
  return true;
}

/** Rewrite every uniform the ramp \a key's stops became, from \a band. False when a record cannot
 * be honored, which keeps this build on the rebuild path. */
static bool gpu_material_ramp_uniforms_update(GPUMaterial *mat,
                                              const std::string &key,
                                              const ColorBand &band,
                                              bool &r_changed)
{
  bool found = false;
  for (const GPUMaterialRampUniform &uniform : mat->ramp_uniforms) {
    if (uniform.key != key) {
      continue;
    }
    found = true;
    if (!gpu_material_ramp_uniform_write(uniform, band, mat->ubo_values, r_changed)) {
      return false;
    }
  }
  /* An entry whose uniforms were never recorded cannot take the edit: writing nothing would leave
   * the drawn pixels stale. */
  return found;
}

bool GPU_material_values_update(GPUMaterial *mat, Material *ma)
{
  if (!mat->values_required) {
    /* Not a layered material: no value edit can reach this build, and the caller keeps whatever it
     * asked for. */
    return true;
  }
  if (!mat->values_tracked) {
    /* A build that cannot be trusted for values never outlives the update that changed them:
     * #GPU_materials_values_supported then refuses, and #BKE_material_eval rebuilds. Between two
     * updates its values are therefore current. Dropping it here would rebuild the material on every
     * request of the draw engine without ever getting a build that can be tracked. One still
     * compiling is left to the rebuild: handing it out has not been checked against the draw
     * engine's wait for a pass. */
    return GPU_material_status(mat) == GPU_MAT_SUCCESS;
  }
  if (ma == nullptr || (mat->value_entries.is_empty() && mat->ramp_uniforms.is_empty()) ||
      mat->ubo == nullptr || mat->ubo_values.is_empty())
  {
    return false;
  }
  /* A rebuild replaces the generated tree, and with it every socket this build names. */
  if (mat->value_generation != BKE_material_paint_layers_topology_generation(*ma)) {
    return false;
  }

  const bNodeTree *trees[] = {ma->nodetree, ma->paint_layers_tree};
  bool changed = false;
  for (const GPUMaterialValueEntry &entry : mat->value_entries) {
    if (entry.scope != GPU_MAT_VALUE_MATERIAL_TREE &&
        entry.scope != GPU_MAT_VALUE_PAINT_LAYERS_TREE)
    {
      /* An entry must name one of the two trees; anything else is a build this cannot serve. */
      mat->values_tracked = false;
      break;
    }
    const bNodeTree *tree = trees[entry.scope];
    const bNode *node = nullptr;
    if (tree != nullptr) {
      tree->ensure_topology_cache();
      node = tree->node_by_id(entry.node_identifier);
    }
    const bNodeSocket *socket = (node != nullptr) ?
                                    static_cast<const bNodeSocket *>(
                                        BLI_findlink(&node->inputs, entry.socket_index)) :
                                    nullptr;
    float value[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if (socket == nullptr || !socket_value_read(*socket, value)) {
      /* The socket cannot be read back, so what changed cannot be told. Stop trusting this build
       * for values, but keep it: dropping it would rebuild the material on every frame without
       * ever getting a build that can be tracked. */
      mat->values_tracked = false;
      break;
    }
    const size_t entry_size = size_t(entry.type) * sizeof(float);
    const bool entry_changed = memcmp(mat->ubo_values.data() + entry.offset, value, entry_size) !=
                               0;
    if (!entry_changed) {
      continue;
    }
    if (!entry.safe) {
      /* This value reaches a node that picks its code or its material flags from it: only a rebuild
       * can apply it. The build that replaces this one bakes the new value, so this happens once
       * per such edit. */
      return false;
    }
    memcpy(mat->ubo_values.data() + entry.offset, value, entry_size);
    changed = true;
  }

  for (GPUMaterialRampEntry &ramp : mat->ramp_entries) {
    const ColorBand *band = BKE_paint_layers_ramp_find(*ma, ramp.key.c_str());
    if (band == nullptr) {
      return false;
    }
    const uint64_t hash = BKE_paint_layers_gradient_ramp_hash(*band);
    if (hash == ramp.hash) {
      continue;
    }
    /* A band that picked another shader function needs the rebuild; the one that follows bakes the
     * new shape. */
    if (BKE_paint_layers_gradient_ramp_shape_hash(*band) != ramp.shape_hash) {
      return false;
    }
    if (ramp.row < 0) {
      /* A ramp whose stops became uniforms of its own making: rewrite those uniforms by role. */
      if (!gpu_material_ramp_uniforms_update(mat, ramp.key, *band, changed)) {
        return false;
      }
      ramp.hash = hash;
      continue;
    }
    if (mat->coba_tex == nullptr) {
      return false;
    }
    float *pixels = nullptr;
    int size = 0;
    BKE_colorband_evaluate_table_rgba(band, &pixels, &size);
    GPU_texture_update_sub(
        mat->coba_tex, GPU_DATA_FLOAT, pixels, 0, ramp.row, 0, size, 1, 1);
    MEM_delete(pixels);
    ramp.hash = hash;
  }

  if (changed) {
    GPU_uniformbuf_update(mat->ubo, mat->ubo_values.data());
  }
  return true;
}

void GPU_material_ramp_track(GPUMaterial *mat, const bNode *node, const ColorBand *band, int row)
{
  if (!mat->values_required || node == nullptr) {
    return;
  }
  const char *key = BKE_paint_layers_ramp_node_key(*node);
  if (key == nullptr) {
    return;
  }
  mat->ramp_entries.append({key,
                            row,
                            BKE_paint_layers_gradient_ramp_hash(*band),
                            BKE_paint_layers_gradient_ramp_shape_hash(*band)});
}

GPUNodeLink *GPU_uniform_ramp(GPUMaterial *mat,
                              const bNode *node,
                              const float *value,
                              const GPURampUniformRole role)
{
  GPUNodeLink *link = GPU_uniform(value);
  /* Only a layered material re-reads its uniforms on a ramp edit, and only a node the generator
   * keyed can be found again by one. Anything else stays the plain uniform, as before. */
  if (mat == nullptr || !mat->values_required || node == nullptr ||
      role == GPU_RAMP_UNIFORM_NONE)
  {
    return link;
  }
  link->ramp_key = BKE_paint_layers_ramp_node_key(*node);
  link->ramp_role = (link->ramp_key != nullptr) ? role : GPU_RAMP_UNIFORM_NONE;
  return link;
}

GPUMaterial *GPU_material_fallback_find(ListBaseT<LinkData> *gpumaterials,
                                        const eGPUMaterialEngine engine,
                                        const uint64_t uuid,
                                        Material *ma)
{
  if (gpumaterials == nullptr) {
    return nullptr;
  }
  for (LinkData &link : *gpumaterials) {
    GPUMaterial *material = static_cast<GPUMaterial *>(link.data);
    if (!material->is_fallback || material->engine != engine || material->uuid != uuid) {
      continue;
    }
    if (GPU_material_status(material) == GPU_MAT_SUCCESS) {
      /* A fallback was carried across the update that replaced it: name the copy it is drawn
       * from. */
      material->source_material = ma;
      return material;
    }
  }
  return nullptr;
}

bool GPU_materials_values_supported(ListBaseT<LinkData> *gpumaterials)
{
  if (gpumaterials == nullptr || gpumaterials->is_empty()) {
    return false;
  }
  for (LinkData &link : *gpumaterials) {
    const GPUMaterial *material = static_cast<const GPUMaterial *>(link.data);
    if (!material->values_required) {
      /* A build no value edit can reach (a replaced default pass): nothing to keep it for. */
      continue;
    }
    if (!material->values_tracked) {
      return false;
    }
  }
  return true;
}

void GPU_materials_free(Main *bmain)
{
  for (Material &ma : bmain->materials) {
    GPU_material_free(&ma.gpumaterial);
  }

  for (World &wo : bmain->worlds) {
    GPU_material_free(&wo.gpumaterial);
  }

  BKE_material_defaults_free_gpu();
}

const char *GPU_material_get_name(GPUMaterial *material)
{
  return material->name.c_str();
}

uint64_t GPU_material_uuid_get(GPUMaterial *mat)
{
  return mat->uuid;
}

Material *GPU_material_get_material(GPUMaterial *material)
{
  return material->source_material;
}

GPUPass *GPU_material_get_pass(GPUMaterial *material)
{
  /* If an optimized pass variant is available, and optimization is
   * flagged as complete, we use this one instead. */
  return GPU_material_optimization_status(material) == GPU_MAT_OPTIMIZATION_SUCCESS ?
             material->optimized_pass :
             material->pass;
}

gpu::Shader *GPU_material_get_shader(GPUMaterial *material)
{
  return GPU_pass_shader_get(GPU_material_get_pass(material));
}

GPUMaterialStatus GPU_material_status(GPUMaterial *mat)
{
  switch (GPU_pass_status(mat->pass)) {
    case GPU_PASS_SUCCESS:
      return GPU_MAT_SUCCESS;
    case GPU_PASS_QUEUED:
      return GPU_MAT_QUEUED;
    default:
      return GPU_MAT_FAILED;
  }
}

eGPUMaterialOptimizationStatus GPU_material_optimization_status(GPUMaterial *mat)
{
  if (!GPU_pass_should_optimize(mat->pass) || mat->optimized_pass == nullptr) {
    return GPU_MAT_OPTIMIZATION_SKIP;
  }

  switch (GPU_pass_status(mat->optimized_pass)) {
    case GPU_PASS_SUCCESS:
      return GPU_MAT_OPTIMIZATION_SUCCESS;
    case GPU_PASS_QUEUED:
      return GPU_MAT_OPTIMIZATION_QUEUED;
    default:
      BLI_assert_unreachable();
      return GPU_MAT_OPTIMIZATION_SKIP;
  }
}

uint64_t GPU_material_compilation_timestamp(GPUMaterial *mat)
{
  return GPU_pass_compilation_timestamp(mat->pass);
}

bool GPU_material_has_surface_output(GPUMaterial *mat)
{
  return mat->has_surface_output;
}

bool GPU_material_has_volume_output(GPUMaterial *mat)
{
  return mat->has_volume_output;
}

bool GPU_material_has_displacement_output(GPUMaterial *mat)
{
  return mat->has_displacement_output;
}

bool GPU_material_flag_get(const GPUMaterial *mat, eGPUMaterialFlag flag)
{
  return (mat->flag & flag) != 0;
}

eGPUMaterialFlag GPU_material_flag(const GPUMaterial *mat)
{
  return mat->flag;
}

void GPU_material_flag_set(GPUMaterial *mat, eGPUMaterialFlag flag)
{
  if ((flag & GPU_MATFLAG_GLOSSY) && (mat->flag & GPU_MATFLAG_GLOSSY)) {
    /* Tag material using multiple glossy BSDF as using clear coat. */
    mat->flag |= GPU_MATFLAG_COAT;
  }
  mat->flag |= flag;
}

void GPU_material_uniform_buffer_create(GPUMaterial *material, ListBaseT<LinkData> *inputs)
{
  Vector<GPUUniformBufSlot> slots;
  material->ubo = GPU_uniformbuf_create_from_list(
      inputs, material->name.c_str(), &slots, &material->ubo_values);
  material->value_entries.clear();
  material->ramp_uniforms.clear();

  if (!material->values_tracked || material->ubo == nullptr || material->source_material == nullptr)
  {
    return;
  }

  /* Both trees the values can come from, in the order of #eGPUMaterialValueScope. */
  const Material &ma = *material->source_material;

  material->value_entries.reserve(int64_t(slots.size()));
  for (const GPUUniformBufSlot &slot : slots) {
    const GPUInput *input = slot.input;
    if (input->ramp_role != GPU_RAMP_UNIFORM_NONE) {
      /* A ramp's stops became this uniform: a ramp edit rewrites it by role, not by socket. The
       * mul/bias pair the linear and ease functions read is one uniform, recorded as one float per
       * role, so an edit can replace either. */
      BLI_assert(input->ramp_key != nullptr);
      BLI_assert_msg(input->ramp_role != GPU_RAMP_UNIFORM_MUL_BIAS_0 || input->type == GPU_VEC2,
                     "The mul/bias pair is always a vec2 of the linear and ease functions");
      /* A color or the constant edge fills the whole slot; only the mul/bias pair is split. */
      const int components = (input->ramp_role == GPU_RAMP_UNIFORM_MUL_BIAS_0) ? 1 :
                                                                                 int(input->type);
      material->ramp_uniforms.append({slot.offset, input->ramp_key, input->ramp_role, components});
      if (input->ramp_role == GPU_RAMP_UNIFORM_MUL_BIAS_0) {
        material->ramp_uniforms.append(
            {slot.offset + sizeof(float), input->ramp_key, GPU_RAMP_UNIFORM_MUL_BIAS_1, 1});
      }
      continue;
    }
    const bNodeSocket *origin = input->origin;
    if (origin == nullptr) {
      /* A value the node code made up (a mapping matrix, a baked constant): a value edit never
       * writes it, so it cannot go stale. */
      continue;
    }
    const bNodeTree &origin_tree = origin->owner_tree();
    const eGPUMaterialValueScope scope = (&origin_tree == ma.paint_layers_tree) ?
                                             GPU_MAT_VALUE_PAINT_LAYERS_TREE :
                                             ((&origin_tree == ma.nodetree) ?
                                                  GPU_MAT_VALUE_MATERIAL_TREE :
                                                  GPU_MAT_VALUE_UNTRACKED);
    if (scope == GPU_MAT_VALUE_UNTRACKED) {
      /* A socket of a helper group (a layer's own nodes, the blend and gradient groups): a value
       * edit writes the instances in the generated root and the material's own nodes, never these,
       * so the uniform made from it cannot go stale. */
      continue;
    }
    const int components = socket_value_components(eNodeSocketDatatype(origin->type));
    /* A uniform made from a node stack holds the first `type` floats of the stack's vector, which
     * starts zeroed and takes the socket's own components. A socket of another width than the
     * uniform (a float read as a vec4, a color read as a vec3) therefore round-trips through
     * #GPU_material_values_update unchanged: the shader converts the type after the buffer, not
     * before it. Only a socket that does not read back as a value at all cannot be served. */
    if (components == 0 || int(input->type) > 4) {
      /* The value of a socket an edit does write cannot be re-read faithfully. Rather than risk a
       * stale pixel, keep the material on the full path. */
      material->values_tracked = false;
      material->value_entries.clear();
      material->ubo_values.clear();
      return;
    }
    material->value_entries.append(GPUMaterialValueEntry{slot.offset,
                                                         input->type,
                                                         scope,
                                                         origin->owner_node().identifier,
                                                         origin->index(),
                                                         input->origin_safe});
  }

  if (material->value_entries.is_empty() && material->ramp_uniforms.is_empty()) {
    /* No uniform could be traced back to a socket or a ramp: nothing to re-read, so this build
     * cannot take an edit in place. Say so once instead of failing on every frame. */
    material->values_tracked = false;
    material->ubo_values.clear();
  }
}

gpu::UniformBuf *GPU_material_uniform_buffer_get(GPUMaterial *material)
{
  return material->ubo;
}

ListBaseT<GPUMaterialAttribute> GPU_material_attributes(const GPUMaterial *material)
{
  return material->graph.attributes;
}

ListBaseT<GPUMaterialTexture> GPU_material_textures(GPUMaterial *material)
{
  return material->graph.textures;
}

const GPUUniformAttrList *GPU_material_uniform_attributes(const GPUMaterial *material)
{
  const GPUUniformAttrList *attrs = &material->graph.uniform_attrs;
  return attrs->count > 0 ? attrs : nullptr;
}

const ListBaseT<GPULayerAttr> *GPU_material_layer_attributes(const GPUMaterial *material)
{
  const ListBaseT<GPULayerAttr> *attrs = &material->graph.layer_attrs;
  return !attrs->is_empty() ? attrs : nullptr;
}

GPUNodeGraph *gpu_material_node_graph(GPUMaterial *material)
{
  return &material->graph;
}

/* Resources */

gpu::Texture **gpu_material_sky_texture_layer_set(
    GPUMaterial *mat, int width, int height, const float *pixels, float *row)
{
  /* In order to put all sky textures into one 2D array texture,
   * we need them to be the same size. */
  BLI_assert(width == GPU_SKY_WIDTH);
  BLI_assert(height == GPU_SKY_HEIGHT);
  UNUSED_VARS_NDEBUG(width, height);

  if (mat->sky_builder == nullptr) {
    mat->sky_builder = MEM_new_uninitialized<GPUSkyBuilder>("GPUSkyBuilder");
    mat->sky_builder->current_layer = 0;
  }

  int layer = mat->sky_builder->current_layer;
  *row = float(layer);

  if (*row == MAX_GPU_SKIES) {
    printf("Too many sky textures in shader!\n");
  }
  else {
    float *dst = reinterpret_cast<float *>(mat->sky_builder->pixels[layer]);
    memcpy(dst, pixels, sizeof(float) * GPU_SKY_WIDTH * GPU_SKY_HEIGHT * 4);
    mat->sky_builder->current_layer += 1;
  }

  return &mat->sky_tex;
}

gpu::Texture **gpu_material_ramp_texture_row_set(GPUMaterial *mat,
                                                 int size,
                                                 const float *pixels,
                                                 float *r_row)
{
  /* In order to put all the color-bands into one 1D array texture,
   * we need them to be the same size. */
  BLI_assert(size == CM_TABLE + 1);
  UNUSED_VARS_NDEBUG(size);

  if (mat->coba_builder == nullptr) {
    mat->coba_builder = MEM_new_uninitialized<GPUColorBandBuilder>("GPUColorBandBuilder");
    mat->coba_builder->current_layer = 0;
  }

  int layer = mat->coba_builder->current_layer;
  *r_row = float(layer);

  if (*r_row == MAX_COLOR_BAND) {
    printf("Too many color band in shader! Remove some Curve, Black Body or Color Ramp Node.\n");
  }
  else {
    float *dst = reinterpret_cast<float *>(mat->coba_builder->pixels[layer]);
    memcpy(dst, pixels, sizeof(float) * (CM_TABLE + 1) * 4);
    mat->coba_builder->current_layer += 1;
  }

  return &mat->coba_tex;
}

static void gpu_material_ramp_texture_build(GPUMaterial *mat)
{
  if (mat->coba_builder == nullptr) {
    return;
  }

  GPUColorBandBuilder *builder = mat->coba_builder;

  mat->coba_tex = GPU_texture_create_1d_array("mat_ramp",
                                              CM_TABLE + 1,
                                              builder->current_layer,
                                              1,
                                              gpu::TextureFormat::SFLOAT_16_16_16_16,
                                              GPU_TEXTURE_USAGE_SHADER_READ,
                                              reinterpret_cast<float *>(builder->pixels));

  MEM_delete(builder);
  mat->coba_builder = nullptr;
}

static void gpu_material_sky_texture_build(GPUMaterial *mat)
{
  if (mat->sky_builder == nullptr) {
    return;
  }

  mat->sky_tex = GPU_texture_create_2d_array("mat_sky",
                                             GPU_SKY_WIDTH,
                                             GPU_SKY_HEIGHT,
                                             mat->sky_builder->current_layer,
                                             1,
                                             gpu::TextureFormat::SFLOAT_32_32_32_32,
                                             GPU_TEXTURE_USAGE_SHADER_READ,
                                             reinterpret_cast<float *>(mat->sky_builder->pixels));

  MEM_delete(mat->sky_builder);
  mat->sky_builder = nullptr;
}

/* Code generation */

void GPU_material_output_surface(GPUMaterial *material, GPUNodeLink *link)
{
  if (!material->graph.outlink_surface) {
    material->graph.outlink_surface = link;
    material->has_surface_output = true;
  }
}

void GPU_material_output_volume(GPUMaterial *material, GPUNodeLink *link)
{
  if (!material->graph.outlink_volume) {
    material->graph.outlink_volume = link;
    material->has_volume_output = true;
  }
}

void GPU_material_output_displacement(GPUMaterial *material, GPUNodeLink *link)
{
  if (!material->graph.outlink_displacement) {
    material->graph.outlink_displacement = link;
    material->has_displacement_output = true;
  }
}

void GPU_material_output_thickness(GPUMaterial *material, GPUNodeLink *link)
{
  if (!material->graph.outlink_thickness) {
    material->graph.outlink_thickness = link;
  }
}

void GPU_material_add_output_link_aov(GPUMaterial *material, GPUNodeLink *link, int hash)
{
  GPUNodeGraphOutputLink *aov_link = MEM_new_zeroed<GPUNodeGraphOutputLink>(__func__);
  aov_link->outlink = link;
  aov_link->hash = hash;
  BLI_addtail(&material->graph.outlink_aovs, aov_link);
}

void GPU_material_add_output_link_composite(GPUMaterial *material, GPUNodeLink *link)
{
  GPUNodeGraphOutputLink *compositor_link = MEM_new_zeroed<GPUNodeGraphOutputLink>(__func__);
  compositor_link->outlink = link;
  BLI_addtail(&material->graph.outlink_compositor, compositor_link);
}

char *GPU_material_split_sub_function(GPUMaterial *material,
                                      GPUType return_type,
                                      GPUNodeLink **link)
{
  /* Force cast to return type. */
  switch (return_type) {
    case GPU_FLOAT:
      GPU_link(material, "set_value", *link, link);
      break;
    case GPU_VEC3:
      GPU_link(material, "set_rgb", *link, link);
      break;
    case GPU_VEC4:
      GPU_link(material, "set_rgba", *link, link);
      break;
    default:
      BLI_assert(0);
      break;
  }

  GPUNodeGraphFunctionLink *func_link = MEM_new_zeroed<GPUNodeGraphFunctionLink>(__func__);
  func_link->outlink = *link;
  SNPRINTF(func_link->name, "ntree_fn%d", material->generated_function_len++);
  BLI_addtail(&material->graph.material_functions, func_link);

  return func_link->name;
}

}  // namespace blender
