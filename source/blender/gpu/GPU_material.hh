/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include <string>

#include "BLI_enum_flags.hh"
#include "BLI_math_base.h"
#include "BLI_set.hh"

#include "DNA_customdata_types.h" /* for eCustomDataType */
#include "DNA_image_types.h"
#include "DNA_listBase.h"

#include "GPU_shader.hh"  /* for GPUShaderCreateInfo */
#include "GPU_texture.hh" /* for GPUSamplerState */

namespace blender {

struct GHash;
struct GPUMaterial;
struct GPUInput;
struct GPUNodeLink;
struct GPUNodeStack;
struct GPUPass;
struct ColorBand;
namespace gpu {
class Texture;
class UniformBuf;
}  // namespace gpu
struct Image;
struct ImageUser;
struct Main;
struct Material;
struct Scene;
struct bNode;
struct bNodeSocket;
struct bNodeTree;

/**
 * High level functions to create and use GPU materials.
 */

enum eGPUMaterialEngine {
  GPU_MAT_EEVEE,
  GPU_MAT_COMPOSITOR,
  GPU_MAT_ENGINE_MAX,
};

enum GPUMaterialStatus {
  GPU_MAT_FAILED = 0,
  GPU_MAT_QUEUED,
  GPU_MAT_SUCCESS,
};

/* GPU_MAT_OPTIMIZATION_SKIP for cases where we do not
 * plan to perform optimization on a given material. */
enum eGPUMaterialOptimizationStatus {
  GPU_MAT_OPTIMIZATION_SKIP = 0,
  GPU_MAT_OPTIMIZATION_QUEUED,
  GPU_MAT_OPTIMIZATION_SUCCESS,
};

enum eGPUMaterialFlag {
  GPU_MATFLAG_DIFFUSE = (1 << 0),
  GPU_MATFLAG_SUBSURFACE = (1 << 1),
  GPU_MATFLAG_GLOSSY = (1 << 2),
  GPU_MATFLAG_REFRACT = (1 << 3),
  GPU_MATFLAG_EMISSION = (1 << 4),
  GPU_MATFLAG_TRANSPARENT = (1 << 5),
  GPU_MATFLAG_HOLDOUT = (1 << 6),
  GPU_MATFLAG_SHADER_TO_RGBA = (1 << 7),
  GPU_MATFLAG_AO = (1 << 8),
  /* Signals the presence of multiple reflection closures. */
  GPU_MATFLAG_COAT = (1 << 9),
  GPU_MATFLAG_TRANSLUCENT = (1 << 10),
  GPU_MATFLAG_RAYCAST = (1 << 11),

  GPU_MATFLAG_VOLUME_SCATTER = (1 << 16),
  GPU_MATFLAG_VOLUME_ABSORPTION = (1 << 17),

  GPU_MATFLAG_OBJECT_INFO = (1 << 18),
  GPU_MATFLAG_AOV = (1 << 19),

  GPU_MATFLAG_BARYCENTRIC = (1 << 20),
  /* Signals that these specific closures might *not* be colorless.
   * If this flag is not set, all closures are ensured to not be tinted. */
  GPU_MATFLAG_REFLECTION_MAYBE_COLORED = (1 << 21),
  GPU_MATFLAG_REFRACTION_MAYBE_COLORED = (1 << 22),
  GPU_MATFLAG_TRANSPARENT_MAYBE_COLORED = (1 << 23),

  /* Set if the material uses the "Is Diffuse / Glossy Ray" output of the light path node. */
  GPU_MATFLAG_IS_DIFFUSE_OR_GLOSSY_RAY_FLAG = (1 << 24),

  /* Signals scene time use. */
  GPU_MATFLAG_SCENE_TIME = (1 << 25),

  /* Tells the render engine the material was just compiled or updated. */
  GPU_MATFLAG_UPDATED = (1 << 29),
};
ENUM_OPERATORS(eGPUMaterialFlag);

using GPUCodegenCallbackFn = void (*)(void *thunk,
                                      GPUMaterial *mat,
                                      struct GPUCodegenOutput *codegen);
/**
 * Should return an already compiled pass if it's functionally equivalent to the one being
 * compiled.
 */
using GPUMaterialPassReplacementCallbackFn = GPUPass *(*)(void *thunk, GPUMaterial *mat);

struct GPUMaterialFromNodeTreeResult {
  GPUMaterial *material = nullptr;

