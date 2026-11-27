/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * See BKE_mesh_maps_bake.hh for the contract. This file never renders: it turns a map type into a
 * pass/space/emission graph and does the pixel and state bookkeeping around the engine call.
 */

#include "BKE_mesh_maps_bake.hh"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <utility>

#include "BLI_array.hh"
#include "BLI_ghash.h"
#include "BLI_hash.h"
#include "BLI_index_range.hh"
#include "BLI_listbase_iterator.hh"
#include "BLI_math_color.h"
#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_set.hh"
#include "BLI_string.h"
#include "BLI_task.hh"
#include "BLI_utildefines.h"
#include "BLI_ustring.hh"
#include "BLI_virtual_array.hh"

#include "BKE_attribute.hh"
#include "BKE_bvhutils.hh"
#include "BKE_image.hh"
#include "BKE_image_tile_buffer.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_maps.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_object.hh"
#include "BKE_scene.hh"

#include "RE_bake.h"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "DNA_customdata_types.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_enums.h"

namespace blender {

/* The descriptor table must keep the classification the bake's callers rely on: one row per type,
 * the render maps through a pass, the CPU fills without one, and the scalar atlas list exactly the
 * one the writer and the composite share. */
BLI_STATIC_ASSERT(ARRAY_SIZE(MESH_MAP_BAKE_TYPE_INFOS) == MA_MESH_MAP_TYPE_NUM,
                  "Every map type must have a descriptor row");
BLI_STATIC_ASSERT((MESH_MAP_BAKE_TYPE_INFOS[MA_MESH_MAP_AO].flags &
                   (MESH_MAP_TYPE_RENDERED | MESH_MAP_TYPE_SCALAR)) ==
                      (MESH_MAP_TYPE_RENDERED | MESH_MAP_TYPE_SCALAR),
                  "AO is a rendered scalar atlas");
BLI_STATIC_ASSERT((MESH_MAP_BAKE_TYPE_INFOS[MA_MESH_MAP_ID_OBJECT].flags &
                   (MESH_MAP_TYPE_CPU_FILL | MESH_MAP_TYPE_SCALAR)) == MESH_MAP_TYPE_CPU_FILL,
                  "The ID maps are CPU fills, not scalar atlases");
BLI_STATIC_ASSERT((MESH_MAP_BAKE_TYPE_INFOS[MA_MESH_MAP_THICKNESS].flags &
                   (MESH_MAP_TYPE_SCALAR | MESH_MAP_TYPE_CPU_FILL |
                    MESH_MAP_TYPE_NEEDS_SOURCES)) ==
                      (MESH_MAP_TYPE_SCALAR | MESH_MAP_TYPE_CPU_FILL |
                       MESH_MAP_TYPE_NEEDS_SOURCES),
                  "Thickness is the scalar ray-marched fill");
BLI_STATIC_ASSERT(MESH_MAP_BAKE_TYPE_INFOS[MA_MESH_MAP_NORMAL_WORLD].pass == SCE_PASS_NORMAL,
                  "The normals bake through the native normal pass");
BLI_STATIC_ASSERT(MESH_MAP_BAKE_TYPE_INFOS[MA_MESH_MAP_POSITION].emit_tree_builder != nullptr,
                  "Position builds an emission graph");
BLI_STATIC_ASSERT(MESH_MAP_BAKE_TYPE_INFOS[MA_MESH_MAP_ID_MATERIAL].emit_tree_builder == nullptr,
                  "A CPU fill builds no emission graph");

namespace {

bool mesh_map_bake_type_valid(const int8_t type)
{
  return type >= 0 && type < MA_MESH_MAP_TYPE_NUM;
}

/** The first output node of \a tree, or null. Material trees have exactly one. */
bNode *mesh_map_bake_output_node(bNodeTree &tree)
{
  for (bNode &node : tree.nodes) {
    if (node.type_legacy == SH_NODE_OUTPUT_MATERIAL) {
      return &node;
    }
  }
  return nullptr;
}

void mesh_map_bake_set_float(bNodeSocket &socket, const float value)
{
  if (bNodeSocketValueFloat *stored = socket.default_value_typed<bNodeSocketValueFloat>()) {
    stored->value = value;
  }
}

/** Link \a from socket to the `Color` input of \a emission, through any implicit conversion. */
void mesh_map_bake_emit_from(bNodeTree &tree,
                             bNode &emission,
                             bNode &source_node,
                             bNodeSocket &from)
{
  bNodeSocket *color = bke::node_find_socket(emission, SOCK_IN, UString("Color"));
  if (color != nullptr) {
    bke::node_add_link(tree, source_node, from, emission, *color);
  }
}

}  // namespace

bool BKE_mesh_maps_bake_type_renders(const int8_t type)
{
  const MeshMapTypeInfo *info = BKE_mesh_maps_type_info_get(type);
  return info != nullptr && (info->flags & MESH_MAP_TYPE_RENDERED) != 0;
}

bool BKE_mesh_maps_bake_type_cpu_simple(const int8_t type)
{
  /* A simple CPU fill: it reads the low-poly mesh and its settings alone, neither casting rays
   * through other surfaces nor composing other atlases. */
  const MeshMapTypeInfo *info = BKE_mesh_maps_type_info_get(type);
  return info != nullptr && (info->flags & MESH_MAP_TYPE_CPU_FILL) != 0 &&
         (info->flags & (MESH_MAP_TYPE_NEEDS_SOURCES | MESH_MAP_TYPE_NEEDS_OTHER_ATLAS)) == 0;
}

bool BKE_mesh_maps_bake_type_scalar(const int8_t type)
{
  const MeshMapTypeInfo *info = BKE_mesh_maps_type_info_get(type);
  return info != nullptr && (info->flags & MESH_MAP_TYPE_SCALAR) != 0;
}

size_t BKE_mesh_maps_bake_estimate_bytes(const int atlas_resolution,
                                         const int supersampling,
                                         const size_t candidate_tiles)
{
  /* The CPU fills have no supersampling to grow the raster with: their raster is the atlas side. */
  const int raster_resolution = atlas_resolution * std::max(supersampling, 1);
  const size_t raster_texels = size_t(std::max(raster_resolution, 0)) *
                               size_t(std::max(raster_resolution, 0));
  const size_t atlas_texels = size_t(std::max(atlas_resolution, 0)) *
                              size_t(std::max(atlas_resolution, 0));
  /* Per raster texel: the populated #BakePixel (36), the RGBA float result (16), the margin mask
   * (1), and the one raster-sized float ImBuf the margin phase runs through (16): 69. The
   * indicator pass the margin used to record its filled texels with is gone, so it is not counted;
   * the commit phase's own coverage and margin masks (2) fit under the ImBuf term, which is freed
   * before they are allocated.
   * Per atlas texel: the commit's resolved result (16) and the two downsampled masks (2), the
   * foreign coverage (1), the variant-A clear region (1), the extend mask (1), and the
   * #IMB_filter_extend scratch -- its duplicated float buffer (16) plus its duplicated mask (1):
   * 38. The atlas image tiles themselves are user data that outlives the pair, so they are not
   * counted; the resolve's transient union buffer (1) is freed before the merge scratch exists.
   * Everything accumulates in `size_t`, sizeof first, so a 16384 atlas at supersampling 4 (a
   * raster of about 4.3e9 texels) cannot overflow the multiplications. */
  const size_t raster_bytes_per_texel = sizeof(BakePixel) + 16 + 1 + 16;
  const size_t atlas_bytes_per_texel = 16 + 2 + 1 + 1 + 1 + 16 + 1;
  return candidate_tiles *
         (raster_bytes_per_texel * raster_texels + atlas_bytes_per_texel * atlas_texels);
}

eScenePassType BKE_mesh_maps_bake_pass_for_type(const int8_t type)
{
  const MeshMapTypeInfo *info = BKE_mesh_maps_type_info_get(type);
  return info != nullptr ? info->pass : eScenePassType(0);
}

bool BKE_mesh_maps_bake_normal_space_for_type(const int8_t type, eBakeSpace &r_space)
{
  const MeshMapTypeInfo *info = BKE_mesh_maps_type_info_get(type);
  if (info == nullptr || info->pass != SCE_PASS_NORMAL) {
    /* The space is claimed by the native normal pass maps only; the others are never rotated. */
    return false;
  }
  r_space = info->space;
  return true;
}

float BKE_mesh_maps_bake_ao_distance(const Mesh &mesh, const MaterialMeshMapSettings &settings)
{
  if (settings.ao_distance > 0.0f) {
    return settings.ao_distance;
  }
  const Span<float3> positions = mesh.vert_positions();
  if (positions.is_empty()) {
    return 1.0f;
  }
  float3 bb_min = positions[0];
  float3 bb_max = positions[0];
  for (const float3 &position : positions) {
    for (int axis = 0; axis < 3; axis++) {
      bb_min[axis] = std::min(bb_min[axis], position[axis]);
      bb_max[axis] = std::max(bb_max[axis], position[axis]);
    }
  }
  const float3 size = bb_max - bb_min;
  const float half_diagonal = 0.5f * std::sqrt(size.x * size.x + size.y * size.y + size.z * size.z);
  /* A flat or single-point mesh would otherwise give 0, which the AO node reads as "global radius".
   * A small positive value keeps the map an object-bound effect. */
  return half_diagonal > 0.0f ? half_diagonal : 1.0f;
}

/* The per-type value sources of the emission graph, declared in BKE_mesh_maps_bake.hh and held in
 * #MESH_MAP_BAKE_TYPE_INFOS; the common tail of #BKE_mesh_maps_bake_build_emit_tree wires whatever
 * they built into an emission node. */

bool mesh_map_bake_emit_source_ao(bNodeTree &tree,
                                  const MaterialMeshMapSettings &settings,
                                  const float ao_distance,
                                  const int samples,
                                  bNodeSocket **r_source,
                                  bNode **r_source_node)
{
  bNode *ao = bke::node_add_static_node(nullptr, tree, SH_NODE_AMBIENT_OCCLUSION);
  if (ao == nullptr) {
    return false;
  }
  /* The per-shader-evaluation ray count is the bake's sample count. The plane stays
   * unlinked so Cycles shades the actual normal. #SHD_AO_LOCAL keeps the occluders to the
   * object being baked; a global bake traces the whole private world, so the high-poly
   * pieces of one source occlude each other. */
  ao->custom1 = std::max(samples, 1);
  if (settings.use_global_ao == 0) {
    ao->custom2 |= SHD_AO_LOCAL;
  }
  if (bNodeSocket *distance = bke::node_find_socket(*ao, SOCK_IN, UString("Distance"))) {
    mesh_map_bake_set_float(*distance, ao_distance > 0.0f ? ao_distance : 1.0f);
  }
  *r_source = bke::node_find_socket(*ao, SOCK_OUT, UString("AO"));
  *r_source_node = ao;
  return true;
}

bool mesh_map_bake_emit_source_position(bNodeTree &tree,
                                        const MaterialMeshMapSettings & /*settings*/,
                                        const float /*ao_distance*/,
                                        const int /*samples*/,
                                        bNodeSocket **r_source,
                                        bNode **r_source_node)
{
  /* The Geometry node's Position output is the world-space position Cycles shades with: the
   * emission pass records it as a color, and the normalization pass (the caller's) decides
   * what the texels mean beyond that. */
  bNode *geometry = bke::node_add_static_node(nullptr, tree, SH_NODE_NEW_GEOMETRY);
  if (geometry == nullptr) {
    return false;
  }
  *r_source = bke::node_find_socket(*geometry, SOCK_OUT, UString("Position"));
  *r_source_node = geometry;
  return true;
}

bool mesh_map_bake_emit_source_curvature(bNodeTree &tree,
                                         const MaterialMeshMapSettings & /*settings*/,
                                         const float /*ao_distance*/,
                                         const int /*samples*/,
                                         bNodeSocket **r_source,
                                         bNode **r_source_node)
{
  bNode *geometry = bke::node_add_static_node(nullptr, tree, SH_NODE_NEW_GEOMETRY);
  if (geometry == nullptr) {
    return false;
  }
  /* Cycles requests #ATTR_STD_POINTINESS itself when the output is linked. */
  *r_source = bke::node_find_socket(*geometry, SOCK_OUT, UString("Pointiness"));
  *r_source_node = geometry;
  return true;
}

bool mesh_map_bake_emit_source_edge(bNodeTree &tree,
                                    const MaterialMeshMapSettings &settings,
                                    const float /*ao_distance*/,
                                    const int samples,
                                    bNodeSocket **r_source,
                                    bNode **r_source_node)
{
  bNode *bevel = bke::node_add_static_node(nullptr, tree, SH_NODE_BEVEL);
  bNode *geometry = bke::node_add_static_node(nullptr, tree, SH_NODE_NEW_GEOMETRY);
  bNode *dot = bke::node_add_static_node(nullptr, tree, SH_NODE_VECTOR_MATH);
  bNode *subtract = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
  bNode *range = bke::node_add_static_node(nullptr, tree, SH_NODE_MAP_RANGE);
  if (bevel == nullptr || geometry == nullptr || dot == nullptr || subtract == nullptr ||
      range == nullptr)
  {
    return false;
  }
  bevel->custom1 = std::clamp(
      samples, MESH_MAP_BAKE_EDGE_BEVEL_SAMPLES_MIN, MESH_MAP_BAKE_EDGE_BEVEL_SAMPLES_MAX);
  if (bNodeSocket *radius = bke::node_find_socket(*bevel, SOCK_IN, UString("Radius"))) {
    mesh_map_bake_set_float(*radius, std::max(settings.edge_radius, 0.0f));
  }

  /* edge = saturate((1 - dot(bevel normal, geometry normal)) / MESH_MAP_BAKE_EDGE_RANGE). */
  dot->custom1 = NODE_VECTOR_MATH_DOT_PRODUCT;
  subtract->custom1 = NODE_MATH_SUBTRACT;
  if (NodeMapRange *storage = static_cast<NodeMapRange *>(range->storage)) {
    storage->data_type = CD_PROP_FLOAT;
    storage->interpolation_type = NODE_MAP_RANGE_LINEAR;
    storage->clamp = 1;
  }
  range->custom1 = true;
  range->custom2 = NODE_MAP_RANGE_LINEAR;

  bNodeSocket *bevel_normal = bke::node_find_socket(*bevel, SOCK_OUT, UString("Normal"));
  bNodeSocket *geometry_normal = bke::node_find_socket(*geometry, SOCK_OUT, UString("Normal"));
  bNodeSocket *dot_a = bke::node_find_socket(*dot, SOCK_IN, UString("Vector"));
  bNodeSocket *dot_b = bke::node_find_socket(*dot, SOCK_IN, UString("Vector_001"));
  bNodeSocket *dot_value = bke::node_find_socket(*dot, SOCK_OUT, UString("Value"));
  bNodeSocket *subtract_a = bke::node_find_socket(*subtract, SOCK_IN, UString("Value"));
  bNodeSocket *subtract_b = bke::node_find_socket(*subtract, SOCK_IN, UString("Value_001"));
  bNodeSocket *subtract_value = bke::node_find_socket(*subtract, SOCK_OUT, UString("Value"));
  bNodeSocket *range_value = bke::node_find_socket(*range, SOCK_IN, UString("Value"));
  bNodeSocket *range_from_min = bke::node_find_socket(*range, SOCK_IN, UString("From Min"));
  bNodeSocket *range_from_max = bke::node_find_socket(*range, SOCK_IN, UString("From Max"));
  bNodeSocket *range_to_min = bke::node_find_socket(*range, SOCK_IN, UString("To Min"));
  bNodeSocket *range_to_max = bke::node_find_socket(*range, SOCK_IN, UString("To Max"));
  *r_source = bke::node_find_socket(*range, SOCK_OUT, UString("Result"));
  if (bevel_normal == nullptr || geometry_normal == nullptr || dot_a == nullptr ||
      dot_b == nullptr || dot_value == nullptr || subtract_a == nullptr ||
      subtract_b == nullptr || subtract_value == nullptr || range_value == nullptr ||
      range_from_min == nullptr || range_from_max == nullptr || range_to_min == nullptr ||
      range_to_max == nullptr || *r_source == nullptr)
  {
    return false;
  }
  mesh_map_bake_set_float(*subtract_a, 1.0f);
  mesh_map_bake_set_float(*range_from_min, 0.0f);
  mesh_map_bake_set_float(*range_from_max, MESH_MAP_BAKE_EDGE_RANGE);
  mesh_map_bake_set_float(*range_to_min, 0.0f);
  mesh_map_bake_set_float(*range_to_max, 1.0f);

  bke::node_add_link(tree, *bevel, *bevel_normal, *dot, *dot_a);
  bke::node_add_link(tree, *geometry, *geometry_normal, *dot, *dot_b);
  bke::node_add_link(tree, *dot, *dot_value, *subtract, *subtract_b);
  bke::node_add_link(tree, *subtract, *subtract_value, *range, *range_value);
  *r_source_node = range;
  return true;
}

bNode *BKE_mesh_maps_bake_build_emit_tree(bNodeTree &tree,
                                          const int8_t type,
                                          const MaterialMeshMapSettings &settings,
                                          const float ao_distance,
                                          const int samples)
{
  const MeshMapTypeInfo *info = BKE_mesh_maps_type_info_get(type);
  if (info == nullptr || info->emit_tree_builder == nullptr) {
    /* The normals bake through the native pass and the CPU maps need no engine: no graph. */
    return nullptr;
  }
  bNodeSocket *source = nullptr;
  bNode *source_node = nullptr;
  if (!info->emit_tree_builder(tree, settings, ao_distance, samples, &source, &source_node)) {
    return nullptr;
  }
  if (source == nullptr || source_node == nullptr) {
    return nullptr;
  }

  bNode *emission = bke::node_add_static_node(nullptr, tree, SH_NODE_EMISSION);
  if (emission == nullptr) {
    return nullptr;
  }
  mesh_map_bake_emit_from(tree, *emission, *source_node, *source);

  bNode *output = mesh_map_bake_output_node(tree);
  if (output == nullptr) {
    output = bke::node_add_static_node(nullptr, tree, SH_NODE_OUTPUT_MATERIAL);
  }
  if (output != nullptr) {
    bNodeSocket *surface = bke::node_find_socket(*output, SOCK_IN, UString("Surface"));
    bNodeSocket *emission_out = bke::node_find_socket(*emission, SOCK_OUT, UString("Emission"));
    if (surface != nullptr && emission_out != nullptr) {
      bke::node_add_link(tree, *emission, *emission_out, *output, *surface);
    }
  }
  return emission;
}

void BKE_mesh_maps_bake_normalize_position(float *result,
                                           const size_t texels_num,
                                           const float bb_min[3],
                                           const float bb_max[3],
                                           const int8_t mode)
{
  if (result == nullptr || mode == MA_MESH_MAP_POSITION_RAW) {
    return;
  }
  float3 size = {std::max(bb_max[0] - bb_min[0], 1e-9f),
                 std::max(bb_max[1] - bb_min[1], 1e-9f),
                 std::max(bb_max[2] - bb_min[2], 1e-9f)};
  if (mode == MA_MESH_MAP_POSITION_SPHERE) {
    /* One radius covers all axes, so a long axis keeps its relative lengths. */
    const float radius = std::max({size.x, size.y, size.z}) * 0.5f;
    size = {radius * 2.0f, radius * 2.0f, radius * 2.0f};
  }
  const float3 center = {(bb_min[0] + bb_max[0]) * 0.5f,
                         (bb_min[1] + bb_max[1]) * 0.5f,
                         (bb_min[2] + bb_max[2]) * 0.5f};
  const float3 origin = mode == MA_MESH_MAP_POSITION_SPHERE ?
                            center - size * 0.5f :
                            float3{bb_min[0], bb_min[1], bb_min[2]};
  for (size_t i = 0; i < texels_num; i++) {
    float *texel = &result[i * 4];
    for (int axis = 0; axis < 3; axis++) {
      texel[axis] = (texel[axis] - origin[axis]) / size[axis];
    }
  }
}

void BKE_mesh_maps_bake_id_object_color(const Object &ob, float r_color[3])
{
  const uint hash = BLI_ghashutil_strhash_p_murmur(ob.id.name);
  hsv_to_rgb(BLI_hash_int_01(hash), 0.5f, 0.8f, &r_color[0], &r_color[1], &r_color[2]);
}

void BKE_mesh_maps_bake_id_material_color(const int material_index, float r_color[3])
{
  /* The golden-ratio walk keeps neighbouring indices far apart in hue. */
  const float hue = std::fmod(float(std::max(material_index, 0)) * 0.61803398875f, 1.0f);
  hsv_to_rgb(hue, 0.5f, 0.8f, &r_color[0], &r_color[1], &r_color[2]);
}

void BKE_mesh_maps_bake_restrict_to_material(BakePixel *pixels,
                                             const size_t pixels_num,
                                             const Mesh &mesh,
                                             const Object &ob,
                                             const Material &ma)
{
  if (pixels == nullptr || mesh.faces_num == 0) {
    return;
  }
  const Span<int> tri_faces = mesh.corner_tri_faces();
  if (tri_faces.is_empty()) {
    return;
  }
  const bke::AttributeAccessor attributes = mesh.attributes();
  const VArraySpan<int> material_indices = *attributes.lookup<int>("material_index",
                                                                   bke::AttrDomain::Face);
  Array<bool> face_allows(mesh.faces_num, false);
  Object *ob_mutable = const_cast<Object *>(&ob);
  for (const int face : IndexRange(mesh.faces_num)) {
    const int index = material_indices.is_empty() ? 0 : material_indices[face];
    face_allows[face] = BKE_object_material_get(ob_mutable, short(index + 1)) == &ma;
  }
  for (size_t i = 0; i < pixels_num; i++) {
    const int primitive = pixels[i].primitive_id;
    if (primitive < 0 || primitive >= tri_faces.size()) {
      continue;
    }
    if (!face_allows[tri_faces[primitive]]) {
      pixels[i].primitive_id = -1;
    }
  }
}

void BKE_mesh_maps_bake_fill_id_pixels(const BakePixel *pixels,
                                       const size_t pixels_num,
                                       const Mesh &mesh,
                                       const Object &ob,
                                       const int8_t type,
                                       const int channels,
                                       float *result)
{
  if (pixels == nullptr || result == nullptr || channels < 3) {
    return;
  }
  const Span<int> tri_faces = mesh.corner_tri_faces();
  const bke::AttributeAccessor attributes = mesh.attributes();
  const VArraySpan<int> material_indices = *attributes.lookup<int>("material_index",
                                                                   bke::AttrDomain::Face);

  float object_color[3] = {0.0f, 0.0f, 0.0f};
  if (type == MA_MESH_MAP_ID_OBJECT) {
    BKE_mesh_maps_bake_id_object_color(ob, object_color);
  }

  for (size_t i = 0; i < pixels_num; i++) {
    const int primitive = pixels[i].primitive_id;
    if (primitive < 0 || primitive >= tri_faces.size()) {
      continue;
    }
    float *destination = result + i * size_t(channels);
    if (type == MA_MESH_MAP_ID_OBJECT) {
      copy_v3_v3(destination, object_color);
    }
    else {
      const int face = tri_faces[primitive];
      const int index = material_indices.is_empty() ? 0 : material_indices[face];
      float color[3];
      BKE_mesh_maps_bake_id_material_color(index, color);
      copy_v3_v3(destination, color);
    }
    if (channels >= 4) {
      destination[3] = 1.0f;
    }
  }
}

bool BKE_mesh_maps_bake_type_cpu_rays_scalar(const int8_t type)
{
  /* Thickness is the scalar half of the ray-marched fills; Bent Normal is the vector half. */
  const MeshMapTypeInfo *info = BKE_mesh_maps_type_info_get(type);
  return info != nullptr && (info->flags & MESH_MAP_TYPE_NEEDS_SOURCES) != 0 &&
         (info->flags & MESH_MAP_TYPE_SCALAR) != 0;
}

bool BKE_mesh_maps_bake_type_cpu_rays_vector(const int8_t type)
{
  const MeshMapTypeInfo *info = BKE_mesh_maps_type_info_get(type);
  return info != nullptr && (info->flags & MESH_MAP_TYPE_NEEDS_SOURCES) != 0 &&
         (info->flags & MESH_MAP_TYPE_SCALAR) == 0;
}

namespace {

/** One interior surface with its BVH and the transforms between its local space and the world. */
struct ThicknessTree {
  bke::BVHTreeFromMesh data;
  float4x4 matrix;
  float4x4 inverse;
};

/** Deterministic direction in the hemisphere around \a normal, from the sample index \a k. */
void thickness_ray_direction(const float3 &normal, const int k, const int texel, float3 &r_dir)
{
  /* A golden-ratio ladder over the [0, 1) square, offset by a hash of the texel so neighbouring
   * texels do not share the same ladder phase. Deterministic across runs and threads. */
  const float u = std::fmod((k + 0.5f) * 0.61803398875f +
                                float(BLI_hash_int_2d(uint(texel), uint(k))) * (1.0f / 4294967296.0f),
                            1.0f);
  const float v = std::fmod((k + 0.5f) * 0.75487766624f +
                                float(BLI_hash_int_2d(uint(texel), 0x9E3779B9u)) * (1.0f / 4294967296.0f),
                            1.0f);
  /* Equal-area hemisphere around +Z, then aligned onto the normal. */
  const float phi = 2.0f * float(M_PI) * u;
  const float cos_theta = 1.0f - v;
  const float sin_theta = std::sqrt(std::max(1.0f - cos_theta * cos_theta, 0.0f));
  float3 local(sin_theta * std::cos(phi), sin_theta * std::sin(phi), cos_theta);
  float3 tangent;
  float3 bitangent;
  ortho_basis_v3v3_v3(tangent, bitangent, normal);
  r_dir = tangent * local.x + bitangent * local.y + normal * local.z;
  r_dir = math::normalize(r_dir);
}

/** Cast \a dir (world, normalized) through one source from \a from; the hit point comes back
 * in world space, so matrix scale cannot skew the distances. */
bool thickness_cast_tree(const ThicknessTree &tree,
                         const float3 &from,
                         const float3 &dir,
                         int &r_tri,
                         float3 &r_world_hit)
{
  const float3 local_from = math::transform_point(tree.inverse, from);
  const float3 local_dir = math::transform_direction(tree.inverse, dir);
  const float local_length = math::length(local_dir);
  if (local_length <= 0.0f) {
    return false;
  }
  const float3 local_dir_normalized = local_dir / local_length;
  BVHTreeRayHit hit;
  hit.index = -1;
  hit.dist = FLT_MAX;
  /* Pass the mesh callback, not null: without it the BVH reports the leaf's bounding-box distance
   * and a box "hit" for rays that miss the triangle entirely, which would turn every thickness into
   * a distance between AABBs and every Bent Normal into a box occlusion test. The radius must be
   * zero (a thin ray): a non-zero radius makes the callback do a sphere cast, and `hit.dist` already
   * carries the unlimited maximum distance. */
  if (BLI_bvhtree_ray_cast(tree.data.tree,
                           local_from,
                           local_dir_normalized,
                           0.0f,
                           &hit,
                           tree.data.raycast_callback,
                           const_cast<bke::BVHTreeFromMesh *>(&tree.data)) < 0)
  {
    return false;
  }
  r_world_hit = math::transform_point(tree.matrix, local_from + local_dir_normalized * hit.dist);
  r_tri = hit.index;
  return true;
}

/** Everything the ray-marched fills need per texel, prepared once per fill: the interior BVH
 * trees, the low-poly sample spans, the world-space offset that keeps a cast off the sampled
 * surface, and the clamped ray count. */
struct ThicknessPrepare {
  Vector<ThicknessTree> trees;
  Span<float3> positions;
  Span<int3> corner_tris;
  Span<int> corner_verts;
  float surface_eps;
  int ray_count;
};

ThicknessPrepare thickness_prepare(const Span<MeshMapBakeThicknessSource> sources,
                                   const Mesh &low_mesh,
                                   const float4x4 &low_matrix,
                                   const int rays)
{
  ThicknessPrepare prepare;
  prepare.positions = low_mesh.vert_positions();
  prepare.corner_tris = low_mesh.corner_tris();
  prepare.corner_verts = low_mesh.corner_verts();

  /* No source given means the low-poly itself is the interior the rays march through. */
  Vector<MeshMapBakeThicknessSource> fallback;
  Span<MeshMapBakeThicknessSource> used_sources = sources;
  if (used_sources.is_empty()) {
    fallback.append({&low_mesh, low_matrix});
    used_sources = fallback;
  }

  const auto add_tree = [&prepare](const Mesh &mesh, const float4x4 &matrix) {
    if (mesh.faces_num == 0) {
      return;
    }
    ThicknessTree tree;
    tree.data = bke::bvhtree_from_mesh_corner_tris_ex(mesh.vert_positions(),
                                                      mesh.faces(),
                                                      mesh.corner_verts(),
                                                      mesh.corner_tris(),
                                                      IndexMask(mesh.faces_num));
    if (tree.data.tree == nullptr) {
      return;
    }
    tree.matrix = matrix;
    tree.inverse = math::invert(matrix);
    prepare.trees.append(std::move(tree));
  };

  prepare.trees.reserve(used_sources.size() + 1);
  for (const MeshMapBakeThicknessSource &source : used_sources) {
    if (source.mesh == nullptr) {
      continue;
    }
    add_tree(*source.mesh, source.matrix);
  }

  /* A source set that resolved to nothing (an empty high-poly object, say) is no source at all:
   * fall back to the low-poly so the map is the object's own volume instead of a silent zero. A
   * BVH that fails on a non-empty source cannot be reported from here -- these fills return no
   * status -- so it falls back the same way; the editor's cast no longer runs for the ray-marched
   * CPU pairs and cannot report it either. A loud failure needs these entries to return one. */
  if (prepare.trees.is_empty()) {
    add_tree(low_mesh, low_matrix);
  }

  /* A cast that starts on the sampled surface must not find it again: skip hits closer than a
   * small fraction of the low-poly's bounds. The bounds are built in world space because the
   * marching compares world-space hit distances, so a scaled object matrix must not shrink or
   * inflate the offset relative to them. */
  BLI_assert(!prepare.positions.is_empty());
  float3 bb_min = math::transform_point(low_matrix, prepare.positions[0]);
  float3 bb_max = bb_min;
  for (const float3 &position : prepare.positions) {
    const float3 world = math::transform_point(low_matrix, position);
    bb_min = math::min(bb_min, world);
    bb_max = math::max(bb_max, world);
  }
  prepare.surface_eps = std::max(math::distance(bb_min, bb_max) * 1e-5f, 1e-6f);

  prepare.ray_count = std::clamp(rays, MESH_MAP_BAKE_CPU_RAYS_MIN, MESH_MAP_BAKE_CPU_RAYS_MAX);
  return prepare;
}

}  // namespace

void BKE_mesh_maps_bake_fill_thickness(const BakePixel *pixels,
                                       const size_t pixels_num,
                                       const Mesh &low_mesh,
                                       const float4x4 &low_matrix,
                                       const Span<MeshMapBakeThicknessSource> sources,
                                       const int rays,
                                       float *result)
{
  if (pixels == nullptr || result == nullptr || low_mesh.faces_num == 0 ||
      low_mesh.verts_num == 0)
  {
    return;
  }

  const ThicknessPrepare prepare = thickness_prepare(sources, low_mesh, low_matrix, rays);
  if (prepare.trees.is_empty()) {
    return;
  }
  const auto &[trees, positions, corner_tris, corner_verts, surface_eps, ray_count] = prepare;

  threading::parallel_for(IndexRange(int(pixels_num)), 1024, [&](const IndexRange range) {
    for (const int i : range) {
      const size_t index = size_t(i);
      const int primitive = pixels[index].primitive_id;
      if (primitive < 0 || primitive >= corner_tris.size()) {
        continue;
      }
      const int3 &tri = corner_tris[primitive];
      /* The bake's barycentric convention: u * v0 + v * v1 + (1 - u - v) * v2. */
      const float u = pixels[index].uv[0];
      const float v = pixels[index].uv[1];
      const float w = 1.0f - u - v;
      const float3 local = positions[corner_verts[tri[0]]] * u +
                           positions[corner_verts[tri[1]]] * v +
                           positions[corner_verts[tri[2]]] * w;
      const float3 origin = math::transform_point(low_matrix, local);
      float3 v0, v1, v2;
      mul_v3_m4v3(v0, low_matrix.ptr(), positions[corner_verts[tri[0]]]);
      mul_v3_m4v3(v1, low_matrix.ptr(), positions[corner_verts[tri[1]]]);
      mul_v3_m4v3(v2, low_matrix.ptr(), positions[corner_verts[tri[2]]]);
      float3 normal = math::cross(v1 - v0, v2 - v0);
      const float normal_length = math::length(normal);
      if (normal_length <= 0.0f) {
        continue;
      }
      normal /= normal_length;
      /* A mirrored `low_matrix` (negative determinant) reflects the world-space winding, so the
       * cross product above points *into* the surface. The sign is undone here so the single-ray
       * probe and the hemisphere always face outward, not into the solid. */
      if (is_negative_m4(low_matrix.ptr())) {
        normal = -normal;
      }

      float sum = 0.0f;
      int found = 0;
      /* One ray is the documented "straight down the normal" probe; only two or more rays spread
       * over the hemisphere. The slab test relies on the exact 1.0 this gives, and a single
       * oblique ray would mostly miss the interior on a thin slab. */
      const bool single_ray = ray_count == 1;
      for (int k = 0; k < ray_count; k++) {
        float3 ray_dir;
        if (single_ray) {
          ray_dir = -normal;
        }
        else {
          thickness_ray_direction(-normal, k, i, ray_dir);
        }
        /* Start a hair off the surface so the cast cannot find the texel's own face, then march
         * through surfaces the ray enters (their normal faces the ray) until one exits (its
         * normal points along the ray, i.e. out of the solid). */
        float3 from = origin + ray_dir * (surface_eps * 4.0f);
        bool exited = false;
        for (int march = 0; march < 8 && !exited; march++) {
          int best_tree = -1;
          int best_tri = -1;
          float best_dist = FLT_MAX;
          float3 best_hit(0.0f);
          for (const int t : trees.index_range()) {
            int hit_tri = -1;
            float3 hit_world(0.0f);
            if (!thickness_cast_tree(trees[t], from, ray_dir, hit_tri, hit_world)) {
              continue;
            }
            const float dist = math::distance(hit_world, from);
            if (dist < best_dist) {
              best_dist = dist;
              best_hit = hit_world;
              best_tri = hit_tri;
              best_tree = t;
            }
          }
          if (best_tree < 0) {
            break;
          }
          const ThicknessTree &tree = trees[best_tree];
          const int3 &hit_tri = tree.data.corner_tris[best_tri];
          /* Build the hit triangle's normal in world space from its transformed vertices rather
           * than moving a local normal: the BVH runs in the source's local space, and the world
           * cross product keeps the true orientation under non-uniform scale. A mirrored
           * (negative-determinant) matrix reverses the world winding, so the cross product points
           * into the solid; the sign is undone explicitly, or "entered" and "exited" swap and
           * every texel reads a thickness of zero. */
          const float3 world_v0 = math::transform_point(
              tree.matrix, tree.data.vert_positions[tree.data.corner_verts[hit_tri[0]]]);
          const float3 world_v1 = math::transform_point(
              tree.matrix, tree.data.vert_positions[tree.data.corner_verts[hit_tri[1]]]);
          const float3 world_v2 = math::transform_point(
              tree.matrix, tree.data.vert_positions[tree.data.corner_verts[hit_tri[2]]]);
          float3 world_no = math::cross(world_v1 - world_v0, world_v2 - world_v0);
          if (math::length(world_no) > 0.0f) {
            world_no = math::normalize(world_no);
            if (is_negative_m4(tree.matrix.ptr())) {
              world_no = -world_no;
            }
          }
          if (math::dot(world_no, ray_dir) > 0.0f) {
            /* An exiting surface: the distance back to the texel's origin is the thickness. */
            sum += math::distance(best_hit, origin);
            found++;
            exited = true;
            break;
          }
          from = best_hit + ray_dir * (surface_eps * 4.0f);
        }
      }
      if (found > 0) {
        const float thickness = sum / float(found);
        float *destination = result + index * 4;
        destination[0] = thickness;
        destination[1] = thickness;
        destination[2] = thickness;
        destination[3] = 1.0f;
      }
      else {
        /* A covered texel whose rays never leave the solid still belongs to the atlas: keep it
         * opaque black so its coverage alpha agrees with every other map, instead of leaving the
         * zeroed (transparent) buffer as if the texel were not covered. */
        float *destination = result + index * 4;
        destination[0] = 0.0f;
        destination[1] = 0.0f;
        destination[2] = 0.0f;
        destination[3] = 1.0f;
      }
    }
  });
}

/* The vector half of the ray-marched fills (see #BKE_mesh_maps_bake_fill_thickness): the same
 * trees and the same hemisphere ladder, but a ray counts only when it escapes every source
 * without a hit, and the texel stores the normalized average of the escaping directions encoded
 * to 0..1. A texel whose rays all hit stays black: with nothing unoccluded there is no bent
 * direction, and black reads as "no average" instead of a direction the surface never had. */
void BKE_mesh_maps_bake_fill_bent_normals(const BakePixel *pixels,
                                          const size_t pixels_num,
                                          const Mesh &low_mesh,
                                          const float4x4 &low_matrix,
                                          const Span<MeshMapBakeThicknessSource> sources,
                                          const int rays,
                                          float *result)
{
  if (pixels == nullptr || result == nullptr || low_mesh.faces_num == 0 ||
      low_mesh.verts_num == 0)
  {
    return;
  }

  const ThicknessPrepare prepare = thickness_prepare(sources, low_mesh, low_matrix, rays);
  if (prepare.trees.is_empty()) {
    return;
  }
  const auto &[trees, positions, corner_tris, corner_verts, surface_eps, ray_count] = prepare;

  threading::parallel_for(IndexRange(int(pixels_num)), 1024, [&](const IndexRange range) {
    for (const int i : range) {
      const size_t index = size_t(i);
      const int primitive = pixels[index].primitive_id;
      if (primitive < 0 || primitive >= corner_tris.size()) {
        continue;
      }
      const int3 &tri = corner_tris[primitive];
      /* The bake's barycentric convention: u * v0 + v * v1 + (1 - u - v) * v2. */
      const float u = pixels[index].uv[0];
      const float v = pixels[index].uv[1];
      const float w = 1.0f - u - v;
      const float3 local = positions[corner_verts[tri[0]]] * u +
                           positions[corner_verts[tri[1]]] * v +
                           positions[corner_verts[tri[2]]] * w;
      const float3 origin = math::transform_point(low_matrix, local);
      float3 v0, v1, v2;
      mul_v3_m4v3(v0, low_matrix.ptr(), positions[corner_verts[tri[0]]]);
      mul_v3_m4v3(v1, low_matrix.ptr(), positions[corner_verts[tri[1]]]);
      mul_v3_m4v3(v2, low_matrix.ptr(), positions[corner_verts[tri[2]]]);
      float3 normal = math::cross(v1 - v0, v2 - v0);
      const float normal_length = math::length(normal);
      if (normal_length <= 0.0f) {
        continue;
      }
      normal /= normal_length;
      /* The same mirror sign as the Thickness fill: a negative-determinant `low_matrix` turns the
       * cross product inward, and the hemisphere must still face outward. */
      if (is_negative_m4(low_matrix.ptr())) {
        normal = -normal;
      }

      float3 sum(0.0f);
      int found = 0;
      /* One ray is the surface normal itself (the unoccluded minimum), matching the Thickness
       * fill's single-ray contract; more rays spread over the hemisphere. */
      const bool single_ray = ray_count == 1;
      for (int k = 0; k < ray_count; k++) {
        float3 ray_dir;
        /* The hemisphere points along the surface normal: the directions that escape are the
         * ones the occlusion does not bend, so their average is the texel's bent direction. */
        if (single_ray) {
          ray_dir = normal;
        }
        else {
          thickness_ray_direction(normal, k, i, ray_dir);
        }
        /* Start a hair off the surface so the cast cannot find the texel's own face. */
        const float3 from = origin + ray_dir * (surface_eps * 4.0f);
        bool occluded = false;
        for (const int t : trees.index_range()) {
          int hit_tri = -1;
          float3 hit_world(0.0f);
          if (thickness_cast_tree(trees[t], from, ray_dir, hit_tri, hit_world)) {
            occluded = true;
            break;
          }
        }
        if (!occluded) {
          sum += ray_dir;
          found++;
        }
      }

      float *destination = result + index * 4;
      const float3 bent = found > 0 ? sum / float(found) : float3(0.0f);
      const float bent_length = math::length(bent);
      if (bent_length > 0.0f) {
        const float3 direction = bent / bent_length;
        destination[0] = 0.5f + 0.5f * direction.x;
        destination[1] = 0.5f + 0.5f * direction.y;
        destination[2] = 0.5f + 0.5f * direction.z;
      }
      else {
        destination[0] = 0.0f;
        destination[1] = 0.0f;
        destination[2] = 0.0f;
      }
      destination[3] = 1.0f;
    }
  });
}

bool BKE_mesh_maps_bake_cage_matches(const Mesh &low, const Mesh &cage)
{
  return low.faces_num == cage.faces_num && low.corners_num == cage.corners_num;
}

void BKE_mesh_maps_bake_fill_id_from_high_poly(const BakePixel *pixels,
                                               const size_t pixels_num,
                                               const Object *const *objects,
                                               const size_t objects_num,
                                               const Mesh *const *meshes,
                                               const size_t meshes_num,
                                               const int8_t type,
                                               const int channels,
                                               float *result)
{
  if (pixels == nullptr || result == nullptr || channels < 3) {
    return;
  }
  for (size_t i = 0; i < pixels_num; i++) {
    const int object_index = pixels[i].object_id;
    if (object_index < 0 || size_t(object_index) >= objects_num) {
      continue;
    }
    float color[3];
    if (type == MA_MESH_MAP_ID_OBJECT) {
      BKE_mesh_maps_bake_id_object_color(*objects[object_index], color);
    }
    else if (type == MA_MESH_MAP_ID_MATERIAL) {
      if (size_t(object_index) >= meshes_num) {
        continue;
      }
      const Mesh *source_mesh = meshes[object_index];
      if (source_mesh == nullptr) {
        continue;
      }
      const int triangle = pixels[i].primitive_id;
      const Span<int> tri_faces = source_mesh->corner_tri_faces();
      if (triangle < 0 || triangle >= int(tri_faces.size())) {
        continue;
      }
      const int face = tri_faces[triangle];
      const VArraySpan<int> material_indices = *source_mesh->attributes().lookup<int>(
          "material_index", bke::AttrDomain::Face);
      const int index = material_indices.is_empty() ? 0 : material_indices[face];
      BKE_mesh_maps_bake_id_material_color(index, color);
    }
    else {
      continue;
    }
    float *destination = result + i * size_t(channels);
    copy_v3_v3(destination, color);
    if (channels >= 4) {
      destination[3] = 1.0f;
    }
  }
}

void BKE_mesh_maps_bake_write_covered(const int8_t type,
                                      const BakePixel *pixels,
                                      const size_t pixels_num,
                                      const int channels,
                                      const float *result,
                                      MutableSpan<float> atlas)
{
  if (pixels == nullptr || result == nullptr || channels < 3) {
    return;
  }
  if (atlas.size() < pixels_num * size_t(channels)) {
    return;
  }
  const bool scalar = BKE_mesh_maps_bake_type_scalar(type);
  for (size_t i = 0; i < pixels_num; i++) {
    if (pixels[i].primitive_id < 0) {
      continue;
    }
    const float *source = result + i * size_t(channels);
    float *destination = atlas.data() + i * size_t(channels);
    copy_v3_v3(destination, source);
    if (channels >= 4) {
      destination[3] = source[3];
    }
    if (scalar) {
      destination[1] = source[0];
      destination[2] = source[0];
    }
  }
}

void BKE_mesh_maps_bake_coverage_from_pixels(const BakePixel *pixels,
                                             const size_t pixels_num,
                                             MutableSpan<uint8_t> r_coverage)
{
  if (pixels == nullptr || size_t(r_coverage.size()) < pixels_num) {
    return;
  }
  for (size_t i = 0; i < pixels_num; i++) {
    r_coverage[i] = pixels[i].primitive_id >= 0 ? uint8_t(1) : uint8_t(0);
  }
}

void BKE_mesh_maps_bake_coverage_union(Span<uint8_t> source, MutableSpan<uint8_t> r_destination)
{
  const size_t num = size_t(std::min(source.size(), r_destination.size()));
  for (size_t i = 0; i < num; i++) {
    if (source[i] != 0) {
      r_destination[i] = 1;
    }
  }
}

size_t BKE_mesh_maps_bake_coverage_overlap(Span<uint8_t> a, Span<uint8_t> b)
{
  const size_t num = size_t(std::min(a.size(), b.size()));
  size_t count = 0;
  for (size_t i = 0; i < num; i++) {
    if (a[i] != 0 && b[i] != 0) {
      count++;
    }
  }
  return count;
}

void BKE_mesh_maps_bake_write_with_margin(const int8_t type,
                                          Span<uint8_t> own_coverage,
                                          Span<uint8_t> own_margin,
                                          Span<uint8_t> foreign_coverage,
                                          const int channels,
                                          const float *result,
                                          MutableSpan<float> atlas)
{
  if (result == nullptr || channels < 3) {
    return;
  }
  const size_t pixels_num = size_t(std::min(
      {own_coverage.size(), own_margin.size(), atlas.size() / int64_t(channels)}));
  const bool scalar = BKE_mesh_maps_bake_type_scalar(type);
  for (size_t i = 0; i < pixels_num; i++) {
    const bool covered = own_coverage[i] != 0;
    const bool margin = own_margin[i] != 0;
    if (!covered && !margin) {
      continue;
    }
    /* An own pixel is never given up; only the margin yields to a foreign island. */
    if (!covered && i < size_t(foreign_coverage.size()) && foreign_coverage[i] != 0) {
      continue;
    }
    const float *source = result + i * size_t(channels);
    float *destination = atlas.data() + i * size_t(channels);
    copy_v3_v3(destination, source);
    if (channels >= 4) {
      /* Alpha is the written mask: a texel is 1 exactly when the merge or the dilation wrote it,
       * so the engine's per-texel alpha, which the margin fill does not carry, is dropped. */
      destination[3] = 1.0f;
    }
    if (scalar) {
      destination[1] = source[0];
      destination[2] = source[0];
    }
  }
}

/**
 * Expand \a source by up to \a passes 8-neighborhood steps over a `width x height` grid, OR-ing
 * the reached texels into \a r_destination. This computes the region the variant-A rebake clears;
 * it is a plain boolean expansion, not the weighted color fill of #IMB_filter_extend.
 */
static void mesh_map_bake_mask_dilate(Span<uint8_t> source,
                                      const int width,
                                      const int height,
                                      const int passes,
                                      MutableSpan<uint8_t> r_destination)
{
  const size_t texels = size_t(width) * size_t(height);
  if (width <= 0 || height <= 0 || passes <= 0 || size_t(source.size()) < texels ||
      size_t(r_destination.size()) < texels)
  {
    return;
  }
  Array<uint8_t> front(texels, 0);
  Array<uint8_t> back(texels, 0);
  for (size_t i = 0; i < texels; i++) {
    front[i] = source[i] != 0 ? uint8_t(1) : uint8_t(0);
  }
  for (int pass = 0; pass < passes; pass++) {
    std::atomic<bool> grew = false;
    threading::parallel_for(IndexRange(height), 64, [&](const IndexRange y_range) {
      bool grew_here = false;
      for (const int y : y_range) {
        for (int x = 0; x < width; x++) {
          const size_t i = size_t(y) * size_t(width) + x;
          if (front[i] != 0) {
            back[i] = 1;
            continue;
          }
          uint8_t any = 0;
          for (int dy = -1; dy <= 1 && any == 0; dy++) {
            for (int dx = -1; dx <= 1 && any == 0; dx++) {
              const int nx = x + dx;
              const int ny = y + dy;
              if (nx < 0 || ny < 0 || nx >= width || ny >= height) {
                continue;
              }
              any = front[size_t(ny) * size_t(width) + nx];
            }
          }
          back[i] = any != 0 ? uint8_t(1) : uint8_t(0);
          grew_here = grew_here || any != 0;
        }
      }
      if (grew_here) {
        grew.store(true, std::memory_order_relaxed);
      }
    });
    std::swap(front, back);
    if (!grew.load(std::memory_order_relaxed)) {
      /* The expansion is stable: further passes add nothing. */
      break;
    }
  }
  for (size_t i = 0; i < texels; i++) {
    if (front[i] != 0) {
      r_destination[i] = 1;
    }
  }
}

void BKE_mesh_maps_bake_extend_passes(const int8_t type,
                                      Span<uint8_t> own_coverage,
                                      Span<uint8_t> own_margin,
                                      Span<uint8_t> foreign_coverage,
                                      const int channels,
                                      const float *result,
                                      const int margin,
                                      const int iterations,
                                      ImBuf &tile_buffer)
{
  const size_t texels = size_t(tile_buffer.x) * size_t(tile_buffer.y);
  if (tile_buffer.float_data() == nullptr || channels < 3 || result == nullptr || texels == 0 ||
      size_t(own_coverage.size()) < texels || size_t(own_margin.size()) < texels ||
      size_t(foreign_coverage.size()) < texels)
  {
    /* All three masks and the result come from the caller's tile buffers: a shape that does not
     * hold the whole tile is a bug in the caller, not an input this function can recover from.
     * Refuse loudly in a debug build and touch nothing in release. */
    BLI_assert_unreachable();
    return;
  }
  const int clear_radius = std::max(margin, 0) + std::max(iterations, 0);

  /* Variant A of the rebake semantics (documented in BKE_mesh_maps_bake.hh): the tile's previous
   * margin ring of this object is stale the moment its geometry changed, so it is wiped before
   * anything is merged. The region is the one the merge and the extend are about to rewrite --
   * `margin + iterations` 8-neighborhood steps past the own coverage -- minus the foreign
   * islands, which stay protected, and minus the own coverage, which the merge rewrites with
   * fresh values. A foreign island's spilled margin inside the region is deliberately not
   * protected: it is that island's own rebake that restores it. */
  if (clear_radius > 0) {
    Array<uint8_t> region(texels, 0);
    mesh_map_bake_mask_dilate(own_coverage,
                              tile_buffer.x,
                              tile_buffer.y,
                              clear_radius,
                              MutableSpan<uint8_t>(region.data(), region.size()));
    float *atlas = tile_buffer.float_data_for_write();
    threading::parallel_for(IndexRange(tile_buffer.y), 64, [&](const IndexRange y_range) {
      for (const int y : y_range) {
        for (int x = 0; x < tile_buffer.x; x++) {
          const size_t i = size_t(y) * size_t(tile_buffer.x) + x;
          if (region[i] == 0 || own_coverage[i] != 0 || foreign_coverage[i] != 0) {
            continue;
          }
          float *texel = atlas + i * 4;
          texel[0] = 0.0f;
          texel[1] = 0.0f;
          texel[2] = 0.0f;
          texel[3] = 0.0f;
        }
      }
    });
  }

  BKE_mesh_maps_bake_write_with_margin(
      type,
      own_coverage,
      own_margin,
      foreign_coverage,
      channels,
      result,
      MutableSpan<float>(tile_buffer.float_data_for_write(), int64_t(texels) * 4));

  if (iterations <= 0) {
    return;
  }
  /* The dilation rule is the one #IMB_filter_extend already implements, and the mask is the whole
   * rule: #FILTER_MASK_USED texels -- this object's own and the other objects' islands -- are
   * sources that are never overwritten, #FILTER_MASK_MARGIN texels -- this object's margin fill --
   * are sources too, and every other texel is writable background. */
  Array<char> mask(texels, FILTER_MASK_NULL);
  for (size_t i = 0; i < texels; i++) {
    if (own_coverage[i] != 0 || foreign_coverage[i] != 0) {
      mask[i] = FILTER_MASK_USED;
    }
    else if (own_margin[i] != 0) {
      mask[i] = FILTER_MASK_MARGIN;
    }
  }
  IMB_filter_extend(&tile_buffer, mask.data(), iterations);
  /* Alpha is the written mask (see #BKE_mesh_maps_bake_write_with_margin): the extend averages the
   * source alpha instead of pinning it, so pin 1 back on every texel a pass wrote. */
  float *atlas = tile_buffer.float_data_for_write();
  for (size_t i = 0; i < texels; i++) {
    if (mask[i] == FILTER_MASK_MARGIN) {
      atlas[i * 4 + 3] = 1.0f;
    }
  }
}

bool BKE_mesh_maps_bake_downsample_result(Span<float> source_rgba,
                                          Span<uint8_t> source_coverage,
                                          const int source_resolution,
                                          MutableSpan<float> destination_rgba,
                                          const int destination_resolution,
                                          const int factor)
{
  const size_t source_texels = size_t(source_resolution) * size_t(source_resolution);
  const size_t destination_texels = size_t(destination_resolution) * size_t(destination_resolution);
  if (factor <= 1 || source_resolution != destination_resolution * factor ||
      source_rgba.size() < source_texels * 4 || source_coverage.size() < source_texels ||
      destination_rgba.size() < destination_texels * 4)
  {
    /* The false return is the contract (see the header): a caller that hands over shapes that do
     * not describe a factor-N box-downsample learns so and keeps its destination, instead of a
     * silent no-op it cannot tell from a written one. No assert here, so a test of this path
     * works in debug builds too. */
    return false;
  }
  threading::parallel_for(IndexRange(destination_resolution), 64, [&](const IndexRange y_range) {
    for (const int y : y_range) {
      for (int x = 0; x < destination_resolution; x++) {
        /* An uncovered subtexel is the zeroed background, so it contributes neither color nor
         * weight: averaging it in would darken the island's partially covered edges. */
        float acc_rgb[3] = {0.0f, 0.0f, 0.0f};
        int covered_num = 0;
        for (int dy = 0; dy < factor; dy++) {
          for (int dx = 0; dx < factor; dx++) {
            const size_t sx = size_t(x * factor + dx);
            const size_t sy = size_t(y * factor + dy);
            const size_t s = sy * size_t(source_resolution) + sx;
            if (source_coverage[s] == 0) {
              continue;
            }
            const float *texel = source_rgba.data() + s * 4;
            for (int c = 0; c < 3; c++) {
              acc_rgb[c] += texel[c];
            }
            covered_num++;
          }
        }
        const size_t d = size_t(y) * size_t(destination_resolution) + x;
        if (covered_num == 0) {
          /* A fully uncovered block is background, whatever the caller's buffer held before. */
          for (int c = 0; c < 4; c++) {
            destination_rgba[d * 4 + c] = 0.0f;
          }
          continue;
        }
        for (int c = 0; c < 3; c++) {
          destination_rgba[d * 4 + c] = acc_rgb[c] / float(covered_num);
        }
        /* Alpha follows the coverage, not the averaged source: the engine only sets it on covered
         * texels and the margin fill leaves it untouched, so it carries no weight information. */
        destination_rgba[d * 4 + 3] = 1.0f;
      }
    }
  });
  return true;
}

bool BKE_mesh_maps_bake_downsample_coverage(Span<uint8_t> source,
                                            const int source_resolution,
                                            MutableSpan<uint8_t> destination,
                                            const int destination_resolution,
                                            const int factor)
{
  const size_t source_texels = size_t(source_resolution) * size_t(source_resolution);
  const size_t destination_texels = size_t(destination_resolution) * size_t(destination_resolution);
  if (factor <= 1 || source_resolution != destination_resolution * factor ||
      source.size() < source_texels || destination.size() < destination_texels)
  {
    /* Same contract as #BKE_mesh_maps_bake_downsample_result: a refused resolve is reported, not
     * silenced, and the destination stays untouched. No assert here, so a test of this path
     * works in debug builds too. */
    return false;
  }
  threading::parallel_for(IndexRange(destination_resolution), 128, [&](const IndexRange y_range) {
    for (const int y : y_range) {
      for (int x = 0; x < destination_resolution; x++) {
        uint8_t any = 0;
        for (int dy = 0; dy < factor && any == 0; dy++) {
          for (int dx = 0; dx < factor && any == 0; dx++) {
            const size_t sx = size_t(x * factor + dx);
            const size_t sy = size_t(y * factor + dy);
            any = source[sy * size_t(source_resolution) + sx];
          }
        }
        destination[size_t(y) * size_t(destination_resolution) + x] = any;
      }
    }
  });
  return true;
}

void BKE_mesh_maps_bake_normalize_normals(MutableSpan<float> rgba, Span<uint8_t> coverage)
{
  const size_t texels = std::min(rgba.size() / 4, coverage.size());
  threading::parallel_for(IndexRange(int64_t(texels)), 1024, [&](const IndexRange range) {
    for (const int64_t i : range) {
      if (coverage[size_t(i)] == 0) {
        continue;
      }
      float *texel = rgba.data() + size_t(i) * 4;
      const float len = std::sqrt(texel[0] * texel[0] + texel[1] * texel[1] + texel[2] * texel[2]);
      if (len > 0.0f) {
        texel[0] /= len;
        texel[1] /= len;
        texel[2] /= len;
      }
    }
  });
}

void BKE_mesh_maps_bake_resolve_raster(Span<float> raster_rgba,
                                       Span<uint8_t> raster_own,
                                       Span<uint8_t> raster_margin,
                                       const int8_t type,
                                       const int raster_resolution,
                                       const int atlas_resolution,
                                       const int tiles_num,
                                       MutableSpan<float> r_atlas_rgba,
                                       MutableSpan<uint8_t> r_atlas_own,
                                       MutableSpan<uint8_t> r_atlas_margin)
{
  const int factor = atlas_resolution > 0 ? raster_resolution / atlas_resolution : 0;
  const int64_t raster_texels_per_tile = int64_t(raster_resolution) * int64_t(raster_resolution);
  const int64_t atlas_texels_per_tile = int64_t(atlas_resolution) * int64_t(atlas_resolution);
  const int64_t raster_total = raster_texels_per_tile * int64_t(tiles_num);
  const int64_t atlas_total = atlas_texels_per_tile * int64_t(tiles_num);
  if (tiles_num <= 0 || atlas_resolution <= 0 || factor < 2 ||
      int64_t(atlas_resolution) * factor != int64_t(raster_resolution) ||
      raster_rgba.size() < raster_total * 4 || raster_own.size() < raster_total ||
      raster_margin.size() < raster_total || r_atlas_rgba.size() < atlas_total ||
      r_atlas_own.size() < atlas_total || r_atlas_margin.size() < atlas_total)
  {
    /* The caller owns the buffer shapes and the factor: a mismatch is a bug in the caller, not an
     * input this resolve can recover from. Refuse loudly in a debug build and leave every output
     * untouched in release. */
    BLI_assert_unreachable();
    return;
  }

  /* The resolve may only average subtexels that carry color: the margin fill already wrote an
   * averaged color into the margin texels, so the union of the two rasters -- not the own
   * coverage alone -- is what keeps partially covered edge texels from fading into the zero
   * background. */
  Array<uint8_t> raster_union(raster_total, 0);
  BKE_mesh_maps_bake_coverage_union(raster_own, raster_union);
  BKE_mesh_maps_bake_coverage_union(raster_margin, raster_union);
  /* The downsampled union is only read by the normal renormalization, but every tile computes it
   * next to the other masks, so one buffer serves the whole pair. */
  Array<uint8_t> union_atlas(atlas_total, 0);

  for (const int tile : IndexRange(tiles_num)) {
    const int64_t raster_start = int64_t(tile) * raster_texels_per_tile;
    const int64_t atlas_start = int64_t(tile) * atlas_texels_per_tile;
    const bool own_ok = BKE_mesh_maps_bake_downsample_coverage(
        Span<uint8_t>(raster_own.data() + raster_start, raster_texels_per_tile),
        raster_resolution,
        MutableSpan<uint8_t>(r_atlas_own.data() + atlas_start, atlas_texels_per_tile),
        atlas_resolution,
        factor);
    const bool margin_ok = BKE_mesh_maps_bake_downsample_coverage(
        Span<uint8_t>(raster_margin.data() + raster_start, raster_texels_per_tile),
        raster_resolution,
        MutableSpan<uint8_t>(r_atlas_margin.data() + atlas_start, atlas_texels_per_tile),
        atlas_resolution,
        factor);
    const bool union_ok = BKE_mesh_maps_bake_downsample_coverage(
        Span<uint8_t>(raster_union.data() + raster_start, raster_texels_per_tile),
        raster_resolution,
        MutableSpan<uint8_t>(union_atlas.data() + atlas_start, atlas_texels_per_tile),
        atlas_resolution,
        factor);
    const bool result_ok = BKE_mesh_maps_bake_downsample_result(
        raster_rgba.slice(raster_start * 4, raster_texels_per_tile * 4),
        Span<uint8_t>(raster_union.data() + raster_start, raster_texels_per_tile),
        raster_resolution,
        MutableSpan<float>(r_atlas_rgba.data() + atlas_start * 4, atlas_texels_per_tile * 4),
        atlas_resolution,
        factor);
    /* The shapes were validated above, so the downsample helpers cannot refuse. */
    BLI_assert(own_ok && margin_ok && union_ok && result_ok);
    UNUSED_VARS_NDEBUG(own_ok, margin_ok, union_ok, result_ok);
  }
  /* Box-averaging a normal field shrinks any block that mixed directions below unit length, and
   * the atlas expects unit normals, so the resolved normals are renormalized before the merge. */
  eBakeSpace space = R_BAKE_SPACE_WORLD;
  if (BKE_mesh_maps_bake_normal_space_for_type(type, space)) {
    BKE_mesh_maps_bake_normalize_normals(r_atlas_rgba, union_atlas.as_span());
  }
}

void BKE_mesh_maps_bake_fill_curvature_from_normal(const BakePixel *pixels,
                                                   const size_t pixels_num,
                                                   const int resolution,
                                                   const Span<float> normal_rgba,
                                                   const int radius,
                                                   const float scale,
                                                   const bool directx,
                                                   float *result)
{
  const size_t texels = size_t(resolution) * size_t(resolution);
  if (pixels == nullptr || result == nullptr || resolution <= 0 ||
      normal_rgba.size() < texels * 4)
  {
    return;
  }
  /* The raster of one tile never holds more texels than the tile itself: bounding the walk keeps a
   * caller that passed a whole-atlas count from reading and writing past this tile. */
  const size_t count = std::min(pixels_num, texels);
  const int r = std::clamp(
      radius, MESH_MAP_BAKE_CURVATURE_RADIUS_MIN, MESH_MAP_BAKE_CURVATURE_RADIUS_MAX);
  /* Decode one texel of the tangent normal atlas into its raw signed direction. The DirectX
   * convention stores the green channel flipped, which the decode undoes. */
  const auto decode_normal = [&](const size_t texel, float3 &r_n) {
    const float *t = normal_rgba.data() + texel * 4;
    r_n.x = t[0] * 2.0f - 1.0f;
    r_n.y = directx ? 1.0f - 2.0f * t[1] : t[1] * 2.0f - 1.0f;
    r_n.z = t[2] * 2.0f - 1.0f;
  };
  threading::parallel_for(IndexRange(int64_t(count)), 1024, [&](const IndexRange range) {
    for (const int i : range) {
      const size_t index = size_t(i);
      if (pixels[index].primitive_id < 0) {
        continue;
      }
      const int x = int(index % size_t(resolution));
      const int y = int(index / size_t(resolution));
      /* The raster's texel index is the atlas's own, so the neighbors are plain texel offsets.
       * A side only counts when both of its samples are covered; with one covered side the
       * difference is one-sided, and without any the texel stays neutral. */
      const auto covered = [&](const int nx, const int ny) {
        return nx >= 0 && ny >= 0 && nx < resolution && ny < resolution &&
               pixels[size_t(ny) * size_t(resolution) + nx].primitive_id >= 0;
      };
      float3 center;
      decode_normal(index, center);
      /* Derivative estimates: a two-sided difference steps 2r texels, a one-sided one r, so
       * both divide by their step and the radius drops out of the magnitude. */
      float div_u = 0.0f;
      float div_v = 0.0f;
      int axes = 0;
      float3 sample;
      if (covered(x + r, y) && covered(x - r, y)) {
        decode_normal(size_t(y) * size_t(resolution) + size_t(x + r), sample);
        div_u += sample.x;
        decode_normal(size_t(y) * size_t(resolution) + size_t(x - r), sample);
        div_u -= sample.x;
        div_u /= float(2 * r);
        axes++;
      }
      else if (covered(x + r, y)) {
        decode_normal(size_t(y) * size_t(resolution) + size_t(x + r), sample);
        div_u += (sample.x - center.x) / float(r);
        axes++;
      }
      else if (covered(x - r, y)) {
        decode_normal(size_t(y) * size_t(resolution) + size_t(x - r), sample);
        div_u += (center.x - sample.x) / float(r);
        axes++;
      }
      if (covered(x, y + r) && covered(x, y - r)) {
        decode_normal(size_t(y + r) * size_t(resolution) + x, sample);
        div_v += sample.y;
        decode_normal(size_t(y - r) * size_t(resolution) + x, sample);
        div_v -= sample.y;
        div_v /= float(2 * r);
        axes++;
      }
      else if (covered(x, y + r)) {
        decode_normal(size_t(y + r) * size_t(resolution) + x, sample);
        div_v += (sample.y - center.y) / float(r);
        axes++;
      }
      else if (covered(x, y - r)) {
        decode_normal(size_t(y - r) * size_t(resolution) + x, sample);
        div_v += (center.y - sample.y) / float(r);
        axes++;
      }
      if (axes == 0) {
        result[index * 4 + 0] = 0.5f;
        result[index * 4 + 1] = 0.5f;
        result[index * 4 + 2] = 0.5f;
        result[index * 4 + 3] = 1.0f;
        continue;
      }
      /* The trace of the tangent normal's image gradient: convex surfaces spread their normals
       * (positive), concave ones converge (negative). */
      const float curvature = std::clamp((div_u + div_v) * scale, -1.0f, 1.0f);
      const float value = 0.5f + 0.5f * curvature;
      result[index * 4 + 0] = value;
      result[index * 4 + 1] = value;
      result[index * 4 + 2] = value;
      result[index * 4 + 3] = 1.0f;
    }
  });
}

int BKE_mesh_maps_bake_foreign_coverage(Main &bmain,
                                        Scene &scene,
                                        ViewLayer &view_layer,
                                        const Object &ob,
                                        const Material &ma,
                                        const int resolution,
                                        const Span<int> tiles,
                                        Span<uint8_t> own_coverage,
                                        FunctionRef<void(Mesh &mesh,
                                                         MutableSpan<BakePixel> pixels,
                                                         int resolution,
                                                         StringRef uv_name,
                                                         int tile_number)> populate_pixels,
                                        MutableSpan<uint8_t> r_coverage,
                                        MutableSpan<char> r_overlap_name)
{
  if (!r_overlap_name.is_empty()) {
    r_overlap_name[0] = '\0';
  }
  const size_t pixels_num = tiles.is_empty() ? 0 : size_t(r_coverage.size()) / tiles.size();
  if (pixels_num == 0 || resolution <= 0) {
    return 0;
  }
  Depsgraph *depsgraph = BKE_scene_ensure_depsgraph(&bmain, &scene, &view_layer);
  if (depsgraph == nullptr) {
    return 0;
  }
  BKE_scene_graph_evaluated_ensure(depsgraph, &bmain);

  Array<BakePixel> pixels(pixels_num);
  Array<uint8_t> object_coverage(pixels_num);
  /* High-poly sources are not "foreign" islands: they are geometry the low-poly samples, even when
   * they happen to use the same material and UV layer. */
  Set<const Object *> source_objects;
  BKE_mesh_maps_source_collect_for_material(bmain, ma, source_objects);

  int foreign_num = 0;
  for (Object &candidate : bmain.objects) {
    if (&candidate == &ob || candidate.type != OB_MESH) {
      continue;
    }
    if (source_objects.contains(&candidate)) {
      continue;
    }
    if (BKE_object_material_index_get(&candidate, &ma) < 0) {
      continue;
    }
    /* Any object evaluated by this depsgraph contributes its viewport mesh; one that is not part
     * of it (another scene, say) falls back to its own data mesh, without modifiers. */
    Object *ob_eval = DEG_get_evaluated(depsgraph, &candidate);
    Mesh *mesh = ob_eval != nullptr ? BKE_object_get_evaluated_mesh(ob_eval) : nullptr;
    if (mesh == nullptr) {
      mesh = id_cast<Mesh *>(candidate.data);
    }
    if (mesh == nullptr) {
      continue;
    }
    /* The atlas is sampled through each object's active UV map; one without a usable map owns no
     * texel of it. */
    const char *uv_name = BKE_mesh_maps_uv_resolve(*mesh);
    if (uv_name == nullptr) {
      continue;
    }
    for (const int tile_index : tiles.index_range()) {
      populate_pixels(*mesh, pixels, resolution, StringRef(uv_name), tiles[tile_index]);
      BKE_mesh_maps_bake_restrict_to_material(pixels.data(), pixels_num, *mesh, candidate, ma);
      BKE_mesh_maps_bake_coverage_from_pixels(pixels.data(), pixels_num, object_coverage);
      const int64_t tile_start = int64_t(tile_index) * int64_t(pixels_num);
      const Span<uint8_t> own_tile = own_coverage.slice(tile_start, int64_t(pixels_num));
      MutableSpan<uint8_t> coverage_tile = r_coverage.slice(tile_start, int64_t(pixels_num));
      if (!r_overlap_name.is_empty() && r_overlap_name[0] == '\0' &&
          BKE_mesh_maps_bake_coverage_overlap(own_tile, object_coverage) > 0)
      {
        BLI_strncpy(r_overlap_name.data(), candidate.id.name + 2, r_overlap_name.size());
      }
      BKE_mesh_maps_bake_coverage_union(object_coverage, coverage_tile);
    }
    foreign_num++;
  }
  return foreign_num;
}

Image *BKE_mesh_maps_bake_atlas_ensure(Main &bmain,
                                       Material &ma,
                                       const int8_t type,
                                       const int resolution)
{
  if (!mesh_map_bake_type_valid(type) || resolution <= 0) {
    return nullptr;
  }
  const MaterialMeshMapSlot *slot = BKE_mesh_maps_slot_find(ma, type);
  if (slot != nullptr && slot->image != nullptr) {
    int width = 0;
    int height = 0;
    BKE_image_get_size(slot->image, nullptr, &width, &height);
    /* The atlas became a tiled image (the bake is one raster per UDIM tile), so a plain atlas from
     * an older file is replaced like one of another resolution: the fresh one takes the slot, and
     * the next bake fills its tiles from scratch. */
    if (width == resolution && height == resolution &&
        slot->image->source == IMA_SRC_TILED)
    {
      return slot->image;
    }
  }
  /* A pristine atlas is transparent black so alpha reads as the written mask; RGB stays 0 so
   * filtering never pulls stale color from the background. The tile ensure
   * (#BKE_paint_layers_map_coverage_tile_ensure) fills every UDIM tile added later from this base
   * tile's gen color, so the tiles follow without an initialization of their own. */
  const float black[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  Image *image = BKE_image_add_generated(&bmain,
                                         unsigned(resolution),
                                         unsigned(resolution),
                                         "MeshMap",
                                         /*depth*/ 32,
                                         /*floatbuf*/ true,
                                         IMA_GENTYPE_BLANK,
                                         black,
                                         /*stereo3d*/ false,
                                         /*is_data*/ true,
                                         /*tiled*/ true);
  if (image == nullptr) {
    return nullptr;
  }
  image->alpha_mode = IMA_ALPHA_STRAIGHT;
  if (!BKE_mesh_maps_slot_image_set(ma, type, image)) {
    return nullptr;
  }
  return image;
}

Vector<int> BKE_mesh_maps_bake_tiles_with_pixels(const Span<BakePixel> pixels,
                                                 const int64_t pixels_per_tile,
                                                 const Span<int> tiles)
{
  Vector<int> kept;
  if (pixels_per_tile <= 0 || tiles.is_empty() ||
      pixels.size() != pixels_per_tile * int64_t(tiles.size()))
  {
    /* A shape this function does not understand keeps every tile: the caller bakes what it
     * allocated, and the commit still writes per tile. */
    for (const int tile_index : tiles.index_range()) {
      kept.append(tile_index);
    }
    return kept;
  }
  for (const int tile_index : tiles.index_range()) {
    const Span<BakePixel> tile_pixels = pixels.slice(int64_t(tile_index) * pixels_per_tile,
                                                     pixels_per_tile);
    const bool has_pixels = std::any_of(
        tile_pixels.begin(), tile_pixels.end(), [](const BakePixel &pixel) {
          return pixel.primitive_id != -1;
        });
    if (has_pixels) {
      kept.append(tile_index);
    }
  }
  return kept;
}

const char *BKE_mesh_maps_bake_uv_resolve_or_fail(ObjectMeshMapState &state,
                                                  const Object &ob_eval)
{
  const Mesh *mesh = nullptr;
  if (ob_eval.type == OB_MESH) {
    mesh = BKE_object_get_evaluated_mesh(&ob_eval);
    if (mesh == nullptr) {
      mesh = id_cast<const Mesh *>(ob_eval.data);
    }
  }
  if (mesh == nullptr) {
    BKE_mesh_maps_bake_state_fail(state);
    return nullptr;
  }
  const char *name = BKE_mesh_maps_uv_resolve(*mesh);
  if (name == nullptr) {
    BKE_mesh_maps_bake_state_fail(state);
    return nullptr;
  }
  return name;
}

bool BKE_mesh_maps_bake_hash_from_viewport(Main &bmain,
                                           Scene &scene,
                                           ViewLayer &view_layer,
                                           const Object &ob,
                                           const Material &ma,
                                           const int8_t type,
                                           uint32_t r_hash[2])
{
  r_hash[0] = 0;
  r_hash[1] = 0;
  Depsgraph *depsgraph = BKE_scene_ensure_depsgraph(&bmain, &scene, &view_layer);
  if (depsgraph == nullptr) {
    return false;
  }
  BKE_scene_graph_evaluated_ensure(depsgraph, &bmain);
  Object *ob_eval = DEG_get_evaluated(depsgraph, const_cast<Object *>(&ob));
  if (ob_eval == nullptr || ob_eval == &ob) {
    return false;
  }
  BKE_mesh_maps_object_hash(*ob_eval, ma, type, r_hash);
  return true;
}

bool BKE_mesh_maps_bake_atlas_is_float(Image &image)
{
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(&image, nullptr, &lock);
  const bool is_float = ibuf != nullptr && ibuf->float_data() != nullptr;
  BKE_image_release_ibuf(&image, ibuf, lock);
  return is_float;
}

bool BKE_mesh_maps_bake_commit_is_current(const uint32_t start_hash[2],
                                          const uint32_t now_hash[2])
{
  return start_hash[0] == now_hash[0] && start_hash[1] == now_hash[1];
}

int8_t BKE_mesh_maps_bake_state_begin(ObjectMeshMapState &state)
{
  const int8_t previous = state.status;
  state.status = OB_MESH_MAP_STATUS_BAKING;
  return previous;
}

void BKE_mesh_maps_bake_state_cancel(ObjectMeshMapState &state, const int8_t previous_status)
{
  state.status = previous_status;
}

void BKE_mesh_maps_bake_state_fail(ObjectMeshMapState &state)
{
  state.status = OB_MESH_MAP_STATUS_ERROR;
}

int8_t BKE_mesh_maps_bake_state_commit(ObjectMeshMapState &state,
                                       const uint32_t start_hash[2],
                                       const uint32_t now_hash[2],
                                       const int baked_time)
{
  if (!BKE_mesh_maps_bake_commit_is_current(start_hash, now_hash)) {
    state.status = OB_MESH_MAP_STATUS_STALE;
    return state.status;
  }
  BKE_mesh_maps_object_state_mark_baked(state, start_hash, baked_time);
  return state.status;
}

bool BKE_mesh_maps_bake_type_is_supported(const int8_t type)
{
  const MeshMapTypeInfo *info = BKE_mesh_maps_type_info_get(type);
  return info != nullptr && info->flags != 0;
}

bool BKE_mesh_maps_bake_object_in_plan(Main &bmain,
                                       Object &ob,
                                       const Material &ma,
                                       const Object *active_object,
                                       const int8_t object_scope,
                                       const Set<const Object *> *source_objects)
{
  if (object_scope == MA_MESH_MAP_BAKE_ACTIVE_OBJECT) {
    return active_object != nullptr && &ob == active_object;
  }
  /* See #BKE_mesh_maps_source_collect_for_material: non-mesh objects have no material count. */
  if (ob.type != OB_MESH || ob.data == nullptr) {
    return false;
  }
  if (BKE_object_material_index_get(&ob, &ma) < 0) {
    return false;
  }
  /* High-poly sources are never baked as low-poly: they are inputs of other objects' bakes. */
  if (source_objects != nullptr) {
    return !source_objects->contains(&ob);
  }
  Set<const Object *> local_source_objects;
  BKE_mesh_maps_source_collect_for_material(bmain, ma, local_source_objects);
  return !local_source_objects.contains(&ob);
}

void BKE_mesh_maps_bake_target_objects(Main &bmain,
                                       Material &ma,
                                       Vector<Object *> &r_objects)
{
  /* Collect the sources once: the predicate must not rescan every object per candidate. */
  Set<const Object *> source_objects;
  BKE_mesh_maps_source_collect_for_material(bmain, ma, source_objects);
  for (Object &ob : bmain.objects) {
    if (BKE_mesh_maps_bake_object_in_plan(
            bmain, ob, ma, nullptr, MA_MESH_MAP_BAKE_ALL_OBJECTS, &source_objects))
    {
      r_objects.append(&ob);
    }
  }
}

/* The Normal Filter curvature reads the baked Tangent Normal atlas of the same object, so the
 * ascending type order would fail the Curvature pair on a fresh material until a second run.
 * Reordering here keeps the plan pure data that the operator and the tests agree on, and the
 * stable reorder preserves the visible "bake all" order otherwise. */
static void mesh_map_bake_plan_pairs_sort_dependencies(Vector<MeshMapBakePair> &r_pairs,
                                                       const Material &ma)
{
  if (ma.mesh_map_settings.curvature_method != MA_MESH_MAP_CURVATURE_NORMAL_FILTER) {
    /* Pointiness and the other methods render the Curvature pair on their own; the ascending
     * order needs no edge enforced. */
    return;
  }
  /* The builder appends every type of an object together, so the pairs form contiguous runs per
   * object. Walk the runs and enforce the dependency inside each one. */
  for (int i = 0; i < r_pairs.size(); i++) {
    int last = i;
    while (last + 1 < r_pairs.size() && r_pairs[last + 1].object == r_pairs[i].object) {
      last++;
    }
    int first_curvature = -1;
    int first_tangent = -1;
    int last_tangent = -1;
    for (int j = i; j <= last; j++) {
      if (r_pairs[j].type == MA_MESH_MAP_CURVATURE && first_curvature == -1) {
        first_curvature = j;
      }
      else if (r_pairs[j].type == MA_MESH_MAP_NORMAL_TANGENT) {
        last_tangent = j;
        if (first_tangent == -1) {
          first_tangent = j;
        }
      }
    }
    if (first_curvature != -1 && first_tangent != -1 && first_tangent > first_curvature) {
      /* Pull the tangent segment in front of the curvature segment: rotating
       * [first_curvature, last_tangent + 1) at the first tangent keeps every other pair, both
       * moved segments' relative order and the object order untouched. */
      std::rotate(r_pairs.begin() + first_curvature,
                  r_pairs.begin() + first_tangent,
                  r_pairs.begin() + last_tangent + 1);
    }
    i = last;
  }
}

void BKE_mesh_maps_bake_plan_pairs(Main &bmain,
                                   Object *active_object,
                                   Material &ma,
                                   const int8_t object_scope,
                                   const uint32_t type_mask,
                                   Vector<MeshMapBakePair> &r_pairs)
{
  /* Collect once for the whole sweep: the predicate must not rescan every object per candidate. */
  Set<const Object *> source_objects;
  const Set<const Object *> *source_objects_ptr = nullptr;
  if (object_scope != MA_MESH_MAP_BAKE_ACTIVE_OBJECT) {
    BKE_mesh_maps_source_collect_for_material(bmain, ma, source_objects);
    source_objects_ptr = &source_objects;
  }
  for (Object &ob : bmain.objects) {
    if (!BKE_mesh_maps_bake_object_in_plan(
            bmain, ob, ma, active_object, object_scope, source_objects_ptr))
    {
      continue;
    }
    for (int type = 0; type < MA_MESH_MAP_TYPE_NUM; type++) {
      if ((type_mask & (1u << uint(type))) == 0) {
        continue;
      }
      if (!BKE_mesh_maps_bake_type_is_supported(int8_t(type))) {
        continue;
      }
      r_pairs.append(MeshMapBakePair{&ob, &ma, int8_t(type)});
    }
  }
  mesh_map_bake_plan_pairs_sort_dependencies(r_pairs, ma);
}

void BKE_mesh_maps_bake_states_rollback(Span<ObjectMeshMapState *> states,
                                        Span<int8_t> previous_statuses)
{
  const size_t num = size_t(std::min(states.size(), previous_statuses.size()));
  for (size_t i = 0; i < num; i++) {
    if (states[i] != nullptr) {
      BKE_mesh_maps_bake_state_cancel(*states[i], previous_statuses[i]);
    }
  }
}

int BKE_mesh_maps_bake_clear_type(Main &bmain, Material &ma, const int8_t type)
{
  if (!mesh_map_bake_type_valid(type)) {
    return 0;
  }
  int states_reset = 0;
  for (Object &ob : bmain.objects) {
    ObjectMeshMapState *state = BKE_mesh_maps_object_state_find(ob, ma, type);
    if (state == nullptr) {
      continue;
    }
    state->status = OB_MESH_MAP_STATUS_NONE;
    state->hash[0] = 0;
    state->hash[1] = 0;
    state->baked_time = 0;
    states_reset++;
  }

  MaterialMeshMapSlot *slot = BKE_mesh_maps_slot_find(ma, type);
  if (slot == nullptr || slot->image == nullptr) {
    return states_reset;
  }
  Image *image = slot->image;
  /* Every tile the atlas holds is cleared: the atlas is a tiled image, so a bake that covered a
   * face on another UDIM tile has its texels there, not only in the base one. A tile without a
   * buffer holds no texels to clear and is skipped rather than generated for. */
  BKE_image_tile_buffers_foreach(*image, [&](const int /*tile_number*/, ImBuf &ibuf) {
    if (ibuf.float_data() != nullptr) {
      float *data = ibuf.float_data_for_write();
      const int channels = 4;
      const size_t num = size_t(ibuf.x) * size_t(ibuf.y);
      for (size_t i = 0; i < num; i++) {
        float *texel = data + i * size_t(channels);
        texel[0] = 0.0f;
        texel[1] = 0.0f;
        texel[2] = 0.0f;
        texel[3] = 0.0f;
      }
      ibuf.userflags |= IB_DISPLAY_BUFFER_INVALID;
      BKE_image_mark_dirty(image, &ibuf);
    }
    return true;
  });
  BKE_image_partial_update_mark_full_update(image);
  BKE_image_free_gputextures(image);
  BKE_image_memorypack(image);
  return states_reset;
}

}  // namespace blender
