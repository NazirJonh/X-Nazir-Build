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
#include <cmath>

#include "BLI_array.hh"
#include "BLI_ghash.h"
#include "BLI_hash.h"
#include "BLI_index_range.hh"
#include "BLI_listbase_iterator.hh"
#include "BLI_math_color.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector_types.hh"
#include "BLI_set.hh"
#include "BLI_string.h"
#include "BLI_ustring.hh"
#include "BLI_utildefines.h"
#include "BLI_virtual_array.hh"

#include "BKE_attribute.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_maps.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_object.hh"
#include "BKE_paint_layers.hh"
#include "BKE_scene.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "IMB_imbuf_types.hh"

#include "DNA_customdata_types.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"

#include "RE_bake.h"

namespace blender {

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
  return ELEM(type,
              MA_MESH_MAP_AO,
              MA_MESH_MAP_CURVATURE,
              MA_MESH_MAP_EDGE,
              MA_MESH_MAP_NORMAL_WORLD,
              MA_MESH_MAP_NORMAL_OBJECT);
}

bool BKE_mesh_maps_bake_type_cpu_id(const int8_t type)
{
  return ELEM(type, MA_MESH_MAP_ID_OBJECT, MA_MESH_MAP_ID_MATERIAL);
}

bool BKE_mesh_maps_bake_type_scalar(const int8_t type)
{
  return ELEM(type, MA_MESH_MAP_AO, MA_MESH_MAP_CURVATURE, MA_MESH_MAP_EDGE);
}

eScenePassType BKE_mesh_maps_bake_pass_for_type(const int8_t type)
{
  switch (type) {
    case MA_MESH_MAP_AO:
    case MA_MESH_MAP_CURVATURE:
    case MA_MESH_MAP_EDGE:
      return SCE_PASS_EMIT;
    case MA_MESH_MAP_NORMAL_WORLD:
    case MA_MESH_MAP_NORMAL_OBJECT:
      return SCE_PASS_NORMAL;
    default:
      return eScenePassType(0);
  }
}

bool BKE_mesh_maps_bake_normal_space_for_type(const int8_t type, eBakeSpace &r_space)
{
  switch (type) {
    case MA_MESH_MAP_NORMAL_WORLD:
      r_space = R_BAKE_SPACE_WORLD;
      return true;
    case MA_MESH_MAP_NORMAL_OBJECT:
      r_space = R_BAKE_SPACE_OBJECT;
      return true;
    default:
      return false;
  }
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

bNode *BKE_mesh_maps_bake_build_emit_tree(bNodeTree &tree,
                                          const int8_t type,
                                          const MaterialMeshMapSettings &settings,
                                          const float ao_distance)
{
  bNodeSocket *source = nullptr;
  bNode *source_node = nullptr;

  switch (type) {
    case MA_MESH_MAP_AO: {
      bNode *ao = bke::node_add_static_node(nullptr, tree, SH_NODE_AMBIENT_OCCLUSION);
      if (ao == nullptr) {
        return nullptr;
      }
      /* Only the object itself occludes, and the per-shader-evaluation ray count is the bake's
       * sample count. The plane stays unlinked so Cycles shades the actual normal. */
      ao->custom1 = std::max(settings.samples, 1);
      ao->custom2 |= SHD_AO_LOCAL;
      if (bNodeSocket *distance = bke::node_find_socket(*ao, SOCK_IN, UString("Distance"))) {
        mesh_map_bake_set_float(*distance, ao_distance > 0.0f ? ao_distance : 1.0f);
      }
      source = bke::node_find_socket(*ao, SOCK_OUT, UString("AO"));
      source_node = ao;
      break;
    }
    case MA_MESH_MAP_CURVATURE: {
      bNode *geometry = bke::node_add_static_node(nullptr, tree, SH_NODE_NEW_GEOMETRY);
      if (geometry == nullptr) {
        return nullptr;
      }
      /* Cycles requests #ATTR_STD_POINTINESS itself when the output is linked. */
      source = bke::node_find_socket(*geometry, SOCK_OUT, UString("Pointiness"));
      source_node = geometry;
      break;
    }
    case MA_MESH_MAP_EDGE: {
      bNode *bevel = bke::node_add_static_node(nullptr, tree, SH_NODE_BEVEL);
      bNode *geometry = bke::node_add_static_node(nullptr, tree, SH_NODE_NEW_GEOMETRY);
      bNode *dot = bke::node_add_static_node(nullptr, tree, SH_NODE_VECTOR_MATH);
      bNode *subtract = bke::node_add_static_node(nullptr, tree, SH_NODE_MATH);
      bNode *range = bke::node_add_static_node(nullptr, tree, SH_NODE_MAP_RANGE);
      if (bevel == nullptr || geometry == nullptr || dot == nullptr || subtract == nullptr ||
          range == nullptr)
      {
        return nullptr;
      }
      bevel->custom1 = std::clamp(settings.samples, 2, 128);
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
      source = bke::node_find_socket(*range, SOCK_OUT, UString("Result"));
      if (bevel_normal == nullptr || geometry_normal == nullptr || dot_a == nullptr ||
          dot_b == nullptr || dot_value == nullptr || subtract_a == nullptr ||
          subtract_b == nullptr || subtract_value == nullptr || range_value == nullptr ||
          range_from_min == nullptr || range_from_max == nullptr || range_to_min == nullptr ||
          range_to_max == nullptr || source == nullptr)
      {
        return nullptr;
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
      source_node = range;
      break;
    }
    default:
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
      destination[3] = source[3];
    }
    if (scalar) {
      destination[1] = source[0];
      destination[2] = source[0];
    }
  }
}

int BKE_mesh_maps_bake_foreign_coverage(Main &bmain,
                                        Scene &scene,
                                        ViewLayer &view_layer,
                                        const Object &ob,
                                        const Material &ma,
                                        const int resolution,
                                        Span<uint8_t> own_coverage,
                                        MutableSpan<uint8_t> r_coverage,
                                        MutableSpan<char> r_overlap_name)
{
  if (!r_overlap_name.is_empty()) {
    r_overlap_name[0] = '\0';
  }
  const size_t pixels_num = size_t(r_coverage.size());
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
  /* `RE_bake_pixels_populate` always indexes `material_to_image`, even with no materials; a single
   * null entry that matches the null bake image keeps the texel pass purely about geometry. */
  Image *material_to_image[1] = {nullptr};
  BakeImage bake_image = {};
  bake_image.image = nullptr;
  bake_image.width = resolution;
  bake_image.height = resolution;
  bake_image.offset = 0;
  BakeTargets targets = {};
  targets.images = &bake_image;
  targets.images_num = 1;
  targets.material_to_image = material_to_image;
  targets.materials_num = 1;
  targets.pixels_num = int(pixels_num);
  targets.channels_num = 4;

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
    bool missing = false;
    const char *uv_name = BKE_paint_layers_uv_map_resolve(*mesh, ma, &missing);
    if (missing || uv_name == nullptr || uv_name[0] == '\0') {
      continue;
    }
    RE_bake_pixels_populate(mesh, pixels.data(), pixels_num, &targets, StringRef(uv_name));
    BKE_mesh_maps_bake_restrict_to_material(pixels.data(), pixels_num, *mesh, candidate, ma);
    BKE_mesh_maps_bake_coverage_from_pixels(pixels.data(), pixels_num, object_coverage);
    if (!r_overlap_name.is_empty() && r_overlap_name[0] == '\0' &&
        BKE_mesh_maps_bake_coverage_overlap(own_coverage, object_coverage) > 0)
    {
      BLI_strncpy(r_overlap_name.data(), candidate.id.name + 2, r_overlap_name.size());
    }
    BKE_mesh_maps_bake_coverage_union(object_coverage, r_coverage);
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
    if (width == resolution && height == resolution) {
      return slot->image;
    }
  }
  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
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
                                         /*tiled*/ false);
  if (image == nullptr) {
    return nullptr;
  }
  image->alpha_mode = IMA_ALPHA_STRAIGHT;
  if (!BKE_mesh_maps_slot_image_set(ma, type, image)) {
    return nullptr;
  }
  return image;
}