  struct Error {
    const bNode *node;
    std::string message;
  };
  Vector<Error> errors;
};

/** WARNING: gpumaterials thread safety must be ensured by the caller. */
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
    GPUMaterialPassReplacementCallbackFn pass_replacement_cb = nullptr);

/* A callback passed to GPU_material_from_callbacks to construct the material graph by adding and
 * linking the necessary GPU material nodes. */
using ConstructGPUMaterialFn = void (*)(void *thunk, GPUMaterial *material);

/* Construct a GPU material from a set of callbacks. See the callback types for more information.
 * The given thunk will be passed as the first parameter of each callback. */
GPUMaterial *GPU_material_from_callbacks(eGPUMaterialEngine engine,
                                         ConstructGPUMaterialFn construct_function_cb,
                                         GPUCodegenCallbackFn generate_code_function_cb,
                                         void *thunk);

void GPU_material_free_single(GPUMaterial *material);
void GPU_material_free(ListBaseT<LinkData> *gpumaterial);

/**
 * Prepare \a gpumaterials for a rebuild: entries already marked as the previous build's fallback
 * are freed, the rest are marked. A fallback is a compiled material kept so a viewport can keep
 * drawing it while its replacement compiles, instead of falling back to the default material.
 */
void GPU_material_rebuild_begin(ListBaseT<LinkData> *gpumaterials);

/**
 * Re-read the uniform values a layered material's edits changed and upload them into the node-tree
 * uniform buffer of \a mat, instead of rebuilding the material.
 *
 * \return false when the edit cannot be covered this way (a value that only a rebuild can apply, a
 * socket that no longer resolves, a material built for a different node tree); the caller then has
 * to take the full path.
 */
bool GPU_material_values_update(GPUMaterial *mat, Material *ma);

/**
 * Whether every build in \a gpumaterials can apply a value edit to its uniform buffer. False when
 * there is no build yet, or when one of them has to be rebuilt for such an edit.
 */
bool GPU_materials_values_supported(ListBaseT<LinkData> *gpumaterials);

/** A compiled fallback in \a gpumaterials for the pipeline \a uuid, or null. */
GPUMaterial *GPU_material_fallback_find(ListBaseT<LinkData> *gpumaterials,
                                        eGPUMaterialEngine engine,
                                        uint64_t uuid,
                                        Material *ma);

void GPU_materials_free(Main *bmain);

GPUPass *GPU_material_get_pass(GPUMaterial *material);
/** Return the most optimal shader configuration for the given material. */
gpu::Shader *GPU_material_get_shader(GPUMaterial *material);

const char *GPU_material_get_name(GPUMaterial *material);

/**
 * Return can be null if it's a world material.
 */
Material *GPU_material_get_material(GPUMaterial *material);

/**
 * The socket whose stored value \a socket's uniform was made from, while a material is being built;
 * null afterwards, and for a value that is not a plain copy of one.
 */
const bNodeSocket *GPU_material_uniform_value_origin(const GPUMaterial *mat,
                                                     const bNodeSocket *socket);
/**
 * Return true if the material compilation has not yet begin or begin.
 */
GPUMaterialStatus GPU_material_status(GPUMaterial *mat);

/**
 * Return status for asynchronous optimization jobs.
 */
eGPUMaterialOptimizationStatus GPU_material_optimization_status(GPUMaterial *mat);

uint64_t GPU_material_compilation_timestamp(GPUMaterial *mat);

gpu::UniformBuf *GPU_material_uniform_buffer_get(GPUMaterial *material);
/**
 * Create dynamic UBO from parameters
 *
 * \param inputs: Items are #LinkData, data is #GPUInput (`BLI_genericNodeN(GPUInput)`).
 */
void GPU_material_uniform_buffer_create(GPUMaterial *material, ListBaseT<LinkData> *inputs);

bool GPU_material_has_surface_output(GPUMaterial *mat);
bool GPU_material_has_volume_output(GPUMaterial *mat);
bool GPU_material_has_displacement_output(GPUMaterial *mat);

bool GPU_material_flag_get(const GPUMaterial *mat, eGPUMaterialFlag flag);

uint64_t GPU_material_uuid_get(GPUMaterial *mat);

struct GPULayerAttr {
  GPULayerAttr *next, *prev;

  /* Meaningful part of the attribute set key. */
  char name[256]; /* Multiple MAX_CUSTOMDATA_LAYER_NAME */
  /** Hash of `name[68]`. */
  uint32_t hash_code;

  /* Helper fields used by code generation. */
  int users;
};

