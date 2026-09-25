/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Tests for #BKE_mesh_maps_bake.hh: the per-type bake plan, the AO distance fallback, the emission
 * graphs, the CPU ID maps, the covered-pixel write and the bake state transitions. None of this
 * renders, so the suite needs no engine.
 */

#include "testing/testing.h"

#include <cmath>
#include <cstring>

#include "BLI_array.hh"
#include "BLI_listbase.h"
#include "BLI_listbase_iterator.hh"
#include "BLI_math_base.h"
#include "BLI_span.hh"
#include "BLI_string.h"

#include "BKE_attribute.hh"
#include "BKE_collection.hh"
#include "BKE_global.hh"
#include "BKE_gtest_base.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_maps.hh"
#include "BKE_mesh_maps_bake.hh"
#include "BKE_modifier.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_object.hh"
#include "BKE_scene.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"
#include "DEG_depsgraph_query.hh"

#include "paint_layers_intern.hh"

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_modifier_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "RE_bake.h"

namespace blender {
/* Internal to blenkernel and not exposed by a public header. */
void BKE_mesh_face_offsets_ensure_alloc(Mesh *mesh);
}  // namespace blender

namespace blender::bke::tests {

class MeshMapBakeTest : public bke::BlenderGTestBase {
 public:
  Main *bmain = nullptr;

  void SetUp() override
  {
    bmain = BKE_main_new();
    G_MAIN = bmain;
  }
  void TearDown() override
  {
    BKE_main_free(bmain);
    G_MAIN = nullptr;
  }

  Material *add_material(const char *name)
  {
    return BKE_material_add(bmain, name);
  }

  Object *add_empty(const char *name)
  {
    return BKE_object_add_only_object(bmain, OB_EMPTY, name);
  }