const char *BKE_mesh_maps_bake_uv_resolve_or_fail(ObjectMeshMapState &state,
                                                  const Object &ob_eval,
                                                  const Material &ma)
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
  bool missing = false;
  const char *name = BKE_paint_layers_uv_map_resolve(*mesh, ma, &missing);
  if (missing || name == nullptr) {
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
  return BKE_mesh_maps_bake_type_renders(type) || BKE_mesh_maps_bake_type_cpu_id(type);
}

void BKE_mesh_maps_bake_plan_pairs(Main &bmain,
                                   Object *active_object,
                                   Material &ma,
                                   const int8_t object_scope,
                                   const uint32_t type_mask,
                                   Vector<MeshMapBakePair> &r_pairs)
{
  const auto append_types = [&](Object &ob) {
    for (int type = 0; type < MA_MESH_MAP_TYPE_NUM; type++) {
      if ((type_mask & (1u << uint(type))) == 0) {
        continue;
      }
      if (!BKE_mesh_maps_bake_type_is_supported(int8_t(type))) {
        continue;
      }
      r_pairs.append(MeshMapBakePair{&ob, &ma, int8_t(type)});
    }
  };

  if (object_scope == MA_MESH_MAP_BAKE_ACTIVE_OBJECT) {
    if (active_object != nullptr) {
      append_types(*active_object);
    }
    return;
  }
  /* High-poly sources are never baked as low-poly: they are inputs of other objects' bakes. */
  Set<const Object *> source_objects;
  BKE_mesh_maps_source_collect_for_material(bmain, ma, source_objects);
  for (Object &ob : bmain.objects) {
    /* See #BKE_mesh_maps_source_collect_for_material: non-mesh objects have no material count. */
    if (ob.type != OB_MESH || ob.data == nullptr) {
      continue;
    }
    if (BKE_object_material_index_get(&ob, &ma) < 0) {
      continue;
    }
    if (source_objects.contains(&ob)) {
      continue;
    }
    append_types(ob);
  }
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
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
  if (ibuf != nullptr) {
    if (ibuf->float_data() != nullptr) {
      float *data = ibuf->float_data_for_write();
      const int channels = 4;
      const size_t num = size_t(ibuf->x) * size_t(ibuf->y);
      for (size_t i = 0; i < num; i++) {
        float *texel = data + i * size_t(channels);
        texel[0] = 0.0f;
        texel[1] = 0.0f;
        texel[2] = 0.0f;
        texel[3] = 1.0f;
      }
      ibuf->userflags |= IB_DISPLAY_BUFFER_INVALID;
      BKE_image_mark_dirty(image, ibuf);
    }
    BKE_image_release_ibuf(image, ibuf, lock);
    BKE_image_partial_update_mark_full_update(image);
    BKE_image_free_gputextures(image);
    BKE_image_memorypack(image);
  }
  return states_reset;
}

}  // namespace blender