const ListBaseT<GPULayerAttr> *GPU_material_layer_attributes(const GPUMaterial *material);

/* Requested Material Attributes and Textures */

enum GPUType {
  /* Keep in sync with GPU_DATATYPE_STR */
  /* The value indicates the number of elements in each type */
  GPU_NONE = 0,
  GPU_FLOAT = 1,
  GPU_VEC2 = 2,
  GPU_VEC3 = 3,
  GPU_VEC4 = 4,
  GPU_MAT3 = 9,
  GPU_MAT4 = 16,
  GPU_MAX_CONSTANT_DATA = GPU_MAT4,

  /* Values not in GPU_DATATYPE_STR */
  GPU_TEX1D_ARRAY = 1001,
  GPU_TEX2D = 1002,
  GPU_TEX2D_ARRAY = 1003,
  GPU_TEX3D = 1004,

  /* GLSL Struct types */
  GPU_CLOSURE = 1007,

  /* Opengl Attributes */
  GPU_ATTR = 3001,
};

enum GPUDefaultValue {
  GPU_DEFAULT_0 = 0,
  GPU_DEFAULT_1,
};

struct GPUMaterialAttribute {
  GPUMaterialAttribute *next, *prev;
  int type; /* eCustomDataType */
  char name[/*MAX_CUSTOMDATA_LAYER_NAME*/ 68];
  char input_name[/*GPU_MAX_SAFE_ATTR_NAME + 1*/ 12 + 1];
  GPUType gputype;
  GPUDefaultValue default_value; /* Only for volumes attributes. */
  int id;
  int users;
  /**
   * If true, the corresponding attribute is the specified default color attribute on the mesh,
   * if it exists. In that case the type and name data can vary per geometry, so it will not be
   * valid here.
   */
  bool is_default_color;
  /**
   * If true, the attribute is the length of hair particles and curves.
   */
  bool is_hair_length;
  /**
   * If true, the attribute is the intercept of hair particles and curves.
   */
  bool is_hair_intercept;
};

struct GPUMaterialTexture {
  GPUMaterialTexture *next, *prev;
  Image *ima;
  ImageUser iuser;
  bool iuser_available;
  gpu::Texture **colorband;
  gpu::Texture **sky;
  char sampler_name[32];       /* Name of sampler in GLSL. */
  char tiled_mapping_name[32]; /* Name of tile mapping sampler in GLSL. */
  int users;
  GPUSamplerState sampler_state;
};

ListBaseT<GPUMaterialAttribute> GPU_material_attributes(const GPUMaterial *material);
ListBaseT<GPUMaterialTexture> GPU_material_textures(GPUMaterial *material);

struct GPUUniformAttr {
  GPUUniformAttr *next, *prev;

  /* Meaningful part of the attribute set key. */
  char name[/*MAX_CUSTOMDATA_LAYER_NAME*/ 68];
  /** Hash of `name[MAX_CUSTOMDATA_LAYER_NAME] + use_dupli`. */
  uint32_t hash_code;
  bool use_dupli;

  /* Helper fields used by code generation. */
  short id;
  int users;
};

struct GPUUniformAttrList {
  ListBaseT<GPUUniformAttr> list;

  /* List length and hash code precomputed for fast lookup and comparison. */
  unsigned int count, hash_code;
};

const GPUUniformAttrList *GPU_material_uniform_attributes(const GPUMaterial *material);

/* Functions to create GPU Materials nodes. */
/* TODO: Move to its own header. */

struct GPUNodeStack {
  GPUType type;
  float vec[4];
  GPUNodeLink *link;
  bool hasinput;
  bool hasoutput;
  short sockettype;
  bool end;
  /**
   * The socket whose stored value #vec holds and whose node reads it as a value, while a material
   * is being built. A uniform made from such a value keeps the socket, so an edit of it can rewrite
   * the uniform buffer of the built material instead of rebuilding it.
   */
  const bNodeSocket *origin;
  /** Whether the node reading this value only uses it as a value; see
   * #GPU_material_node_is_value_only. */
  bool origin_safe;

  /* Return true if the socket might contain a polychromatic value.
   * This is a conservative heuristic that allows for optimization. */
  bool might_be_tinted() const
  {
    return this->link || (this->vec[0] != this->vec[1]) || (this->vec[1] != this->vec[2]);
  }

  bool socket_not_zero() const
  {
    return this->link || (saturate_f(this->vec[0]) > near_zero);
  }