  /** Two triangles: face 0 uses slot 0, face 1 uses slot 1. */
  Mesh *add_pair_mesh(const char *name)
  {
    Mesh *mesh = BKE_mesh_add(bmain, name);
    mesh->verts_num = 4;
    mesh->edges_num = 5;
    mesh->faces_num = 2;
    mesh->corners_num = 6;
    BKE_mesh_face_offsets_ensure_alloc(mesh);
    bke::mesh_ensure_required_data_layers(*mesh);

    const float3 co[4] = {
        {0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
    mesh->vert_positions_for_write().copy_from(Span<float3>(co, 4));
    MutableSpan<int> offsets = mesh->face_offsets_for_write();
    offsets[0] = 0;
    offsets[1] = 3;
    offsets[2] = 6;
    MutableSpan<int> corner_verts = mesh->corner_verts_for_write();
    MutableSpan<int> corner_edges = mesh->corner_edges_for_write();
    const int cverts[6] = {0, 1, 2, 0, 2, 3};
    const int cedges[6] = {0, 1, 2, 2, 3, 4};
    for (int i = 0; i < 6; i++) {
      corner_verts[i] = cverts[i];
      corner_edges[i] = cedges[i];
    }
    MutableSpan<int2> edges = mesh->edges_for_write();
    edges[0] = {0, 1};
    edges[1] = {1, 2};
    edges[2] = {2, 0};
    edges[3] = {2, 3};
    edges[4] = {3, 0};

    bke::SpanAttributeWriter<int> material = mesh->attributes_for_write()
                                                 .lookup_or_add_for_write_span<int>(
                                                     "material_index", bke::AttrDomain::Face);
    material.span[0] = 0;
    material.span[1] = 1;
    material.finish();

    mesh->tag_topology_changed();
    mesh->tag_positions_changed();
    return mesh;
  }

  Object *add_object(const char *name, Mesh *mesh, Material *first, Material *second)
  {
    Object *ob = BKE_object_add_only_object(bmain, OB_MESH, name);
    ob->data = &mesh->id;
    id_us_plus(&mesh->id);
    EXPECT_TRUE(BKE_object_material_slot_add(bmain, ob));
    EXPECT_TRUE(BKE_object_material_slot_add(bmain, ob));
    BKE_object_material_assign(bmain, ob, first, 1, BKE_MAT_ASSIGN_OBJECT);
    BKE_object_material_assign(bmain, ob, second, 2, BKE_MAT_ASSIGN_OBJECT);
    return ob;
  }

  Object *add_simple_object(const char *name, Mesh *mesh, Material *ma)
  {
    Object *ob = BKE_object_add_only_object(bmain, OB_MESH, name);
    ob->data = &mesh->id;
    id_us_plus(&mesh->id);
    EXPECT_TRUE(BKE_object_material_slot_add(bmain, ob));
    BKE_object_material_assign(bmain, ob, ma, 1, BKE_MAT_ASSIGN_OBJECT);
    return ob;
  }

  Scene *add_scene_with_object(const char *name, Object *ob)
  {
    Scene *scene = BKE_scene_add(bmain, name);
    BKE_collection_object_add(bmain, scene->master_collection, ob);
    return scene;
  }

  static void add_subsurf(Object &ob, const int levels, const int render_levels)
  {
    SubsurfModifierData *smd = reinterpret_cast<SubsurfModifierData *>(
        BKE_modifier_new(eModifierType_Subsurf));
    smd->levels = levels;
    smd->renderLevels = render_levels;
    BLI_addtail(&ob.modifiers, smd);
  }

  static Object *eval_object(Main &bmain,
                             Scene &scene,
                             ViewLayer &view_layer,
                             Object &ob,
                             const eEvaluationMode mode,
                             Depsgraph *&r_depsgraph)
  {
    r_depsgraph = DEG_graph_new(&bmain, &scene, &view_layer, mode);
    DEG_graph_build_from_view_layer(r_depsgraph);
    BKE_scene_graph_update_tagged(r_depsgraph, &bmain);
    return DEG_get_evaluated(r_depsgraph, &ob);
  }

  /** A unit quad whose active `UVMap` spans 0..1 and whose `Chosen` spans 0..0.5. */
  Mesh *add_quad_two_uv(const char *name)
  {
    Mesh *mesh = BKE_mesh_add(bmain, name);
    mesh->verts_num = 4;
    mesh->edges_num = 4;
    mesh->faces_num = 1;
    mesh->corners_num = 4;
    BKE_mesh_face_offsets_ensure_alloc(mesh);
    bke::mesh_ensure_required_data_layers(*mesh);

    const float3 co[4] = {
        {0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
    mesh->vert_positions_for_write().copy_from(Span<float3>(co, 4));
    MutableSpan<int> offsets = mesh->face_offsets_for_write();
    offsets[0] = 0;
    offsets[1] = 4;
    MutableSpan<int> corner_verts = mesh->corner_verts_for_write();
    MutableSpan<int> corner_edges = mesh->corner_edges_for_write();
    for (int i = 0; i < 4; i++) {
      corner_verts[i] = i;
      corner_edges[i] = i;
    }
    MutableSpan<int2> edges = mesh->edges_for_write();
    edges[0] = {0, 1};
    edges[1] = {1, 2};
    edges[2] = {2, 3};
    edges[3] = {3, 0};

    bke::MutableAttributeAccessor attributes = mesh->attributes_for_write();
    bke::SpanAttributeWriter<float2> uv = attributes.lookup_or_add_for_write_span<float2>(
        "UVMap", bke::AttrDomain::Corner);
    for (int i = 0; i < 4; i++) {
      uv.span[i] = float2(co[i].x, co[i].y);
    }
    uv.finish();
    bke::SpanAttributeWriter<float2> chosen = attributes.lookup_or_add_for_write_span<float2>(
        "Chosen", bke::AttrDomain::Corner);
    for (int i = 0; i < 4; i++) {
      chosen.span[i] = float2(co[i].x * 0.5f, co[i].y * 0.5f);
    }
    chosen.finish();
    mesh->uv_maps_active_set("UVMap");

    mesh->tag_topology_changed();
    mesh->tag_positions_changed();
    return mesh;
  }

  static bNode *find_node(bNodeTree &tree, const int type)
  {
    for (bNode &node : tree.nodes) {
      if (node.type_legacy == type) {
        return &node;
      }
    }
    return nullptr;
  }

  static int count_nodes(bNodeTree &tree, const int type)
  {
    int count = 0;
    for (bNode &node : tree.nodes) {
      if (node.type_legacy == type) {
        count++;
      }
    }
    return count;
  }

  static float socket_float(bNode &node, const eNodeSocketInOut in_out, const char *name)
  {
    bNodeSocket *socket = bke::node_find_socket(node, in_out, UString(name));
    if (socket == nullptr) {
      return NAN;
    }
    const bNodeSocketValueFloat *stored = socket->default_value_typed<bNodeSocketValueFloat>();
    return stored != nullptr ? stored->value : NAN;
  }

  static bNodeSocket *socket_find(bNode &node, const eNodeSocketInOut in_out, const char *name)
  {
    return bke::node_find_socket(node, in_out, UString(name));
  }

  static bool has_link(bNodeTree &tree, const bNode &from, const bNode &to)
  {
    for (bNodeLink &link : tree.links) {
      if (link.fromnode == &from && link.tonode == &to) {
        return true;
      }
    }
    return false;
  }
};

TEST_F(MeshMapBakeTest, plan_renders_cpu_and_scalar_per_type)
{
  const int8_t rendered[] = {
      MA_MESH_MAP_AO, MA_MESH_MAP_CURVATURE, MA_MESH_MAP_EDGE,
      MA_MESH_MAP_NORMAL_WORLD, MA_MESH_MAP_NORMAL_OBJECT};
  for (const int8_t type : rendered) {
    EXPECT_TRUE(BKE_mesh_maps_bake_type_renders(type)) << int(type);
    EXPECT_FALSE(BKE_mesh_maps_bake_type_cpu_id(type)) << int(type);
  }
  const int8_t cpu[] = {MA_MESH_MAP_ID_OBJECT, MA_MESH_MAP_ID_MATERIAL};
  for (const int8_t type : cpu) {
    EXPECT_TRUE(BKE_mesh_maps_bake_type_cpu_id(type)) << int(type);
    EXPECT_FALSE(BKE_mesh_maps_bake_type_renders(type)) << int(type);
  }
  for (const int8_t type : {int8_t(MA_MESH_MAP_THICKNESS), int8_t(MA_MESH_MAP_POSITION)}) {
    EXPECT_FALSE(BKE_mesh_maps_bake_type_renders(type));
    EXPECT_FALSE(BKE_mesh_maps_bake_type_cpu_id(type));
  }
}

TEST_F(MeshMapBakeTest, pass_and_normal_space_per_type)
{
  EXPECT_EQ(BKE_mesh_maps_bake_pass_for_type(MA_MESH_MAP_AO), SCE_PASS_EMIT);
  EXPECT_EQ(BKE_mesh_maps_bake_pass_for_type(MA_MESH_MAP_CURVATURE), SCE_PASS_EMIT);
  EXPECT_EQ(BKE_mesh_maps_bake_pass_for_type(MA_MESH_MAP_EDGE), SCE_PASS_EMIT);
  EXPECT_EQ(BKE_mesh_maps_bake_pass_for_type(MA_MESH_MAP_NORMAL_WORLD), SCE_PASS_NORMAL);
  EXPECT_EQ(BKE_mesh_maps_bake_pass_for_type(MA_MESH_MAP_NORMAL_OBJECT), SCE_PASS_NORMAL);
  EXPECT_EQ(BKE_mesh_maps_bake_pass_for_type(MA_MESH_MAP_ID_OBJECT), eScenePassType(0));

  eBakeSpace space = R_BAKE_SPACE_CAMERA;
  EXPECT_TRUE(BKE_mesh_maps_bake_normal_space_for_type(MA_MESH_MAP_NORMAL_WORLD, space));
  EXPECT_EQ(space, R_BAKE_SPACE_WORLD);
  EXPECT_TRUE(BKE_mesh_maps_bake_normal_space_for_type(MA_MESH_MAP_NORMAL_OBJECT, space));
  EXPECT_EQ(space, R_BAKE_SPACE_OBJECT);
  EXPECT_FALSE(BKE_mesh_maps_bake_normal_space_for_type(MA_MESH_MAP_AO, space));
}

TEST_F(MeshMapBakeTest, scalar_types_match_paint_layers)
{
  for (int type = 0; type < MA_MESH_MAP_TYPE_NUM; type++) {
    EXPECT_EQ(BKE_mesh_maps_bake_type_scalar(int8_t(type)),
              paint_layer_mesh_map_is_scalar(int8_t(type)))
        << type;
  }
}

TEST_F(MeshMapBakeTest, ao_distance_uses_setting_when_positive)
{
  Mesh *mesh = add_pair_mesh("Pair");
  MaterialMeshMapSettings settings;
  settings.ao_distance = 2.5f;
  EXPECT_FLOAT_EQ(BKE_mesh_maps_bake_ao_distance(*mesh, settings), 2.5f);
}

TEST_F(MeshMapBakeTest, ao_distance_falls_back_to_half_bbox_diagonal)
{
  Mesh *mesh = add_pair_mesh("Pair");
  MaterialMeshMapSettings settings;
  settings.ao_distance = 0.0f;
  /* The unit quad spans (1, 1, 0), so the half diagonal is sqrt(2) / 2. */
  EXPECT_NEAR(BKE_mesh_maps_bake_ao_distance(*mesh, settings), float(M_SQRT2) * 0.5f, 1e-5f);

  for (MutableSpan<float3> positions = mesh->vert_positions_for_write(); float3 &position :
       positions)
  {
    position = float3(0.0f);
  }
  mesh->tag_positions_changed();
  /* A flat mesh would give 0, which the AO node reads as "global radius"; the fallback keeps it
   * positive. */
  EXPECT_GT(BKE_mesh_maps_bake_ao_distance(*mesh, settings), 0.0f);
}

TEST_F(MeshMapBakeTest, emit_tree_ao_is_local_with_distance_and_samples)
{
  Material *ma = add_material("MapsMat");
  MaterialMeshMapSettings settings;
  settings.samples = 64;
  bNode *emission = BKE_mesh_maps_bake_build_emit_tree(*ma->nodetree, MA_MESH_MAP_AO, settings,
                                                       3.5f);
  ASSERT_NE(emission, nullptr);
  EXPECT_EQ(emission->type_legacy, SH_NODE_EMISSION);

  bNode *ao = find_node(*ma->nodetree, SH_NODE_AMBIENT_OCCLUSION);
  ASSERT_NE(ao, nullptr);
  EXPECT_EQ(ao->custom1, 64);
  EXPECT_TRUE(ao->custom2 & SHD_AO_LOCAL);
  EXPECT_FLOAT_EQ(socket_float(*ao, SOCK_IN, "Distance"), 3.5f);

  bNode *output = find_node(*ma->nodetree, SH_NODE_OUTPUT_MATERIAL);
  ASSERT_NE(output, nullptr);
  EXPECT_TRUE(has_link(*ma->nodetree, *emission, *output));
}

TEST_F(MeshMapBakeTest, emit_tree_curvature_reads_pointiness)
{
  Material *ma = add_material("MapsMat");
  MaterialMeshMapSettings settings;
  bNode *emission = BKE_mesh_maps_bake_build_emit_tree(*ma->nodetree, MA_MESH_MAP_CURVATURE,
                                                       settings, 0.0f);
  ASSERT_NE(emission, nullptr);
  bNode *geometry = find_node(*ma->nodetree, SH_NODE_NEW_GEOMETRY);
  ASSERT_NE(geometry, nullptr);
  EXPECT_TRUE(has_link(*ma->nodetree, *geometry, *emission));
}

TEST_F(MeshMapBakeTest, emit_tree_edge_uses_bevel_and_map_range)
{
  Material *ma = add_material("MapsMat");
  MaterialMeshMapSettings settings;
  settings.samples = 8;
  settings.edge_radius = 0.02f;
  bNode *emission = BKE_mesh_maps_bake_build_emit_tree(*ma->nodetree, MA_MESH_MAP_EDGE, settings,
                                                       0.0f);
  ASSERT_NE(emission, nullptr);

  bNode *bevel = find_node(*ma->nodetree, SH_NODE_BEVEL);
  ASSERT_NE(bevel, nullptr);
  EXPECT_EQ(bevel->custom1, 8);
  EXPECT_FLOAT_EQ(socket_float(*bevel, SOCK_IN, "Radius"), 0.02f);
  EXPECT_NE(find_node(*ma->nodetree, SH_NODE_VECTOR_MATH), nullptr);
  EXPECT_NE(find_node(*ma->nodetree, SH_NODE_MATH), nullptr);

  bNode *range = find_node(*ma->nodetree, SH_NODE_MAP_RANGE);
  ASSERT_NE(range, nullptr);
  const NodeMapRange *storage = static_cast<const NodeMapRange *>(range->storage);
  ASSERT_NE(storage, nullptr);
  EXPECT_EQ(storage->clamp, 1);
  EXPECT_FLOAT_EQ(socket_float(*range, SOCK_IN, "From Min"), 0.0f);
  EXPECT_FLOAT_EQ(socket_float(*range, SOCK_IN, "From Max"), MESH_MAP_BAKE_EDGE_RANGE);
  EXPECT_FLOAT_EQ(socket_float(*range, SOCK_IN, "To Min"), 0.0f);
  EXPECT_FLOAT_EQ(socket_float(*range, SOCK_IN, "To Max"), 1.0f);

  bNodeSocket *color = socket_find(*emission, SOCK_IN, "Color");
  ASSERT_NE(color, nullptr);
  EXPECT_TRUE(has_link(*ma->nodetree, *range, *emission));
}

TEST_F(MeshMapBakeTest, emit_tree_is_null_for_non_emission_types)
{
  Material *ma = add_material("MapsMat");
  MaterialMeshMapSettings settings;
  EXPECT_EQ(BKE_mesh_maps_bake_build_emit_tree(*ma->nodetree, MA_MESH_MAP_NORMAL_WORLD, settings,
                                               0.0f),
            nullptr);
  EXPECT_EQ(BKE_mesh_maps_bake_build_emit_tree(*ma->nodetree, MA_MESH_MAP_ID_MATERIAL, settings,
                                               0.0f),
            nullptr);
  EXPECT_EQ(count_nodes(*ma->nodetree, SH_NODE_EMISSION), 0);
  /* The negative guard is only meaningful next to a type that does build an emission surface. */
  EXPECT_NE(BKE_mesh_maps_bake_build_emit_tree(*ma->nodetree, MA_MESH_MAP_AO, settings, 1.0f),
            nullptr);
}

TEST_F(MeshMapBakeTest, id_object_color_is_deterministic_and_in_range)
{
  Object *ob = add_empty("Ob");
  float first[3] = {0.0f, 0.0f, 0.0f};
  float second[3] = {0.0f, 0.0f, 0.0f};
  BKE_mesh_maps_bake_id_object_color(*ob, first);
  BKE_mesh_maps_bake_id_object_color(*ob, second);
  EXPECT_EQ(memcmp(first, second, sizeof(first)), 0);
  for (const float channel : first) {
    EXPECT_GE(channel, 0.0f);
    EXPECT_LE(channel, 1.0f);
  }
}

TEST_F(MeshMapBakeTest, id_material_color_differs_per_index)
{
  float zero[3];
  float one[3];
  float five[3];
  BKE_mesh_maps_bake_id_material_color(0, zero);
  BKE_mesh_maps_bake_id_material_color(1, one);
  BKE_mesh_maps_bake_id_material_color(5, five);
  EXPECT_NE(memcmp(zero, one, sizeof(zero)), 0);
  EXPECT_NE(memcmp(zero, five, sizeof(zero)), 0);
  EXPECT_NE(memcmp(one, five, sizeof(zero)), 0);
}

TEST_F(MeshMapBakeTest, restrict_to_material_clears_foreign_faces)
{
  Mesh *mesh = add_pair_mesh("Pair");
  Material *first = add_material("First");
  Material *second = add_material("Second");
  Object *ob = add_object("Ob", mesh, first, second);

  BakePixel pixels[3] = {};
  pixels[0].primitive_id = 0;
  pixels[1].primitive_id = 1;
  pixels[2].primitive_id = -1;

  BKE_mesh_maps_bake_restrict_to_material(pixels, 3, *mesh, *ob, *first);
  EXPECT_EQ(pixels[0].primitive_id, 0);
  EXPECT_EQ(pixels[1].primitive_id, -1);
  EXPECT_EQ(pixels[2].primitive_id, -1);

  pixels[0].primitive_id = 0;
  pixels[1].primitive_id = 1;
  BKE_mesh_maps_bake_restrict_to_material(pixels, 3, *mesh, *ob, *second);
  EXPECT_EQ(pixels[0].primitive_id, -1);
  EXPECT_EQ(pixels[1].primitive_id, 1);
}

TEST_F(MeshMapBakeTest, fill_id_material_pixels_per_face)
{
  Mesh *mesh = add_pair_mesh("Pair");
  Material *first = add_material("First");
  Material *second = add_material("Second");
  Object *ob = add_object("Ob", mesh, first, second);

  BakePixel pixels[2] = {};
  pixels[0].primitive_id = 0;
  pixels[1].primitive_id = 1;
  float result[8] = {};
  BKE_mesh_maps_bake_fill_id_pixels(pixels, 2, *mesh, *ob, MA_MESH_MAP_ID_MATERIAL, 4, result);

  float expected[3];
  BKE_mesh_maps_bake_id_material_color(0, expected);
  EXPECT_FLOAT_EQ(result[0], expected[0]);
  EXPECT_FLOAT_EQ(result[1], expected[1]);
  EXPECT_FLOAT_EQ(result[2], expected[2]);
  EXPECT_FLOAT_EQ(result[3], 1.0f);
  BKE_mesh_maps_bake_id_material_color(1, expected);
  EXPECT_FLOAT_EQ(result[4], expected[0]);
  EXPECT_FLOAT_EQ(result[5], expected[1]);
  EXPECT_FLOAT_EQ(result[6], expected[2]);
  EXPECT_FLOAT_EQ(result[7], 1.0f);
}

TEST_F(MeshMapBakeTest, fill_id_object_pixels_constant_and_skips_uncovered)
{
  Mesh *mesh = add_pair_mesh("Pair");
  Material *first = add_material("First");
  Material *second = add_material("Second");
  Object *ob = add_object("Ob", mesh, first, second);

  BakePixel pixels[2] = {};
  pixels[0].primitive_id = 0;
  pixels[1].primitive_id = -1;
  float result[8] = {};
  BKE_mesh_maps_bake_fill_id_pixels(pixels, 2, *mesh, *ob, MA_MESH_MAP_ID_OBJECT, 4, result);

  float expected[3];
  BKE_mesh_maps_bake_id_object_color(*ob, expected);
  EXPECT_FLOAT_EQ(result[0], expected[0]);
  EXPECT_FLOAT_EQ(result[1], expected[1]);
  EXPECT_FLOAT_EQ(result[2], expected[2]);
  EXPECT_FLOAT_EQ(result[3], 1.0f);
  for (int i = 4; i < 8; i++) {
    EXPECT_FLOAT_EQ(result[i], 0.0f);
  }
}

TEST_F(MeshMapBakeTest, write_covered_skips_uncovered_and_spreads_scalar)
{
  BakePixel pixels[2] = {};
  pixels[0].primitive_id = 0;
  pixels[1].primitive_id = -1;
  const float result[8] = {0.1f, 0.2f, 0.3f, 1.0f, 9.0f, 9.0f, 9.0f, 9.0f};

  float atlas[8] = {};
  BKE_mesh_maps_bake_write_covered(MA_MESH_MAP_AO, pixels, 2, 4, result, MutableSpan<float>(atlas, 8));
  EXPECT_FLOAT_EQ(atlas[0], 0.1f);
  EXPECT_FLOAT_EQ(atlas[1], 0.1f);
  EXPECT_FLOAT_EQ(atlas[2], 0.1f);
  EXPECT_FLOAT_EQ(atlas[3], 1.0f);
  for (int i = 4; i < 8; i++) {
    EXPECT_FLOAT_EQ(atlas[i], 0.0f);
  }

  float rgb_atlas[8] = {};
  BKE_mesh_maps_bake_write_covered(
      MA_MESH_MAP_NORMAL_WORLD, pixels, 2, 4, result, MutableSpan<float>(rgb_atlas, 8));
  EXPECT_FLOAT_EQ(rgb_atlas[0], 0.1f);
  EXPECT_FLOAT_EQ(rgb_atlas[1], 0.2f);
  EXPECT_FLOAT_EQ(rgb_atlas[2], 0.3f);
  for (int i = 4; i < 8; i++) {
    EXPECT_FLOAT_EQ(rgb_atlas[i], 0.0f);
  }

  /* A too-short atlas is left untouched. */
  float short_atlas[4] = {};
  BKE_mesh_maps_bake_write_covered(
      MA_MESH_MAP_AO, pixels, 2, 4, result, MutableSpan<float>(short_atlas, 4));
  for (const float value : short_atlas) {
    EXPECT_FLOAT_EQ(value, 0.0f);
  }
}

TEST_F(MeshMapBakeTest, atlas_ensure_creates_float_noncolor_and_reuses)
{
  Material *ma = add_material("MapsMat");
  Image *image = BKE_mesh_maps_bake_atlas_ensure(*bmain, *ma, MA_MESH_MAP_AO, 16);
  ASSERT_NE(image, nullptr);
  EXPECT_EQ(BKE_mesh_maps_slot_find(*ma, MA_MESH_MAP_AO)->image, image);

  int width = 0;
  int height = 0;
  BKE_image_get_size(image, nullptr, &width, &height);
  EXPECT_EQ(width, 16);
  EXPECT_EQ(height, 16);
  EXPECT_TRUE(IMB_colormanagement_space_name_is_data(image->colorspace_settings.name));

  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
  ASSERT_NE(ibuf, nullptr);
  EXPECT_NE(ibuf->float_data(), nullptr);
  BKE_image_release_ibuf(image, ibuf, lock);

  /* Same resolution reuses the very same image. */
  EXPECT_EQ(BKE_mesh_maps_bake_atlas_ensure(*bmain, *ma, MA_MESH_MAP_AO, 16), image);

  /* A new resolution replaces the atlas. */
  Image *larger = BKE_mesh_maps_bake_atlas_ensure(*bmain, *ma, MA_MESH_MAP_AO, 32);
  ASSERT_NE(larger, nullptr);
  EXPECT_NE(larger, image);
  EXPECT_EQ(BKE_mesh_maps_slot_find(*ma, MA_MESH_MAP_AO)->image, larger);
  BKE_image_get_size(larger, nullptr, &width, &height);
  EXPECT_EQ(width, 32);

  /* An invalid type is refused. */
  EXPECT_EQ(BKE_mesh_maps_bake_atlas_ensure(*bmain, *ma, int8_t(-1), 16), nullptr);
}

TEST_F(MeshMapBakeTest, commit_is_current_compares_both_words)
{
  const uint32_t start[2] = {0x11111111u, 0x22222222u};
  const uint32_t same[2] = {0x11111111u, 0x22222222u};
  const uint32_t low[2] = {0x33333333u, 0x22222222u};
  const uint32_t high[2] = {0x11111111u, 0x44444444u};
  EXPECT_TRUE(BKE_mesh_maps_bake_commit_is_current(start, same));
  EXPECT_FALSE(BKE_mesh_maps_bake_commit_is_current(start, low));
  EXPECT_FALSE(BKE_mesh_maps_bake_commit_is_current(start, high));
}

TEST_F(MeshMapBakeTest, state_transitions_none_baking_valid)
{
  Material *ma = add_material("MapsMat");
  Object *ob = add_empty("Ob");
  ObjectMeshMapState *state = BKE_mesh_maps_object_state_ensure(*ob, *ma, MA_MESH_MAP_AO);
  ASSERT_NE(state, nullptr);
  EXPECT_EQ(state->status, OB_MESH_MAP_STATUS_NONE);

  const int8_t previous = BKE_mesh_maps_bake_state_begin(*state);
  EXPECT_EQ(previous, OB_MESH_MAP_STATUS_NONE);
  EXPECT_EQ(state->status, OB_MESH_MAP_STATUS_BAKING);

  const uint32_t hash[2] = {0xAAAAAAAAu, 0xBBBBBBBBu};
  EXPECT_EQ(BKE_mesh_maps_bake_state_commit(*state, hash, hash, 123), OB_MESH_MAP_STATUS_VALID);
  EXPECT_EQ(state->status, OB_MESH_MAP_STATUS_VALID);
  EXPECT_EQ(state->hash[0], 0xAAAAAAAAu);
  EXPECT_EQ(state->hash[1], 0xBBBBBBBBu);
  EXPECT_EQ(state->baked_time, 123);
}

TEST_F(MeshMapBakeTest, state_transition_cancel_restores_previous)
{
  Material *ma = add_material("MapsMat");
  Object *ob = add_empty("Ob");
  ObjectMeshMapState *state = BKE_mesh_maps_object_state_ensure(*ob, *ma, MA_MESH_MAP_AO);
  ASSERT_NE(state, nullptr);

  state->status = OB_MESH_MAP_STATUS_VALID;
  const int8_t previous = BKE_mesh_maps_bake_state_begin(*state);
  EXPECT_EQ(previous, OB_MESH_MAP_STATUS_VALID);
  EXPECT_EQ(state->status, OB_MESH_MAP_STATUS_BAKING);
  BKE_mesh_maps_bake_state_cancel(*state, previous);
  EXPECT_EQ(state->status, OB_MESH_MAP_STATUS_VALID);

  state->status = OB_MESH_MAP_STATUS_NONE;
  const int8_t from_none = BKE_mesh_maps_bake_state_begin(*state);
  EXPECT_EQ(state->status, OB_MESH_MAP_STATUS_BAKING);
  BKE_mesh_maps_bake_state_cancel(*state, from_none);
  EXPECT_EQ(state->status, OB_MESH_MAP_STATUS_NONE);
}

TEST_F(MeshMapBakeTest, state_transition_error)
{
  Material *ma = add_material("MapsMat");
  Object *ob = add_empty("Ob");
  ObjectMeshMapState *state = BKE_mesh_maps_object_state_ensure(*ob, *ma, MA_MESH_MAP_AO);
  ASSERT_NE(state, nullptr);

  BKE_mesh_maps_bake_state_begin(*state);
  BKE_mesh_maps_bake_state_fail(*state);
  EXPECT_EQ(state->status, OB_MESH_MAP_STATUS_ERROR);
}

TEST_F(MeshMapBakeTest, state_transition_hash_change_is_stale)
{
  Material *ma = add_material("MapsMat");
  Object *ob = add_empty("Ob");
  ObjectMeshMapState *state = BKE_mesh_maps_object_state_ensure(*ob, *ma, MA_MESH_MAP_AO);
  ASSERT_NE(state, nullptr);

  const uint32_t start[2] = {0x11111111u, 0x22222222u};
  const uint32_t moved[2] = {0x11111111u, 0x33333333u};
  state->status = OB_MESH_MAP_STATUS_VALID;
  BKE_mesh_maps_bake_state_begin(*state);
  EXPECT_EQ(BKE_mesh_maps_bake_state_commit(*state, start, moved, 456), OB_MESH_MAP_STATUS_STALE);
  EXPECT_EQ(state->status, OB_MESH_MAP_STATUS_STALE);
  /* A stale commit writes nothing: the old hash and time stay. */
  EXPECT_NE(state->hash[0], start[0]);
}

TEST_F(MeshMapBakeTest, uv_resolve_prefers_named_layer)
{
  Mesh *mesh = add_quad_two_uv("Quad");
  Material *ma = add_material("MapsMat");
  STRNCPY(ma->paint_layers_uv_map, "Chosen");
  Object *ob = add_simple_object("Ob", mesh, ma);
  ObjectMeshMapState *state = BKE_mesh_maps_object_state_ensure(*ob, *ma, MA_MESH_MAP_AO);
  ASSERT_NE(state, nullptr);
  const int8_t previous = BKE_mesh_maps_bake_state_begin(*state);
  EXPECT_EQ(previous, OB_MESH_MAP_STATUS_NONE);

  const char *name = BKE_mesh_maps_bake_uv_resolve_or_fail(*state, *ob, *ma);
  ASSERT_NE(name, nullptr);
  EXPECT_STREQ(name, "Chosen");
  /* A successful resolve leaves the in-flight status alone. */
  EXPECT_EQ(state->status, OB_MESH_MAP_STATUS_BAKING);
}

TEST_F(MeshMapBakeTest, uv_resolve_fails_when_named_layer_missing)
{
  Mesh *mesh = add_quad_two_uv("Quad");
  Material *ma = add_material("MapsMat");
  STRNCPY(ma->paint_layers_uv_map, "Missing");
  Object *ob = add_simple_object("Ob", mesh, ma);
  ObjectMeshMapState *state = BKE_mesh_maps_object_state_ensure(*ob, *ma, MA_MESH_MAP_AO);
  ASSERT_NE(state, nullptr);
  BKE_mesh_maps_bake_state_begin(*state);

  EXPECT_EQ(BKE_mesh_maps_bake_uv_resolve_or_fail(*state, *ob, *ma), nullptr);
  EXPECT_EQ(state->status, OB_MESH_MAP_STATUS_ERROR);
}

TEST_F(MeshMapBakeTest, pixels_populate_uses_named_uv_not_active)
{
  const int size = 8;
  Mesh *mesh = add_quad_two_uv("Quad");
  Material *ma = add_material("MapsMat");
  STRNCPY(ma->paint_layers_uv_map, "Chosen");
  Object *ob = add_simple_object("Ob", mesh, ma);
  ObjectMeshMapState *state = BKE_mesh_maps_object_state_ensure(*ob, *ma, MA_MESH_MAP_AO);
  ASSERT_NE(state, nullptr);
  BKE_mesh_maps_bake_state_begin(*state);
  const char *name = BKE_mesh_maps_bake_uv_resolve_or_fail(*state, *ob, *ma);
  ASSERT_NE(name, nullptr);
  ASSERT_STREQ(name, "Chosen");

  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *atlas = BKE_image_add_generated(
      bmain, size, size, "Atlas", 32, false, IMA_GENTYPE_BLANK, black, false, false, false);
  ASSERT_NE(atlas, nullptr);
  Image *material_to_image[1] = {atlas};
  BakeImage image = {};
  image.image = atlas;
  image.width = size;
  image.height = size;
  image.offset = 0;
  BakeTargets targets = {};
  targets.images = &image;
  targets.images_num = 1;
  targets.material_to_image = material_to_image;
  targets.materials_num = 1;
  targets.pixels_num = size * size;
  targets.channels_num = 4;

  Array<BakePixel> named(size * size);
  RE_bake_pixels_populate(mesh, named.data(), size * size, &targets, StringRef(name));
  int named_covered = 0;
  for (const BakePixel &pixel : named) {
    if (pixel.primitive_id != -1) {
      named_covered++;
    }
  }

  Array<BakePixel> active(size * size);
  RE_bake_pixels_populate(mesh, active.data(), size * size, &targets, StringRef("UVMap"));
  int active_covered = 0;
  for (const BakePixel &pixel : active) {
    if (pixel.primitive_id != -1) {
      active_covered++;
    }
  }

  /* The named layer covers the 0..0.5 corner, so it fills strictly fewer texels than the full
   * active `UVMap`. */
  EXPECT_GT(named_covered, 0);
  EXPECT_LT(named_covered, active_covered);
}

/**
 * Guard: a modifier with different viewport and render levels evaluates to different geometry, so
 * the render depsgraph hash the bake used to take disagrees with the viewport one `refresh()` uses.
 * This is why the commit must hash from the viewport depsgraph.
 */
TEST_F(MeshMapBakeTest, viewport_and_render_hashes_differ_for_subsurf)
{
  Mesh *mesh = add_pair_mesh("Pair");
  Material *ma = add_material("MapsMat");
  Object *ob = add_simple_object("Ob", mesh, ma);
  /* Default Subdivision: levels=1 in the viewport, renderLevels=2. */
  add_subsurf(*ob, 1, 2);
  Scene *scene = add_scene_with_object("Scene", ob);
  ViewLayer *view_layer = static_cast<ViewLayer *>(scene->view_layers.first);

  Depsgraph *viewport_graph = nullptr;
  Object *ob_viewport = eval_object(
      *bmain, *scene, *view_layer, *ob, DAG_EVAL_VIEWPORT, viewport_graph);
  Depsgraph *render_graph = nullptr;
  Object *ob_render = eval_object(*bmain, *scene, *view_layer, *ob, DAG_EVAL_RENDER, render_graph);
  ASSERT_NE(ob_viewport, nullptr);
  ASSERT_NE(ob_render, nullptr);
  ASSERT_NE(ob_viewport, ob_render);

  uint32_t viewport_hash[2];
  uint32_t render_hash[2];
  BKE_mesh_maps_object_hash(*ob_viewport, *ma, MA_MESH_MAP_AO, viewport_hash);
  BKE_mesh_maps_object_hash(*ob_render, *ma, MA_MESH_MAP_AO, render_hash);
  EXPECT_NE(memcmp(viewport_hash, render_hash, sizeof(viewport_hash)), 0);

  /* The helper must return the viewport hash, not the render one. */
  uint32_t helper_hash[2];
  ASSERT_TRUE(
      BKE_mesh_maps_bake_hash_from_viewport(*bmain, *scene, *view_layer, *ob, *ma, MA_MESH_MAP_AO,
                                            helper_hash));
  EXPECT_EQ(memcmp(helper_hash, viewport_hash, sizeof(helper_hash)), 0);

  DEG_graph_free(viewport_graph);
  DEG_graph_free(render_graph);
}

/**
 * A successful bake must stay VALID under the very evaluation `refresh()` uses; hashing a fresh
 * render depsgraph would leave it STALE immediately. Fixes defect 1.
 */
TEST_F(MeshMapBakeTest, commit_hash_from_viewport_keeps_state_valid)
{
  Mesh *mesh = add_pair_mesh("Pair");
  Material *ma = add_material("MapsMat");
  Object *ob = add_simple_object("Ob", mesh, ma);
  add_subsurf(*ob, 1, 2);
  Scene *scene = add_scene_with_object("Scene", ob);
  ViewLayer *view_layer = static_cast<ViewLayer *>(scene->view_layers.first);
  ObjectMeshMapState *state = BKE_mesh_maps_object_state_ensure(*ob, *ma, MA_MESH_MAP_AO);
  ASSERT_NE(state, nullptr);

  uint32_t start_hash[2];
  ASSERT_TRUE(
      BKE_mesh_maps_bake_hash_from_viewport(*bmain, *scene, *view_layer, *ob, *ma, MA_MESH_MAP_AO,
                                            start_hash));
  BKE_mesh_maps_object_state_mark_baked(*state, start_hash, 1);
  EXPECT_EQ(state->status, OB_MESH_MAP_STATUS_VALID);

  /* `refresh()` sees the same viewport geometry, so the bake stays current. */
  Depsgraph *viewport_graph = nullptr;
  Object *ob_viewport = eval_object(
      *bmain, *scene, *view_layer, *ob, DAG_EVAL_VIEWPORT, viewport_graph);
  ASSERT_NE(ob_viewport, nullptr);
  EXPECT_TRUE(BKE_mesh_maps_object_state_is_current(*state, *ob_viewport, *ma));
  EXPECT_EQ(BKE_mesh_maps_object_state_refresh(*state, *ob_viewport, *ma),
            OB_MESH_MAP_STATUS_VALID);

  /* The render evaluation would have disagreed. */
  Depsgraph *render_graph = nullptr;
  Object *ob_render = eval_object(*bmain, *scene, *view_layer, *ob, DAG_EVAL_RENDER, render_graph);
  ASSERT_NE(ob_render, nullptr);
  EXPECT_FALSE(BKE_mesh_maps_object_state_is_current(*state, *ob_render, *ma));

  DEG_graph_free(viewport_graph);
  DEG_graph_free(render_graph);
}

/** Fixes defect 2: a byte atlas must not be reported as a valid bake. */
TEST_F(MeshMapBakeTest, atlas_is_float_detects_byte_image)
{
  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *float_image = BKE_image_add_generated(
      bmain, 8, 8, "Float", 32, true, IMA_GENTYPE_BLANK, black, false, true, false);
  ASSERT_NE(float_image, nullptr);
  EXPECT_TRUE(BKE_mesh_maps_bake_atlas_is_float(*float_image));

  Image *byte_image = BKE_image_add_generated(
      bmain, 8, 8, "Byte", 24, false, IMA_GENTYPE_BLANK, black, false, false, false);
  ASSERT_NE(byte_image, nullptr);
  EXPECT_FALSE(BKE_mesh_maps_bake_atlas_is_float(*byte_image));
}

}  // namespace blender::bke::tests
