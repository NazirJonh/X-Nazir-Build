/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Tests for #BKE_mesh_maps_bake.hh: the per-type bake plan, the CPU ID maps, the covered-pixel
 * write, the guarded atlas merge, the bake state transitions and the main/worker handshake. None of
 * this renders, so the suite needs no engine.
 */

#include "testing/testing.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

#include "BLI_array.hh"
#include "BLI_listbase.h"
#include "BLI_listbase_iterator.hh"
#include "BLI_math_base.h"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_string.h"
#include "BLI_ustring.hh"

#include "BKE_attribute.hh"
#include "BKE_collection.hh"
#include "BKE_global.hh"
#include "BKE_gtest_base.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_mesh.h"
#include "BKE_mesh.hh"
#include "BKE_mesh_maps.hh"
#include "BKE_mesh_maps_bake.hh"
#include "BKE_mesh_maps_bake_handshake.hh"
#include "BKE_modifier.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_scene.hh"

#include "RE_bake.h"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"
#include "DEG_depsgraph_query.hh"

#include "../../paint_layers/paint_layers_intern.hh"

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_modifier_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_enums.h"
#include "DNA_scene_types.h"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

namespace blender::bke::tests {

/** The caller-side texel pass #BKE_mesh_maps_bake_foreign_coverage asks for (the editor's twin). */
static void populate_geometry_pixels(Mesh &mesh,
                                     MutableSpan<BakePixel> pixels,
                                     const int resolution,
                                     const StringRef uv_name,
                                     const int tile_number)
{
  Image *material_to_image[1] = {nullptr};
  BakeImage bake_image = {};
  bake_image.tile_number = tile_number;
  bake_image.width = resolution;
  bake_image.height = resolution;
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

/** A zeroed float atlas tile of \a res x \a res texels; release with #IMB_freeImBuf. */
static ImBuf *make_tile_buffer(const int res)
{
  ImBuf *ibuf = IMB_allocImBuf(uint(res), uint(res), ImBufFlags::FloatData);
  if (ibuf != nullptr) {
    if (float *data = ibuf->float_data_for_write()) {
      MutableSpan<float> pixels(data, int64_t(res) * res * 4);
      pixels.fill(0.0f);
    }
  }
  return ibuf;
}

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

  /** Two triangles: face 0 uses slot 0, face 1 uses slot 1. No UV layer at all. */
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

  /** Two quads a unit apart: the top face at z = 1, the bottom at z = 0, both wound CCW from
   * +Z, so their world normals point out of the slab. */
  Mesh *add_slab_mesh(const char *name)
  {
    Mesh *mesh = BKE_mesh_add(bmain, name);
    mesh->verts_num = 8;
    mesh->edges_num = 8;
    mesh->faces_num = 2;
    mesh->corners_num = 8;
    BKE_mesh_face_offsets_ensure_alloc(mesh);
    bke::mesh_ensure_required_data_layers(*mesh);

    const float3 co[8] = {{0.0f, 0.0f, 1.0f},
                          {1.0f, 0.0f, 1.0f},
                          {1.0f, 1.0f, 1.0f},
                          {0.0f, 1.0f, 1.0f},
                          {0.0f, 0.0f, 0.0f},
                          {0.0f, 1.0f, 0.0f},
                          {1.0f, 1.0f, 0.0f},
                          {1.0f, 0.0f, 0.0f}};
    mesh->vert_positions_for_write().copy_from(Span<float3>(co, 8));
    MutableSpan<int> offsets = mesh->face_offsets_for_write();
    offsets[0] = 0;
    offsets[1] = 4;
    offsets[2] = 8;
    MutableSpan<int> corner_verts = mesh->corner_verts_for_write();
    MutableSpan<int> corner_edges = mesh->corner_edges_for_write();
    const int cverts[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    const int cedges[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    for (int i = 0; i < 8; i++) {
      corner_verts[i] = cverts[i];
      corner_edges[i] = cedges[i];
    }
    MutableSpan<int2> edges = mesh->edges_for_write();
    edges[0] = {0, 1};
    edges[1] = {1, 2};
    edges[2] = {2, 3};
    edges[3] = {3, 0};
    edges[4] = {4, 5};
    edges[5] = {5, 6};
    edges[6] = {6, 7};
    edges[7] = {7, 4};
    mesh->tag_topology_changed();
    mesh->tag_positions_changed();
    return mesh;
  }

  /** Mirror across the `x = 0.5` plane: a negative determinant that still keeps `[0, 1]`
   * geometry over `[0, 1]`, so a mirrored source and an unmirrored low-poly overlap. */
  static float4x4 mirror_x_about_half()
  {
    float4x4 matrix = float4x4::identity();
    /* Column-major `float4x4`: `[0][0]` is the X scale, `[3][0]` the X translation. */
    matrix[0][0] = -1.0f;
    matrix[3][0] = 1.0f;
    return matrix;
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
      MA_MESH_MAP_AO, MA_MESH_MAP_CURVATURE, MA_MESH_MAP_EDGE, MA_MESH_MAP_POSITION,
      MA_MESH_MAP_NORMAL_WORLD, MA_MESH_MAP_NORMAL_OBJECT, MA_MESH_MAP_NORMAL_TANGENT};
  EXPECT_FALSE(BKE_mesh_maps_bake_type_renders(MA_MESH_MAP_BENT_NORMAL));
  EXPECT_FALSE(BKE_mesh_maps_bake_type_cpu_simple(MA_MESH_MAP_BENT_NORMAL));
  EXPECT_TRUE(BKE_mesh_maps_bake_type_cpu_rays_vector(MA_MESH_MAP_BENT_NORMAL));
  for (const int8_t type : rendered) {
    EXPECT_TRUE(BKE_mesh_maps_bake_type_renders(type)) << int(type);
    EXPECT_FALSE(BKE_mesh_maps_bake_type_cpu_simple(type)) << int(type);
  }
  const int8_t cpu[] = {MA_MESH_MAP_ID_OBJECT, MA_MESH_MAP_ID_MATERIAL};
  for (const int8_t type : cpu) {
    EXPECT_TRUE(BKE_mesh_maps_bake_type_cpu_simple(type)) << int(type);
    EXPECT_FALSE(BKE_mesh_maps_bake_type_renders(type)) << int(type);
  }
  /* Thickness is CPU-only: neither a render pass nor an ID fill. */
  EXPECT_FALSE(BKE_mesh_maps_bake_type_renders(MA_MESH_MAP_THICKNESS));
  EXPECT_FALSE(BKE_mesh_maps_bake_type_cpu_simple(MA_MESH_MAP_THICKNESS));
  EXPECT_TRUE(BKE_mesh_maps_bake_type_cpu_rays_scalar(MA_MESH_MAP_THICKNESS));
}

/** The two families are strictly disjoint: nothing is both rendered and CPU-filled. */
TEST_F(MeshMapBakeTest, render_types_are_not_cpu_simple)
{
  for (int type = 0; type < MA_MESH_MAP_TYPE_NUM; type++) {
    const int8_t t = int8_t(type);
    const bool renders = BKE_mesh_maps_bake_type_renders(t);
    const bool cpu = BKE_mesh_maps_bake_type_cpu_simple(t);
    EXPECT_FALSE(renders && cpu) << type;
  }
}

TEST_F(MeshMapBakeTest, pass_and_normal_space_per_type)
{
  EXPECT_EQ(BKE_mesh_maps_bake_pass_for_type(MA_MESH_MAP_AO), SCE_PASS_EMIT);
  EXPECT_EQ(BKE_mesh_maps_bake_pass_for_type(MA_MESH_MAP_CURVATURE), SCE_PASS_EMIT);
  EXPECT_EQ(BKE_mesh_maps_bake_pass_for_type(MA_MESH_MAP_EDGE), SCE_PASS_EMIT);
  EXPECT_EQ(BKE_mesh_maps_bake_pass_for_type(MA_MESH_MAP_POSITION), SCE_PASS_EMIT);
  EXPECT_EQ(BKE_mesh_maps_bake_pass_for_type(MA_MESH_MAP_NORMAL_WORLD), SCE_PASS_NORMAL);
  EXPECT_EQ(BKE_mesh_maps_bake_pass_for_type(MA_MESH_MAP_NORMAL_OBJECT), SCE_PASS_NORMAL);
  EXPECT_EQ(BKE_mesh_maps_bake_pass_for_type(MA_MESH_MAP_NORMAL_TANGENT), SCE_PASS_NORMAL);
  EXPECT_EQ(BKE_mesh_maps_bake_pass_for_type(MA_MESH_MAP_ID_OBJECT), eScenePassType(0));

  eBakeSpace space = R_BAKE_SPACE_CAMERA;
  EXPECT_TRUE(BKE_mesh_maps_bake_normal_space_for_type(MA_MESH_MAP_NORMAL_WORLD, space));
  EXPECT_EQ(space, R_BAKE_SPACE_WORLD);
  EXPECT_TRUE(BKE_mesh_maps_bake_normal_space_for_type(MA_MESH_MAP_NORMAL_OBJECT, space));
  EXPECT_EQ(space, R_BAKE_SPACE_OBJECT);
  EXPECT_TRUE(BKE_mesh_maps_bake_normal_space_for_type(MA_MESH_MAP_NORMAL_TANGENT, space));
  EXPECT_EQ(space, R_BAKE_SPACE_TANGENT);
  EXPECT_FALSE(BKE_mesh_maps_bake_normal_space_for_type(MA_MESH_MAP_AO, space));
}

TEST_F(MeshMapBakeTest, normalize_position_raw_box_and_sphere)
{
  const float bb_min[3] = {0.0f, 0.0f, 0.0f};
  const float bb_max[3] = {2.0f, 2.0f, 10.0f};
  const float raw[8] = {2.0f, 2.0f, 10.0f, 1.0f, -3.0f, 1.0f, 0.0f, 0.5f};
  float texels[8];

  std::memcpy(texels, raw, sizeof(raw));
  BKE_mesh_maps_bake_normalize_position(texels, 2, bb_min, bb_max, MA_MESH_MAP_POSITION_RAW);
  for (int i = 0; i < 8; i++) {
    EXPECT_FLOAT_EQ(texels[i], raw[i]) << i;
  }

  std::memcpy(texels, raw, sizeof(raw));
  BKE_mesh_maps_bake_normalize_position(texels, 2, bb_min, bb_max, MA_MESH_MAP_POSITION_BOX);
  EXPECT_FLOAT_EQ(texels[0], 1.0f);
  EXPECT_FLOAT_EQ(texels[1], 1.0f);
  EXPECT_FLOAT_EQ(texels[2], 1.0f);
  /* Alpha is not part of the normalization. */
  EXPECT_FLOAT_EQ(texels[3], 1.0f);
  EXPECT_FLOAT_EQ(texels[4], -1.5f);
  EXPECT_FLOAT_EQ(texels[5], 0.5f);
  EXPECT_FLOAT_EQ(texels[6], 0.0f);
  EXPECT_FLOAT_EQ(texels[7], 0.5f);

  std::memcpy(texels, raw, sizeof(raw));
  BKE_mesh_maps_bake_normalize_position(texels, 2, bb_min, bb_max, MA_MESH_MAP_POSITION_SPHERE);
  /* The longest axis sets one diameter: size 10, centre (1, 1, 5), origin (-4, -4, 0). */
  EXPECT_FLOAT_EQ(texels[0], 0.6f);
  EXPECT_FLOAT_EQ(texels[1], 0.6f);
  EXPECT_FLOAT_EQ(texels[2], 1.0f);
  EXPECT_FLOAT_EQ(texels[3], 1.0f);
  EXPECT_FLOAT_EQ(texels[4], 0.1f);
  EXPECT_FLOAT_EQ(texels[5], 0.5f);
  EXPECT_FLOAT_EQ(texels[6], 0.0f);
  EXPECT_FLOAT_EQ(texels[7], 0.5f);
}

TEST_F(MeshMapBakeTest, type_table_describes_every_type)
{
  for (int type = 0; type < MA_MESH_MAP_TYPE_NUM; type++) {
    const int8_t t = int8_t(type);
    const MeshMapTypeInfo *info = BKE_mesh_maps_type_info_get(t);
    /* A type added to the enum without a descriptor row fails here, not silently in a bake. */
    ASSERT_NE(info, nullptr) << type;
    EXPECT_EQ(info->id, t) << type;
    ASSERT_NE(info->idname, nullptr) << type;
    EXPECT_NE(info->idname[0], '\0') << type;
    ASSERT_NE(info->ui_name, nullptr) << type;
    EXPECT_NE(info->ui_name[0], '\0') << type;
    ASSERT_NE(info->ui_description, nullptr) << type;
    EXPECT_NE(info->ui_description[0], '\0') << type;
  }
  /* Outside the described range there is no descriptor, and the callers must refuse the type. */
  EXPECT_EQ(BKE_mesh_maps_type_info_get(int8_t(MA_MESH_MAP_TYPE_NUM)), nullptr);
  EXPECT_EQ(BKE_mesh_maps_type_info_get(int8_t(-1)), nullptr);
}

/** The public predicates are thin reads of the descriptor table; this pins them to it bit for
 * bit, so a table edit that disagrees with the bake's behavior fails here. */
TEST_F(MeshMapBakeTest, type_predicates_and_mask_follow_the_table)
{
  for (int type = 0; type < MA_MESH_MAP_TYPE_NUM; type++) {
    const int8_t t = int8_t(type);
    const MeshMapTypeInfo *info = BKE_mesh_maps_type_info_get(t);
    ASSERT_NE(info, nullptr) << type;
    const bool rendered = (info->flags & MESH_MAP_TYPE_RENDERED) != 0;
    const bool cpu_fill = (info->flags & MESH_MAP_TYPE_CPU_FILL) != 0;
    const bool scalar = (info->flags & MESH_MAP_TYPE_SCALAR) != 0;
    const bool needs_sources = (info->flags & MESH_MAP_TYPE_NEEDS_SOURCES) != 0;
    const bool needs_other_atlas = (info->flags & MESH_MAP_TYPE_NEEDS_OTHER_ATLAS) != 0;
    EXPECT_EQ(BKE_mesh_maps_bake_type_renders(t), rendered) << type;
    EXPECT_EQ(BKE_mesh_maps_bake_type_scalar(t), scalar) << type;
    EXPECT_EQ(BKE_mesh_maps_bake_type_cpu_simple(t),
              cpu_fill && !needs_sources && !needs_other_atlas)
        << type;
    EXPECT_EQ(BKE_mesh_maps_bake_type_cpu_rays_scalar(t), needs_sources && scalar) << type;
    EXPECT_EQ(BKE_mesh_maps_bake_type_cpu_rays_vector(t), needs_sources && !scalar) << type;
    /* Every described type is one the bake can produce when its flags say so, and every scalar
     * map is filled somehow: through the engine or on the CPU. */
    EXPECT_EQ(BKE_mesh_maps_bake_type_is_supported(t), info->flags != 0) << type;
    EXPECT_TRUE(!scalar || rendered || cpu_fill) << type;
    EXPECT_TRUE(!needs_sources || cpu_fill) << type;
    EXPECT_TRUE(!needs_other_atlas || cpu_fill) << type;
    /* A rendered map bakes through the row's pass; a CPU fill needs no pass and no space.
     * Thickness and Bent Normal are CPU fills with no pass, and the ID maps likewise, so neither
     * family is mistaken for a render. */
    EXPECT_EQ(rendered, info->pass != eScenePassType(0)) << type;
    EXPECT_EQ(cpu_fill, !rendered && info->flags != 0u) << type;
    eBakeSpace space = R_BAKE_SPACE_CAMERA;
    const bool has_space = BKE_mesh_maps_bake_normal_space_for_type(t, space);
    EXPECT_EQ(has_space, info->pass == SCE_PASS_NORMAL) << type;
    if (has_space) {
      EXPECT_EQ(space, info->space) << type;
    }
  }

  /* The supported mask is the bits of the types the table supports, and the CPU mask is the two
   * ID maps. */
  uint32_t expected_mask = 0;
  for (int type = 0; type < MA_MESH_MAP_TYPE_NUM; type++) {
    const MeshMapTypeInfo *info = BKE_mesh_maps_type_info_get(int8_t(type));
    if (info != nullptr && info->flags != 0) {
      expected_mask |= 1u << uint32_t(type);
    }
  }
  EXPECT_EQ(uint32_t(MESH_MAP_BAKE_TYPE_MASK_SUPPORTED), expected_mask);
  EXPECT_EQ(uint32_t(MESH_MAP_BAKE_TYPE_MASK_ALL), uint32_t(MESH_MAP_BAKE_TYPE_MASK_SUPPORTED));
  EXPECT_EQ(uint32_t(MESH_MAP_BAKE_TYPE_MASK_CPU),
            uint32_t((1u << MA_MESH_MAP_ID_OBJECT) | (1u << MA_MESH_MAP_ID_MATERIAL)));
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
  bNode *emission = BKE_mesh_maps_bake_build_emit_tree(
      *ma->nodetree, MA_MESH_MAP_AO, settings, 3.5f, settings.samples);
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
  bNode *emission = BKE_mesh_maps_bake_build_emit_tree(
      *ma->nodetree, MA_MESH_MAP_CURVATURE, settings, 0.0f, settings.samples);
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
  bNode *emission = BKE_mesh_maps_bake_build_emit_tree(
      *ma->nodetree, MA_MESH_MAP_EDGE, settings, 0.0f, settings.samples);
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
  EXPECT_EQ(BKE_mesh_maps_bake_build_emit_tree(
                *ma->nodetree, MA_MESH_MAP_NORMAL_WORLD, settings, 0.0f, settings.samples),
            nullptr);
  EXPECT_EQ(BKE_mesh_maps_bake_build_emit_tree(
                *ma->nodetree, MA_MESH_MAP_ID_MATERIAL, settings, 0.0f, settings.samples),
            nullptr);
  EXPECT_EQ(count_nodes(*ma->nodetree, SH_NODE_EMISSION), 0);
  /* The negative guard is only meaningful next to a type that does build an emission surface. */
  EXPECT_NE(BKE_mesh_maps_bake_build_emit_tree(
                *ma->nodetree, MA_MESH_MAP_AO, settings, 1.0f, settings.samples),
            nullptr);
}

/** The Bevel node clamps its sample count into [2, 128] regardless of what the settings say. */
TEST_F(MeshMapBakeTest, edge_samples_are_clamped)
{
  const int inputs[] = {0, 1, 2, 8, 128, 512, 4096};
  const int expected[] = {2, 2, 2, 8, 128, 128, 128};
  for (int i = 0; i < 7; i++) {
    Material *ma = add_material("MapsMat");
    MaterialMeshMapSettings settings;
    settings.samples = inputs[i];
    bNode *emission = BKE_mesh_maps_bake_build_emit_tree(
        *ma->nodetree, MA_MESH_MAP_EDGE, settings, 0.0f, settings.samples);
    ASSERT_NE(emission, nullptr) << inputs[i];
    bNode *bevel = find_node(*ma->nodetree, SH_NODE_BEVEL);
    ASSERT_NE(bevel, nullptr) << inputs[i];
    EXPECT_EQ(bevel->custom1, expected[i]) << inputs[i];
  }
}

/**
 * The supersampling clamp the editor's pair preparation stands on (the clamp itself lives in the
 * editor, so the math it clamps with is what is tested here): a pair whose
 * #BKE_mesh_maps_bake_estimate_bytes stays under #MESH_MAP_BAKE_MEMORY_LIMIT_MB fits, and one over
 * it is refused -- whether the cost grew through the resolution, the supersampling or the UDIM
 * tile count.
 */
TEST_F(MeshMapBakeTest, supersampling_limit_clamps_or_fails)
{
  const size_t budget = size_t(MESH_MAP_BAKE_MEMORY_LIMIT_MB) * 1024 * 1024;
  /* 4096 at supersampling 1 fits the budget. */
  EXPECT_LE(BKE_mesh_maps_bake_estimate_bytes(4096, 1, 1), budget);
  /* 2048 at supersampling 4 (an 8192 raster) fits the budget. */
  EXPECT_LE(BKE_mesh_maps_bake_estimate_bytes(2048, 4, 1), budget);
  /* 4096 at supersampling 4 (a 16384 raster) is over the budget. */
  EXPECT_GT(BKE_mesh_maps_bake_estimate_bytes(4096, 4, 1), budget);
  /* 8192 at supersampling 1 stays inside the budget and the raster-side limit, so nothing refuses
   * it: the budget is what bounds the larger combinations below. */
  EXPECT_LE(BKE_mesh_maps_bake_estimate_bytes(8192, 1, 1), budget);
  /* The same 2048x4 pair times four candidate UDIM tiles is over the budget. */
  EXPECT_GT(BKE_mesh_maps_bake_estimate_bytes(2048, 4, 4), budget);
}

/** A slab: two quads a unit apart. A texel on the top face casts one ray down and reads 1. */
TEST_F(MeshMapBakeTest, fill_thickness_measures_interior_distance)
{
  Mesh *mesh = add_slab_mesh("Slab");

  /* One covered texel on the top face's first triangle, one uncovered. */
  BakePixel pixels[2] = {};
  pixels[0].primitive_id = 0;
  pixels[0].uv[0] = 0.25f;
  pixels[0].uv[1] = 0.25f;
  pixels[1].primitive_id = -1;
  float result[8] = {0.0f};
  const float4x4 identity = float4x4::identity();
  BKE_mesh_maps_bake_fill_thickness(pixels, 2, *mesh, identity, {}, 1, result);
  EXPECT_FLOAT_EQ(result[0], 1.0f);
  EXPECT_FLOAT_EQ(result[1], 1.0f);
  EXPECT_FLOAT_EQ(result[7], 0.0f);
}

/**
 * A mirrored low-poly matrix (negative determinant) flips the world-space winding of its faces, so
 * a normal built from the transformed vertices points into the solid. The fill must undo that, or
 * the ray walks the wrong way and every texel reads zero on physically thick geometry.
 */
TEST_F(MeshMapBakeTest, fill_thickness_handles_mirrored_low_matrix)
{
  Mesh *mesh = add_slab_mesh("Slab");
  BakePixel pixels[1] = {};
  pixels[0].primitive_id = 0;
  pixels[0].uv[0] = 0.25f;
  pixels[0].uv[1] = 0.25f;
  float result[4] = {0.0f};
  BKE_mesh_maps_bake_fill_thickness(pixels, 1, *mesh, mirror_x_about_half(), {}, 1, result);
  EXPECT_FLOAT_EQ(result[0], 1.0f);
  EXPECT_FLOAT_EQ(result[3], 1.0f);
}

/** The same sign problem on the hit side: a mirrored high-poly source must still classify its
 * bottom face as an exit, not an entry. */
TEST_F(MeshMapBakeTest, fill_thickness_handles_mirrored_source_matrix)
{
  Mesh *low = add_slab_mesh("LowSlab");
  Mesh *source = add_slab_mesh("SourceSlab");
  BakePixel pixels[1] = {};
  pixels[0].primitive_id = 0;
  pixels[0].uv[0] = 0.25f;
  pixels[0].uv[1] = 0.25f;
  MeshMapBakeThicknessSource sources[1] = {{source, mirror_x_about_half()}};
  float result[4] = {0.0f};
  BKE_mesh_maps_bake_fill_thickness(
      pixels, 1, *low, float4x4::identity(), Span<MeshMapBakeThicknessSource>(sources, 1), 1, result);
  EXPECT_FLOAT_EQ(result[0], 1.0f);
  EXPECT_FLOAT_EQ(result[3], 1.0f);
}

/** A covered texel whose rays never exit the solid is still covered: keep it opaque black rather
 * than the transparent zero the buffer starts with. */
TEST_F(MeshMapBakeTest, fill_thickness_open_surface_is_opaque_black)
{
  Mesh *plane = add_quad_two_uv("Plane");
  BakePixel pixels[1] = {};
  pixels[0].primitive_id = 0;
  pixels[0].uv[0] = 0.25f;
  pixels[0].uv[1] = 0.25f;
  float result[4] = {9.0f, 9.0f, 9.0f, 9.0f};
  BKE_mesh_maps_bake_fill_thickness(
      pixels, 1, *plane, float4x4::identity(), {}, 1, result);
  EXPECT_FLOAT_EQ(result[0], 0.0f);
  EXPECT_FLOAT_EQ(result[1], 0.0f);
  EXPECT_FLOAT_EQ(result[2], 0.0f);
  EXPECT_FLOAT_EQ(result[3], 1.0f);
}

/** An unoccluded open surface: the single ray is the outward normal, encoded as `0.5 + 0.5 * dir`. */
TEST_F(MeshMapBakeTest, fill_bent_normals_encodes_the_escaping_direction)
{
  Mesh *plane = add_quad_two_uv("Plane");
  BakePixel pixels[2] = {};
  pixels[0].primitive_id = 0;
  pixels[0].uv[0] = 0.25f;
  pixels[0].uv[1] = 0.25f;
  pixels[1].primitive_id = -1;
  float result[8] = {0.0f};
  BKE_mesh_maps_bake_fill_bent_normals(
      pixels, 2, *plane, float4x4::identity(), {}, 1, result);
  /* dir = (0, 0, 1) -> (0.5, 0.5, 1.0), opaque. */
  EXPECT_FLOAT_EQ(result[0], 0.5f);
  EXPECT_FLOAT_EQ(result[1], 0.5f);
  EXPECT_FLOAT_EQ(result[2], 1.0f);
  EXPECT_FLOAT_EQ(result[3], 1.0f);
  for (int i = 4; i < 8; i++) {
    EXPECT_FLOAT_EQ(result[i], 0.0f);
  }
}

/** A fully occluded texel has no unoccluded direction: black, but still opaque. */
TEST_F(MeshMapBakeTest, fill_bent_normals_occluded_texel_is_black_and_opaque)
{
  Mesh *plane = add_quad_two_uv("Plane");
  Mesh *cover = add_quad_two_uv("Cover");
  for (MutableSpan<float3> positions = cover->vert_positions_for_write(); float3 &position :
       positions)
  {
    position.z = 0.5f;
  }
  cover->tag_positions_changed();

  BakePixel pixels[1] = {};
  pixels[0].primitive_id = 0;
  pixels[0].uv[0] = 0.25f;
  pixels[0].uv[1] = 0.25f;
  MeshMapBakeThicknessSource sources[1] = {{cover, float4x4::identity()}};
  float result[4] = {9.0f, 9.0f, 9.0f, 9.0f};
  BKE_mesh_maps_bake_fill_bent_normals(
      pixels, 1, *plane, float4x4::identity(), Span<MeshMapBakeThicknessSource>(sources, 1), 1, result);
  EXPECT_FLOAT_EQ(result[0], 0.0f);
  EXPECT_FLOAT_EQ(result[1], 0.0f);
  EXPECT_FLOAT_EQ(result[2], 0.0f);
  EXPECT_FLOAT_EQ(result[3], 1.0f);
}

/** A mirrored low-poly must still send the hemisphere outward, so the encoded Z stays positive. */
TEST_F(MeshMapBakeTest, fill_bent_normals_handles_mirrored_low_matrix)
{
  Mesh *plane = add_quad_two_uv("Plane");
  BakePixel pixels[1] = {};
  pixels[0].primitive_id = 0;
  pixels[0].uv[0] = 0.25f;
  pixels[0].uv[1] = 0.25f;
  float result[4] = {0.0f};
  BKE_mesh_maps_bake_fill_bent_normals(
      pixels, 1, *plane, mirror_x_about_half(), {}, 1, result);
  EXPECT_NEAR(result[0], 0.5f, 1e-6f);
  EXPECT_NEAR(result[1], 0.5f, 1e-6f);
  EXPECT_NEAR(result[2], 1.0f, 1e-6f);
  EXPECT_FLOAT_EQ(result[3], 1.0f);
}

/** The default AO (global off) keeps the local flag: the bake traces the object alone. */
TEST_F(MeshMapBakeTest, emit_tree_default_ao_sets_the_local_flag)
{
  Material *ma = add_material("MapsMat");
  MaterialMeshMapSettings settings;
  settings.use_global_ao = 0;
  bNode *emission = BKE_mesh_maps_bake_build_emit_tree(
      *ma->nodetree, MA_MESH_MAP_AO, settings, 3.5f, settings.samples);
  ASSERT_NE(emission, nullptr);
  bNode *ao = find_node(*ma->nodetree, SH_NODE_AMBIENT_OCCLUSION);
  ASSERT_NE(ao, nullptr);
  EXPECT_TRUE(ao->custom2 & SHD_AO_LOCAL);
}

TEST_F(MeshMapBakeTest, emit_tree_global_ao_drops_the_local_flag)
{
  Material *ma = add_material("MapsMat");
  MaterialMeshMapSettings settings;
  settings.use_global_ao = 1;
  bNode *emission = BKE_mesh_maps_bake_build_emit_tree(
      *ma->nodetree, MA_MESH_MAP_AO, settings, 3.5f, settings.samples);
  ASSERT_NE(emission, nullptr);
  bNode *ao = find_node(*ma->nodetree, SH_NODE_AMBIENT_OCCLUSION);
  ASSERT_NE(ao, nullptr);
  EXPECT_FALSE(ao->custom2 & SHD_AO_LOCAL);
}

/**
 * Two extend passes fill the empty rows from their written neighbors and never cross a foreign
 * island: the row between own and foreign mixes both with the 1-2-1 weights, the row past the
 * foreign island reads the foreign value alone, and the second pass is what reaches that far.
 */
TEST_F(MeshMapBakeTest, dilate_fills_empty_and_respects_foreign)
{
  const int res = 4;
  ImBuf *ibuf = make_tile_buffer(res);
  ASSERT_NE(ibuf, nullptr);
  Array<uint8_t> own(res * res, 0);
  Array<uint8_t> own_margin(res * res, 0);
  Array<uint8_t> foreign(res * res, 0);
  /* Row 0: own covered with 1. Row 2: a foreign island with 0.25. Rows 1 and 3 are empty. */
  for (int x = 0; x < res; x++) {
    own[x] = 1;
    foreign[2 * res + x] = 1;
  }
  const Array<float> result(res * res * 4, 1.0f);
  float *atlas = ibuf->float_data_for_write();
  ASSERT_NE(atlas, nullptr);
  for (int x = 0; x < res; x++) {
    /* The foreign island arrived from its own bake, so it already carries the written alpha 1;
     * its RGB is what the extend must never overwrite. */
    for (int c = 0; c < 3; c++) {
      atlas[(2 * res + x) * 4 + c] = 0.25f;
    }
    atlas[(2 * res + x) * 4 + 3] = 1.0f;
  }
  BKE_mesh_maps_bake_extend_passes(
      MA_MESH_MAP_NORMAL_WORLD, own, own_margin, foreign, 4, result.data(), 0, 2, *ibuf);
  for (int x = 0; x < res; x++) {
    /* The own row is merged, not extended. */
    EXPECT_FLOAT_EQ(atlas[(0 * res + x) * 4 + 0], 1.0f);
    /* The pass-1 fill of row 1 weights the own row and the foreign row equally:
     * (1 * 4 + 0.25 * 4) / 8. */
    EXPECT_FLOAT_EQ(atlas[(1 * res + x) * 4 + 0], 0.625f);
    /* The foreign island is never overwritten. */
    EXPECT_FLOAT_EQ(atlas[(2 * res + x) * 4 + 0], 0.25f);
    /* The second pass reaches one row past the foreign island. */
    EXPECT_FLOAT_EQ(atlas[(3 * res + x) * 4 + 0], 0.25f);
    /* Alpha is the written mask: every texel the merge or a pass wrote carries 1. */
    EXPECT_FLOAT_EQ(atlas[(1 * res + x) * 4 + 3], 1.0f);
    EXPECT_FLOAT_EQ(atlas[(2 * res + x) * 4 + 3], 1.0f);
    EXPECT_FLOAT_EQ(atlas[(3 * res + x) * 4 + 3], 1.0f);
  }
  IMB_freeImBuf(ibuf);
}

/** A foreign island fully surrounded by own coverage stays exactly as it was: the mask marks it
 * #FILTER_MASK_USED, so no extend pass may write over it however many neighbors are written. */
TEST_F(MeshMapBakeTest, dilate_does_not_overwrite_foreign)
{
  const int res = 4;
  ImBuf *ibuf = make_tile_buffer(res);
  ASSERT_NE(ibuf, nullptr);
  Array<uint8_t> own(res * res, 1);
  Array<uint8_t> own_margin(res * res, 0);
  Array<uint8_t> foreign(res * res, 0);
  /* A 2x2 foreign block in the middle holds 0.25; everything else is this object's own. */
  const int block[4] = {1 * res + 1, 1 * res + 2, 2 * res + 1, 2 * res + 2};
  for (const int i : block) {
    own[i] = 0;
    foreign[i] = 1;
  }
  const Array<float> result(res * res * 4, 1.0f);
  float *atlas = ibuf->float_data_for_write();
  ASSERT_NE(atlas, nullptr);
  for (const int i : block) {
    for (int c = 0; c < 4; c++) {
      atlas[i * 4 + c] = 0.25f;
    }
  }
  BKE_mesh_maps_bake_extend_passes(
      MA_MESH_MAP_NORMAL_WORLD, own, own_margin, foreign, 4, result.data(), 0, 3, *ibuf);
  for (int i = 0; i < res * res; i++) {
    const bool is_foreign = foreign[i] != 0;
    for (int c = 0; c < 4; c++) {
      EXPECT_FLOAT_EQ(atlas[i * 4 + c], is_foreign ? 0.25f : 1.0f) << i << ", " << c;
    }
  }
  IMB_freeImBuf(ibuf);
}

/** With every texel assigned there is nothing writable and nothing changes however many passes
 * run; with zero passes only the merge lands and the background stays untouched. */
TEST_F(MeshMapBakeTest, dilate_stops_when_nothing_writable)
{
  const int res = 4;
  {
    ImBuf *ibuf = make_tile_buffer(res);
    ASSERT_NE(ibuf, nullptr);
    Array<uint8_t> own(res * res, 1);
    Array<uint8_t> no_margin(res * res, 0);
    Array<uint8_t> no_foreign(res * res, 0);
    const Array<float> result(res * res * 4, 0.5f);
    BKE_mesh_maps_bake_extend_passes(
        MA_MESH_MAP_NORMAL_WORLD, own, no_margin, no_foreign, 4, result.data(), 0, 8, *ibuf);
    const float *atlas = ibuf->float_data();
    ASSERT_NE(atlas, nullptr);
    for (int i = 0; i < res * res; i++) {
      for (int c = 0; c < 3; c++) {
        EXPECT_FLOAT_EQ(atlas[i * 4 + c], 0.5f) << i << ", " << c;
      }
      EXPECT_FLOAT_EQ(atlas[i * 4 + 3], 1.0f) << i;
    }
    IMB_freeImBuf(ibuf);
  }
  {
    ImBuf *ibuf = make_tile_buffer(res);
    ASSERT_NE(ibuf, nullptr);
    Array<uint8_t> own(res * res, 0);
    Array<uint8_t> no_margin(res * res, 0);
    Array<uint8_t> no_foreign(res * res, 0);
    for (int x = 0; x < res; x++) {
      own[x] = 1;
    }
    const Array<float> result(res * res * 4, 1.0f);
    BKE_mesh_maps_bake_extend_passes(
        MA_MESH_MAP_NORMAL_WORLD, own, no_margin, no_foreign, 4, result.data(), 0, 0, *ibuf);
    const float *atlas = ibuf->float_data();
    ASSERT_NE(atlas, nullptr);
    for (int x = 0; x < res; x++) {
      EXPECT_FLOAT_EQ(atlas[x * 4 + 0], 1.0f);
      EXPECT_FLOAT_EQ(atlas[x * 4 + 3], 1.0f);
    }
    for (int i = res; i < res * res; i++) {
      for (int c = 0; c < 4; c++) {
        EXPECT_FLOAT_EQ(atlas[i * 4 + c], 0.0f) << i << ", " << c;
      }
    }
    IMB_freeImBuf(ibuf);
  }
}

/**
 * The variant-A rebake: the tile holds bake A's island with its ring, then bake A runs again with
 * a smaller island, so the old ring is stale. The clear wipes it where the extend will not
 * rewrite it, the foreign island keeps its content, and nothing survives past the new ring.
 */
TEST_F(MeshMapBakeTest, dilate_rebake_does_not_leave_stale)
{
  const int res = 4;
  ImBuf *ibuf = make_tile_buffer(res);
  ASSERT_NE(ibuf, nullptr);
  float *atlas = ibuf->float_data_for_write();
  ASSERT_NE(atlas, nullptr);
  /* The previous bake covered the whole tile with its island and ring; one foreign island of
   * another object holds 0.5. */
  for (int i = 0; i < res * res; i++) {
    for (int c = 0; c < 4; c++) {
      atlas[i * 4 + c] = 1.0f;
    }
  }
  for (int c = 0; c < 4; c++) {
    atlas[(3 * res + 0) * 4 + c] = 0.5f;
  }
  Array<uint8_t> own(res * res, 0);
  Array<uint8_t> own_margin(res * res, 0);
  Array<uint8_t> foreign(res * res, 0);
  for (int x = 0; x < res; x++) {
    /* The new island is row 0 only, with one row of margin below it. */
    own[x] = 1;
    own_margin[res + x] = 1;
  }
  foreign[3 * res + 0] = 1;
  const Array<float> result(res * res * 4, 1.0f);
  /* A margin of 2 widens the clear to 2 + 1 steps, one row past the extend's reach. */
  BKE_mesh_maps_bake_extend_passes(
      MA_MESH_MAP_NORMAL_WORLD, own, own_margin, foreign, 4, result.data(), 2, 1, *ibuf);
  for (int x = 0; x < res; x++) {
    EXPECT_FLOAT_EQ(atlas[(0 * res + x) * 4 + 0], 1.0f) << x;
    EXPECT_FLOAT_EQ(atlas[(0 * res + x) * 4 + 3], 1.0f) << x;
    EXPECT_FLOAT_EQ(atlas[(1 * res + x) * 4 + 0], 1.0f) << x;
    EXPECT_FLOAT_EQ(atlas[(1 * res + x) * 4 + 3], 1.0f) << x;
  }
  /* Row 2 is the pass-1 fill. The two left texels mix the foreign island at (0, 3) as a source,
   * with the 1-2-1 weights: (1 * 3 + 0.5 * 2) / 5 and (1 * 4 + 0.5 * 1) / 5; the two right ones
   * read the margin row alone. */
  EXPECT_FLOAT_EQ(atlas[(2 * res + 0) * 4 + 0], 0.8f);
  EXPECT_FLOAT_EQ(atlas[(2 * res + 1) * 4 + 0], 0.9f);
  EXPECT_FLOAT_EQ(atlas[(2 * res + 2) * 4 + 0], 1.0f);
  EXPECT_FLOAT_EQ(atlas[(2 * res + 3) * 4 + 0], 1.0f);
  for (int x = 0; x < res; x++) {
    EXPECT_FLOAT_EQ(atlas[(2 * res + x) * 4 + 3], 1.0f) << x;
  }
  /* The foreign island is untouched. */
  EXPECT_FLOAT_EQ(atlas[(3 * res + 0) * 4 + 0], 0.5f);
  /* The texel next to it is refilled from it as a source. */
  EXPECT_FLOAT_EQ(atlas[(3 * res + 1) * 4 + 0], 0.5f);
  /* Past the new ring the stale 1 of the old bake is gone: cleared back to the zero background. */
  for (int x = 2; x < res; x++) {
    for (int c = 0; c < 4; c++) {
      EXPECT_FLOAT_EQ(atlas[(3 * res + x) * 4 + c], 0.0f) << x << ", " << c;
    }
  }
  IMB_freeImBuf(ibuf);
}

/** A 2x box downsample averages only the covered subtexels: one covered subtexel of a block keeps
 * its color instead of fading toward the zero background, and alpha follows the coverage. */
TEST_F(MeshMapBakeTest, downsample_result_is_coverage_weighted)
{
  const int src_res = 2;
  const int dst_res = 1;
  Array<float> source(src_res * src_res * 4, 0.0f);
  Array<uint8_t> source_cov(src_res * src_res, 0);
  /* Only the first subtexel is covered, with rgb (1, 0.5, 0); the rest are the background. */
  source[0] = 1.0f;
  source[1] = 0.5f;
  source[3] = 1.0f;
  source_cov[0] = 1;
  Array<float> dest(dst_res * dst_res * 4, 0.0f);
  Array<uint8_t> dest_cov(dst_res * dst_res, 0);
  BKE_mesh_maps_bake_downsample_result(
      Span<float>(source.data(), source.size()), source_cov, src_res, dest, dst_res, 2);
  BKE_mesh_maps_bake_downsample_coverage(
      Span<uint8_t>(source_cov.data(), source_cov.size()), src_res, dest_cov, dst_res, 2);
  /* The uncovered subtexels contribute neither color nor weight: the covered texel's color
   * survives undiluted, not the (0.25, 0.125, 0) an average over all four would give. */
  EXPECT_FLOAT_EQ(dest[0], 1.0f);
  EXPECT_FLOAT_EQ(dest[1], 0.5f);
  EXPECT_FLOAT_EQ(dest[2], 0.0f);
  /* Alpha mirrors the ORed coverage, so a partially covered block is still opaque. */
  EXPECT_FLOAT_EQ(dest[3], 1.0f);
  EXPECT_EQ(dest_cov[0], 1);
}

/** A block with no covered subtexel resolves to the zero background, whatever the raster held. */
TEST_F(MeshMapBakeTest, downsample_result_empty_block_is_zero)
{
  const int src_res = 2;
  const int dst_res = 1;
  Array<float> source(src_res * src_res * 4, 1.0f);
  Array<uint8_t> source_cov(src_res * src_res, 0);
  Array<float> dest(dst_res * dst_res * 4, 1.0f);
  BKE_mesh_maps_bake_downsample_result(
      Span<float>(source.data(), source.size()), source_cov, src_res, dest, dst_res, 2);
  for (int c = 0; c < 4; c++) {
    EXPECT_FLOAT_EQ(dest[c], 0.0f);
  }
}

/** Two of four covered subtexels with the same color resolve to that color, alpha 1. */
TEST_F(MeshMapBakeTest, downsample_result_two_of_four_covered)
{
  const int src_res = 2;
  const int dst_res = 1;
  Array<float> source(src_res * src_res * 4, 0.0f);
  Array<uint8_t> source_cov(src_res * src_res, 0);
  for (int i = 0; i < 4; i++) {
    source[i * 4 + 0] = 0.25f;
    source[i * 4 + 1] = 0.5f;
    source[i * 4 + 2] = 0.75f;
    source_cov[i] = (i % 2 == 0) ? 1 : 0;
  }
  Array<float> dest(dst_res * dst_res * 4, 0.0f);
  BKE_mesh_maps_bake_downsample_result(
      Span<float>(source.data(), source.size()), source_cov, src_res, dest, dst_res, 2);
  EXPECT_FLOAT_EQ(dest[0], 0.25f);
  EXPECT_FLOAT_EQ(dest[1], 0.5f);
  EXPECT_FLOAT_EQ(dest[2], 0.75f);
  EXPECT_FLOAT_EQ(dest[3], 1.0f);
}

/** A block averaging two different unit normals resolves below unit length; the renormalize
 * helper brings it back to one without touching alpha. */
TEST_F(MeshMapBakeTest, normalize_normals_restores_unit_length)
{
  const int src_res = 2;
  const int dst_res = 1;
  Array<float> source(src_res * src_res * 4, 0.0f);
  Array<uint8_t> source_cov(src_res * src_res, 0);
  /* One covered subtexel per normal: (0, 0, 1) and (1, 0, 0) share the block. */
  source[2] = 1.0f;
  source[3] = 1.0f;
  source_cov[0] = 1;
  source[4] = 1.0f;
  source[7] = 1.0f;
  source_cov[1] = 1;
  Array<float> dest(dst_res * dst_res * 4, 0.0f);
  Array<uint8_t> dest_cov(dst_res * dst_res, 0);
  BKE_mesh_maps_bake_downsample_result(
      Span<float>(source.data(), source.size()), source_cov, src_res, dest, dst_res, 2);
  BKE_mesh_maps_bake_downsample_coverage(
      Span<uint8_t>(source_cov.data(), source_cov.size()), src_res, dest_cov, dst_res, 2);
  BKE_mesh_maps_bake_normalize_normals(dest, dest_cov);
  /* The raw average (0.5, 0, 0.5) is too short; renormalized it is a unit diagonal. */
  const float len = std::sqrt(dest[0] * dest[0] + dest[1] * dest[1] + dest[2] * dest[2]);
  EXPECT_FLOAT_EQ(len, 1.0f);
  EXPECT_FLOAT_EQ(dest[0], std::sqrt(0.5f));
  EXPECT_FLOAT_EQ(dest[1], 0.0f);
  EXPECT_FLOAT_EQ(dest[3], 1.0f);
}

/** The commit's resolve: a 4x4 raster tile boxes down into a 2x2 atlas tile -- the own and margin
 * coverage OR down separately, and the result follows the coverage-weighted rules: a block with
 * one covered subtexel keeps its color undiluted, an empty block resolves to the zero background,
 * and two covered subtexels of the same color resolve to that color. */
TEST_F(MeshMapBakeTest, resolve_raster_one_tile_factor_two)
{
  const int raster_res = 4;
  const int atlas_res = 2;
  const int64_t raster_texels = int64_t(raster_res) * raster_res;
  const int64_t atlas_texels = int64_t(atlas_res) * atlas_res;
  Array<float> raster(raster_texels * 4, 0.0f);
  Array<uint8_t> raster_own(raster_texels, 0);
  Array<uint8_t> raster_margin(raster_texels, 0);

  /* Atlas block (0, 0) covers raster texels {0, 1, 4, 5}: one own subtexel, color (1, 0.5, 0).
   * Block (0, 1) covers {8, 9, 12, 13}: two own subtexels with the same color. The margin-only
   * texel 10 fills block (1, 1) with its own color, and the margin texel 0 joins the own texel of
   * block (0, 0). Block (1, 0) stays empty. */
  raster[0] = 1.0f;
  raster[1] = 0.5f;
  raster_own[0] = 1;
  raster_margin[0] = 1;
  for (const int texel : {8, 9}) {
    raster[texel * 4 + 0] = 0.25f;
    raster[texel * 4 + 1] = 0.5f;
    raster[texel * 4 + 2] = 0.75f;
    raster_own[texel] = 1;
  }
  raster[10 * 4 + 0] = 0.2f;
  raster[10 * 4 + 1] = 0.4f;
  raster[10 * 4 + 2] = 0.6f;
  raster_margin[10] = 1;

  /* Prefilled with sentinels, so the assertions tell an actively written texel from a skipped
   * one. */
  Array<float> atlas(atlas_texels * 4, 9.0f);
  Array<uint8_t> atlas_own(atlas_texels, 9);
  Array<uint8_t> atlas_margin(atlas_texels, 9);
  BKE_mesh_maps_bake_resolve_raster(raster,
                                    raster_own,
                                    raster_margin,
                                    MA_MESH_MAP_AO,
                                    raster_res,
                                    atlas_res,
                                    1,
                                    atlas,
                                    atlas_own,
                                    atlas_margin);

  /* The own and the margin masks OR down separately: texel 0 is both, texel 1 is neither (its
   * block's own and margin coverage both stayed empty), texel 2 is own only, texel 3 margin
   * only. */
  EXPECT_EQ(atlas_own[0], 1);
  EXPECT_EQ(atlas_own[1], 0);
  EXPECT_EQ(atlas_own[2], 1);
  EXPECT_EQ(atlas_own[3], 0);
  EXPECT_EQ(atlas_margin[0], 1);
  EXPECT_EQ(atlas_margin[1], 0);
  EXPECT_EQ(atlas_margin[2], 0);
  EXPECT_EQ(atlas_margin[3], 1);
  /* Block (0, 0): the one covered subtexel's color survives undiluted, alpha 1 by coverage. */
  EXPECT_FLOAT_EQ(atlas[0], 1.0f);
  EXPECT_FLOAT_EQ(atlas[1], 0.5f);
  EXPECT_FLOAT_EQ(atlas[2], 0.0f);
  EXPECT_FLOAT_EQ(atlas[3], 1.0f);
  /* Block (1, 0): fully empty, so the resolve zeroes it against the sentinel. */
  for (int c = 0; c < 4; c++) {
    EXPECT_FLOAT_EQ(atlas[4 + c], 0.0f) << c;
  }
  /* Block (0, 1): two covered subtexels of the same color resolve to that color. */
  EXPECT_FLOAT_EQ(atlas[8], 0.25f);
  EXPECT_FLOAT_EQ(atlas[9], 0.5f);
  EXPECT_FLOAT_EQ(atlas[10], 0.75f);
  EXPECT_FLOAT_EQ(atlas[11], 1.0f);
  /* Block (1, 1): the margin texel carries the block, color included. */
  EXPECT_FLOAT_EQ(atlas[12], 0.2f);
  EXPECT_FLOAT_EQ(atlas[13], 0.4f);
  EXPECT_FLOAT_EQ(atlas[14], 0.6f);
  EXPECT_FLOAT_EQ(atlas[15], 1.0f);
}

/** A block averaging two different unit normals resolves below unit length; the resolve brings
 * normal-space maps back to unit length wherever the downsampled union marks. */
TEST_F(MeshMapBakeTest, resolve_raster_normals_are_renormalized)
{
  const int raster_res = 2;
  const int atlas_res = 1;
  Array<float> raster(4 * 4, 0.0f);
  Array<uint8_t> raster_own(4, 0);
  Array<uint8_t> raster_margin(4, 0);
  /* One covered subtexel per normal: (0, 0, 1) and (1, 0, 0) share the single atlas block. */
  raster[2] = 1.0f;
  raster[3] = 1.0f;
  raster_own[0] = 1;
  raster[4] = 1.0f;
  raster[7] = 1.0f;
  raster_own[1] = 1;
  Array<float> atlas(4, 0.0f);
  Array<uint8_t> atlas_own(1, 0);
  Array<uint8_t> atlas_margin(1, 0);
  BKE_mesh_maps_bake_resolve_raster(raster,
                                    raster_own,
                                    raster_margin,
                                    MA_MESH_MAP_NORMAL_WORLD,
                                    raster_res,
                                    atlas_res,
                                    1,
                                    atlas,
                                    atlas_own,
                                    atlas_margin);
  /* The raw average (0.5, 0, 0.5) is too short; renormalized it is a unit diagonal. */
  const float len = std::sqrt(atlas[0] * atlas[0] + atlas[1] * atlas[1] + atlas[2] * atlas[2]);
  EXPECT_FLOAT_EQ(len, 1.0f);
  EXPECT_FLOAT_EQ(atlas[0], std::sqrt(0.5f));
  EXPECT_FLOAT_EQ(atlas[1], 0.0f);
  EXPECT_FLOAT_EQ(atlas[3], 1.0f);
  EXPECT_EQ(atlas_own[0], 1);
}

/** The downsample helpers report a refused resolve instead of silently doing nothing: a false
 * return leaves every destination texel exactly as it was. */
TEST_F(MeshMapBakeTest, downsample_invalid_factor_is_a_no_op)
{
  const int src_res = 2;
  const int dst_res = 1;
  Array<float> source(src_res * src_res * 4, 1.0f);
  Array<uint8_t> source_cov(src_res * src_res, 1);
  Array<float> dest(dst_res * dst_res * 4, 9.0f);
  Array<uint8_t> dest_cov(dst_res * dst_res, 9);

  /* A factor below 2 is no downsample. */
  EXPECT_FALSE(BKE_mesh_maps_bake_downsample_result(
      source, source_cov, src_res, dest, dst_res, 1));
  EXPECT_FALSE(BKE_mesh_maps_bake_downsample_coverage(
      source_cov, src_res, dest_cov, dst_res, 1));
  /* A source that is not the destination times the factor is a shape mismatch. */
  EXPECT_FALSE(BKE_mesh_maps_bake_downsample_result(
      source, source_cov, src_res, dest, dst_res, 3));
  EXPECT_FALSE(BKE_mesh_maps_bake_downsample_coverage(
      source_cov, src_res, dest_cov, dst_res, 3));
  /* A destination too small for its resolution is refused. */
  EXPECT_FALSE(BKE_mesh_maps_bake_downsample_result(
      source, source_cov, src_res, MutableSpan<float>(dest.data(), 3), dst_res, 2));
  EXPECT_FALSE(BKE_mesh_maps_bake_downsample_coverage(
      source_cov, src_res, MutableSpan<uint8_t>(dest_cov.data(), 0), dst_res, 2));

  /* Nothing was written anywhere. */
  for (const float value : dest) {
    EXPECT_FLOAT_EQ(value, 9.0f);
  }
  for (const uint8_t value : dest_cov) {
    EXPECT_EQ(value, 9);
  }

  /* A valid shape still reports success and writes. */
  Array<float> written(dst_res * dst_res * 4, 9.0f);
  Array<uint8_t> written_cov(dst_res * dst_res, 9);
  EXPECT_TRUE(
      BKE_mesh_maps_bake_downsample_result(source, source_cov, src_res, written, dst_res, 2));
  EXPECT_TRUE(
      BKE_mesh_maps_bake_downsample_coverage(source_cov, src_res, written_cov, dst_res, 2));
  EXPECT_FLOAT_EQ(written[0], 1.0f);
  EXPECT_FLOAT_EQ(written[3], 1.0f);
  EXPECT_EQ(written_cov[0], 1);
}

/** The Normal Filter curvature reads a tangent normal field: a ramp along U bends positive. */
TEST_F(MeshMapBakeTest, curvature_from_normal_reads_divergence)
{
  const int res = 8;
  Array<float> normals(res * res * 4, 0.0f);
  BakePixel pixels[res * res] = {};
  for (int y = 0; y < res; y++) {
    for (int x = 0; x < res; x++) {
      const size_t i = size_t(y) * res + x;
      pixels[i].primitive_id = 0;
      /* A normal that tilts along U: nx grows with x, the surface bends convex. */
      normals[i * 4 + 0] = 0.5f + float(x) * 0.25f / float(res);
      normals[i * 4 + 1] = 0.5f;
      normals[i * 4 + 2] = 1.0f;
      normals[i * 4 + 3] = 1.0f;
    }
  }
  float result[res * res * 4] = {0.0f};
  BKE_mesh_maps_bake_fill_curvature_from_normal(
      pixels, res * res, res, Span<float>(normals.data(), normals.size()), 2, 1.0f, false, result);
  /* The middle columns read above neutral (convex), the map stays scalar in R. */
  EXPECT_GT(result[(4 * res + 4) * 4 + 0], 0.5f);
  EXPECT_FLOAT_EQ(result[(4 * res + 4) * 4 + 1], result[(4 * res + 4) * 4 + 0]);
  EXPECT_FLOAT_EQ(result[(4 * res + 4) * 4 + 3], 1.0f);
}

/**
 * The DirectX decode flips the green channel before the 2x-1 expansion: it must be exactly
 * `1 - 2 * encoded`, not `-encoded`. A vertical ramp turns the sign and magnitude of the V
 * divergence into a numeric check of the decode.
 */
TEST_F(MeshMapBakeTest, curvature_from_normal_decodes_directx_green)
{
  const int res = 8;
  Array<float> normals(res * res * 4, 0.0f);
  BakePixel pixels[res * res] = {};
  for (int y = 0; y < res; y++) {
    for (int x = 0; x < res; x++) {
      const size_t i = size_t(y) * res + x;
      pixels[i].primitive_id = 0;
      /* Encoded ty grows with y: GL decodes it as increasing, DirectX as decreasing. */
      normals[i * 4 + 0] = 0.5f;
      normals[i * 4 + 1] = 0.5f + float(y) * 0.25f / float(res);
      normals[i * 4 + 2] = 1.0f;
      normals[i * 4 + 3] = 1.0f;
    }
  }
  const size_t center = (4 * res + 4) * 4;
  float gl_result[res * res * 4] = {0.0f};
  BKE_mesh_maps_bake_fill_curvature_from_normal(
      pixels,
      res * res,
      res,
      Span<float>(normals.data(), normals.size()),
      2,
      1.0f,
      false,
      gl_result);
  /* GL: (-1 + 0.25*y) has slope 0.0625 per texel; over 2r = 4 texels the divergence is 0.0625. */
  EXPECT_NEAR(gl_result[center + 0], 0.53125f, 1e-6f);
  EXPECT_FLOAT_EQ(gl_result[center + 1], gl_result[center + 0]);

  float dx_result[res * res * 4] = {0.0f};
  BKE_mesh_maps_bake_fill_curvature_from_normal(
      pixels,
      res * res,
      res,
      Span<float>(normals.data(), normals.size()),
      2,
      1.0f,
      true,
      dx_result);
  /* DirectX decode is `1 - 2*t = -0.0625*y`, so the same ramp bends the other way with the same
   * magnitude. The old `-t` decode gave -0.03125 instead. */
  EXPECT_NEAR(dx_result[center + 0], 0.46875f, 1e-6f);
  EXPECT_FLOAT_EQ(dx_result[center + 1], dx_result[center + 0]);
  EXPECT_FLOAT_EQ(dx_result[center + 3], 1.0f);
}

TEST_F(MeshMapBakeTest, scalar_types_match_paint_layers)
{
  for (int type = 0; type < MA_MESH_MAP_TYPE_NUM; type++) {
    EXPECT_EQ(BKE_mesh_maps_bake_type_scalar(int8_t(type)),
              paint_layer_mesh_map_is_scalar(int8_t(type)))
        << type;
  }
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
  BKE_mesh_maps_bake_write_covered(
      MA_MESH_MAP_AO, pixels, 2, 4, result, MutableSpan<float>(atlas, 8));
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
  /* A pristine atlas is transparent black: alpha reads as the written mask, so the untouched
   * background is 0 from the first texel to the last. */
  const float *data = ibuf->float_data();
  ASSERT_NE(data, nullptr);
  const size_t texels = size_t(width) * size_t(height);
  for (const size_t texel : {size_t(0), texels / 2, texels - 1}) {
    for (int c = 0; c < 4; c++) {
      EXPECT_FLOAT_EQ(data[texel * 4 + c], 0.0f) << "texel " << texel << " channel " << c;
    }
  }
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

TEST_F(MeshMapBakeTest, coverage_from_pixels_marks_covered)
{
  BakePixel pixels[3] = {};
  pixels[0].primitive_id = 0;
  pixels[1].primitive_id = 5;
  pixels[2].primitive_id = -1;

  uint8_t coverage[3] = {9, 9, 9};
  BKE_mesh_maps_bake_coverage_from_pixels(pixels, 3, MutableSpan<uint8_t>(coverage, 3));
  EXPECT_EQ(coverage[0], 1);
  EXPECT_EQ(coverage[1], 1);
  EXPECT_EQ(coverage[2], 0);

  /* A destination shorter than the pixel array is left untouched rather than overrun. */
  uint8_t short_coverage[2] = {7, 7};
  BKE_mesh_maps_bake_coverage_from_pixels(pixels, 3, MutableSpan<uint8_t>(short_coverage, 2));
  EXPECT_EQ(short_coverage[0], 7);
  EXPECT_EQ(short_coverage[1], 7);
}

TEST_F(MeshMapBakeTest, coverage_union_and_overlap)
{
  uint8_t a[4] = {1, 1, 0, 0};
  uint8_t b[4] = {0, 1, 1, 0};
  uint8_t destination[4] = {0, 0, 1, 0};
  BKE_mesh_maps_bake_coverage_union(Span<uint8_t>(a, 4), MutableSpan<uint8_t>(destination, 4));
  EXPECT_EQ(destination[0], 1);
  EXPECT_EQ(destination[1], 1);
  EXPECT_EQ(destination[2], 1);
  EXPECT_EQ(destination[3], 0);

  EXPECT_EQ(BKE_mesh_maps_bake_coverage_overlap(Span<uint8_t>(a, 4), Span<uint8_t>(b, 4)), 1);
}

TEST_F(MeshMapBakeTest, write_with_margin_protects_foreign_islands)
{
  uint8_t own_coverage[4] = {1, 0, 0, 0};
  uint8_t own_margin[4] = {0, 1, 1, 0};
  uint8_t foreign[4] = {0, 0, 1, 1};
  const float result[16] = {
      0.10f, 0.20f, 0.30f, 1.0f, 0.40f, 0.50f, 0.60f, 1.0f,
      0.70f, 0.80f, 0.90f, 1.0f, 0.15f, 0.25f, 0.35f, 1.0f};
  float atlas[16] = {};

  BKE_mesh_maps_bake_write_with_margin(MA_MESH_MAP_NORMAL_WORLD,
                                       Span<uint8_t>(own_coverage, 4),
                                       Span<uint8_t>(own_margin, 4),
                                       Span<uint8_t>(foreign, 4),
                                       4,
                                       result,
                                       MutableSpan<float>(atlas, 16));

  /* Own covered pixel: written. */
  EXPECT_FLOAT_EQ(atlas[0], 0.10f);
  EXPECT_FLOAT_EQ(atlas[1], 0.20f);
  EXPECT_FLOAT_EQ(atlas[2], 0.30f);
  EXPECT_FLOAT_EQ(atlas[3], 1.0f);
  /* Margin over an unowned texel: written. */
  EXPECT_FLOAT_EQ(atlas[4], 0.40f);
  EXPECT_FLOAT_EQ(atlas[5], 0.50f);
  EXPECT_FLOAT_EQ(atlas[6], 0.60f);
  /* Margin over a foreign island and over an untouched foreign texel: refused. */
  for (int i = 8; i < 16; i++) {
    EXPECT_FLOAT_EQ(atlas[i], 0.0f);
  }
}

TEST_F(MeshMapBakeTest, write_with_margin_own_coverage_beats_foreign)
{
  uint8_t own_coverage[1] = {1};
  uint8_t no_margin[1] = {0};
  uint8_t foreign[1] = {1};
  const float result[4] = {0.25f, 0.5f, 0.75f, 1.0f};
  float atlas[4] = {};

  BKE_mesh_maps_bake_write_with_margin(MA_MESH_MAP_NORMAL_WORLD,
                                       Span<uint8_t>(own_coverage, 1),
                                       Span<uint8_t>(no_margin, 1),
                                       Span<uint8_t>(foreign, 1),
                                       4,
                                       result,
                                       MutableSpan<float>(atlas, 4));
  EXPECT_FLOAT_EQ(atlas[0], 0.25f);
  EXPECT_FLOAT_EQ(atlas[1], 0.5f);
  EXPECT_FLOAT_EQ(atlas[2], 0.75f);
}

TEST_F(MeshMapBakeTest, write_with_margin_spreads_scalar)
{
  uint8_t own_coverage[1] = {1};
  uint8_t no_margin[1] = {0};
  uint8_t no_foreign[1] = {0};
  const float result[4] = {0.4f, 0.0f, 0.0f, 1.0f};
  float atlas[4] = {};

  BKE_mesh_maps_bake_write_with_margin(MA_MESH_MAP_AO,
                                       Span<uint8_t>(own_coverage, 1),
                                       Span<uint8_t>(no_margin, 1),
                                       Span<uint8_t>(no_foreign, 1),
                                       4,
                                       result,
                                       MutableSpan<float>(atlas, 4));
  EXPECT_FLOAT_EQ(atlas[0], 0.4f);
  EXPECT_FLOAT_EQ(atlas[1], 0.4f);
  EXPECT_FLOAT_EQ(atlas[2], 0.4f);
  EXPECT_FLOAT_EQ(atlas[3], 1.0f);
}

/**
 * The alpha invariant over a pristine atlas (the transparent black #BKE_mesh_maps_bake_atlas_ensure
 * fills): after the merge, alpha is 1 exactly on the covered and the filled margin texels, and the
 * untouched background keeps RGBA = 0.
 */
TEST_F(MeshMapBakeTest, write_with_margin_alpha_is_the_written_mask)
{
  uint8_t own_coverage[4] = {1, 0, 0, 0};
  uint8_t own_margin[4] = {0, 1, 0, 0};
  uint8_t no_foreign[4] = {0, 0, 0, 0};
  const float result[16] = {0.10f, 0.20f, 0.30f, 1.0f, 0.40f, 0.50f, 0.60f, 1.0f,
                            0.70f, 0.80f, 0.90f, 1.0f, 0.15f, 0.25f, 0.35f, 1.0f};
  /* The pristine background of a fresh atlas: transparent black everywhere. */
  float atlas[16] = {};

  BKE_mesh_maps_bake_write_with_margin(MA_MESH_MAP_NORMAL_WORLD,
                                       Span<uint8_t>(own_coverage, 4),
                                       Span<uint8_t>(own_margin, 4),
                                       Span<uint8_t>(no_foreign, 4),
                                       4,
                                       result,
                                       MutableSpan<float>(atlas, 16));

  /* The covered and the margin texels carry alpha 1. */
  EXPECT_FLOAT_EQ(atlas[3], 1.0f);
  EXPECT_FLOAT_EQ(atlas[7], 1.0f);
  /* The untouched background stays transparent black, RGB and alpha alike. */
  for (int i = 8; i < 16; i++) {
    EXPECT_FLOAT_EQ(atlas[i], 0.0f) << "index " << i;
  }
}

/* -------------------------------------------------------------------- */
/** \name UV resolve (the active-UV stand-in for the material-named layer)
 * \{ */

TEST_F(MeshMapBakeTest, uv_resolve_returns_the_active_layer)
{
  Mesh *mesh = add_quad_two_uv("Quad");
  /* The active map is `UVMap`; `Chosen` exists but is not active. */
  const char *name = BKE_mesh_maps_uv_resolve(*mesh);
  ASSERT_NE(name, nullptr);
  EXPECT_STREQ(name, "UVMap");
}

TEST_F(MeshMapBakeTest, uv_resolve_is_null_without_any_uv_layer)
{
  Mesh *mesh = add_pair_mesh("Pair");
  EXPECT_EQ(BKE_mesh_maps_uv_resolve(*mesh), nullptr);
}

TEST_F(MeshMapBakeTest, uv_resolve_or_fail_keeps_baking_when_a_layer_exists)
{
  Mesh *mesh = add_quad_two_uv("Quad");
  Material *ma = add_material("MapsMat");
  Object *ob = add_simple_object("Ob", mesh, ma);
  ObjectMeshMapState *state = BKE_mesh_maps_object_state_ensure(*ob, *ma, MA_MESH_MAP_ID_OBJECT);
  ASSERT_NE(state, nullptr);
  const int8_t previous = BKE_mesh_maps_bake_state_begin(*state);
  EXPECT_EQ(previous, OB_MESH_MAP_STATUS_NONE);

  const char *name = BKE_mesh_maps_bake_uv_resolve_or_fail(*state, *ob);
  ASSERT_NE(name, nullptr);
  EXPECT_STREQ(name, "UVMap");
  /* A successful resolve leaves the in-flight status alone. */
  EXPECT_EQ(state->status, OB_MESH_MAP_STATUS_BAKING);
}

TEST_F(MeshMapBakeTest, uv_resolve_or_fail_marks_error_without_a_layer)
{
  Mesh *mesh = add_pair_mesh("Pair");
  Material *ma = add_material("MapsMat");
  Object *ob = add_simple_object("Ob", mesh, ma);
  ObjectMeshMapState *state = BKE_mesh_maps_object_state_ensure(*ob, *ma, MA_MESH_MAP_ID_OBJECT);
  ASSERT_NE(state, nullptr);
  BKE_mesh_maps_bake_state_begin(*state);

  EXPECT_EQ(BKE_mesh_maps_bake_uv_resolve_or_fail(*state, *ob), nullptr);
  EXPECT_EQ(state->status, OB_MESH_MAP_STATUS_ERROR);
}

/** \} */

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
  ASSERT_TRUE(BKE_mesh_maps_bake_hash_from_viewport(
      *bmain, *scene, *view_layer, *ob, *ma, MA_MESH_MAP_AO, start_hash));
  BKE_mesh_maps_object_state_mark_baked(*state, start_hash, 1);
  EXPECT_EQ(state->status, OB_MESH_MAP_STATUS_VALID);

  /* `refresh()` sees the same viewport geometry, so the bake stays current. */
  Depsgraph *viewport_graph = nullptr;
  Object *ob_viewport =
      eval_object(*bmain, *scene, *view_layer, *ob, DAG_EVAL_VIEWPORT, viewport_graph);
  ASSERT_NE(ob_viewport, nullptr);
  EXPECT_TRUE(BKE_mesh_maps_object_state_is_current(*state, *ob_viewport, *ma));
  EXPECT_EQ(BKE_mesh_maps_object_state_refresh(*state, *ob_viewport, *ma),
            OB_MESH_MAP_STATUS_VALID);

  /* The render evaluation would have disagreed. */
  Depsgraph *render_graph = nullptr;
  Object *ob_render =
      eval_object(*bmain, *scene, *view_layer, *ob, DAG_EVAL_RENDER, render_graph);
  ASSERT_NE(ob_render, nullptr);
  EXPECT_FALSE(BKE_mesh_maps_object_state_is_current(*state, *ob_render, *ma));

  DEG_graph_free(viewport_graph);
  DEG_graph_free(render_graph);
}

/**
 * The Normal Filter gate is "Tangent Normal status VALID *and* current", not just VALID: a state
 * recorded from earlier content must stop matching when a setting the tangent hash folds in moves,
 * even though `is_current` itself leaves the status VALID. This pins the second half of the gate.
 */
TEST_F(MeshMapBakeTest, valid_state_is_not_current_after_content_change)
{
  Mesh *mesh = add_quad_two_uv("Quad");
  Material *ma = add_material("MapsMat");
  Object *ob = add_simple_object("Ob", mesh, ma);
  Scene *scene = add_scene_with_object("Scene", ob);
  ViewLayer *view_layer = static_cast<ViewLayer *>(scene->view_layers.first);

  Depsgraph *graph = nullptr;
  Object *ob_eval = eval_object(*bmain, *scene, *view_layer, *ob, DAG_EVAL_VIEWPORT, graph);
  ASSERT_NE(ob_eval, nullptr);
  ObjectMeshMapState *state = BKE_mesh_maps_object_state_ensure(
      *ob, *ma, MA_MESH_MAP_NORMAL_TANGENT);
  ASSERT_NE(state, nullptr);

  uint32_t hash[2];
  BKE_mesh_maps_object_hash(*ob_eval, *ma, MA_MESH_MAP_NORMAL_TANGENT, hash);
  BKE_mesh_maps_object_state_mark_baked(*state, hash, 1);
  ASSERT_EQ(state->status, OB_MESH_MAP_STATUS_VALID);
  EXPECT_TRUE(BKE_mesh_maps_object_state_is_current(*state, *ob_eval, *ma));

  /* The DirectX flag is part of the Tangent Normal hash; flipping it makes the old bake stale. */
  ma->mesh_map_settings.use_directx_normal = ma->mesh_map_settings.use_directx_normal ? 0 : 1;
  EXPECT_FALSE(BKE_mesh_maps_object_state_is_current(*state, *ob_eval, *ma));
  /* `is_current` reads; it must not have rewritten the status the filter checks first. */
  EXPECT_EQ(state->status, OB_MESH_MAP_STATUS_VALID);

  DEG_graph_free(graph);
}

/**
 * Guard: a modifier with different viewport and render levels evaluates to different geometry, so
 * a hash from a render depsgraph disagrees with the viewport one `refresh()` uses. This is why the
 * commit must hash from the viewport depsgraph.
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
  Object *ob_viewport =
      eval_object(*bmain, *scene, *view_layer, *ob, DAG_EVAL_VIEWPORT, viewport_graph);
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
  ASSERT_TRUE(BKE_mesh_maps_bake_hash_from_viewport(
      *bmain, *scene, *view_layer, *ob, *ma, MA_MESH_MAP_AO, helper_hash));
  EXPECT_EQ(memcmp(helper_hash, viewport_hash, sizeof(helper_hash)), 0);

  DEG_graph_free(viewport_graph);
  DEG_graph_free(render_graph);
}

/**
 * Position in BOX/SPHERE mode fits the object's *world-space* bounds, so a rotation that changes
 * the world AABB changes the baked pixels even though RAW/BOX/SPHERE are otherwise transform
 * agnostic. The hash must therefore include the object matrix in every mode, not only RAW.
 */
TEST_F(MeshMapBakeTest, position_hash_follows_object_rotation_in_box_mode)
{
  Mesh *mesh = add_quad_two_uv("Quad");
  Material *ma = add_material("MapsMat");
  Object *ob = add_simple_object("Ob", mesh, ma);
  ASSERT_NE(ob->runtime, nullptr);

  ma->mesh_map_settings.position_normalization = MA_MESH_MAP_POSITION_BOX;

  uint32_t before[2];
  BKE_mesh_maps_object_hash(*ob, *ma, MA_MESH_MAP_POSITION, before);

  /* Rotate 90 degrees around X: the quad leaves the XY plane, so its world AABB gains a Z
   * extent and the normalized Position map would change. */
  ob->runtime->object_to_world[1][1] = 0.0f;
  ob->runtime->object_to_world[1][2] = -1.0f;
  ob->runtime->object_to_world[2][1] = 1.0f;
  ob->runtime->object_to_world[2][2] = 0.0f;

  uint32_t after[2];
  BKE_mesh_maps_object_hash(*ob, *ma, MA_MESH_MAP_POSITION, after);
  EXPECT_NE(memcmp(before, after, sizeof(before)), 0);
}

TEST_F(MeshMapBakeTest, foreign_coverage_only_material_and_active_uv)
{
  Material *ma = add_material("MapsMat");
  Material *other = add_material("OtherMat");

  Mesh *quad = add_quad_two_uv("Quad");
  Mesh *quad_other = add_quad_two_uv("QuadOther");
  Mesh *quad_outside = add_quad_two_uv("QuadOutside");
  Mesh *pair = add_pair_mesh("Pair");
  Object *self = add_simple_object("Self", quad, ma);
  Object *good = add_simple_object("Good", quad, ma);
  Object *wrong_material = add_simple_object("WrongMaterial", quad_other, other);
  /* The material is in the slot but the mesh has no UV layer at all. */
  Object *no_uv = add_simple_object("NoUv", pair, ma);
  /* Uses the material and the layer but lives outside the scene: the atlas is a material resource,
   * so it still owns texels and is taken from its own (unevaluated) mesh. */
  Object *outside = add_simple_object("Outside", quad_outside, ma);

  Scene *scene = add_scene_with_object("Scene", self);
  BKE_collection_object_add(bmain, scene->master_collection, good);
  BKE_collection_object_add(bmain, scene->master_collection, wrong_material);
  BKE_collection_object_add(bmain, scene->master_collection, no_uv);
  ViewLayer *view_layer = static_cast<ViewLayer *>(scene->view_layers.first);

  const int resolution = 8;
  const size_t pixels_num = size_t(resolution) * resolution;
  /* Claiming every texel as our own makes any foreign coverage an overlap. */
  Array<uint8_t> own_coverage(pixels_num, 1);
  Array<uint8_t> foreign_coverage(pixels_num, 0);
  char overlap_name[MAX_ID_NAME] = "";
  const int foreign_num = BKE_mesh_maps_bake_foreign_coverage(
      *bmain,
      *scene,
      *view_layer,
      *self,
      *ma,
      resolution,
      Span<int>({1001}),
      own_coverage,
      populate_geometry_pixels,
      foreign_coverage,
      MutableSpan<char>(overlap_name, sizeof(overlap_name)));

  /* `good` and the out-of-scene `outside` qualify; `self` is the baking object, `wrong_material`
   * does not use `ma` and `no_uv` lacks any UV layer. The first overlapping name in
   * `bmain.objects` order is `good`. */
  EXPECT_EQ(foreign_num, 2);
  EXPECT_STREQ(overlap_name, "Good");
  size_t covered = 0;
  for (const uint8_t value : foreign_coverage) {
    covered += value;
  }
  EXPECT_GT(covered, 0);
}

TEST_F(MeshMapBakeTest, plan_pairs_active_and_all_scopes)
{
  Material *ma = add_material("MapsMat");
  Material *other = add_material("OtherMat");
  Mesh *mesh = add_pair_mesh("Pair");
  Object *a = add_simple_object("A", mesh, ma);
  Object *b = add_simple_object("B", mesh, ma);
  Object *c = add_simple_object("C", mesh, other);

  Vector<MeshMapBakePair> active;
  BKE_mesh_maps_bake_plan_pairs(
      *bmain, a, *ma, MA_MESH_MAP_BAKE_ACTIVE_OBJECT, MESH_MAP_BAKE_TYPE_MASK_SUPPORTED, active);
  ASSERT_EQ(active.size(), 11u);
  for (const MeshMapBakePair &pair : active) {
    EXPECT_EQ(pair.object, a);
    EXPECT_EQ(pair.material, ma);
  }
  EXPECT_EQ(active[0].type, MA_MESH_MAP_AO);
  EXPECT_EQ(active[6].type, MA_MESH_MAP_EDGE);

  /* A mask keeps only its set, supported bits; a bit outside the enum is ignored. Thickness is a
   * supported type now, so the ignored bit must be one no type names. */
  Vector<MeshMapBakePair> masked;
  const uint32_t mask = (1u << MA_MESH_MAP_AO) | (1u << MA_MESH_MAP_ID_MATERIAL) |
                        (1u << MA_MESH_MAP_TYPE_NUM);
  BKE_mesh_maps_bake_plan_pairs(*bmain, a, *ma, MA_MESH_MAP_BAKE_ACTIVE_OBJECT, mask, masked);
  ASSERT_EQ(masked.size(), 2u);
  EXPECT_EQ(masked[0].type, MA_MESH_MAP_AO);
  EXPECT_EQ(masked[1].type, MA_MESH_MAP_ID_MATERIAL);

  Vector<MeshMapBakePair> all;
  BKE_mesh_maps_bake_plan_pairs(
      *bmain, nullptr, *ma, MA_MESH_MAP_BAKE_ALL_OBJECTS, MESH_MAP_BAKE_TYPE_MASK_SUPPORTED, all);
  /* A and B use `ma`; each contributes the eleven supported types. */
  ASSERT_EQ(all.size(), 22u);
  bool saw_a = false;
  bool saw_b = false;
  bool saw_c = false;
  for (const MeshMapBakePair &pair : all) {
    saw_a = saw_a || pair.object == a;
    saw_b = saw_b || pair.object == b;
    saw_c = saw_c || pair.object == c;
  }
  EXPECT_TRUE(saw_a);
  EXPECT_TRUE(saw_b);
  EXPECT_FALSE(saw_c);
}

TEST_F(MeshMapBakeTest, plan_pairs_normal_filter_bakes_tangent_normal_before_curvature)
{
  Material *ma = add_material("MapsMat");
  ma->mesh_map_settings.curvature_method = MA_MESH_MAP_CURVATURE_NORMAL_FILTER;
  Mesh *mesh = add_pair_mesh("Pair");
  Object *a = add_simple_object("A", mesh, ma);
  Object *b = add_simple_object("B", mesh, ma);

  /* The Normal Filter fills the Curvature pair from the Tangent Normal atlas of the same object,
   * so the plan must bake that atlas first: the tangent pair is pulled in front of the curvature
   * pair while every other pair keeps its ascending place. */
  const uint32_t mask = (1u << MA_MESH_MAP_AO) | (1u << MA_MESH_MAP_CURVATURE) |
                        (1u << MA_MESH_MAP_NORMAL_TANGENT);
  Vector<MeshMapBakePair> active;
  BKE_mesh_maps_bake_plan_pairs(*bmain, a, *ma, MA_MESH_MAP_BAKE_ACTIVE_OBJECT, mask, active);
  ASSERT_EQ(active.size(), 3u);
  EXPECT_EQ(active[0].type, MA_MESH_MAP_AO);
  EXPECT_EQ(active[1].type, MA_MESH_MAP_NORMAL_TANGENT);
  EXPECT_EQ(active[2].type, MA_MESH_MAP_CURVATURE);
  for (const MeshMapBakePair &pair : active) {
    EXPECT_EQ(pair.object, a);
    EXPECT_EQ(pair.material, ma);
  }

  /* The all-objects sweep still groups every type of an object together and enforces the edge
   * inside each group, so each object's atlas is ready before its own curvature reads it. */
  Vector<MeshMapBakePair> all;
  BKE_mesh_maps_bake_plan_pairs(*bmain, nullptr, *ma, MA_MESH_MAP_BAKE_ALL_OBJECTS, mask, all);
  ASSERT_EQ(all.size(), 6u);
  Object *objects[2] = {a, b};
  for (int group = 0; group < 2; group++) {
    for (int i = 0; i < 3; i++) {
      const MeshMapBakePair &pair = all[group * 3 + i];
      EXPECT_EQ(pair.object, objects[group]);
      EXPECT_EQ(pair.material, ma);
    }
    EXPECT_EQ(all[group * 3 + 0].type, MA_MESH_MAP_AO);
    EXPECT_EQ(all[group * 3 + 1].type, MA_MESH_MAP_NORMAL_TANGENT);
    EXPECT_EQ(all[group * 3 + 2].type, MA_MESH_MAP_CURVATURE);
  }

  /* The default Pointiness curvature renders on its own, so the ascending order stays untouched. */
  Material *pointiness = add_material("PointinessMat");
  Vector<MeshMapBakePair> ascending;
  BKE_mesh_maps_bake_plan_pairs(
      *bmain, a, *pointiness, MA_MESH_MAP_BAKE_ACTIVE_OBJECT, mask, ascending);
  ASSERT_EQ(ascending.size(), 3u);
  EXPECT_EQ(ascending[0].type, MA_MESH_MAP_AO);
  EXPECT_EQ(ascending[1].type, MA_MESH_MAP_CURVATURE);
  EXPECT_EQ(ascending[2].type, MA_MESH_MAP_NORMAL_TANGENT);
  for (const MeshMapBakePair &pair : ascending) {
    EXPECT_EQ(pair.material, pointiness);
  }

  /* Without the tangent bit the plan holds no dependency edge, so the reorder leaves it alone. */
  const uint32_t no_tangent_mask = (1u << MA_MESH_MAP_AO) | (1u << MA_MESH_MAP_CURVATURE);
  Vector<MeshMapBakePair> without_tangent;
  BKE_mesh_maps_bake_plan_pairs(
      *bmain, a, *ma, MA_MESH_MAP_BAKE_ACTIVE_OBJECT, no_tangent_mask, without_tangent);
  ASSERT_EQ(without_tangent.size(), 2u);
  EXPECT_EQ(without_tangent[0].type, MA_MESH_MAP_AO);
  EXPECT_EQ(without_tangent[1].type, MA_MESH_MAP_CURVATURE);
}

TEST_F(MeshMapBakeTest, states_rollback_restores_previous)
{
  Material *ma = add_material("MapsMat");
  Object *a = add_empty("A");
  Object *b = add_empty("B");
  Object *c = add_empty("C");
  ObjectMeshMapState *sa = BKE_mesh_maps_object_state_ensure(*a, *ma, MA_MESH_MAP_AO);
  ObjectMeshMapState *sb = BKE_mesh_maps_object_state_ensure(*b, *ma, MA_MESH_MAP_AO);
  ObjectMeshMapState *sc = BKE_mesh_maps_object_state_ensure(*c, *ma, MA_MESH_MAP_AO);
  ASSERT_NE(sa, nullptr);
  ASSERT_NE(sb, nullptr);
  ASSERT_NE(sc, nullptr);

  sa->status = OB_MESH_MAP_STATUS_VALID;
  const int8_t previous[3] = {
      BKE_mesh_maps_bake_state_begin(*sa),
      BKE_mesh_maps_bake_state_begin(*sb),
      BKE_mesh_maps_bake_state_begin(*sc),
  };
  EXPECT_EQ(previous[0], OB_MESH_MAP_STATUS_VALID);
  EXPECT_EQ(previous[1], OB_MESH_MAP_STATUS_NONE);

  ObjectMeshMapState *states[3] = {sa, sb, sc};
  BKE_mesh_maps_bake_states_rollback(Span<ObjectMeshMapState *>(states, 3),
                                     Span<int8_t>(previous, 3));
  EXPECT_EQ(sa->status, OB_MESH_MAP_STATUS_VALID);
  EXPECT_EQ(sb->status, OB_MESH_MAP_STATUS_NONE);
  EXPECT_EQ(sc->status, OB_MESH_MAP_STATUS_NONE);
}

TEST_F(MeshMapBakeTest, error_of_one_pair_leaves_the_others_alone)
{
  Material *ma = add_material("MapsMat");
  Object *a = add_empty("A");
  Object *b = add_empty("B");
  Object *c = add_empty("C");
  ObjectMeshMapState *sa = BKE_mesh_maps_object_state_ensure(*a, *ma, MA_MESH_MAP_AO);
  ObjectMeshMapState *sb = BKE_mesh_maps_object_state_ensure(*b, *ma, MA_MESH_MAP_AO);
  ObjectMeshMapState *sc = BKE_mesh_maps_object_state_ensure(*c, *ma, MA_MESH_MAP_AO);
  ASSERT_NE(sa, nullptr);
  ASSERT_NE(sb, nullptr);
  ASSERT_NE(sc, nullptr);
  BKE_mesh_maps_bake_state_begin(*sa);
  BKE_mesh_maps_bake_state_begin(*sb);
  BKE_mesh_maps_bake_state_begin(*sc);

  const uint32_t hash[2] = {0x12345678u, 0x9ABCDEF0u};
  BKE_mesh_maps_bake_state_fail(*sb);
  BKE_mesh_maps_bake_state_commit(*sa, hash, hash, 1);
  BKE_mesh_maps_bake_state_commit(*sc, hash, hash, 2);

  EXPECT_EQ(sa->status, OB_MESH_MAP_STATUS_VALID);
  EXPECT_EQ(sb->status, OB_MESH_MAP_STATUS_ERROR);
  EXPECT_EQ(sc->status, OB_MESH_MAP_STATUS_VALID);
}

TEST_F(MeshMapBakeTest, clear_type_resets_atlas_to_transparent_black)
{
  Material *ma = add_material("MapsMat");
  Object *a = add_empty("A");
  Object *b = add_empty("B");
  ObjectMeshMapState *sa = BKE_mesh_maps_object_state_ensure(*a, *ma, MA_MESH_MAP_AO);
  ObjectMeshMapState *sb = BKE_mesh_maps_object_state_ensure(*b, *ma, MA_MESH_MAP_AO);
  ObjectMeshMapState *other_type = BKE_mesh_maps_object_state_ensure(*b, *ma,
                                                                     MA_MESH_MAP_CURVATURE);
  ASSERT_NE(sa, nullptr);
  ASSERT_NE(sb, nullptr);
  ASSERT_NE(other_type, nullptr);
  const uint32_t hash[2] = {7u, 8u};
  BKE_mesh_maps_object_state_mark_baked(*sa, hash, 5);
  BKE_mesh_maps_object_state_mark_baked(*sb, hash, 5);
  BKE_mesh_maps_object_state_mark_baked(*other_type, hash, 5);

  Image *atlas = BKE_mesh_maps_bake_atlas_ensure(*bmain, *ma, MA_MESH_MAP_AO, 8);
  ASSERT_NE(atlas, nullptr);
  {
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(atlas, nullptr, &lock);
    ASSERT_NE(ibuf, nullptr);
    float *data = ibuf->float_data_for_write();
    ASSERT_NE(data, nullptr);
    data[0] = 0.5f;
    data[1] = 0.5f;
    data[2] = 0.5f;
    /* A written texel's alpha is 1; the clear has to take it back to the pristine 0. */
    data[3] = 1.0f;
    BKE_image_release_ibuf(atlas, ibuf, lock);
  }

  const int reset = BKE_mesh_maps_bake_clear_type(*bmain, *ma, MA_MESH_MAP_AO);
  EXPECT_EQ(reset, 2);
  EXPECT_EQ(sa->status, OB_MESH_MAP_STATUS_NONE);
  EXPECT_EQ(sb->status, OB_MESH_MAP_STATUS_NONE);
  EXPECT_EQ(sa->hash[0], 0u);
  EXPECT_EQ(sb->baked_time, 0);
  /* Another map type is untouched. */
  EXPECT_EQ(other_type->status, OB_MESH_MAP_STATUS_VALID);

  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(atlas, nullptr, &lock);
  ASSERT_NE(ibuf, nullptr);
  const float *data = ibuf->float_data();
  ASSERT_NE(data, nullptr);
  /* The cleared atlas is the pristine transparent black: alpha reads as unwritten again. */
  EXPECT_FLOAT_EQ(data[0], 0.0f);
  EXPECT_FLOAT_EQ(data[1], 0.0f);
  EXPECT_FLOAT_EQ(data[2], 0.0f);
  EXPECT_FLOAT_EQ(data[3], 0.0f);
  BKE_image_release_ibuf(atlas, ibuf, lock);
}

/** A byte atlas must not be reported as a valid bake target. */
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

/**
 * The main/worker protocol must not let a slow main thread (its update runs on the WM timer, which
 * can be slower than the worker's 50 ms wait timeout) make the worker give up after the first pair.
 */
TEST_F(MeshMapBakeTest, handshake_worker_survives_a_slow_main_thread)
{
  MeshMapBakeHandshake handshake;
  std::atomic<int> rendered{0};
  std::atomic<bool> stop{false};
  const int pairs = 3;

  std::thread worker([&] {
    for (;;) {
      if (!handshake.worker_await_world([&] { return stop.load(); })) {
        break;
      }
      rendered.fetch_add(1);
      if (!handshake.worker_publish_result([&] { return stop.load(); })) {
        break;
      }
    }
  });

  for (int i = 0; i < pairs; i++) {
    /* Slower than the worker's wait timeout: a single timed wait would give up here. */
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    handshake.main_publish_world(true);
    /* Bounded, so a worker that gave up fails the assertion instead of hanging the test. */
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!handshake.main_try_take_result()) {
      if (std::chrono::steady_clock::now() >= deadline) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    handshake.main_finish_commit();
  }
  handshake.main_publish_world(false);
  worker.join();
  EXPECT_EQ(rendered.load(), pairs);
}

/** A kill while the worker waits for a world must exit promptly and render nothing. */
TEST_F(MeshMapBakeTest, handshake_stop_while_waiting_for_a_world)
{
  MeshMapBakeHandshake handshake;
  std::atomic<bool> stop{false};
  std::atomic<int> rendered{0};
  std::thread worker([&] {
    if (handshake.worker_await_world([&] { return stop.load(); })) {
      rendered.fetch_add(1);
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  const auto start = std::chrono::steady_clock::now();
  stop.store(true);
  worker.join();
  const auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_EQ(rendered.load(), 0);
  /* The stop flag is only re-checked when the wait poll times out (#MeshMapBakeHandshake::kTimeout
   * is 50 ms and nothing notifies on stop), so allow a full poll cycle plus scheduler jitter. */
  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 250);
}

/** A kill while the worker waits for the commit must exit promptly and commit nothing. */
TEST_F(MeshMapBakeTest, handshake_stop_while_waiting_for_the_commit)
{
  MeshMapBakeHandshake handshake;
  std::atomic<bool> stop{false};
  std::atomic<int> rendered{0};
  std::atomic<bool> committed{false};
  std::thread worker([&] {
    if (!handshake.worker_await_world([&] { return stop.load(); })) {
      return;
    }
    rendered.fetch_add(1);
    if (handshake.worker_publish_result([&] { return stop.load(); })) {
      committed.store(true);
    }
  });

  handshake.main_publish_world(true);
  while (rendered.load() == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  const auto start = std::chrono::steady_clock::now();
  stop.store(true);
  worker.join();
  const auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_EQ(rendered.load(), 1);
  EXPECT_FALSE(committed.load());
  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 100);
}

/** High-poly sources are inputs, never foreign atlas owners and never low-poly bake targets. */
TEST_F(MeshMapBakeTest, source_objects_are_not_foreign_or_baked)
{
  Material *ma = add_material("MapsMat");

  Mesh *low_mesh = add_quad_two_uv("LowQuad");
  Mesh *high_mesh = add_quad_two_uv("HighQuad");
  Object *low = add_simple_object("Low", low_mesh, ma);
  Object *high = add_simple_object("High", high_mesh, ma);

  Scene *scene = add_scene_with_object("Scene", low);
  BKE_collection_object_add(bmain, scene->master_collection, high);
  ViewLayer *view_layer = static_cast<ViewLayer *>(scene->view_layers.first);

  ObjectMeshMapSource *source = BKE_mesh_maps_source_ensure(*low, *ma);
  ASSERT_NE(source, nullptr);
  source->high_poly = high;

  const int resolution = 8;
  const size_t pixels_num = size_t(resolution) * resolution;
  Array<uint8_t> own_coverage(pixels_num, 1);
  Array<uint8_t> foreign_coverage(pixels_num, 0);
  char overlap_name[MAX_ID_NAME] = "";
  const int foreign_num = BKE_mesh_maps_bake_foreign_coverage(
      *bmain,
      *scene,
      *view_layer,
      *low,
      *ma,
      resolution,
      Span<int>({1001}),
      own_coverage,
      populate_geometry_pixels,
      foreign_coverage,
      MutableSpan<char>(overlap_name, sizeof(overlap_name)));
  EXPECT_EQ(foreign_num, 0);

  Vector<MeshMapBakePair> pairs;
  BKE_mesh_maps_bake_plan_pairs(
      *bmain, nullptr, *ma, MA_MESH_MAP_BAKE_ALL_OBJECTS, MESH_MAP_BAKE_TYPE_MASK_SUPPORTED, pairs);
  ASSERT_EQ(pairs.size(), 11u);
  for (const MeshMapBakePair &pair : pairs) {
    EXPECT_EQ(pair.object, low);
  }
}

/**
 * The panel's RNA query (#Object.mesh_map_bake_target_for) asks the predicate with a null source
 * set, so the predicate must collect the sources itself and still keep a high-poly source out of
 * the plan: the same rule the pre-collected sweep uses, just without a shared set.
 */
TEST_F(MeshMapBakeTest, object_in_plan_collects_sources_when_none_supplied)
{
  Material *ma = add_material("MapsMat");
  Material *other = add_material("OtherMat");

  Object *low = add_simple_object("Low", add_quad_two_uv("LowQuad"), ma);
  Object *high = add_simple_object("High", add_quad_two_uv("HighQuad"), ma);
  Object *unrelated = add_simple_object("Unrelated", add_quad_two_uv("UnrelatedQuad"), other);

  ObjectMeshMapSource *source = BKE_mesh_maps_source_ensure(*low, *ma);
  ASSERT_NE(source, nullptr);
  source->high_poly = high;

  /* The low-poly is a target; its high-poly source and an object of another material are not. */
  EXPECT_TRUE(BKE_mesh_maps_bake_object_in_plan(
      *bmain, *low, *ma, nullptr, MA_MESH_MAP_BAKE_ALL_OBJECTS, nullptr));
  EXPECT_FALSE(BKE_mesh_maps_bake_object_in_plan(
      *bmain, *high, *ma, nullptr, MA_MESH_MAP_BAKE_ALL_OBJECTS, nullptr));
  EXPECT_FALSE(BKE_mesh_maps_bake_object_in_plan(
      *bmain, *unrelated, *ma, nullptr, MA_MESH_MAP_BAKE_ALL_OBJECTS, nullptr));
}

/** The panel's whole-list query resolves every target in one sweep, sources excluded. */
TEST_F(MeshMapBakeTest, bake_target_objects_lists_the_all_objects_scope)
{
  Material *ma = add_material("MapsMat");
  Material *other = add_material("OtherMat");

  Object *low = add_simple_object("Low", add_quad_two_uv("LowQuad"), ma);
  Object *high = add_simple_object("High", add_quad_two_uv("HighQuad"), ma);
  add_simple_object("Unrelated", add_quad_two_uv("UnrelatedQuad"), other);

  ObjectMeshMapSource *source = BKE_mesh_maps_source_ensure(*low, *ma);
  ASSERT_NE(source, nullptr);
  source->high_poly = high;

  Vector<Object *> objects;
  BKE_mesh_maps_bake_target_objects(*bmain, *ma, objects);
  ASSERT_EQ(objects.size(), 1);
  EXPECT_EQ(objects[0], low);
}

TEST_F(MeshMapBakeTest, cage_matches_requires_same_face_and_corner_count)
{
  Mesh *pair = add_pair_mesh("Pair");
  Mesh *pair2 = add_pair_mesh("Pair2");
  Mesh *quad = add_quad_two_uv("Quad");
  EXPECT_TRUE(BKE_mesh_maps_bake_cage_matches(*pair, *pair2));
  EXPECT_FALSE(BKE_mesh_maps_bake_cage_matches(*pair, *quad));
}

TEST_F(MeshMapBakeTest, fill_id_from_high_poly_reads_hit_object_and_material)
{
  Mesh *mesh = add_pair_mesh("Pair");
  Material *first = add_material("First");
  Material *second = add_material("Second");
  Object *ob = add_object("Ob", mesh, first, second);

  /* Triangle 0 belongs to face 0 (slot 0), triangle 1 to face 1 (slot 1); a -1 object is a miss. */
  BakePixel pixels[3] = {};
  pixels[0].object_id = 0;
  pixels[0].primitive_id = 0;
  pixels[1].object_id = 0;
  pixels[1].primitive_id = 1;
  pixels[2].object_id = -1;

  const Object *objects[1] = {ob};
  const Mesh *meshes[1] = {mesh};
  float expected[3];

  float material_result[12] = {};
  BKE_mesh_maps_bake_fill_id_from_high_poly(
      pixels, 3, objects, 1, meshes, 1, MA_MESH_MAP_ID_MATERIAL, 4, material_result);
  BKE_mesh_maps_bake_id_material_color(0, expected);
  EXPECT_FLOAT_EQ(material_result[0], expected[0]);
  EXPECT_FLOAT_EQ(material_result[1], expected[1]);
  EXPECT_FLOAT_EQ(material_result[2], expected[2]);
  BKE_mesh_maps_bake_id_material_color(1, expected);
  EXPECT_FLOAT_EQ(material_result[4], expected[0]);
  EXPECT_FLOAT_EQ(material_result[5], expected[1]);
  EXPECT_FLOAT_EQ(material_result[6], expected[2]);
  for (int i = 8; i < 12; i++) {
    EXPECT_FLOAT_EQ(material_result[i], 0.0f);
  }

  float object_result[12] = {};
  BKE_mesh_maps_bake_fill_id_from_high_poly(
      pixels, 3, objects, 1, meshes, 1, MA_MESH_MAP_ID_OBJECT, 4, object_result);
  BKE_mesh_maps_bake_id_object_color(*ob, expected);
  EXPECT_FLOAT_EQ(object_result[0], expected[0]);
  EXPECT_FLOAT_EQ(object_result[1], expected[1]);
  EXPECT_FLOAT_EQ(object_result[2], expected[2]);
  EXPECT_FLOAT_EQ(object_result[4], expected[0]);
  for (int i = 8; i < 12; i++) {
    EXPECT_FLOAT_EQ(object_result[i], 0.0f);
  }
}

TEST_F(MeshMapBakeTest, fill_id_from_high_poly_skips_out_of_range_object)
{
  Mesh *mesh = add_pair_mesh("Pair");
  Material *first = add_material("First");
  Material *second = add_material("Second");
  Object *ob = add_object("Ob", mesh, first, second);

  /* Texel 0 is a valid hit, texel 1 names an object past the end of the array. */
  BakePixel pixels[2] = {};
  pixels[0].object_id = 0;
  pixels[0].primitive_id = 0;
  pixels[1].object_id = 5;
  pixels[1].primitive_id = 0;
  const Object *objects[1] = {ob};
  const Mesh *meshes[1] = {mesh};

  float result[8];
  for (int i = 0; i < 8; i++) {
    result[i] = 9.0f;
  }
  BKE_mesh_maps_bake_fill_id_from_high_poly(
      pixels, 2, objects, 1, meshes, 1, MA_MESH_MAP_ID_OBJECT, 4, result);

  float expected[3];
  BKE_mesh_maps_bake_id_object_color(*ob, expected);
  EXPECT_FLOAT_EQ(result[0], expected[0]);
  EXPECT_FLOAT_EQ(result[1], expected[1]);
  EXPECT_FLOAT_EQ(result[2], expected[2]);
  /* The out-of-range texel is left untouched, sentinel included. */
  for (int i = 4; i < 8; i++) {
    EXPECT_FLOAT_EQ(result[i], 9.0f);
  }
}

}  // namespace blender::bke::tests