  bool socket_not_one() const
  {
    return this->link || (saturate_f(this->vec[0]) < near_one);
  }

  bool socket_not_black() const
  {
    return this->link || saturate_f(this->vec[0]) > near_zero ||
           saturate_f(this->vec[1]) > near_zero || saturate_f(this->vec[2]) > near_zero;
  }

  bool socket_not_white() const
  {
    return this->link || saturate_f(this->vec[0]) < near_one ||
           saturate_f(this->vec[1]) < near_one || saturate_f(this->vec[2]) < near_one;
  }

 private:
  static constexpr float near_zero = 1e-5f;
  static constexpr float near_one = 1.0f - 1e-5f;
  float saturate_f(const float f) const
  {
    return clamp_f(f, 0.0f, 1.0f);
  }
};

struct GPUGraphOutput {
  std::string serialized;
  Vector<StringRefNull> dependencies;

  bool empty() const
  {
    return serialized.empty();
  }

  std::string serialized_or_default(std::string value) const
  {
    return serialized.empty() ? value : serialized;
  }
};

struct GPUCodegenOutput {
  std::string attr_load;
  /* Node-tree functions calls. */
  GPUGraphOutput displacement;
  GPUGraphOutput surface;
  GPUGraphOutput volume;
  GPUGraphOutput thickness;
  GPUGraphOutput composite;
  Vector<GPUGraphOutput> material_functions;

  GPUShaderCreateInfo *create_info;
};

/** What one ColorRamp uniform holds, so a ramp edit can rewrite it without a rebuild. */
enum GPURampUniformRole {
  GPU_RAMP_UNIFORM_NONE = 0,
  /** First float of the mul/bias pair the linear and ease functions read: the pair is one uniform. */
  GPU_RAMP_UNIFORM_MUL_BIAS_0,
  /** Second float of that pair, recorded right after the first; never a uniform of its own. */
  GPU_RAMP_UNIFORM_MUL_BIAS_1,
  /** The single edge position the two-stop constant function reads. */
  GPU_RAMP_UNIFORM_CONST_POS,
  /** The whole color of the first stop. */
  GPU_RAMP_UNIFORM_COLOR_0,
  /** The whole color of the second stop. */
  GPU_RAMP_UNIFORM_COLOR_1,
  /** The whole color of the only stop of a one-stop ramp. */
  GPU_RAMP_UNIFORM_COLOR_SINGLE,
};

GPUNodeLink *GPU_constant(const float *num);
GPUNodeLink *GPU_uniform(const float *num);
/**
 * A uniform a ramp's stops became, whose value a ramp edit rewrites without a rebuild: \a role
 * names what \a value holds of the one-stop or two-stop optimization of \a node (a ColorRamp), and
 * the node's ramp key lets the edit find the edited band again. Falls back to the plain
 * #GPU_uniform for a node without a key, or a material that cannot take such an edit (see
 * #GPU_material_values_update). The link must be passed to one of the node's own
 * `valtorgb_opti_*`/`set_rgba` calls, which read it as a plain value.
 */
GPUNodeLink *GPU_uniform_ramp(GPUMaterial *mat,
                              const bNode *node,
                              const float *value,
                              GPURampUniformRole role);
/**
 * A uniform holding \a stack's value that also remembers where the value came from, so the uniform
 * remains refreshable by a value edit.
 */
GPUNodeLink *GPU_uniform_from_stack(const GPUNodeStack *stack);
/** Whether \a node only reads its unconnected inputs as values, and no code choice from one. */
bool GPU_material_node_is_value_only(const bNode &node);
GPUNodeLink *GPU_attribute(GPUMaterial *mat, eCustomDataType type, const char *name);
/**
 * Add a GPU attribute that refers to the default color attribute on a geometry.
 * The name, type, and domain are unknown and do not depend on the material.
 */
GPUNodeLink *GPU_attribute_default_color(GPUMaterial *mat);
/**
 * Add a GPU attribute that refers to the approximate length of curves/hairs.
 */
GPUNodeLink *GPU_attribute_hair_length(GPUMaterial *mat);
GPUNodeLink *GPU_attribute_hair_intercept(GPUMaterial *mat);
GPUNodeLink *GPU_attribute_with_default(GPUMaterial *mat,
                                        eCustomDataType type,
                                        const char *name,
                                        GPUDefaultValue default_value);
GPUNodeLink *GPU_uniform_attribute(GPUMaterial *mat,
                                   const char *name,
                                   bool use_dupli,
                                   uint32_t *r_hash);
GPUNodeLink *GPU_layer_attribute(GPUMaterial *mat, const char *name);
GPUNodeLink *GPU_image(GPUMaterial *mat,
                       Image *ima,
                       ImageUser *iuser,
                       GPUSamplerState sampler_state);
void GPU_image_tiled(GPUMaterial *mat,
                     Image *ima,
                     ImageUser *iuser,
                     GPUSamplerState sampler_state,
                     GPUNodeLink **r_image_tiled_link,
                     GPUNodeLink **r_image_tiled_mapping_link);
GPUNodeLink *GPU_image_sky(GPUMaterial *mat,
                           int width,
                           int height,
                           const float *pixels,
                           float *layer,
                           GPUSamplerState sampler_state);
GPUNodeLink *GPU_color_band(GPUMaterial *mat, int size, float *pixels, float *r_row);

/**
 * Record that \a node (a ColorRamp the paint layer generator made) was built from \a band, into
 * row \a row of the band texture, or -1 when its stops became uniforms of the node's own making
 * (see #GPU_uniform_ramp). A layered material's value edit then rewrites the row or those uniforms
 * instead of rebuilding (#GPU_material_values_update). Nothing is recorded for another node or
 * material.
 */
void GPU_material_ramp_track(GPUMaterial *mat, const bNode *node, const ColorBand *band, int row);

/**
 * Create an implementation defined differential calculation of a float function.
 * The given function should return a float.
 * The result will be a vec2 containing dFdx and dFdy result of that function.
 */
GPUNodeLink *GPU_differentiate_float_function(const char *function_name, const float filter_width);

bool GPU_link(GPUMaterial *mat, const char *name, ...);
bool GPU_stack_link(GPUMaterial *mat,
                    const bNode *node,
                    const char *name,
                    GPUNodeStack *in,
                    GPUNodeStack *out,
                    ...);

bool GPU_stack_link_zone(GPUMaterial *material,
                         const bNode *bnode,
                         const char *name,
                         GPUNodeStack *in,
                         GPUNodeStack *out,
                         int zone_index,
                         bool is_zone_end,
                         int in_argument_count,
                         int out_argument_count);

void GPU_material_output_surface(GPUMaterial *material, GPUNodeLink *link);
void GPU_material_output_volume(GPUMaterial *material, GPUNodeLink *link);
void GPU_material_output_displacement(GPUMaterial *material, GPUNodeLink *link);
void GPU_material_output_thickness(GPUMaterial *material, GPUNodeLink *link);

void GPU_material_add_output_link_aov(GPUMaterial *material, GPUNodeLink *link, int hash);

void GPU_material_add_output_link_composite(GPUMaterial *material, GPUNodeLink *link);

/**
 * Wrap a part of the material graph into a function. You need then need to call the function by
 * using something like #GPU_differentiate_float_function.
 * \note This replace the link by a constant to break the link with the main graph.
 * \param return_type: sub function return type. Output is cast to this type.
 * \param link: link to use as the sub function output.
 * \return the name of the generated function.
 */
char *GPU_material_split_sub_function(GPUMaterial *material,
                                      GPUType return_type,
                                      GPUNodeLink **link);

void GPU_material_flag_set(GPUMaterial *mat, eGPUMaterialFlag flag);
eGPUMaterialFlag GPU_material_flag(const GPUMaterial *mat);

GHash *GPU_uniform_attr_list_hash_new(const char *info);
void GPU_uniform_attr_list_copy(GPUUniformAttrList *dest, const GPUUniformAttrList *src);
void GPU_uniform_attr_list_free(GPUUniformAttrList *set);

/* Returns the GPU node stack of the input with the given identifier in the given node within the
 * given inputs stack array. */
GPUNodeStack &GPU_node_get_input(const bNode &node, GPUNodeStack inputs[], StringRef identifier);

/* Returns the GPU node stack of the output with the given identifier in the given node within the
 * given output stack array. */
GPUNodeStack &GPU_node_get_output(const bNode &node, GPUNodeStack outputs[], StringRef identifier);

/* Returns the GPU node link of the input with the given identifier in the given node within the
 * given inputs stack array, if the input is not linked, a uniform link carrying the value of the
 * input will be created and returned. It is expected that the caller will use the returned link in
 * a GPU material, otherwise, the link may not be properly freed. */
GPUNodeLink *GPU_node_get_input_link(const bNode &node,
                                     GPUNodeStack inputs[],
                                     StringRef identifier);

}  // namespace blender
